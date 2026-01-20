#include "openai-client.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "log.h"

OpenaiChatMessage *
openai_chat_message_new(const gchar *role, const gchar *content)
{
  OpenaiChatMessage *msg = g_new0(OpenaiChatMessage, 1);
  msg->role = g_strdup(role ? role : "user");
  msg->content = g_strdup(content ? content : "");
  return msg;
}

void
openai_chat_message_free(OpenaiChatMessage *msg)
{
  if (!msg)
    return;
  g_free(msg->role);
  g_free(msg->content);
  g_free(msg);
}

static OpenaiClientResult *
openai_client_result_new_error(gint http_status, const gchar *message)
{
  OpenaiClientResult *r = g_new0(OpenaiClientResult, 1);
  r->ok = FALSE;
  r->http_status = http_status;
  r->error_message = g_strdup(message ? message : "Request failed.");
  return r;
}

static OpenaiClientResult *
openai_client_result_new_ok(const gchar *content)
{
  OpenaiClientResult *r = g_new0(OpenaiClientResult, 1);
  r->ok = TRUE;
  r->http_status = 200;
  r->content = g_strdup(content ? content : "");
  return r;
}

static void
openai_client_result_free(OpenaiClientResult *r)
{
  if (!r)
    return;
  g_free(r->content);
  g_free(r->error_message);
  g_free(r);
}

static gchar *
openai_client_build_body(const gchar *model, gdouble temperature, GPtrArray *messages)
{
  g_autoptr(JsonBuilder) b = json_builder_new();

  json_builder_begin_object(b);
  json_builder_set_member_name(b, "model");
  json_builder_add_string_value(b, model ? model : "");

  json_builder_set_member_name(b, "temperature");
  json_builder_add_double_value(b, temperature);

  json_builder_set_member_name(b, "messages");
  json_builder_begin_array(b);
  for (guint i = 0; i < messages->len; i++)
  {
    OpenaiChatMessage *m = g_ptr_array_index(messages, i);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "role");
    json_builder_add_string_value(b, m->role ? m->role : "user");
    json_builder_set_member_name(b, "content");
    json_builder_add_string_value(b, m->content ? m->content : "");
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  return json_generator_to_data(gen, NULL);
}

static gchar *
json_read_string_member(JsonObject *obj, const gchar *name)
{
  if (!obj || !name)
    return NULL;
  if (!json_object_has_member(obj, name))
    return NULL;
  JsonNode *node = json_object_get_member(obj, name);
  if (!node || json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return g_strdup(json_node_get_string(node));
}

static OpenaiClientResult *
openai_client_parse_response(gint http_status, const gchar *body)
{
  if (!body)
    return openai_client_result_new_error(http_status, "Empty response body.");

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) error = NULL;
  if (!json_parser_load_from_data(parser, body, -1, &error))
    return openai_client_result_new_error(http_status, error ? error->message : "Invalid JSON response.");

  JsonNode *root = json_parser_get_root(parser);
  if (!root || json_node_get_node_type(root) != JSON_NODE_OBJECT)
    return openai_client_result_new_error(http_status, "Unexpected JSON response.");

  JsonObject *obj = json_node_get_object(root);

  if (json_object_has_member(obj, "error"))
  {
    JsonObject *err_obj = json_object_get_object_member(obj, "error");
    g_autofree gchar *msg = json_read_string_member(err_obj, "message");
    return openai_client_result_new_error(http_status, msg ? msg : "Provider returned an error.");
  }

  if (!json_object_has_member(obj, "choices"))
    return openai_client_result_new_error(http_status, "No choices in response.");

  JsonArray *choices = json_object_get_array_member(obj, "choices");
  if (!choices || json_array_get_length(choices) == 0)
    return openai_client_result_new_error(http_status, "Empty choices array.");

  JsonObject *choice0 = json_array_get_object_element(choices, 0);
  if (!choice0)
    return openai_client_result_new_error(http_status, "Invalid choices[0].");

  if (json_object_has_member(choice0, "message"))
  {
    JsonObject *msg = json_object_get_object_member(choice0, "message");
    g_autofree gchar *content = json_read_string_member(msg, "content");
    if (content)
      return openai_client_result_new_ok(content);
  }

  if (json_object_has_member(choice0, "text"))
  {
    g_autofree gchar *content = json_read_string_member(choice0, "text");
    if (content)
      return openai_client_result_new_ok(content);
  }

  return openai_client_result_new_error(http_status, "Unable to find message content in response.");
}

typedef struct
{
  SoupSession *session;
  SoupMessage *msg;
  OpenaiClientCallback callback;
  gpointer user_data;
} OpenaiClientCtx;

static void
openai_client_finish(OpenaiClientCtx *ctx, OpenaiClientResult *result)
{
  if (ctx->callback)
    ctx->callback(result, ctx->user_data);
  openai_client_result_free(result);

  g_clear_object(&ctx->msg);
  g_clear_object(&ctx->session);
  g_free(ctx);
}

static void
openai_client_on_send_finish(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  OpenaiClientCtx *ctx = user_data;

  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(ctx->session, res, &error);

  gint status = soup_message_get_status(ctx->msg);
  if (!bytes)
  {
    openai_ask_log("http error status=%d msg=%s", status, error ? error->message : "request failed");
    return openai_client_finish(ctx, openai_client_result_new_error(status, error ? error->message : "Request failed."));
  }

  gsize size = 0;
  const gchar *data = g_bytes_get_data(bytes, &size);
  g_autofree gchar *body = g_strndup(data ? data : "", size);

  if (status < 200 || status >= 300)
  {
    g_autofree gchar *snippet = g_strndup(body, 800);
    openai_ask_log("http non-2xx status=%d body=%s", status, snippet ? snippet : "");
    OpenaiClientResult *r = openai_client_parse_response(status, body);
    if (r->ok)
    {
      r->ok = FALSE;
      r->http_status = status;
      g_free(r->content);
      r->content = NULL;
      if (!r->error_message)
        r->error_message = g_strdup("HTTP error from provider.");
    }
    if (!r->error_message || !*r->error_message)
    {
      g_free(r->error_message);
      r->error_message = g_strdup_printf("HTTP %d from provider.", status);
    }
    return openai_client_finish(ctx, r);
  }

  openai_ask_log("http ok status=%d bytes=%zu", status, (size_t)size);
  return openai_client_finish(ctx, openai_client_parse_response(status, body));
}

void
openai_client_send_chat_async(const gchar *endpoint,
                              const gchar *api_key,
                              const gchar *model,
                              gdouble temperature,
                              GPtrArray *messages,
                              GCancellable *cancellable,
                              OpenaiClientCallback callback,
                              gpointer user_data)
{
  g_return_if_fail(endpoint && *endpoint);
  g_return_if_fail(model && *model);
  g_return_if_fail(messages != NULL);

  g_autofree gchar *body = openai_client_build_body(model, temperature, messages);
  g_autoptr(GBytes) body_bytes = g_bytes_new(body ? body : "{}", body ? strlen(body) : 2);

  OpenaiClientCtx *ctx = g_new0(OpenaiClientCtx, 1);
  ctx->session = soup_session_new();
  ctx->msg = soup_message_new("POST", endpoint);
  ctx->callback = callback;
  ctx->user_data = user_data;

  SoupMessageHeaders *hdrs = soup_message_get_request_headers(ctx->msg);
  soup_message_headers_append(hdrs, "Content-Type", "application/json");
  soup_message_headers_append(hdrs, "Accept", "application/json");
  if (api_key && *api_key)
  {
    g_autofree gchar *auth = g_strdup_printf("Bearer %s", api_key);
    soup_message_headers_append(hdrs, "Authorization", auth);
  }

  soup_message_set_request_body_from_bytes(ctx->msg, "application/json", body_bytes);

  soup_session_send_and_read_async(
    ctx->session,
    ctx->msg,
    G_PRIORITY_DEFAULT,
    cancellable,
    openai_client_on_send_finish,
    ctx);
}

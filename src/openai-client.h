#pragma once

#include <glib.h>
#include <gio/gio.h>

typedef struct
{
  gchar *role;
  gchar *content;
} OpenaiChatMessage;

OpenaiChatMessage *openai_chat_message_new(const gchar *role, const gchar *content);
void openai_chat_message_free(OpenaiChatMessage *msg);

typedef struct
{
  gboolean ok;
  gint http_status;
  gchar *content;
  gchar *error_message;
} OpenaiClientResult;

typedef void (*OpenaiClientCallback)(OpenaiClientResult *result, gpointer user_data);

void openai_client_send_chat_async(const gchar *endpoint,
                                   const gchar *api_key,
                                   const gchar *model,
                                   gdouble temperature,
                                   GPtrArray *messages, /* element-type OpenaiChatMessage* */
                                   GCancellable *cancellable,
                                   OpenaiClientCallback callback,
                                   gpointer user_data);

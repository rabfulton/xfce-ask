#include "keyring.h"

#include <libsecret/secret.h>

static const SecretSchema *
openai_ask_secret_schema(void)
{
  static const SecretSchema schema = {
    "com.rab.xfce-openai-ask",
    SECRET_SCHEMA_NONE,
    {
      {"endpoint", SECRET_SCHEMA_ATTRIBUTE_STRING},
      {NULL, 0},
    },
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
  };
  return &schema;
}

gchar *
keyring_lookup_api_key(const gchar *endpoint)
{
  if (!endpoint || !*endpoint)
    return NULL;

  g_autoptr(GError) error = NULL;
  gchar *pw = secret_password_lookup_sync(openai_ask_secret_schema(), NULL, &error, "endpoint", endpoint, NULL);
  if (error)
    return NULL;
  if (!pw)
    return NULL;
  gchar *dup = g_strdup(pw);
  secret_password_free(pw);
  return dup;
}

gboolean
keyring_store_api_key(const gchar *endpoint, const gchar *api_key)
{
  if (!endpoint || !*endpoint || !api_key || !*api_key)
    return FALSE;

  g_autoptr(GError) error = NULL;
  gboolean ok = secret_password_store_sync(
    openai_ask_secret_schema(),
    SECRET_COLLECTION_DEFAULT,
    "OpenAI Ask API key",
    api_key,
    NULL,
    &error,
    "endpoint",
    endpoint,
    NULL);
  return ok && !error;
}

gboolean
keyring_clear_api_key(const gchar *endpoint)
{
  if (!endpoint || !*endpoint)
    return FALSE;

  g_autoptr(GError) error = NULL;
  gboolean ok = secret_password_clear_sync(openai_ask_secret_schema(), NULL, &error, "endpoint", endpoint, NULL);
  return ok && !error;
}

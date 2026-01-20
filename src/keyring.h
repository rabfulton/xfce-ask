#pragma once

#include <glib.h>

gchar *keyring_lookup_api_key(const gchar *endpoint);
gboolean keyring_store_api_key(const gchar *endpoint, const gchar *api_key);
gboolean keyring_clear_api_key(const gchar *endpoint);


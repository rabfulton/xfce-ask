#pragma once

#include <glib.h>

void openai_ask_log_init(void);
void openai_ask_log(const gchar *fmt, ...) G_GNUC_PRINTF(1, 2);


#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static gchar *g_log_path = NULL;

static const gchar *
openai_ask_log_path(void)
{
  if (g_log_path)
    return g_log_path;

  const gchar *cache = g_get_user_cache_dir();
  if (!cache || !*cache)
    cache = g_get_home_dir();

  g_autofree gchar *dir = g_build_filename(cache, "openai-ask", NULL);
  g_mkdir_with_parents(dir, 0700);

  g_log_path = g_build_filename(dir, "openai-ask.log", NULL);
  return g_log_path;
}

void
openai_ask_log_init(void)
{
  (void)openai_ask_log_path();
}

void
openai_ask_log(const gchar *fmt, ...)
{
  const gchar *path = openai_ask_log_path();

  va_list ap;
  va_start(ap, fmt);
  g_autofree gchar *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);

  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree gchar *ts = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
  FILE *fp = fopen(path, "a");
  if (!fp)
    return;
  fprintf(fp, "%s %s\n", ts ? ts : "", msg ? msg : "");
  fclose(fp);
}

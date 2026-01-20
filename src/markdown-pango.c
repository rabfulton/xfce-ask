#include "markdown-pango.h"

#include <string.h>

static gchar *
rgba_to_hex(const GdkRGBA *rgba)
{
  guint r = (guint)(CLAMP(rgba->red, 0.0, 1.0) * 255.0);
  guint g = (guint)(CLAMP(rgba->green, 0.0, 1.0) * 255.0);
  guint b = (guint)(CLAMP(rgba->blue, 0.0, 1.0) * 255.0);
  return g_strdup_printf("#%02x%02x%02x", r, g, b);
}

static void
get_code_colors(GtkWidget *style_widget, gchar **out_bg, gchar **out_fg)
{
  *out_bg = g_strdup("#404040");
  *out_fg = g_strdup("#ffffff");

  if (!style_widget)
    return;

  GtkStyleContext *ctx = gtk_widget_get_style_context(style_widget);
  if (!ctx)
    return;

  GdkRGBA bg = {0};
  GdkRGBA fg = {0};
  gtk_style_context_get_background_color(ctx, GTK_STATE_FLAG_SELECTED, &bg);
  gtk_style_context_get_color(ctx, GTK_STATE_FLAG_SELECTED, &fg);

  g_free(*out_bg);
  g_free(*out_fg);
  *out_bg = rgba_to_hex(&bg);
  *out_fg = rgba_to_hex(&fg);
}

static gchar *
wrap_inline_code(const gchar *escaped, const gchar *bg, const gchar *fg)
{
  return g_strdup_printf(
    "<span font_family=\"monospace\" background=\"%s\" foreground=\"%s\">%s</span>",
    bg,
    fg,
    escaped ? escaped : "");
}

static gboolean
linkify_bare_url_eval(const GMatchInfo *match_info, GString *res, gpointer user_data)
{
  (void)user_data;
  g_autofree gchar *url = g_match_info_fetch(match_info, 1);
  if (!url)
    return FALSE;

  gsize len = strlen(url);
  gsize trimmed_len = len;
  while (trimmed_len > 0 && strchr(")]>.,;!?", url[trimmed_len - 1]))
    trimmed_len--;

  g_autofree gchar *core = g_strndup(url, trimmed_len);
  g_string_append_printf(res, "<a href=\"%s\">%s</a>", core, core);

  if (trimmed_len < len)
    g_string_append(res, url + trimmed_len);

  return FALSE;
}

typedef struct
{
  const gchar *bg;
  const gchar *fg;
} InlineColors;

static gboolean
bold_code_eval(const GMatchInfo *match_info, GString *res, gpointer user_data)
{
  InlineColors *c = user_data;
  g_autofree gchar *code = g_match_info_fetch(match_info, 1);
  g_string_append_printf(
    res,
    "<b><span font_family=\"monospace\" background=\"%s\" foreground=\"%s\">%s</span></b>",
    c->bg,
    c->fg,
    code ? code : "");
  return FALSE;
}

static gchar *
process_inline(const gchar *text, const gchar *bg, const gchar *fg)
{
  if (!text)
    return g_strdup("");

  g_autofree gchar *escaped = g_markup_escape_text(text, -1);

  /* Markdown links: [label](https://...) */
  g_autoptr(GRegex) md_link = g_regex_new("\\[([^\\]]+)\\]\\((https?://[^\\s)]+|#[^\\s)]+)\\)", 0, 0, NULL);
  g_autofree gchar *stage0 = g_regex_replace(md_link, escaped, -1, 0, "<a href=\"\\2\">\\1</a>", 0, NULL);

  /* Bare URLs (avoid those already inside href="..."). */
  g_autoptr(GRegex) bare_url = g_regex_new("(?<!href=\")((?:https?://)[^\\s<\\[]+)", 0, 0, NULL);
  g_autofree gchar *stage1 = g_regex_replace_eval(bare_url, stage0, -1, 0, 0, linkify_bare_url_eval, NULL, NULL);

  /* Bold inline-code: **`code`** */
  InlineColors colors = {bg, fg};
  g_autoptr(GRegex) bold_code = g_regex_new("\\*\\*`([^`]+)`\\*\\*", 0, 0, NULL);
  g_autofree gchar *stage2 = g_regex_replace_eval(bold_code, stage1, -1, 0, 0, bold_code_eval, &colors, NULL);

  /* Bold: **text** */
  g_autoptr(GRegex) bold = g_regex_new("\\*\\*([^\\n]+?)\\*\\*", G_REGEX_UNGREEDY, 0, NULL);
  g_autofree gchar *stage3 = g_regex_replace(bold, stage2, -1, 0, "<b>\\1</b>", 0, NULL);

  /* Inline code: `code` */
  GString *out = g_string_new(NULL);
  const gchar *p = stage3;
  while (*p)
  {
    const gchar *bt1 = strchr(p, '`');
    if (!bt1)
    {
      g_string_append(out, p);
      break;
    }
    const gchar *bt2 = strchr(bt1 + 1, '`');
    if (!bt2)
    {
      g_string_append(out, p);
      break;
    }
    g_string_append_len(out, p, bt1 - p);
    g_autofree gchar *code = g_strndup(bt1 + 1, bt2 - (bt1 + 1));
    g_autofree gchar *code_span = wrap_inline_code(code, bg, fg);
    g_string_append(out, code_span);
    p = bt2 + 1;
  }
  g_autofree gchar *stage4 = g_string_free(out, FALSE);

  /* Italic: *text* */
  g_autoptr(GRegex) italic = g_regex_new("\\*([^\\*\\n]+)\\*", 0, 0, NULL);
  return g_regex_replace(italic, stage4, -1, 0, "<i>\\1</i>", 0, NULL);
}

static gchar *
render_headers_and_lists(const gchar *text, const gchar *bg, const gchar *fg)
{
  if (!text)
    return g_strdup("");

  g_autoptr(GRegex) audio = g_regex_new("\\n?<audio_file>.*?</audio_file>", G_REGEX_DOTALL, 0, NULL);
  g_autofree gchar *no_audio = g_regex_replace(audio, text, -1, 0, "", 0, NULL);

  g_autofree gchar *unescaped_dollars = g_strdup(no_audio ? no_audio : "");
  for (gchar *p = unescaped_dollars; p && *p; p++)
  {
    if (p[0] == '\\' && p[1] == '$')
    {
      memmove(p, p + 1, strlen(p));
    }
  }

  g_auto(GStrv) lines = g_strsplit(unescaped_dollars, "\n", -1);
  GString *out = g_string_new(NULL);
  for (gint i = 0; lines[i]; i++)
  {
    const gchar *line = lines[i];
    const gchar *trim = line;
    while (*trim == ' ' || *trim == '\t')
      trim++;

    if (g_strcmp0(trim, "***") == 0 || g_strcmp0(trim, "---") == 0)
    {
      g_string_append(out, "<span foreground=\"#888888\">────────</span>");
      g_string_append_c(out, '\n');
      continue;
    }

    /* Bullets: - item / * item */
    if ((trim[0] == '-' || trim[0] == '*') && trim[1] == ' ')
    {
      g_autofree gchar *rest = process_inline(trim + 2, bg, fg);
      g_string_append(out, "• ");
      g_string_append(out, rest);
      g_string_append_c(out, '\n');
      continue;
    }

    /* Headers: #..#### */
    guint level = 0;
    const gchar *h = trim;
    while (*h == '#' && level < 4)
    {
      level++;
      h++;
    }
    if (level > 0 && *h == ' ')
    {
      const gchar *content = h + 1;
      g_autofree gchar *inline_markup = process_inline(content, bg, fg);
      const gchar *size = "large";
      if (level == 1)
        size = "xx-large";
      else if (level == 2)
        size = "x-large";
      else if (level == 3)
        size = "large";
      else if (level == 4)
        size = "medium";
      g_string_append_printf(out, "<span size=\"%s\"><b>%s</b></span>\n", size, inline_markup);
      continue;
    }

    g_autofree gchar *inline_markup = process_inline(line, bg, fg);
    g_string_append(out, inline_markup);
    if (lines[i + 1])
      g_string_append_c(out, '\n');
  }

  return g_string_free(out, FALSE);
}

static gchar *
render_code_block(const gchar *code, const gchar *lang, const gchar *bg, const gchar *fg)
{
  g_autofree gchar *escaped = g_markup_escape_text(code ? code : "", -1);
  g_autofree gchar *lang_line = NULL;
  if (lang && *lang)
  {
    g_autofree gchar *lang_escaped = g_markup_escape_text(lang, -1);
    lang_line = g_strdup_printf("<span size=\"small\" foreground=\"#888888\">%s</span>\n", lang_escaped);
  }
  return g_strdup_printf(
    "%s<span font_family=\"monospace\" background=\"%s\" foreground=\"%s\">%s</span>",
    lang_line ? lang_line : "",
    bg,
    fg,
    escaped);
}

gchar *
markdown_to_pango(const gchar *markdown, GtkWidget *style_widget)
{
  g_autofree gchar *bg = NULL;
  g_autofree gchar *fg = NULL;
  get_code_colors(style_widget, &bg, &fg);

  if (!markdown)
    return g_strdup("");

  g_autoptr(GRegex) code_re = g_regex_new("```([A-Za-z0-9_+-]+)?\\s*([\\s\\S]*?)```", G_REGEX_DOTALL, 0, NULL);

  GString *out = g_string_new(NULL);
  GMatchInfo *mi = NULL;
  gboolean matched = g_regex_match(code_re, markdown, 0, &mi);
  gsize last_end = 0;

  while (matched)
  {
    gint start = 0, end = 0;
    g_match_info_fetch_pos(mi, 0, &start, &end);

    if (start > (gint)last_end)
    {
      g_autofree gchar *segment = g_strndup(markdown + last_end, (gsize)start - last_end);
      g_autofree gchar *rendered = render_headers_and_lists(segment, bg, fg);
      g_string_append(out, rendered);
      if (out->len > 0 && out->str[out->len - 1] != '\n')
        g_string_append_c(out, '\n');
    }

    g_autofree gchar *lang = g_match_info_fetch(mi, 1);
    g_autofree gchar *code = g_match_info_fetch(mi, 2);
    g_autofree gchar *code_markup = render_code_block(code, lang, bg, fg);
    g_string_append(out, code_markup);
    g_string_append_c(out, '\n');

    last_end = (gsize)end;
    matched = g_match_info_next(mi, NULL);
  }

  if (mi)
    g_match_info_free(mi);

  if (last_end < strlen(markdown))
  {
    g_autofree gchar *tail = g_strdup(markdown + last_end);
    g_autofree gchar *rendered = render_headers_and_lists(tail, bg, fg);
    g_string_append(out, rendered);
  }

  return g_string_free(out, FALSE);
}

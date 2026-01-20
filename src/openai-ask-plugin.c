#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <string.h>

#include "keyring.h"
#include "log.h"
#include "markdown-pango.h"
#include "openai-client.h"

typedef struct _OpenaiAskPlugin OpenaiAskPlugin;
typedef struct _OpenaiAskPluginClass OpenaiAskPluginClass;

struct _OpenaiAskPlugin
{
  XfcePanelPlugin parent_instance;

  GtkWidget *container;
  GtkWidget *entry;
  GtkWidget *popup;
  GtkWidget *header;
  GtkWidget *scrolled;
  GtkWidget *popover_title;
  GtkWidget *popover_stack;
  GtkWidget *popover_spinner;
  GtkWidget *popover_label;
  guint relayout_source_id;

  GCancellable *request_cancellable;
  gboolean request_in_flight;

  GPtrArray *messages; /* element-type OpenaiChatMessage* */

  gchar *endpoint;
  gchar *model;
  gchar *system_prompt;
  gdouble temperature;
  gint width_chars;
  gint reply_width_px; /* 0 = match anchor width */
};

struct _OpenaiAskPluginClass
{
  XfcePanelPluginClass parent_class;
};

static void openai_ask_plugin_cancel_inflight(OpenaiAskPlugin *self);
static void openai_ask_plugin_move_popup_near_entry(OpenaiAskPlugin *self);

XFCE_PANEL_DEFINE_PLUGIN(OpenaiAskPlugin, openai_ask_plugin)

static const gchar *KF_GROUP = "config";
static const gchar *KF_ENDPOINT = "endpoint";
static const gchar *KF_MODEL = "model";
static const gchar *KF_SYSTEM_PROMPT = "system_prompt";
static const gchar *KF_TEMPERATURE = "temperature";
static const gchar *KF_WIDTH_CHARS = "width_chars";
static const gchar *KF_REPLY_WIDTH_PX = "reply_width_px";

static void
openai_ask_plugin_apply_css(OpenaiAskPlugin *self)
{
  (void)self;
  static gboolean installed = FALSE;
  if (installed)
    return;
  installed = TRUE;

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(
    provider,
    "#openai-ask-popup {"
    "  background-color: rgba(0,0,0,0);"
    "}"
    "#openai-ask-frame {"
    "  background-color: @theme_base_color;"
    "  border: 1px solid @borders;"
    "  border-radius: 10px;"
    "}"
    "#openai-ask-header button {"
    "  padding: 2px 6px;"
    "}"
    "#openai-ask-header label {"
    "  font-weight: bold;"
    "}",
    -1,
    NULL);

  GdkScreen *screen = gdk_screen_get_default();
  gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static void
openai_ask_plugin_enable_transparency(GtkWidget *window)
{
  if (!GTK_IS_WIDGET(window))
    return;

  gtk_widget_set_app_paintable(window, TRUE);

  GdkScreen *screen = gtk_widget_get_screen(window);
  if (!screen)
    return;

  GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
  if (visual)
    gtk_widget_set_visual(window, visual);
}

static gboolean
openai_ask_plugin_relayout_idle(gpointer user_data)
{
  OpenaiAskPlugin *self = user_data;
  self->relayout_source_id = 0;
  if (gtk_widget_get_visible(self->popup))
    openai_ask_plugin_move_popup_near_entry(self);
  return G_SOURCE_REMOVE;
}

static void
openai_ask_plugin_request_relayout(OpenaiAskPlugin *self)
{
  if (self->relayout_source_id != 0)
    return;
  self->relayout_source_id =
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, openai_ask_plugin_relayout_idle, g_object_ref(self), g_object_unref);
}

static gboolean
openai_ask_plugin_get_anchor_rect(OpenaiAskPlugin *self, GdkRectangle *out_rect)
{
  GtkWidget *plugin_widget = GTK_WIDGET(XFCE_PANEL_PLUGIN(self));
  GtkWidget *toplevel = gtk_widget_get_toplevel(plugin_widget);
  if (!GTK_IS_WIDGET(toplevel) || !gtk_widget_get_realized(toplevel))
    return FALSE;

  GdkWindow *top_win = gtk_widget_get_window(toplevel);
  if (!top_win)
    return FALSE;

  gint top_x = 0;
  gint top_y = 0;
  gdk_window_get_origin(top_win, &top_x, &top_y);

  gboolean have = FALSE;
  GdkRectangle rect = {0};

  /* Walk up a few levels and union allocations. This catches the panel's
   * wrapper widgets that add padding around the plugin. */
  GtkWidget *w = plugin_widget;
  for (gint depth = 0; w && depth < 6; depth++)
  {
    GtkAllocation a = {0};
    gtk_widget_get_allocation(w, &a);

    gint rel_x = 0;
    gint rel_y = 0;
    if (gtk_widget_translate_coordinates(w, toplevel, 0, 0, &rel_x, &rel_y))
    {
      GdkRectangle r = {top_x + rel_x, top_y + rel_y, a.width, a.height};
      if (!have)
      {
        rect = r;
        have = TRUE;
      }
      else
      {
        gint x1 = MIN(rect.x, r.x);
        gint y1 = MIN(rect.y, r.y);
        gint x2 = MAX(rect.x + rect.width, r.x + r.width);
        gint y2 = MAX(rect.y + rect.height, r.y + r.height);
        rect.x = x1;
        rect.y = y1;
        rect.width = x2 - x1;
        rect.height = y2 - y1;
      }

      openai_ask_log("anchor[%d] %s x=%d y=%d w=%d h=%d",
                     depth,
                     G_OBJECT_TYPE_NAME(w),
                     r.x,
                     r.y,
                     r.width,
                     r.height);
    }

    w = gtk_widget_get_parent(w);
  }

  if (!have)
    return FALSE;

  *out_rect = rect;
  openai_ask_log("anchor union x=%d y=%d w=%d h=%d", rect.x, rect.y, rect.width, rect.height);
  return TRUE;
}

static void
openai_ask_plugin_move_popup_near_entry(OpenaiAskPlugin *self)
{
  GtkWidget *plugin_widget = GTK_WIDGET(XFCE_PANEL_PLUGIN(self));
  if (!gtk_widget_get_realized(self->popup) || !gtk_widget_get_realized(plugin_widget))
    return;

  GdkRectangle anchor = {0};
  if (!openai_ask_plugin_get_anchor_rect(self, &anchor))
    return;
  gint anchor_x = anchor.x;
  gint anchor_y = anchor.y;

  XfceScreenPosition pos = xfce_panel_plugin_get_screen_position(XFCE_PANEL_PLUGIN(self));
  const gint gap = 6;
  const gint margin = 8;
  const gint border = 24; /* openai-ask-frame border width (12px) top+bottom */
  const gint spacing = 8; /* popover_box spacing */

  /* Match the popup width to the union of wrapper allocations unless overridden. */
  gint popup_w = MAX(200, anchor.width);
  if (self->reply_width_px > 0)
    popup_w = self->reply_width_px;
  gint popup_h = 120;

  gint x = anchor_x;
  gint y = anchor_y + anchor.height + gap;

  /* Compute desired height from content natural height-for-width. */
  gint content_w = MAX(60, popup_w - border);
  gint header_h = 0;
  if (self->header)
  {
    gint hmin = 0, hnat = 0;
    gtk_widget_get_preferred_height_for_width(self->header, content_w, &hmin, &hnat);
    header_h = MAX(hmin, hnat);
    header_h += gtk_widget_get_margin_top(self->header) + gtk_widget_get_margin_bottom(self->header);
  }

  gint content_h = 0;
  GtkWidget *visible_child = gtk_stack_get_visible_child(GTK_STACK(self->popover_stack));
  if (visible_child == self->scrolled && self->popover_label)
  {
    gint label_margin_h =
      gtk_widget_get_margin_start(self->popover_label) + gtk_widget_get_margin_end(self->popover_label);
    gint label_margin_v = gtk_widget_get_margin_top(self->popover_label) + gtk_widget_get_margin_bottom(self->popover_label);

    gint label_w = MAX(20, content_w - label_margin_h);
    gint lmin = 0, lnat = 0;
    gtk_widget_get_preferred_height_for_width(self->popover_label, label_w, &lmin, &lnat);
    content_h = MAX(lmin, lnat) + label_margin_v;
  }
  else
  {
    gint cmin = 0, cnat = 0;
    gtk_widget_get_preferred_height_for_width(visible_child, content_w, &cmin, &cnat);
    content_h = MAX(cmin, cnat);
  }

  gint desired_h = border + header_h + spacing + MAX(0, content_h);

  /* Clamp popup height:
   * - never larger than 1/3 of the monitor height
   * - never larger than space available above/below the panel
   * - never smaller than 120px
   */
  gint max_h = 0;
  gint max_h_fraction = 0;

  GdkDisplay *display = gdk_display_get_default();
  if (display)
  {
    GdkMonitor *mon = gdk_display_get_monitor_at_point(display, anchor_x, anchor_y);
    if (mon)
    {
      GdkRectangle geo = {0};
      gdk_monitor_get_geometry(mon, &geo);

      max_h_fraction = MAX(120, geo.height / 3);

      if (xfce_screen_position_is_bottom(pos))
      {
        max_h = (anchor_y - geo.y) - gap - margin;
        popup_h = MIN(desired_h, MIN(MAX(120, max_h), max_h_fraction));
        y = anchor_y - popup_h - gap;
      }
      else if (xfce_screen_position_is_top(pos))
      {
        max_h = (geo.y + geo.height - (anchor_y + anchor.height)) - gap - margin;
        popup_h = MIN(desired_h, MIN(MAX(120, max_h), max_h_fraction));
        y = anchor_y + anchor.height + gap;
      }
      else if (xfce_screen_position_is_left(pos))
      {
        x = anchor_x + anchor.width + gap;
        max_h = geo.height - 2 * margin;
        popup_h = MIN(desired_h, MIN(MAX(120, max_h), max_h_fraction));
      }
      else if (xfce_screen_position_is_right(pos))
      {
        x = anchor_x - popup_w - gap;
        max_h = geo.height - 2 * margin;
        popup_h = MIN(desired_h, MIN(MAX(120, max_h), max_h_fraction));
      }
      else
      {
        max_h = (geo.y + geo.height - (anchor_y + anchor.height)) - gap - margin;
        popup_h = MIN(desired_h, MIN(MAX(120, max_h), max_h_fraction));
      }

      x = CLAMP(x, geo.x + margin, geo.x + geo.width - popup_w - margin);
      y = CLAMP(y, geo.y + margin, geo.y + geo.height - popup_h - margin);
    }
  }

  popup_h = MAX(120, popup_h);
  openai_ask_log("popup move x=%d y=%d w=%d h=%d desired_h=%d anchor_w=%d content_w=%d",
                 x,
                 y,
                 popup_w,
                 popup_h,
                 desired_h,
                 anchor.width,
                 content_w);
  gtk_window_move(GTK_WINDOW(self->popup), x, y);
  gtk_window_resize(GTK_WINDOW(self->popup), popup_w, popup_h);
}

static void
openai_ask_plugin_set_request_state(OpenaiAskPlugin *self, gboolean in_flight)
{
  self->request_in_flight = in_flight;
  gtk_editable_set_editable(GTK_EDITABLE(self->entry), !in_flight);
  gtk_widget_set_sensitive(self->entry, !in_flight);

  if (in_flight)
  {
    gtk_spinner_start(GTK_SPINNER(self->popover_spinner));
    gtk_stack_set_visible_child_name(GTK_STACK(self->popover_stack), "loading");
  }
  else
  {
    gtk_spinner_stop(GTK_SPINNER(self->popover_spinner));
  }
}

static void
openai_ask_plugin_clear_messages(OpenaiAskPlugin *self)
{
  if (!self->messages)
    return;

  while (self->messages->len > 0)
    g_ptr_array_remove_index(self->messages, self->messages->len - 1);
}

static gboolean
openai_ask_plugin_popover_is_open(OpenaiAskPlugin *self)
{
  return gtk_widget_get_visible(self->popup);
}

static void
openai_ask_plugin_popover_show(OpenaiAskPlugin *self)
{
  openai_ask_log("popup show (before) visible=%d", gtk_widget_get_visible(self->popup));
  gtk_widget_show_all(self->popup);
  gtk_widget_realize(self->popup);
  openai_ask_plugin_move_popup_near_entry(self);
  gtk_window_present(GTK_WINDOW(self->popup));
  openai_ask_plugin_request_relayout(self);
  openai_ask_log("popup show (after) visible=%d", gtk_widget_get_visible(self->popup));
}

static void
openai_ask_plugin_popover_hide(OpenaiAskPlugin *self)
{
  openai_ask_log("popup hide requested");
  gtk_widget_hide(self->popup);
}

static void
openai_ask_plugin_on_screen_position_changed(XfcePanelPlugin *plugin,
                                             XfceScreenPosition position,
                                             gpointer user_data)
{
  (void)plugin;
  (void)position;
  OpenaiAskPlugin *self = user_data;
  if (gtk_widget_get_visible(self->popup))
    openai_ask_plugin_move_popup_near_entry(self);
}

static void
openai_ask_plugin_set_answer(OpenaiAskPlugin *self, const gchar *answer)
{
  g_autofree gchar *markup = markdown_to_pango(answer ? answer : "", self->popover_label);
  gtk_label_set_markup(GTK_LABEL(self->popover_label), markup ? markup : "");
  gtk_stack_set_visible_child_name(GTK_STACK(self->popover_stack), "answer");
  openai_ask_plugin_popover_show(self);
  openai_ask_plugin_request_relayout(self);
}

static void
openai_ask_plugin_set_error(OpenaiAskPlugin *self, const gchar *message)
{
  g_autofree gchar *escaped = g_markup_escape_text(message ? message : "Request failed.", -1);
  g_autofree gchar *markup = g_strdup_printf("<b>Error</b>\n%s", escaped ? escaped : "");
  gtk_label_set_markup(GTK_LABEL(self->popover_label), markup);
  gtk_stack_set_visible_child_name(GTK_STACK(self->popover_stack), "answer");
  openai_ask_plugin_popover_show(self);
  openai_ask_plugin_request_relayout(self);
}

static void
openai_ask_plugin_copy_answer(OpenaiAskPlugin *self)
{
  GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  const gchar *text = gtk_label_get_text(GTK_LABEL(self->popover_label));
  if (text && *text)
    gtk_clipboard_set_text(cb, text, -1);
}

static void
openai_ask_plugin_on_popover_hide(GtkWidget *widget, OpenaiAskPlugin *self)
{
  (void)widget;
  openai_ask_log("popover hide");
  openai_ask_plugin_cancel_inflight(self);
  openai_ask_plugin_clear_messages(self);
  gtk_label_set_text(GTK_LABEL(self->popover_title), "XFCE Ask");
}

static void
openai_ask_plugin_begin_new(OpenaiAskPlugin *self)
{
  openai_ask_plugin_clear_messages(self);
  gtk_label_set_text(GTK_LABEL(self->popover_title), "XFCE Ask");
}

static void
openai_ask_plugin_on_close_clicked(GtkButton *button, OpenaiAskPlugin *self)
{
  (void)button;
  openai_ask_plugin_popover_hide(self);
}

static void
openai_ask_plugin_on_copy_clicked(GtkButton *button, OpenaiAskPlugin *self)
{
  (void)button;
  openai_ask_plugin_copy_answer(self);
}

static void
openai_ask_plugin_cancel_inflight(OpenaiAskPlugin *self)
{
  if (!self->request_in_flight)
    return;
  if (self->request_cancellable)
    g_cancellable_cancel(self->request_cancellable);
}

static void
openai_ask_plugin_append_system_if_needed(OpenaiAskPlugin *self)
{
  if (!self->system_prompt || !*self->system_prompt)
    return;
  if (self->messages->len > 0)
    return;

  g_ptr_array_add(self->messages, openai_chat_message_new("system", self->system_prompt));
}

static void
openai_ask_plugin_trim_followup(OpenaiAskPlugin *self)
{
  const guint max_messages = 6;
  if (self->messages->len <= max_messages)
    return;

  guint remove_count = self->messages->len - max_messages;
  for (guint i = 0; i < remove_count; i++)
  {
    g_ptr_array_remove_index(self->messages, 0);
  }
}

static void
openai_ask_plugin_on_client_result(OpenaiClientResult *result, gpointer user_data)
{
  OpenaiAskPlugin *plugin = user_data;

  openai_ask_plugin_set_request_state(plugin, FALSE);
  if (!result->ok)
  {
    openai_ask_log("request failed http=%d err=%s",
                   result->http_status,
                   result->error_message ? result->error_message : "");
    openai_ask_plugin_set_error(plugin, result->error_message ? result->error_message : "Request failed.");
    g_object_unref(plugin);
    return;
  }

  openai_ask_log("request ok");
  openai_ask_plugin_set_answer(plugin, result->content);
  g_ptr_array_add(plugin->messages, openai_chat_message_new("assistant", result->content ? result->content : ""));
  openai_ask_plugin_trim_followup(plugin);
  g_object_unref(plugin);
}

static gboolean
openai_ask_plugin_on_popup_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  (void)widget;
  (void)event;
  OpenaiAskPlugin *self = user_data;
  openai_ask_plugin_popover_hide(self);
  return GDK_EVENT_PROPAGATE;
}

static gboolean
openai_ask_plugin_on_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  (void)widget;
  OpenaiAskPlugin *self = user_data;
  if (event->keyval == GDK_KEY_Escape)
  {
    openai_ask_plugin_popover_hide(self);
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static void
openai_ask_plugin_send(OpenaiAskPlugin *self, const gchar *prompt)
{
  if (!prompt || !*prompt)
    return;
  if (!self->endpoint || !*self->endpoint)
  {
    g_warning("XFCE Ask: missing endpoint");
    openai_ask_plugin_set_error(self, "No endpoint configured.");
    openai_ask_plugin_popover_show(self);
    return;
  }
  if (!self->model || !*self->model)
  {
    g_warning("XFCE Ask: missing model");
    openai_ask_plugin_set_error(self, "No model configured.");
    openai_ask_plugin_popover_show(self);
    return;
  }

  if (!openai_ask_plugin_popover_is_open(self))
    openai_ask_plugin_begin_new(self);
  else
    gtk_label_set_text(GTK_LABEL(self->popover_title), "Follow-up");

  openai_ask_plugin_append_system_if_needed(self);
  g_ptr_array_add(self->messages, openai_chat_message_new("user", prompt));
  openai_ask_plugin_trim_followup(self);
  openai_ask_log("send prompt len=%zu", (size_t)strlen(prompt));

  g_clear_object(&self->request_cancellable);
  self->request_cancellable = g_cancellable_new();

  openai_ask_plugin_set_request_state(self, TRUE);
  openai_ask_plugin_popover_show(self);
  openai_ask_plugin_request_relayout(self);

  g_autofree gchar *api_key = keyring_lookup_api_key(self->endpoint);
  if (!api_key || !*api_key)
  {
    g_warning("XFCE Ask: no API key found for endpoint");
    openai_ask_log("no api key for endpoint=%s", self->endpoint ? self->endpoint : "");
    openai_ask_plugin_set_request_state(self, FALSE);
    openai_ask_plugin_set_error(self,
                                "No API key found for this endpoint.\n"
                                "Right-click the plugin → Properties → save an API key.");
    return;
  }

  openai_ask_log("sending request endpoint=%s model=%s temp=%.2f",
                 self->endpoint ? self->endpoint : "",
                 self->model ? self->model : "",
                 self->temperature);
  g_object_ref(self);
  openai_client_send_chat_async(
    self->endpoint,
    api_key,
    self->model,
    self->temperature,
    self->messages,
    self->request_cancellable,
    openai_ask_plugin_on_client_result,
    self);
}

typedef struct
{
  GtkWidget *endpoint_entry;
  GtkWidget *key_entry;
} OpenaiAskKeyDialogCtx;

static void
openai_ask_plugin_on_save_key_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  OpenaiAskKeyDialogCtx *ctx = user_data;
  const gchar *endpoint = gtk_entry_get_text(GTK_ENTRY(ctx->endpoint_entry));
  const gchar *key = gtk_entry_get_text(GTK_ENTRY(ctx->key_entry));
  if (!endpoint || !*endpoint || !key || !*key)
    return;
  keyring_store_api_key(endpoint, key);
  gtk_entry_set_text(GTK_ENTRY(ctx->key_entry), "");
}

static void
openai_ask_plugin_on_clear_key_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  OpenaiAskKeyDialogCtx *ctx = user_data;
  const gchar *endpoint = gtk_entry_get_text(GTK_ENTRY(ctx->endpoint_entry));
  if (!endpoint || !*endpoint)
    return;
  keyring_clear_api_key(endpoint);
}

static void
openai_ask_plugin_on_entry_activate(GtkEntry *entry, OpenaiAskPlugin *self)
{
  if (self->request_in_flight)
    return;

  const gchar *text = gtk_entry_get_text(entry);
  if (!text || !*text)
    return;

  openai_ask_plugin_send(self, text);
  gtk_entry_set_text(entry, "");
}

static gboolean
openai_ask_plugin_on_entry_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  OpenaiAskPlugin *self = user_data;
  if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter || event->keyval == GDK_KEY_ISO_Enter ||
      event->keyval == GDK_KEY_Linefeed)
  {
    openai_ask_log("enter key pressed");
    if (!self->request_in_flight)
    {
      const gchar *text = gtk_entry_get_text(GTK_ENTRY(widget));
      if (text && *text)
      {
        openai_ask_plugin_send(self, text);
        gtk_entry_set_text(GTK_ENTRY(widget), "");
      }
    }
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static gboolean
openai_ask_plugin_on_entry_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  (void)event;
  OpenaiAskPlugin *self = user_data;
  xfce_panel_plugin_focus_widget(XFCE_PANEL_PLUGIN(self), widget);
  gtk_widget_grab_focus(widget);
  return GDK_EVENT_PROPAGATE;
}

static void
openai_ask_plugin_load_settings(OpenaiAskPlugin *self)
{
  g_clear_pointer(&self->endpoint, g_free);
  g_clear_pointer(&self->model, g_free);
  g_clear_pointer(&self->system_prompt, g_free);

  self->endpoint = g_strdup("https://api.openai.com/v1/chat/completions");
  self->model = g_strdup("gpt-4o-mini");
  self->system_prompt = g_strdup("");
  self->temperature = 0.7;
  self->width_chars = 18;
  self->reply_width_px = 0;

  g_autofree gchar *rc = xfce_panel_plugin_save_location(XFCE_PANEL_PLUGIN(self), FALSE);
  if (!rc)
    return;

  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_autoptr(GError) error = NULL;
  if (!g_key_file_load_from_file(kf, rc, G_KEY_FILE_NONE, &error))
    return;

  g_autofree gchar *endpoint = g_key_file_get_string(kf, KF_GROUP, KF_ENDPOINT, NULL);
  g_autofree gchar *model = g_key_file_get_string(kf, KF_GROUP, KF_MODEL, NULL);
  g_autofree gchar *system_prompt = g_key_file_get_string(kf, KF_GROUP, KF_SYSTEM_PROMPT, NULL);
  gdouble temperature = self->temperature;
  if (g_key_file_has_key(kf, KF_GROUP, KF_TEMPERATURE, NULL))
    temperature = g_key_file_get_double(kf, KF_GROUP, KF_TEMPERATURE, NULL);

  if (endpoint && *endpoint)
  {
    g_free(self->endpoint);
    self->endpoint = g_strdup(endpoint);
  }
  if (model && *model)
  {
    g_free(self->model);
    self->model = g_strdup(model);
  }
  if (system_prompt)
  {
    g_free(self->system_prompt);
    self->system_prompt = g_strdup(system_prompt);
  }
  self->temperature = temperature;

  if (g_key_file_has_key(kf, KF_GROUP, KF_WIDTH_CHARS, NULL))
    self->width_chars = g_key_file_get_integer(kf, KF_GROUP, KF_WIDTH_CHARS, NULL);

  if (g_key_file_has_key(kf, KF_GROUP, KF_REPLY_WIDTH_PX, NULL))
    self->reply_width_px = g_key_file_get_integer(kf, KF_GROUP, KF_REPLY_WIDTH_PX, NULL);
}

static void
openai_ask_plugin_save_settings(OpenaiAskPlugin *self)
{
  g_autofree gchar *rc = xfce_panel_plugin_save_location(XFCE_PANEL_PLUGIN(self), TRUE);
  if (!rc)
    return;

  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_key_file_set_string(kf, KF_GROUP, KF_ENDPOINT, self->endpoint ? self->endpoint : "");
  g_key_file_set_string(kf, KF_GROUP, KF_MODEL, self->model ? self->model : "");
  g_key_file_set_string(kf, KF_GROUP, KF_SYSTEM_PROMPT, self->system_prompt ? self->system_prompt : "");
  g_key_file_set_double(kf, KF_GROUP, KF_TEMPERATURE, self->temperature);
  g_key_file_set_integer(kf, KF_GROUP, KF_WIDTH_CHARS, self->width_chars);
  g_key_file_set_integer(kf, KF_GROUP, KF_REPLY_WIDTH_PX, self->reply_width_px);

  gsize len = 0;
  g_autofree gchar *data = g_key_file_to_data(kf, &len, NULL);
  if (!data)
    return;
  g_file_set_contents(rc, data, (gssize)len, NULL);
}

static void
openai_ask_plugin_show_configure(XfcePanelPlugin *plugin, gpointer user_data)
{
  (void)user_data;
  OpenaiAskPlugin *self = (OpenaiAskPlugin *)plugin;

  GtkWidget *dialog = gtk_dialog_new_with_buttons("XFCE Ask",
                                                  NULL,
                                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  "_Cancel",
                                                  GTK_RESPONSE_CANCEL,
                                                  "_Save",
                                                  GTK_RESPONSE_OK,
                                                  NULL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
  gtk_container_add(GTK_CONTAINER(content), grid);

  GtkWidget *endpoint_label = gtk_label_new("Endpoint");
  gtk_widget_set_halign(endpoint_label, GTK_ALIGN_END);
  GtkWidget *endpoint_entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(endpoint_entry), self->endpoint ? self->endpoint : "");
  gtk_grid_attach(GTK_GRID(grid), endpoint_label, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), endpoint_entry, 1, 0, 1, 1);

  GtkWidget *model_label = gtk_label_new("Model");
  gtk_widget_set_halign(model_label, GTK_ALIGN_END);
  GtkWidget *model_entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(model_entry), self->model ? self->model : "");
  gtk_grid_attach(GTK_GRID(grid), model_label, 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), model_entry, 1, 1, 1, 1);

  GtkWidget *temp_label = gtk_label_new("Temperature");
  gtk_widget_set_halign(temp_label, GTK_ALIGN_END);
  GtkAdjustment *temp_adj = gtk_adjustment_new(self->temperature, 0.0, 2.0, 0.1, 0.1, 0.0);
  GtkWidget *temp_spin = gtk_spin_button_new(temp_adj, 0.1, 1);
  gtk_grid_attach(GTK_GRID(grid), temp_label, 0, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), temp_spin, 1, 2, 1, 1);

  GtkWidget *system_label = gtk_label_new("System prompt");
  gtk_widget_set_halign(system_label, GTK_ALIGN_END);
  GtkWidget *system_entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(system_entry), self->system_prompt ? self->system_prompt : "");
  gtk_grid_attach(GTK_GRID(grid), system_label, 0, 3, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), system_entry, 1, 3, 1, 1);

  GtkWidget *width_label = gtk_label_new("Width (chars)");
  gtk_widget_set_halign(width_label, GTK_ALIGN_END);
  GtkAdjustment *width_adj = gtk_adjustment_new(self->width_chars, 6.0, 80.0, 1.0, 1.0, 0.0);
  GtkWidget *width_spin = gtk_spin_button_new(width_adj, 1.0, 0);
  gtk_grid_attach(GTK_GRID(grid), width_label, 0, 4, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), width_spin, 1, 4, 1, 1);

  GtkWidget *reply_width_label = gtk_label_new("Reply width (px)");
  gtk_widget_set_halign(reply_width_label, GTK_ALIGN_END);
  GtkAdjustment *reply_width_adj = gtk_adjustment_new(self->reply_width_px, 0.0, 4000.0, 10.0, 50.0, 0.0);
  GtkWidget *reply_width_spin = gtk_spin_button_new(reply_width_adj, 10.0, 0);
  gtk_widget_set_tooltip_text(reply_width_spin, "0 = match question box width");
  gtk_grid_attach(GTK_GRID(grid), reply_width_label, 0, 5, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), reply_width_spin, 1, 5, 1, 1);

  GtkWidget *key_label = gtk_label_new("API key (keyring)");
  gtk_widget_set_halign(key_label, GTK_ALIGN_END);
  GtkWidget *key_entry = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(key_entry), FALSE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(key_entry), "Leave blank to keep existing");

  GtkWidget *key_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *btn_save_key = gtk_button_new_with_label("Save key");
  GtkWidget *btn_clear_key = gtk_button_new_with_label("Clear key");
  gtk_box_pack_start(GTK_BOX(key_buttons), btn_save_key, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(key_buttons), btn_clear_key, FALSE, FALSE, 0);

  gtk_grid_attach(GTK_GRID(grid), key_label, 0, 6, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), key_entry, 1, 6, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), key_buttons, 1, 7, 1, 1);

  OpenaiAskKeyDialogCtx key_ctx = {endpoint_entry, key_entry};
  g_signal_connect(btn_save_key, "clicked", G_CALLBACK(openai_ask_plugin_on_save_key_clicked), &key_ctx);
  g_signal_connect(btn_clear_key, "clicked", G_CALLBACK(openai_ask_plugin_on_clear_key_clicked), &key_ctx);

  gtk_widget_show_all(dialog);
  gint resp = gtk_dialog_run(GTK_DIALOG(dialog));
  if (resp == GTK_RESPONSE_OK)
  {
    g_free(self->endpoint);
    g_free(self->model);
    g_free(self->system_prompt);
    self->endpoint = g_strdup(gtk_entry_get_text(GTK_ENTRY(endpoint_entry)));
    self->model = g_strdup(gtk_entry_get_text(GTK_ENTRY(model_entry)));
    self->system_prompt = g_strdup(gtk_entry_get_text(GTK_ENTRY(system_entry)));
    self->temperature = gtk_spin_button_get_value(GTK_SPIN_BUTTON(temp_spin));
    self->width_chars = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(width_spin));
    self->reply_width_px = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(reply_width_spin));
    if (self->width_chars < 6)
      self->width_chars = 6;
    if (self->reply_width_px < 0)
      self->reply_width_px = 0;
    if (self->entry)
      gtk_entry_set_width_chars(GTK_ENTRY(self->entry), self->width_chars);
    openai_ask_plugin_save_settings(self);
  }
  gtk_widget_destroy(dialog);
}

static void
openai_ask_plugin_construct(XfcePanelPlugin *plugin)
{
  OpenaiAskPlugin *self = (OpenaiAskPlugin *)plugin;
  openai_ask_log_init();
  openai_ask_log("plugin construct");
  openai_ask_plugin_apply_css(self);
  openai_ask_plugin_load_settings(self);

  /* Make sure the panel allocates visible space for the entry. */
  xfce_panel_plugin_set_expand(plugin, TRUE);
  xfce_panel_plugin_set_shrink(plugin, TRUE);
  xfce_panel_plugin_set_small(plugin, FALSE);

  xfce_panel_plugin_menu_show_configure(plugin);
  g_signal_connect(plugin, "configure-plugin", G_CALLBACK(openai_ask_plugin_show_configure), NULL);

  self->container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add(GTK_CONTAINER(plugin), self->container);

  self->entry = gtk_entry_new();
  gtk_widget_set_can_focus(self->entry, TRUE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry), "Ask…");
  gtk_entry_set_width_chars(GTK_ENTRY(self->entry), self->width_chars);
  gtk_box_pack_start(GTK_BOX(self->container), self->entry, TRUE, TRUE, 0);
  g_signal_connect(self->entry, "activate", G_CALLBACK(openai_ask_plugin_on_entry_activate), self);
  gtk_widget_add_events(self->entry, GDK_KEY_PRESS_MASK);
  g_signal_connect(self->entry, "key-press-event", G_CALLBACK(openai_ask_plugin_on_entry_key_press), self);
  gtk_widget_add_events(self->entry, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(self->entry, "button-press-event", G_CALLBACK(openai_ask_plugin_on_entry_button_press), self);
  xfce_panel_plugin_focus_widget(plugin, self->entry);

  self->popup = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_widget_set_name(self->popup, "openai-ask-popup");
  openai_ask_plugin_enable_transparency(self->popup);
  gtk_window_set_decorated(GTK_WINDOW(self->popup), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(self->popup), TRUE);
  gtk_window_set_skip_taskbar_hint(GTK_WINDOW(self->popup), TRUE);
  gtk_window_set_skip_pager_hint(GTK_WINDOW(self->popup), TRUE);
  gtk_window_set_type_hint(GTK_WINDOW(self->popup), GDK_WINDOW_TYPE_HINT_DROPDOWN_MENU);
  gtk_window_set_accept_focus(GTK_WINDOW(self->popup), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(self->popup), GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin))));
  gtk_window_set_default_size(GTK_WINDOW(self->popup), 520, 320);
  g_signal_connect(self->popup, "focus-out-event", G_CALLBACK(openai_ask_plugin_on_popup_focus_out), self);
  g_signal_connect(self->popup, "key-press-event", G_CALLBACK(openai_ask_plugin_on_popup_key_press), self);
  g_signal_connect(self->popup, "hide", G_CALLBACK(openai_ask_plugin_on_popover_hide), self);
  g_signal_connect(plugin,
                   "screen-position-changed",
                   G_CALLBACK(openai_ask_plugin_on_screen_position_changed),
                   self);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(self->popup), outer);

  GtkWidget *popover_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_name(popover_box, "openai-ask-frame");
  gtk_container_set_border_width(GTK_CONTAINER(popover_box), 12);
  gtk_widget_set_hexpand(popover_box, TRUE);
  gtk_widget_set_vexpand(popover_box, TRUE);
  gtk_box_pack_start(GTK_BOX(outer), popover_box, TRUE, TRUE, 0);

  self->header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_name(self->header, "openai-ask-header");
  /* Match header left/right padding to the reply text padding for visual alignment. */
  gtk_widget_set_margin_start(self->header, 14);
  gtk_widget_set_margin_end(self->header, 14);
  gtk_widget_set_margin_top(self->header, 10);
  gtk_widget_set_margin_bottom(self->header, 10);
  self->popover_title = gtk_label_new("XFCE Ask");
  gtk_label_set_xalign(GTK_LABEL(self->popover_title), 0.0f);
  gtk_widget_set_hexpand(self->popover_title, TRUE);

  GtkWidget *btn_copy = gtk_button_new_from_icon_name("edit-copy-symbolic", GTK_ICON_SIZE_BUTTON);
  GtkWidget *btn_close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_button_set_relief(GTK_BUTTON(btn_copy), GTK_RELIEF_NONE);
  gtk_button_set_relief(GTK_BUTTON(btn_close), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(btn_copy, "Copy");
  gtk_widget_set_tooltip_text(btn_close, "Close");
  g_signal_connect(btn_copy, "clicked", G_CALLBACK(openai_ask_plugin_on_copy_clicked), self);
  g_signal_connect(btn_close, "clicked", G_CALLBACK(openai_ask_plugin_on_close_clicked), self);

  gtk_box_pack_start(GTK_BOX(self->header), self->popover_title, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->header), btn_copy, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->header), btn_close, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(popover_box), self->header, FALSE, FALSE, 0);

  self->popover_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(self->popover_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_hexpand(self->popover_stack, TRUE);
  gtk_widget_set_vexpand(self->popover_stack, TRUE);
  gtk_box_pack_start(GTK_BOX(popover_box), self->popover_stack, TRUE, TRUE, 0);

  GtkWidget *loading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  self->popover_spinner = gtk_spinner_new();
  GtkWidget *loading_label = gtk_label_new("Thinking…");
  gtk_box_pack_start(GTK_BOX(loading), self->popover_spinner, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(loading), loading_label, FALSE, FALSE, 0);
  gtk_stack_add_named(GTK_STACK(self->popover_stack), loading, "loading");

  self->scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(self->scrolled, TRUE);
  gtk_widget_set_vexpand(self->scrolled, TRUE);

  self->popover_label = gtk_label_new(NULL);
  gtk_label_set_selectable(GTK_LABEL(self->popover_label), TRUE);
  gtk_label_set_line_wrap(GTK_LABEL(self->popover_label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(self->popover_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(self->popover_label), 0.0f);
  gtk_label_set_yalign(GTK_LABEL(self->popover_label), 0.0f);
  gtk_widget_set_margin_start(self->popover_label, 14);
  gtk_widget_set_margin_end(self->popover_label, 14);
  gtk_widget_set_margin_top(self->popover_label, 14);
  gtk_widget_set_margin_bottom(self->popover_label, 14);
  gtk_container_add(GTK_CONTAINER(self->scrolled), self->popover_label);
  gtk_stack_add_named(GTK_STACK(self->popover_stack), self->scrolled, "answer");

  gtk_stack_set_visible_child_name(GTK_STACK(self->popover_stack), "answer");

  self->messages = g_ptr_array_new_with_free_func((GDestroyNotify)openai_chat_message_free);
  openai_ask_plugin_set_request_state(self, FALSE);

  /* XFCE does not always show child widgets automatically. */
  gtk_widget_show_all(GTK_WIDGET(plugin));
}

static void
openai_ask_plugin_dispose(GObject *object)
{
  OpenaiAskPlugin *self = (OpenaiAskPlugin *)object;

  openai_ask_plugin_cancel_inflight(self);
  g_clear_object(&self->request_cancellable);

  g_clear_pointer(&self->messages, g_ptr_array_unref);

  G_OBJECT_CLASS(openai_ask_plugin_parent_class)->dispose(object);
}

static void
openai_ask_plugin_finalize(GObject *object)
{
  OpenaiAskPlugin *self = (OpenaiAskPlugin *)object;
  g_clear_pointer(&self->endpoint, g_free);
  g_clear_pointer(&self->model, g_free);
  g_clear_pointer(&self->system_prompt, g_free);
  G_OBJECT_CLASS(openai_ask_plugin_parent_class)->finalize(object);
}

static void
openai_ask_plugin_class_init(OpenaiAskPluginClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = openai_ask_plugin_dispose;
  gobject_class->finalize = openai_ask_plugin_finalize;

  XfcePanelPluginClass *plugin_class = XFCE_PANEL_PLUGIN_CLASS(klass);
  plugin_class->construct = openai_ask_plugin_construct;
}

static void
openai_ask_plugin_init(OpenaiAskPlugin *self)
{
  self->temperature = 0.7;
  self->width_chars = 18;
  self->reply_width_px = 0;
}

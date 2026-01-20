#pragma once

#include <gtk/gtk.h>

/* Returns newly-allocated Pango markup suitable for GtkLabel. */
gchar *markdown_to_pango(const gchar *markdown, GtkWidget *style_widget);


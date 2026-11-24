/* bz-gnome-extensions-page.c
 *
 * Copyright 2026 The Bazaar Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* DOWNSTREAM PATCH (GNOME Extensions): see ai-patches/PLAN.md §13.
 *
 * Nothing is fetched until the tab is first shown. Scrolling near the end
 * of the grid loads the next page of results. */

#define G_LOG_DOMAIN "BAZAAR::GNOME-EXTENSIONS"

#include "config.h"

#include <glib/gi18n.h>

#include "bz-gnome-extension-dialog.h"
#include "bz-gnome-extensions-page.h"
#include "bz-search-bar.h"

/* Load the next page when less than this many pixels are left below. */
#define LOAD_MORE_MARGIN 800
/* Search this long after the last keystroke (Enter searches at once). */
#define SEARCH_DELAY_MS 300

/* The three dots under the results while the next page loads: a card
 * shaped like a pill, each dot fading in and out in turn. Loaded by the
 * page, so the patch needs no change to upstream's style.css. */
static const char loading_more_css[] =
    ".extensions-loading-more {"
    "  padding: 10px 14px;"
    "  border-radius: 999px;"
    "}"
    ".extensions-loading-more > .dot {"
    "  min-width: 8px;"
    "  min-height: 8px;"
    "  border-radius: 999px;"
    "  background-color: currentColor;"
    "  opacity: 0.2;"
    "  animation: extensions-loading-dot 1.5s ease-in-out infinite;"
    "}"
    ".extensions-loading-more > .dot:nth-child(2) { animation-delay: 0.25s; }"
    ".extensions-loading-more > .dot:nth-child(3) { animation-delay: 0.5s; }"
    "@keyframes extensions-loading-dot {"
    "  0%, 70%, 100% { opacity: 0.2; }"
    "  35% { opacity: 0.9; }"
    "}";

/* The topic pills: { label, search keyword }. extensions.gnome.org has no
 * categories; a pill searches for its keyword. */
static const struct
{
  const char *label;
  const char *keyword;
} pills[] = {
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Dock"), "dock" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Top Bar"), "panel" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Workspaces"), "workspace" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Windows"), "window" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("App Menu"), "menu" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Clipboard"), "clipboard" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("System Monitor"), "system monitor" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Battery"), "battery" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Weather"), "weather" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Appearance"), "theme" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Wallpaper"), "wallpaper" },
  /* Translators: a topic of GNOME Shell extensions, shown as a search suggestion */
  { N_ ("Screenshot"), "screenshot" },
};

struct _BzGnomeExtensionsPage
{
  AdwBin parent_instance;

  BzGnomeExtensions *extensions;
  gboolean           started;
  int                sort;
  guint              search_timeout;

  /* Template widgets */
  BzSearchBar       *search_bar;
  GtkMenuButton     *sort_button;
  GtkBox            *pill_box;
  GtkBox            *pill_row;
  GtkStack          *stack;
  GtkScrolledWindow *scrolled_window;
  GtkGridView       *grid_view;
  AdwStatusPage     *empty_page;
};

G_DEFINE_FINAL_TYPE (BzGnomeExtensionsPage, bz_gnome_extensions_page, ADW_TYPE_BIN)

enum
{
  PROP_0,
  PROP_EXTENSIONS,
  PROP_AVAILABLE,
  PROP_SORT,
  PROP_SORT_NAME,
  N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* The sort menu's targets, in BzGnomeExtensionsSort order. */
static const char *sort_names[] = { "popular", "trending", "recent", "installed" };

static gboolean
invert_boolean (gpointer object,
                gboolean value)
{
  return !value;
}

/* The first line of a description, for the tiles. */
static char *
first_line (gpointer    object,
            const char *description)
{
  const char *end = NULL;

  if (description == NULL)
    return g_strdup ("");
  end = strchr (description, '\n');
  return end != NULL ? g_strndup (description, end - description) : g_strdup (description);
}

static void
search (BzGnomeExtensionsPage *self)
{
  g_clear_handle_id (&self->search_timeout, g_source_remove);
  self->started = TRUE;
  bz_gnome_extensions_search (self->extensions,
                              gtk_editable_get_text (GTK_EDITABLE (self->search_bar)),
                              self->sort);
}

static void
search_timeout_cb (BzGnomeExtensionsPage *self)
{
  self->search_timeout = 0;
  search (self);
}

static void
search_changed_cb (BzGnomeExtensionsPage *self)
{
  g_clear_handle_id (&self->search_timeout, g_source_remove);
  self->search_timeout = g_timeout_add_once (SEARCH_DELAY_MS, (GSourceOnceFunc) search_timeout_cb, self);
}

static void
search_activated_cb (BzGnomeExtensionsPage *self)
{
  search (self);
}

static void
set_sort (BzGnomeExtensionsPage *self,
          int                    sort)
{
  if (sort == self->sort || sort < 0 || sort > BZ_GNOME_EXTENSIONS_SORT_INSTALLED)
    return;
  self->sort = sort;
  /* Highlighted when not the default, like the Search page's filter button. */
  if (sort == BZ_GNOME_EXTENSIONS_SORT_DOWNLOADS)
    gtk_widget_remove_css_class (GTK_WIDGET (self->sort_button), "accent");
  else
    gtk_widget_add_css_class (GTK_WIDGET (self->sort_button), "accent");
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SORT]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SORT_NAME]);
  if (self->started)
    search (self);
}

static void
pill_clicked_cb (BzGnomeExtensionsPage *self,
                 GtkButton             *button)
{
  gtk_editable_set_text (GTK_EDITABLE (self->search_bar),
                         g_object_get_data (G_OBJECT (button), "keyword"));
  search (self);
}

static void
add_pills (BzGnomeExtensionsPage *self,
           GtkBox                *box)
{
  for (guint i = 0; i < G_N_ELEMENTS (pills); i++)
    {
      GtkWidget *button = gtk_button_new_with_label (_ (pills[i].label));

      /* The Search page's pill style (style.css). */
      gtk_widget_add_css_class (button, "small-pill");
      gtk_widget_add_css_class (button, "search-pill");
      g_object_set_data (G_OBJECT (button), "keyword", (gpointer) pills[i].keyword);
      g_signal_connect_swapped (button, "clicked", G_CALLBACK (pill_clicked_cb), self);
      gtk_box_append (box, button);
    }
}

static void
retry_cb (BzGnomeExtensionsPage *self)
{
  bz_gnome_extensions_reload (self->extensions);
}

static void
turn_on_cb (BzGnomeExtensionsPage *self)
{
  bz_gnome_extensions_set_user_extensions_enabled (self->extensions, TRUE);
}

static void
activate_cb (BzGnomeExtensionsPage *self,
             guint                  position)
{
  g_autoptr (BzGnomeExtension) extension = NULL;
  AdwDialog *dialog                     = NULL;

  extension = g_list_model_get_item (bz_gnome_extensions_get_results (self->extensions), position);
  if (extension == NULL)
    return;
  dialog = bz_gnome_extension_dialog_new (self->extensions, extension);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
maybe_load_more (BzGnomeExtensionsPage *self)
{
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (self->scrolled_window);
  double         left       = gtk_adjustment_get_upper (adjustment) -
                   (gtk_adjustment_get_value (adjustment) + gtk_adjustment_get_page_size (adjustment));

  if (left < LOAD_MORE_MARGIN)
    bz_gnome_extensions_load_more (self->extensions);
}

static void
status_changed_cb (BzGnomeExtensionsPage *self)
{
  BzGnomeExtensionsStatus status    = bz_gnome_extensions_get_status (self->extensions);
  gboolean                installed = self->sort == BZ_GNOME_EXTENSIONS_SORT_INSTALLED;
  const char             *page      = "loading";

  switch (status)
    {
    case BZ_GNOME_EXTENSIONS_STATUS_RESULTS: page = "results"; break;
    case BZ_GNOME_EXTENSIONS_STATUS_EMPTY: page = "empty"; break;
    case BZ_GNOME_EXTENSIONS_STATUS_ERROR: page = "error"; break;
    case BZ_GNOME_EXTENSIONS_STATUS_LOADING:
    default: page = "loading"; break;
    }

  if (status == BZ_GNOME_EXTENSIONS_STATUS_EMPTY)
    {
      const char *text = gtk_editable_get_text (GTK_EDITABLE (self->search_bar));
      gboolean    all  = text == NULL || *text == '\0';

      adw_status_page_set_icon_name (self->empty_page,
                                     installed && all ? "application-x-addon-symbolic" : "system-search-symbolic");
      adw_status_page_set_title (self->empty_page,
                                 installed && all ? _ ("No Extensions Installed") : _ ("No Extensions Found"));
      adw_status_page_set_description (self->empty_page,
                                       installed && all ? _ ("Extensions you install appear here.")
                                                        : _ ("Try a different search."));
    }

  gtk_stack_set_visible_child_name (self->stack, page);

  /* A fresh search starts at the top. */
  /* (A first page too short to fill the window loads the next one through
   * the adjustment's "changed" signal.) */
  if (status == BZ_GNOME_EXTENSIONS_STATUS_LOADING)
    gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (self->scrolled_window), 0);
}

static void
available_changed_cb (BzGnomeExtensionsPage *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AVAILABLE]);
}

static void
map_cb (BzGnomeExtensionsPage *self)
{
  if (!self->started)
    search (self);
  gtk_widget_grab_focus (GTK_WIDGET (self->search_bar));
}

static void
bz_gnome_extensions_page_dispose (GObject *object)
{
  BzGnomeExtensionsPage *self = BZ_GNOME_EXTENSIONS_PAGE (object);

  g_clear_handle_id (&self->search_timeout, g_source_remove);
  if (self->extensions != NULL)
    g_signal_handlers_disconnect_by_data (self->extensions, self);
  g_clear_object (&self->extensions);

  G_OBJECT_CLASS (bz_gnome_extensions_page_parent_class)->dispose (object);
}

static void
bz_gnome_extensions_page_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  BzGnomeExtensionsPage *self = BZ_GNOME_EXTENSIONS_PAGE (object);

  switch (prop_id)
    {
    case PROP_EXTENSIONS:
      g_value_set_object (value, self->extensions);
      break;
    case PROP_AVAILABLE:
      g_value_set_boolean (value, bz_gnome_extensions_page_get_available (self));
      break;
    case PROP_SORT:
      g_value_set_int (value, self->sort);
      break;
    case PROP_SORT_NAME:
      g_value_set_string (value, sort_names[self->sort]);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
bz_gnome_extensions_page_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  BzGnomeExtensionsPage *self = BZ_GNOME_EXTENSIONS_PAGE (object);

  switch (prop_id)
    {
    case PROP_SORT:
      set_sort (self, g_value_get_int (value));
      break;
    case PROP_SORT_NAME:
      for (guint i = 0; i < G_N_ELEMENTS (sort_names); i++)
        if (g_strcmp0 (g_value_get_string (value), sort_names[i]) == 0)
          set_sort (self, i);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
bz_gnome_extensions_page_class_init (BzGnomeExtensionsPageClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose      = bz_gnome_extensions_page_dispose;
  object_class->get_property = bz_gnome_extensions_page_get_property;
  object_class->set_property = bz_gnome_extensions_page_set_property;

  props[PROP_EXTENSIONS] = g_param_spec_object ("extensions", NULL, NULL, BZ_TYPE_GNOME_EXTENSIONS,
                                                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_AVAILABLE] = g_param_spec_boolean ("available", NULL, NULL, FALSE,
                                                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  /* A BzGnomeExtensionsSort, and the same as a name for the sort menu's
   * "page.sort" action (Blueprint menu targets are strings). */
  props[PROP_SORT] = g_param_spec_int ("sort", NULL, NULL, 0, BZ_GNOME_EXTENSIONS_SORT_INSTALLED, 0,
                                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_SORT_NAME] = g_param_spec_string ("sort-name", NULL, NULL, "popular",
                                               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties (object_class, N_PROPS, props);

  g_type_ensure (BZ_TYPE_GNOME_EXTENSIONS);
  g_type_ensure (BZ_TYPE_GNOME_EXTENSION);
  g_type_ensure (BZ_TYPE_SEARCH_BAR);

  gtk_widget_class_install_property_action (widget_class, "page.sort", "sort-name");
  gtk_widget_class_set_template_from_resource (widget_class, "/io/github/kolunmi/Bazaar/bz-gnome-extensions-page.ui");
  gtk_widget_class_bind_template_child (widget_class, BzGnomeExtensionsPage, search_bar);
  gtk_widget_class_bind_template_child (widget_class, BzGnomeExtensionsPage, sort_button);
  gtk_widget_class_bind_template_child (widget_class, BzGnomeExtensionsPage, pill_box);
  gtk_widget_class_bind_template_child (widget_class, BzGnomeExtensionsPage, pill_row);
  gtk_widget_class_bind_template_child (widget_class, BzGnomeExtensionsPage, stack);
  gtk_widget_class_bind_template_child (widget_class, BzGnomeExtensionsPage, scrolled_window);
  gtk_widget_class_bind_template_child (widget_class, BzGnomeExtensionsPage, grid_view);
  gtk_widget_class_bind_template_child (widget_class, BzGnomeExtensionsPage, empty_page);
  gtk_widget_class_bind_template_callback (widget_class, invert_boolean);
  gtk_widget_class_bind_template_callback (widget_class, first_line);
  gtk_widget_class_bind_template_callback (widget_class, search_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, search_activated_cb);
  gtk_widget_class_bind_template_callback (widget_class, retry_cb);
  gtk_widget_class_bind_template_callback (widget_class, turn_on_cb);
  gtk_widget_class_bind_template_callback (widget_class, activate_cb);
}

static void
ensure_css (void)
{
  static gboolean           loaded   = FALSE;
  g_autoptr (GtkCssProvider) provider = NULL;
  GdkDisplay               *display  = gdk_display_get_default ();

  if (loaded || display == NULL)
    return;
  loaded   = TRUE;
  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider, loading_more_css);
  gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void
bz_gnome_extensions_page_init (BzGnomeExtensionsPage *self)
{
  g_autoptr (GtkNoSelection) selection = NULL;
  GtkAdjustment *adjustment            = NULL;

  ensure_css ();

  /* Created before the template, which binds to it. */
  self->extensions = bz_gnome_extensions_new ();
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Wide: wrapped like the Search page's pills. Narrow: one scrolling row. */
  {
    GtkLayoutManager *layout = GTK_LAYOUT_MANAGER (adw_wrap_layout_new ());

    adw_wrap_layout_set_child_spacing (ADW_WRAP_LAYOUT (layout), 10);
    adw_wrap_layout_set_line_spacing (ADW_WRAP_LAYOUT (layout), 8);
    adw_wrap_layout_set_justify (ADW_WRAP_LAYOUT (layout), ADW_JUSTIFY_FILL);
    gtk_widget_set_layout_manager (GTK_WIDGET (self->pill_box), layout);
  }
  add_pills (self, self->pill_box);
  add_pills (self, self->pill_row);

  selection = gtk_no_selection_new (g_object_ref (bz_gnome_extensions_get_results (self->extensions)));
  gtk_grid_view_set_model (self->grid_view, GTK_SELECTION_MODEL (selection));

  g_signal_connect_swapped (self->extensions, "notify::status", G_CALLBACK (status_changed_cb), self);
  g_signal_connect_swapped (self->extensions, "notify::available", G_CALLBACK (available_changed_cb), self);
  g_signal_connect_swapped (self, "map", G_CALLBACK (map_cb), self);

  adjustment = gtk_scrolled_window_get_vadjustment (self->scrolled_window);
  g_signal_connect_swapped (adjustment, "value-changed", G_CALLBACK (maybe_load_more), self);
  g_signal_connect_swapped (adjustment, "changed", G_CALLBACK (maybe_load_more), self);
}

BzGnomeExtensions *
bz_gnome_extensions_page_get_extensions (BzGnomeExtensionsPage *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS_PAGE (self), NULL);
  return self->extensions;
}

gboolean
bz_gnome_extensions_page_get_available (BzGnomeExtensionsPage *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS_PAGE (self), FALSE);
  return bz_gnome_extensions_get_available (self->extensions);
}

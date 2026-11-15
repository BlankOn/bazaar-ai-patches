/* DOWNSTREAM PATCH (APT system updates): standalone test harness.
 *
 * Shows only BzAptUpdatesCard in a window so the PackageKit integration can
 * be tested without building (or even being able to build) all of Bazaar.
 * See ai-patches/TEST.md.
 *
 * Environment variables:
 *   HARNESS_ACTION=apt-updates.check  activate a card action after 4 seconds
 *   HARNESS_SHOT=/path/out.png        save a screenshot, then quit
 *   HARNESS_DELAY=20                  seconds to wait before the screenshot
 *   HARNESS_EXPAND=1                  expand the package list 2 s before the screenshot
 *   HARNESS_HEIGHT=640                window height in pixels
 *   HARNESS_LOCALEDIR=/path           load translations (bazaar.mo, or HARNESS_DOMAIN.mo) from there,
 *                                     e.g. with LANGUAGE=id
 */

#include <stdlib.h>

#include <adwaita.h>
#include <glib/gi18n.h>

#include "bz-apt-updates-card.h"

static GtkWidget *card = NULL;

static gboolean
action_cb (gpointer data)
{
  gtk_widget_activate_action (card, g_getenv ("HARNESS_ACTION"), NULL);
  return G_SOURCE_REMOVE;
}

static void
expand_rows (GtkWidget *widget)
{
  if (ADW_IS_EXPANDER_ROW (widget))
    adw_expander_row_set_expanded (ADW_EXPANDER_ROW (widget), TRUE);

  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    expand_rows (child);
}

static gboolean
expand_cb (gpointer data)
{
  expand_rows (card);
  return G_SOURCE_REMOVE;
}

static gboolean
screenshot_cb (gpointer data)
{
  GtkWidget    *window    = data;
  GdkPaintable *paintable = NULL;
  GtkSnapshot  *snapshot  = NULL;
  GskRenderNode *node     = NULL;
  GdkTexture   *texture   = NULL;
  int           width     = 0;
  int           height    = 0;

  width     = gtk_widget_get_width (window);
  height    = gtk_widget_get_height (window);
  paintable = gtk_widget_paintable_new (window);
  snapshot  = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, snapshot, width, height);
  node    = gtk_snapshot_free_to_node (snapshot);
  texture = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (window)),
                                         node, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gdk_texture_save_to_png (texture, g_getenv ("HARNESS_SHOT"));

  g_application_quit (g_application_get_default ());
  return G_SOURCE_REMOVE;
}

static void
activate (GtkApplication *app)
{
  GtkWidget  *window = NULL;
  GtkWidget  *view   = NULL;
  GtkWidget  *scroll = NULL;
  GtkWidget  *clamp  = NULL;
  const char *icons  = NULL;

  window = adw_application_window_new (app);
  view   = adw_toolbar_view_new ();
  scroll = gtk_scrolled_window_new ();
  clamp  = adw_clamp_new ();
  card   = bz_apt_updates_card_new ();

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), adw_header_bar_new ());
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 650);
  adw_clamp_set_child (ADW_CLAMP (clamp), card);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), clamp);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), scroll);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window), view);
  gtk_window_set_default_size (GTK_WINDOW (window), 760,
                               g_getenv ("HARNESS_HEIGHT") != NULL ? atoi (g_getenv ("HARNESS_HEIGHT")) : 640);

  icons = g_getenv ("HARNESS_ICONS");
  if (icons != NULL)
    gtk_icon_theme_add_search_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()), icons);

  gtk_window_present (GTK_WINDOW (window));

  if (g_getenv ("HARNESS_ACTION") != NULL)
    g_timeout_add_seconds (4, action_cb, NULL);
  if (g_getenv ("HARNESS_EXPAND") != NULL)
    g_timeout_add_seconds (MAX (1, (g_getenv ("HARNESS_DELAY") != NULL ? atoi (g_getenv ("HARNESS_DELAY")) : 20) - 2),
                           expand_cb, NULL);
  if (g_getenv ("HARNESS_SHOT") != NULL)
    g_timeout_add_seconds (g_getenv ("HARNESS_DELAY") != NULL ? atoi (g_getenv ("HARNESS_DELAY")) : 20,
                           screenshot_cb, window);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (AdwApplication) app = NULL;

  if (g_getenv ("HARNESS_LOCALEDIR") != NULL)
    {
      const char *domain = g_getenv ("HARNESS_DOMAIN") != NULL ? g_getenv ("HARNESS_DOMAIN") : "bazaar";

      bindtextdomain (domain, g_getenv ("HARNESS_LOCALEDIR"));
      bind_textdomain_codeset (domain, "UTF-8");
      textdomain (domain);
    }

  app = adw_application_new ("io.github.kolunmi.BazaarAptHarness", G_APPLICATION_NON_UNIQUE);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}

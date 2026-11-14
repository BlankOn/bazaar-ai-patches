/* DOWNSTREAM PATCH (APT system updates): screenshot shim for the real app.
 * See ai-patches/TEST.md. Used by app-shot.sh; never linked into Bazaar.
 *
 * Loaded with LD_PRELOAD into ./build/src/bazaar. Once a second it asks
 * every toplevel to open the Library page ("window.open-library"; re-sent
 * because Bazaar ignores it during its startup "Refreshing" screen), then
 * after APP_SHOT_DELAY seconds saves the first window to APP_SHOT as PNG
 * and exits (the process showing the window; app-shot.sh's timeout ends
 * the rest). With APP_SHOT_EXPAND=1 the APT card's package list is
 * expanded 2 seconds before the screenshot.
 *
 * APP_SHOT_DEBUG=1 logs each tick (pid, window type, mapped).
 *
 * GTK's Broadway backend crashed on Bazaar, hence this approach.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <adwaita.h>

static int ticks = 0;

static void
expand_apt_card (GtkWidget *widget,
                 gboolean   inside)
{
  inside = inside || g_strcmp0 (G_OBJECT_TYPE_NAME (widget), "BzAptUpdatesCard") == 0;
  if (inside && ADW_IS_EXPANDER_ROW (widget))
    adw_expander_row_set_expanded (ADW_EXPANDER_ROW (widget), TRUE);

  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    expand_apt_card (child, inside);
}

static void
save_png (GtkWidget  *window,
          const char *path)
{
  GdkPaintable  *paintable = NULL;
  GtkSnapshot   *snapshot  = NULL;
  GskRenderNode *node      = NULL;
  GdkTexture    *texture   = NULL;
  GskRenderer   *renderer  = NULL;
  int            width     = 0;
  int            height    = 0;

  width     = gtk_widget_get_width (window);
  height    = gtk_widget_get_height (window);
  paintable = gtk_widget_paintable_new (window);
  snapshot  = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, snapshot, width, height);
  node    = gtk_snapshot_free_to_node (snapshot);
  /* The window's own (GPU) renderer produced only the background when
   * rendering Bazaar's window offscreen; GTK's software renderer works. */
  renderer = gsk_cairo_renderer_new ();
  gsk_renderer_realize_for_display (renderer, gtk_widget_get_display (window), NULL);
  texture = gsk_renderer_render_texture (renderer, node, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gsk_renderer_unrealize (renderer);
  gdk_texture_save_to_png (texture, path);
}

static gboolean
tick_cb (gpointer data)
{
  GList *toplevels = NULL;
  int    delay     = 0;

  delay = g_getenv ("APP_SHOT_DELAY") != NULL ? atoi (g_getenv ("APP_SHOT_DELAY")) : 40;
  ticks++;

  toplevels = gtk_window_list_toplevels ();
  if (g_getenv ("APP_SHOT_DEBUG") != NULL)
    for (GList *l = toplevels; l != NULL; l = l->next)
      g_printerr ("app-shot: pid %d tick %d: %s mapped=%d\n", (int) getpid (), ticks,
                  G_OBJECT_TYPE_NAME (l->data), gtk_widget_get_mapped (l->data));
  for (GList *l = toplevels; l != NULL; l = l->next)
    {
      GtkWidget *window = l->data;

      if (!ADW_IS_APPLICATION_WINDOW (window) || !gtk_widget_get_mapped (window))
        continue;

      gtk_widget_activate_action (window, "window.open-library", NULL);

      if (ticks == delay - 2 && g_getenv ("APP_SHOT_EXPAND") != NULL)
        expand_apt_card (window, FALSE);

      if (ticks >= delay)
        {
          save_png (window, g_getenv ("APP_SHOT"));
          g_printerr ("app-shot: saved %s\n", g_getenv ("APP_SHOT"));
          g_list_free (toplevels);
          _exit (0);
        }
    }
  g_list_free (toplevels);

  if (ticks > delay + 30)
    return G_SOURCE_REMOVE;
  return G_SOURCE_CONTINUE;
}

/* Adding the timeout directly from the constructor did not work: it runs
 * before GLib and the app have set up the main context, and the timeout
 * never fired. Add it from a thread once the app is running instead. */
static gpointer
start_thread (gpointer data)
{
  g_usleep (2 * G_USEC_PER_SEC);
  g_timeout_add_seconds (1, tick_cb, NULL);
  g_main_context_wakeup (NULL);
  return NULL;
}

__attribute__ ((constructor)) static void
app_shot_init (void)
{
  if (g_getenv ("APP_SHOT") == NULL)
    return;
  /* Keep LD_PRELOAD set: Bazaar shows its window from a child process it
   * spawns, so the shim must be loaded there too. Processes that never
   * show a window just stop their timer (see tick_cb). */
  g_thread_unref (g_thread_new ("app-shot", start_thread, NULL));
}

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

static int           ticks     = 0;
/* Created when the window first shows and kept: a paintable created only
 * at screenshot time came out empty while the About dialog was open, one
 * that watched the window from the start did not. */
static GdkPaintable *paintable = NULL;

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

/* First descendant of widget (itself included) of the named type. */
static GtkWidget *
find_typed (GtkWidget  *widget,
            const char *type_name,
            GType       type)
{
  if ((type_name != NULL && g_strcmp0 (G_OBJECT_TYPE_NAME (widget), type_name) == 0) ||
      (type != G_TYPE_INVALID && g_type_is_a (G_OBJECT_TYPE (widget), type)))
    return widget;

  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_typed (child, type_name, type);
      if (found != NULL)
        return found;
    }
  return NULL;
}

static GtkWidget *
find_css_class (GtkWidget  *widget,
                const char *css_class)
{
  if (gtk_widget_has_css_class (widget, css_class))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_css_class (child, css_class);
      if (found != NULL)
        return found;
    }
  return NULL;
}

/* APP_SHOT_PAGE=NAME: show that page of the window's main view stack. */
static gboolean
open_page (GtkWidget  *widget,
           const char *name)
{
  if (ADW_IS_VIEW_STACK (widget) && adw_view_stack_get_child_by_name (ADW_VIEW_STACK (widget), name) != NULL)
    {
      adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (widget), name);
      return TRUE;
    }
  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    if (open_page (child, name))
      return TRUE;
  return FALSE;
}

/* DOWNSTREAM PATCH (GNOME Extensions): APP_SHOT_EXT_SEARCH=TEXT and
 * APP_SHOT_EXT_SORT=N (0 Popular, 1 Trending, 2 Recent, 3 Installed, or
 * the name: popular, trending, recent, installed) set
 * the Extensions page's search; APP_SHOT_EXT_OPEN=N opens the details of
 * result N; APP_SHOT_EXT_LOADING=1 shows the "loading more" dots. */
static void
drive_extensions_page (GtkWidget *window,
                       int        ticks_left)
{
  GtkWidget *page = find_typed (window, "BzGnomeExtensionsPage", G_TYPE_INVALID);

  if (page == NULL)
    return;
  /* A number sets the "sort" property; a name (popular, trending, recent,
   * installed) goes through the sort menu's "page.sort" action. */
  if (ticks == 3 && g_getenv ("APP_SHOT_EXT_SORT") != NULL)
    {
      const char *sort = g_getenv ("APP_SHOT_EXT_SORT");

      if (g_ascii_isdigit (sort[0]))
        g_object_set (page, "sort", atoi (sort), NULL);
      else
        gtk_widget_activate_action (page, "page.sort", "s", sort);
    }
  if (ticks == 3 && g_getenv ("APP_SHOT_EXT_SEARCH") != NULL)
    gtk_editable_set_text (GTK_EDITABLE (find_typed (page, "BzSearchBar", G_TYPE_INVALID)),
                           g_getenv ("APP_SHOT_EXT_SEARCH"));
  /* APP_SHOT_EXT_LOADING=1: show the "loading more" dots (normally only
   * visible for the second or so the next page takes). */
  if (ticks_left <= 2 && g_getenv ("APP_SHOT_EXT_LOADING") != NULL)
    {
      GtkWidget *dots = find_css_class (page, "extensions-loading-more");
      if (dots != NULL)
        gtk_widget_set_visible (dots, TRUE);
    }
  if (ticks_left == 4 && g_getenv ("APP_SHOT_EXT_OPEN") != NULL)
    g_signal_emit_by_name (find_typed (page, NULL, GTK_TYPE_GRID_VIEW), "activate",
                           (guint) atoi (g_getenv ("APP_SHOT_EXT_OPEN")));
}

static gboolean
save_png (GtkWidget  *window,
          const char *path)
{
  GtkSnapshot   *snapshot  = NULL;
  GskRenderNode *node      = NULL;
  GdkTexture    *texture   = NULL;
  GskRenderer   *renderer  = NULL;
  int            width     = 0;
  int            height    = 0;

  width     = gtk_widget_get_width (window);
  height    = gtk_widget_get_height (window);
  snapshot  = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, snapshot, width, height);
  node    = gtk_snapshot_free_to_node (snapshot);
  /* A window that never got a frame (covered by other windows: the
   * compositor sends it no frame callbacks) has no drawing for the
   * paintable to show: draw its widgets directly. */
  if (node == NULL)
    {
      snapshot = gtk_snapshot_new ();
      GTK_WIDGET_GET_CLASS (window)->snapshot (window, snapshot);
      node = gtk_snapshot_free_to_node (snapshot);
    }
  /* The window's own (GPU) renderer produced only the background when
   * rendering Bazaar's window offscreen; GTK's software renderer works. */
  renderer = gsk_cairo_renderer_new ();
  gsk_renderer_realize_for_display (renderer, gtk_widget_get_display (window), NULL);
  texture = gsk_renderer_render_texture (renderer, node, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gsk_renderer_unrealize (renderer);
  /* The software renderer gives up on some nodes (seen with the About
   * dialog's SVG app icon on GTK 4.23); then try the window's renderer. */
  if (texture == NULL)
    texture = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (window)),
                                           node, &GRAPHENE_RECT_INIT (0, 0, width, height));
  if (texture == NULL)
    {
      g_printerr ("app-shot: could not render the window (%dx%d, %s), trying again\n", width, height,
                  node == NULL ? "nothing drawn yet" : "renderer failed");
      return FALSE;
    }
  return gdk_texture_save_to_png (texture, path);
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

      if (paintable == NULL)
        paintable = gtk_widget_paintable_new (window);


      if (g_getenv ("APP_SHOT_PAGE") != NULL)
        open_page (window, g_getenv ("APP_SHOT_PAGE"));
      else
        gtk_widget_activate_action (window, "window.open-library", NULL);
      drive_extensions_page (window, delay - ticks);

      /* APP_SHOT_WIDTH=N: resize the window, e.g. below the 700 px
       * breakpoint for the narrow layout. */
      if (ticks == 2 && g_getenv ("APP_SHOT_WIDTH") != NULL)
        {
          gtk_window_unmaximize (GTK_WINDOW (window));
          gtk_window_set_default_size (GTK_WINDOW (window), atoi (g_getenv ("APP_SHOT_WIDTH")), 700);
        }

      if (ticks == delay - 2 && g_getenv ("APP_SHOT_EXPAND") != NULL)
        expand_apt_card (window, FALSE);

      /* APP_SHOT_ABOUT=1: open the About dialog for the screenshot (checks
       * the Warsi rebrand's "Originally Bazaar" note). */
      if (ticks == 10 && g_getenv ("APP_SHOT_ABOUT") != NULL)
        g_action_group_activate_action (G_ACTION_GROUP (g_application_get_default ()), "about", NULL);

      /* A snapshot taken while the window is between frames can come out
       * empty (seen with the About dialog open); retry for 10 s. */
      if (ticks >= delay && (save_png (window, g_getenv ("APP_SHOT")) || ticks >= delay + 10))
        {
          g_printerr ("app-shot: %s %s\n", ticks < delay + 10 ? "saved" : "gave up on", g_getenv ("APP_SHOT"));
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

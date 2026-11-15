/* DOWNSTREAM PATCH (APT system updates): main-loop stall probe for the real
 * app. See ai-patches/TEST.md "Search page freeze" (hook H13). Test-only;
 * never linked into the app.
 *
 * Loaded with LD_PRELOAD by stall-probe.sh. After STALL_PROBE_DELAY seconds
 * it switches the window to the Search page (what clicking the Search tab
 * does), types STALL_PROBE_TYPE into its search entry 6 s later if set, and
 * after 25 s more prints the longest time the main loop was blocked, then
 * exits. While the main loop is blocked for more than 0.4 s, a watchdog
 * thread prints the main thread's stack (glibc backtrace; resolve offsets
 * with addr2line), which shows what blocks it.
 */

#define _GNU_SOURCE
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <adwaita.h>

static pthread_t      main_thread;
static _Atomic gint64 last_beat   = 0;
static _Atomic gint64 switched_at = 0;
static _Atomic int    is_gui      = 0;
static gint64         max_stall   = 0;
static int            ticks       = 0;

static void
print_stack (int sig)
{
  void *frames[64];
  int   n = backtrace (frames, 64);

  (void) sig;
  (void) !write (2, "STALL-PROBE-STACK-BEGIN\n", 24);
  backtrace_symbols_fd (frames, n, 2);
  (void) !write (2, "STALL-PROBE-STACK-END\n", 22);
}

static gboolean
beat (gpointer data)
{
  gint64 now  = g_get_monotonic_time ();
  gint64 prev = atomic_exchange (&last_beat, now);

  if (prev != 0 && atomic_load (&switched_at) != 0 && now - prev > max_stall)
    max_stall = now - prev;
  return G_SOURCE_CONTINUE;
}

static long
rss_mb (void)
{
  long  pages = 0;
  long  rss   = 0;
  FILE *f     = fopen ("/proc/self/statm", "r");

  if (f != NULL)
    {
      if (fscanf (f, "%ld %ld", &pages, &rss) != 2)
        rss = 0;
      fclose (f);
    }
  return rss * sysconf (_SC_PAGESIZE) / (1024 * 1024);
}

static GtkWidget *
find_widget (GtkWidget *widget,
             gboolean (*match) (GtkWidget *))
{
  if (match (widget))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_widget (child, match);

      if (found != NULL)
        return found;
    }
  return NULL;
}

static gboolean
is_main_stack (GtkWidget *widget)
{
  return ADW_IS_VIEW_STACK (widget) &&
         adw_view_stack_get_child_by_name (ADW_VIEW_STACK (widget), "search") != NULL;
}

static gboolean
is_mapped_text (GtkWidget *widget)
{
  return GTK_IS_TEXT (widget) && gtk_widget_get_mapped (widget);
}

static gboolean
tick (gpointer data)
{
  const char *delay_env = g_getenv ("STALL_PROBE_DELAY");
  int         delay     = delay_env != NULL ? atoi (delay_env) : 30;
  GList      *toplevels = gtk_window_list_toplevels ();

  ticks++;
  for (GList *l = toplevels; l != NULL; l = l->next)
    {
      GtkWidget *window = l->data;
      GtkWidget *stack  = NULL;

      if (!ADW_IS_APPLICATION_WINDOW (window) || !gtk_widget_get_mapped (window))
        continue;
      atomic_store (&is_gui, 1);
      stack = find_widget (window, is_main_stack);
      if (stack == NULL)
        continue;

      if (ticks == delay)
        {
          g_printerr ("stall-probe: switching to the Search page\n");
          atomic_store (&switched_at, g_get_monotonic_time ());
          adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (stack), "search");
        }
      if (ticks == delay || ticks == delay + 12 || ticks == delay + 24)
        g_printerr ("stall-probe: t=+%ds memory (RSS) %ld MB\n", ticks - delay, rss_mb ());
      if (ticks == delay + 6 && g_getenv ("STALL_PROBE_TYPE") != NULL)
        {
          GtkWidget *page = adw_view_stack_get_child_by_name (ADW_VIEW_STACK (stack), "search");
          GtkWidget *text = find_widget (page, is_mapped_text);

          if (text != NULL)
            gtk_editable_set_text (GTK_EDITABLE (text), g_getenv ("STALL_PROBE_TYPE"));
          g_printerr ("stall-probe: typed '%s'%s\n", g_getenv ("STALL_PROBE_TYPE"),
                      text != NULL ? "" : " (search entry not found)");
        }
      if (ticks == delay + 25)
        {
          g_printerr ("stall-probe: longest main-loop stall after opening Search: %.2f s\n",
                      max_stall / 1e6);
          _exit (0);
        }
    }
  g_list_free (toplevels);
  return G_SOURCE_CONTINUE;
}

static gpointer
watchdog (gpointer data)
{
  gint64 reported = 0;

  /* timeouts added from the constructor never fire (see app-shot.c) */
  g_usleep (2 * G_USEC_PER_SEC);
  g_timeout_add (50, beat, NULL);
  g_timeout_add_seconds (1, tick, NULL);
  g_main_context_wakeup (NULL);

  for (;;)
    {
      gint64 now  = g_get_monotonic_time ();
      gint64 last = atomic_load (&last_beat);

      g_usleep (100000);
      if (atomic_load (&is_gui) && atomic_load (&switched_at) != 0 && last != 0 &&
          now - last > 400000 && now - reported > 400000)
        {
          reported = now;
          g_printerr ("stall-probe: main loop blocked for %.2f s\n", (now - last) / 1e6);
          pthread_kill (main_thread, SIGUSR1);
        }
    }
  return NULL;
}

__attribute__ ((constructor)) static void
stall_probe_init (void)
{
  if (g_getenv ("STALL_PROBE") == NULL)
    return;
  /* LD_PRELOAD stays set: the window lives in a process the app spawns */
  main_thread = pthread_self ();
  signal (SIGUSR1, print_stack);
  g_thread_unref (g_thread_new ("stall-probe", watchdog, NULL));
}

/* DOWNSTREAM PATCH (GNOME Extensions): test program for the extensions
 * model (bz-gnome-extensions.c). See ai-patches/TEST.md E1.
 *
 * Runs on a private session bus where fake-shell-extensions.py stands in
 * for GNOME Shell (ext-check.sh sets that up), and queries the real
 * extensions.gnome.org. Prints PASS/FAIL per check; exit 1 on any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz-gnome-extensions.h"

static BzGnomeExtensions *ext    = NULL;
static int                failed = 0;

static void
check (gboolean    ok,
       const char *what)
{
  printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    failed = 1;
}

/* Runs the main loop until cond () or the timeout. */
static gboolean
wait_for (gboolean (*cond) (void),
          int        seconds)
{
  gint64 end = g_get_monotonic_time () + seconds * G_USEC_PER_SEC;

  while (!cond ())
    {
      if (g_get_monotonic_time () > end)
        return FALSE;
      g_main_context_iteration (NULL, FALSE);
      g_usleep (10000);
    }
  return TRUE;
}

static gboolean
settled (void)
{
  return bz_gnome_extensions_get_status (ext) != BZ_GNOME_EXTENSIONS_STATUS_LOADING &&
         !bz_gnome_extensions_get_loading_more (ext);
}

static gboolean
available (void)
{
  return bz_gnome_extensions_get_available (ext);
}

static guint
n_results (void)
{
  return g_list_model_get_n_items (bz_gnome_extensions_get_results (ext));
}

static BzGnomeExtension *
find (const char *uuid)
{
  for (guint i = 0; i < n_results (); i++)
    {
      g_autoptr (BzGnomeExtension) e = g_list_model_get_item (bz_gnome_extensions_get_results (ext), i);
      if (g_strcmp0 (bz_gnome_extension_get_uuid (e), uuid) == 0)
        return e; /* the list keeps it alive */
    }
  return NULL;
}

static gboolean
find_name (const char *name)
{
  for (guint i = 0; i < n_results (); i++)
    {
      g_autoptr (BzGnomeExtension) e = g_list_model_get_item (bz_gnome_extensions_get_results (ext), i);
      if (g_strcmp0 (bz_gnome_extension_get_name (e), name) == 0)
        return TRUE;
    }
  return FALSE;
}

static BzGnomeExtension *watched = NULL;

static gboolean watched_active (void) { return bz_gnome_extension_get_active (watched) && !bz_gnome_extension_get_busy (watched); }
static gboolean watched_gone (void) { return !bz_gnome_extension_get_installed (watched) && !bz_gnome_extension_get_busy (watched); }
static gboolean watched_error (void) { return bz_gnome_extension_get_error (watched) != NULL && !bz_gnome_extension_get_busy (watched); }
static gboolean user_off (void) { return !bz_gnome_extensions_get_user_extensions_enabled (ext); }
static gboolean two_installed (void) { return n_results () == 2; }
static gboolean has_icon (void) { return bz_gnome_extension_get_icon (watched) != NULL; }

static void
search (const char           *text,
        BzGnomeExtensionsSort sort)
{
  bz_gnome_extensions_search (ext, text, sort);
  wait_for (settled, 60);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (BzGnomeExtension) dock = NULL;
  BzGnomeExtension *e               = NULL;

  ext = bz_gnome_extensions_new ();
  check (wait_for (available, 10), "GNOME Shell's extension service is found (the fake one)");
  check (g_strcmp0 (bz_gnome_extensions_get_shell_version (ext), "50.4") == 0, "Shell version 50.4");

  search ("", BZ_GNOME_EXTENSIONS_SORT_DOWNLOADS);
  check (bz_gnome_extensions_get_status (ext) == BZ_GNOME_EXTENSIONS_STATUS_RESULTS && n_results () == 24,
         "Popular: first page of 24 from extensions.gnome.org");
  check (bz_gnome_extensions_get_has_more (ext), "Popular: more pages");
  e = find ("dash-to-dock@micxgx.gmail.com");
  check (e != NULL && bz_gnome_extension_get_active (e), "Dash to Dock listed and shown as enabled (installed in the fake)");
  if (e != NULL)
    {
      watched = e;
      check (wait_for (has_icon, 30), "Dash to Dock's icon loads");
    }
  e = find ("caffeine@patapon.info");
  check (e != NULL && bz_gnome_extension_get_installed (e) && !bz_gnome_extension_get_active (e) &&
             g_strcmp0 (bz_gnome_extension_get_status_label (e), "Disabled") == 0,
         "Caffeine listed as installed but disabled");

  bz_gnome_extensions_load_more (ext);
  wait_for (settled, 60);
  check (n_results () == 48, "Scrolling: the second page is appended (48)");

  search ("clipboard", BZ_GNOME_EXTENSIONS_SORT_DOWNLOADS);
  check (find_name ("Clipboard Indicator"), "Search \"clipboard\" finds Clipboard Indicator");

  search ("zzqqxxnosuchthing", BZ_GNOME_EXTENSIONS_SORT_RECENT);
  check (bz_gnome_extensions_get_status (ext) == BZ_GNOME_EXTENSIONS_STATUS_EMPTY, "A search with no match is empty");

  search ("", BZ_GNOME_EXTENSIONS_SORT_INSTALLED);
  check (n_results () == 3 && !bz_gnome_extensions_get_has_more (ext), "Installed: the fake's three extensions");
  search ("caff", BZ_GNOME_EXTENSIONS_SORT_INSTALLED);
  check (n_results () == 1 && find ("caffeine@patapon.info") != NULL, "Installed, search \"caff\": Caffeine only");

  watched = find ("caffeine@patapon.info");
  if (watched != NULL)
    {
      bz_gnome_extensions_set_enabled (ext, watched, TRUE);
      check (wait_for (watched_active, 10), "Enable Caffeine: shown as enabled");
    }

  search ("", BZ_GNOME_EXTENSIONS_SORT_INSTALLED);
  e = find ("dash-to-dock@micxgx.gmail.com");
  if (e != NULL)
    {
      dock    = g_object_ref (e);
      watched = dock;
      check (bz_gnome_extension_get_can_uninstall (dock), "Dash to Dock (per user) can be removed");
      bz_gnome_extensions_uninstall (ext, dock);
      check (wait_for (watched_gone, 10), "Remove Dash to Dock: no longer installed");
      check (wait_for (two_installed, 10), "Installed view drops it (2 left)");
    }

  watched = find ("fake-system@example.invalid");
  if (watched != NULL)
    {
      check (!bz_gnome_extension_get_can_uninstall (watched) &&
                 g_strcmp0 (bz_gnome_extension_get_error (watched), "TypeError: this is a fake error") == 0,
             "System extension: cannot be removed, shows Shell's error");
      bz_gnome_extensions_uninstall (ext, watched);
      check (wait_for (watched_error, 10), "Removing it anyway reports that Shell refused");
    }

  search ("clipboard", BZ_GNOME_EXTENSIONS_SORT_DOWNLOADS);
  watched = find ("clipboard-indicator@tudmotu.com");
  if (watched != NULL)
    {
      check (!bz_gnome_extension_get_installed (watched), "Clipboard Indicator not installed yet");
      bz_gnome_extensions_install (ext, watched);
      check (wait_for (watched_active, 10), "Install Clipboard Indicator (fake Shell accepts): enabled");
    }
  else
    check (FALSE, "Clipboard Indicator found for the install test");

  bz_gnome_extensions_set_user_extensions_enabled (ext, FALSE);
  check (wait_for (user_off, 10), "Turning extensions off follows UserExtensionsEnabled");

  g_clear_object (&ext);
  return failed;
}

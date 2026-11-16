/* DOWNSTREAM PATCH (APT system updates): test program for the .deb file
 * installer's model (bz-deb-installer.c). See ai-patches/TEST.md D1.
 *
 * Usage: deb-check FILE.deb
 * Reads the file the way the install dialog does (PackageKit's
 * GetDetailsLocal, GetFilesLocal, Resolve, and a *simulated* InstallFiles),
 * then prints what it found as KEY=VALUE lines. Never installs anything.
 */

#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>

#include "bz-deb-installer.h"

static GMainLoop *loop = NULL;

static const char *
state_name (BzDebState state)
{
  switch (state)
    {
    case BZ_DEB_STATE_LOADING: return "loading";
    case BZ_DEB_STATE_READY: return "ready";
    case BZ_DEB_STATE_BLOCKED: return "blocked";
    case BZ_DEB_STATE_INSTALLING: return "installing";
    case BZ_DEB_STATE_INSTALLED: return "installed";
    case BZ_DEB_STATE_FAILED: return "failed";
    default: return "?";
    }
}

static const char *
action_name (BzDebAction action)
{
  switch (action)
    {
    case BZ_DEB_ACTION_INSTALL: return "install";
    case BZ_DEB_ACTION_UPGRADE: return "upgrade";
    case BZ_DEB_ACTION_DOWNGRADE: return "downgrade";
    case BZ_DEB_ACTION_NONE: return "none";
    default: return "?";
    }
}

static void
changed_cb (BzDebInstaller *installer,
            gpointer        data)
{
  if (bz_deb_installer_get_state (installer) != BZ_DEB_STATE_LOADING)
    /* give the launcher and icon (read with dpkg-deb) time to arrive */
    g_timeout_add_seconds_once (2, (GSourceOnceFunc) g_main_loop_quit, loop);
}

static gboolean
timeout_cb (gpointer data)
{
  g_printerr ("deb-check: timed out\n");
  exit (2);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (GFile) file              = NULL;
  g_autoptr (BzDebInstaller) installer = NULL;
  GIcon      *icon                    = NULL;
  const char *icon_kind               = "none";

  if (argc != 2)
    {
      g_printerr ("usage: deb-check FILE.deb\n");
      return 2;
    }

  loop      = g_main_loop_new (NULL, FALSE);
  file      = g_file_new_for_commandline_arg (argv[1]);
  installer = bz_deb_installer_new (file);
  g_signal_connect (installer, "changed", G_CALLBACK (changed_cb), NULL);
  if (bz_deb_installer_get_state (installer) != BZ_DEB_STATE_LOADING)
    changed_cb (installer, NULL);
  g_timeout_add_seconds (120, timeout_cb, NULL);
  g_main_loop_run (loop);

  icon = bz_deb_installer_get_icon (installer);
  if (G_IS_BYTES_ICON (icon))
    icon_kind = "from-package";
  else if (G_IS_THEMED_ICON (icon))
    icon_kind = "themed";

  printf ("state=%s\n", state_name (bz_deb_installer_get_state (installer)));
  printf ("action=%s\n", action_name (bz_deb_installer_get_action (installer)));
  printf ("name=%s\n", bz_deb_installer_get_name (installer));
  printf ("version=%s\n", bz_deb_installer_get_version (installer));
  printf ("installed_version=%s\n", bz_deb_installer_get_installed_version (installer));
  printf ("summary=%s\n", bz_deb_installer_get_summary (installer));
  printf ("installed_size=%" G_GUINT64_FORMAT "\n", bz_deb_installer_get_installed_size (installer));
  printf ("app_name=%s\n", bz_deb_installer_get_app_name (installer));
  printf ("desktop_id=%s\n", bz_deb_installer_get_desktop_id (installer));
  printf ("icon=%s\n", icon_kind);
  printf ("extra=%u\n", bz_deb_installer_get_extra_packages (installer)->len);
  printf ("removed=%u\n", bz_deb_installer_get_removed_packages (installer)->len);
  printf ("error=%s\n", bz_deb_installer_get_error (installer));
  return 0;
}

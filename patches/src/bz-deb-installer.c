/* bz-deb-installer.c
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

/* DOWNSTREAM PATCH (APT system updates): .deb file installer, see
 * ai-patches/PLAN.md §12.
 *
 * Loading runs four PackageKit transactions in a row, each started when the
 * previous one finished: GetDetailsLocal (name, version, description, ...),
 * GetFilesLocal (to find the app's launcher and icon), Resolve with the
 * "installed" filter (the installed version), and InstallFiles with the
 * SIMULATE flag (other packages it adds or removes, or why it cannot be
 * installed). Installing is InstallFiles without SIMULATE. Local files
 * cannot be "trusted" (PackageKit refuses ONLY_TRUSTED with
 * "Local packages cannot be authenticated"), so the install uses polkit's
 * package-install-untrusted action, like GNOME Software.
 */

#define G_LOG_DOMAIN "BAZAAR::DEB"

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <glib/gi18n.h>

#include "bz-deb-installer.h"

#define PK_NAME          "org.freedesktop.PackageKit"
#define PK_PATH          "/org/freedesktop/PackageKit"
#define PK_IFACE         "org.freedesktop.PackageKit"
#define PK_IFACE_TXN     "org.freedesktop.PackageKit.Transaction"
#define DBUS_IFACE_PROPS "org.freedesktop.DBus.Properties"

/* Values from PackageKit's pk-enum.h (stable D-Bus ABI). Bitfields are
 * (1 << enum value). See ai-patches/API.md §3.1. */
#define PK_FILTER_INSTALLED                 (G_GUINT64_CONSTANT (1) << 2)
#define PK_TRANSACTION_FLAG_SIMULATE        (G_GUINT64_CONSTANT (1) << 2)
#define PK_TRANSACTION_FLAG_ALLOW_DOWNGRADE (G_GUINT64_CONSTANT (1) << 6)
#define PK_INFO_INSTALLED                   1
#define PK_INFO_UPDATING                    11
#define PK_INFO_INSTALLING                  12
#define PK_INFO_REMOVING                    13
#define PK_INFO_OBSOLETING                  15
#define PK_INFO_REINSTALLING                19
#define PK_INFO_DOWNGRADING                 20
#define PK_EXIT_SUCCESS                     1
#define PK_EXIT_CANCELLED                   3
#define PK_ERROR_NOT_AUTHORIZED             48
#define PK_ERROR_TRANSACTION_CANCELLED      17
#define PK_PERCENTAGE_UNKNOWN               101
#define PK_RESTART_SYSTEM                   4
#define PK_RESTART_SECURITY_SYSTEM          6

/* Icons and launchers larger than this are ignored */
#define MAX_MEMBER_SIZE (4 * 1024 * 1024)

/* ------------------------------------------------------------------------ */
/* One PackageKit transaction                                               */
/* ------------------------------------------------------------------------ */

typedef struct _Run Run;
typedef void (*RunDone) (BzDebInstaller *self,
                         Run            *run);

typedef struct
{
  guint info;
  char *id;
  char *summary;
} RunPackage;

struct _Run
{
  gint            ref_count;
  BzDebInstaller *self;
  char           *method;
  GVariant       *params;
  gboolean        interactive;
  RunDone         done;

  char    *path;
  guint    signals_id;
  guint    props_id;
  gboolean finished;

  guint      exit;
  guint      error_code;
  char      *error;
  GPtrArray *packages;
  GVariant  *details;
  char     **files;
  guint      restart;
};

struct _BzDebInstaller
{
  GObject parent_instance;

  GDBusConnection *bus;
  char            *path;
  char            *file_name;

  BzDebState  state;
  BzDebAction action;

  char   *name;
  char   *version;
  char   *arch;
  char   *summary;
  char   *description;
  char   *license;
  char   *url;
  guint64 installed_size;
  char   *installed_version;

  char  *app_name;
  GIcon *icon;
  char  *desktop_id;
  char **files;

  GPtrArray *extra;
  GPtrArray *removed;

  char    *status;
  double   progress;
  gboolean can_cancel;
  char    *error;
  char    *error_details;
  gboolean restart_required;

  Run *run;
};

G_DEFINE_FINAL_TYPE (BzDebInstaller, bz_deb_installer, G_TYPE_OBJECT)

enum
{
  SIGNAL_CHANGED,
  LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

static void
start_run (BzDebInstaller *self,
           const char     *method,
           GVariant       *params,
           gboolean        interactive,
           RunDone         done);

static void
load_app_info (BzDebInstaller *self);

static void
changed (BzDebInstaller *self)
{
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
deb_package_free (BzDebPackage *package)
{
  g_free (package->name);
  g_free (package->version);
  g_free (package->summary);
  g_free (package);
}

static void
run_package_free (RunPackage *package)
{
  g_free (package->id);
  g_free (package->summary);
  g_free (package);
}

/* A PackageKit package ID is "name;version;arch;data" */
static gboolean
split_package_id (const char *package_id,
                  char      **name,
                  char      **version,
                  char      **arch)
{
  g_auto (GStrv) parts = g_strsplit (package_id, ";", 4);

  if (g_strv_length (parts) < 3)
    return FALSE;

  if (name != NULL)
    *name = g_strdup (parts[0]);
  if (version != NULL)
    *version = g_strdup (parts[1]);
  if (arch != NULL)
    *arch = g_strdup (parts[2]);
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                  */
/* ------------------------------------------------------------------------ */

static void
bz_deb_installer_dispose (GObject *object)
{
  BzDebInstaller *self = BZ_DEB_INSTALLER (object);

  /* A running transaction holds a reference, so self->run is NULL here */
  g_clear_object (&self->bus);
  g_clear_object (&self->icon);

  G_OBJECT_CLASS (bz_deb_installer_parent_class)->dispose (object);
}

static void
bz_deb_installer_finalize (GObject *object)
{
  BzDebInstaller *self = BZ_DEB_INSTALLER (object);

  g_clear_pointer (&self->path, g_free);
  g_clear_pointer (&self->file_name, g_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->version, g_free);
  g_clear_pointer (&self->arch, g_free);
  g_clear_pointer (&self->summary, g_free);
  g_clear_pointer (&self->description, g_free);
  g_clear_pointer (&self->license, g_free);
  g_clear_pointer (&self->url, g_free);
  g_clear_pointer (&self->installed_version, g_free);
  g_clear_pointer (&self->app_name, g_free);
  g_clear_pointer (&self->desktop_id, g_free);
  g_clear_pointer (&self->files, g_strfreev);
  g_clear_pointer (&self->extra, g_ptr_array_unref);
  g_clear_pointer (&self->removed, g_ptr_array_unref);
  g_clear_pointer (&self->status, g_free);
  g_clear_pointer (&self->error, g_free);
  g_clear_pointer (&self->error_details, g_free);

  G_OBJECT_CLASS (bz_deb_installer_parent_class)->finalize (object);
}

static void
bz_deb_installer_class_init (BzDebInstallerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose  = bz_deb_installer_dispose;
  object_class->finalize = bz_deb_installer_finalize;

  signals[SIGNAL_CHANGED] =
      g_signal_new ("changed",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    0, NULL, NULL, NULL,
                    G_TYPE_NONE, 0);
}

static void
bz_deb_installer_init (BzDebInstaller *self)
{
  self->state    = BZ_DEB_STATE_LOADING;
  self->action   = BZ_DEB_ACTION_INSTALL;
  self->progress = -1.0;
  self->extra    = g_ptr_array_new_with_free_func ((GDestroyNotify) deb_package_free);
  self->removed  = g_ptr_array_new_with_free_func ((GDestroyNotify) deb_package_free);
}

/* ------------------------------------------------------------------------ */
/* State changes                                                            */
/* ------------------------------------------------------------------------ */

static void
set_error (BzDebInstaller *self,
           const char     *error,
           const char     *details)
{
  g_free (self->error);
  self->error = g_strdup (error);
  g_free (self->error_details);
  self->error_details = g_strcmp0 (details, error) != 0 ? g_strdup (details) : NULL;
}

/* run->error is PackageKit's (apt's) text when the transaction itself
 * failed, and our own, already readable, text when PackageKit could not be
 * reached (run->error_code is 0 then) */
static void
fail_load (BzDebInstaller *self,
           Run            *run,
           const char     *message)
{
  g_debug ("Cannot read %s: %s", self->path, run->error);
  self->state = BZ_DEB_STATE_FAILED;
  if (run->error_code == 0 && run->error != NULL)
    set_error (self, run->error, NULL);
  else
    set_error (self, message, run->error);
  changed (self);
}

static const char *
status_to_string (guint status)
{
  /* PkStatusEnum. The same strings as bz-apt-updates.c, so they share
   * translations. */
  switch (status)
    {
    case 1: return _ ("Waiting in queue");
    case 2: return _ ("Starting");
    case 3: return _ ("Running");
    case 4: return _ ("Querying");
    case 5: return _ ("Getting information");
    case 8: return _ ("Downloading packages");
    case 9: return _ ("Installing packages");
    case 10: return _ ("Updating packages");
    case 11: return _ ("Cleaning up");
    case 13: return _ ("Resolving dependencies");
    case 14: return _ ("Checking signatures");
    case 15: return _ ("Testing changes");
    case 16: return _ ("Committing changes");
    case 18: return _ ("Finished");
    case 19: return _ ("Cancelling");
    case 27: return _ ("Loading cache");
    case 30: return _ ("Waiting for package manager lock");
    case 31: return _ ("Waiting for authentication");
    case 36: return _ ("Running hooks");
    default: return NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Loading                                                                  */
/* ------------------------------------------------------------------------ */

static void
simulate_done (BzDebInstaller *self,
               Run            *run)
{
  gboolean found_self = FALSE;

  if (run->exit != PK_EXIT_SUCCESS)
    {
      g_debug ("Simulated install of %s failed: %s", self->path, run->error);
      self->state = BZ_DEB_STATE_BLOCKED;
      if (run->error_code == 0 && run->error != NULL)
        set_error (self, run->error, NULL);
      else
        set_error (self,
                   _ ("Other packages it needs are missing, or conflict with the installed system."),
                   run->error);
      changed (self);
      return;
    }

  for (guint i = 0; i < run->packages->len; i++)
    {
      RunPackage *package          = g_ptr_array_index (run->packages, i);
      g_autofree char *name        = NULL;
      g_autofree char *version     = NULL;
      BzDebPackage    *deb_package = NULL;

      if (!split_package_id (package->id, &name, &version, NULL))
        continue;

      if (!found_self && g_strcmp0 (name, self->name) == 0 &&
          g_strcmp0 (version, self->version) == 0)
        {
          found_self = TRUE;
          if (package->info == PK_INFO_DOWNGRADING)
            self->action = BZ_DEB_ACTION_DOWNGRADE;
          else if (package->info == PK_INFO_INSTALLING && self->installed_version == NULL)
            self->action = BZ_DEB_ACTION_INSTALL;
          else
            self->action = BZ_DEB_ACTION_UPGRADE;
          continue;
        }

      deb_package          = g_new0 (BzDebPackage, 1);
      deb_package->name    = g_steal_pointer (&name);
      deb_package->version = g_steal_pointer (&version);
      deb_package->summary = g_strdup (package->summary);

      if (package->info == PK_INFO_REMOVING || package->info == PK_INFO_OBSOLETING)
        g_ptr_array_add (self->removed, deb_package);
      else if (package->info == PK_INFO_INSTALLING || package->info == PK_INFO_UPDATING ||
               package->info == PK_INFO_DOWNGRADING || package->info == PK_INFO_REINSTALLING)
        g_ptr_array_add (self->extra, deb_package);
      else
        deb_package_free (deb_package);
    }

  if (!found_self)
    self->action = self->installed_version == NULL
                       ? BZ_DEB_ACTION_INSTALL
                       : BZ_DEB_ACTION_UPGRADE;

  self->state = BZ_DEB_STATE_READY;
  changed (self);
}

static void
simulate (BzDebInstaller *self)
{
  const char *paths[] = { self->path, NULL };

  start_run (self, "InstallFiles",
             g_variant_new ("(t^as)", PK_TRANSACTION_FLAG_SIMULATE, paths),
             FALSE, simulate_done);
}

static void
resolve_done (BzDebInstaller *self,
              Run            *run)
{
  /* Failure only means the installed version is unknown */
  for (guint i = 0; i < run->packages->len; i++)
    {
      RunPackage *package      = g_ptr_array_index (run->packages, i);
      g_autofree char *name    = NULL;
      g_autofree char *version = NULL;
      g_autofree char *arch    = NULL;

      if (package->info != PK_INFO_INSTALLED ||
          !split_package_id (package->id, &name, &version, &arch) ||
          g_strcmp0 (name, self->name) != 0)
        continue;

      /* With multiarch, the same name may be installed for several
       * architectures; "all" matches any */
      if (g_strcmp0 (arch, self->arch) != 0 &&
          g_strcmp0 (arch, "all") != 0 &&
          g_strcmp0 (self->arch, "all") != 0)
        continue;

      g_free (self->installed_version);
      self->installed_version = g_steal_pointer (&version);
      break;
    }

  if (g_strcmp0 (self->installed_version, self->version) == 0)
    {
      /* PackageKit's apt backend does nothing when asked to reinstall the
       * same version from a file, so there is nothing to offer */
      self->action = BZ_DEB_ACTION_NONE;
      self->state  = BZ_DEB_STATE_READY;
      changed (self);
      return;
    }

  changed (self);
  simulate (self);
}

static void
files_done (BzDebInstaller *self,
            Run            *run)
{
  const char *names[] = { self->name, NULL };

  if (run->files != NULL)
    {
      g_clear_pointer (&self->files, g_strfreev);
      self->files = g_strdupv (run->files);
      load_app_info (self);
    }

  start_run (self, "Resolve",
             g_variant_new ("(t^as)", PK_FILTER_INSTALLED, names),
             FALSE, resolve_done);
}

static void
details_done (BzDebInstaller *self,
              Run            *run)
{
  const char *package_id = NULL;
  const char *text       = NULL;
  const char *paths[]    = { self->path, NULL };

  if (run->exit != PK_EXIT_SUCCESS || run->details == NULL ||
      !g_variant_lookup (run->details, "package-id", "&s", &package_id) ||
      !split_package_id (package_id, &self->name, &self->version, &self->arch))
    {
      fail_load (self, run, _ ("The file is damaged, or it is not a software package for this system."));
      return;
    }

  if (g_variant_lookup (run->details, "summary", "&s", &text) && *text != '\0')
    self->summary = g_strdup (text);
  if (g_variant_lookup (run->details, "description", "&s", &text) && *text != '\0')
    self->description = g_strdup (text);
  if (g_variant_lookup (run->details, "license", "&s", &text) && *text != '\0' &&
      g_ascii_strcasecmp (text, "unknown") != 0)
    self->license = g_strdup (text);
  if (g_variant_lookup (run->details, "url", "&s", &text) && *text != '\0')
    self->url = g_strdup (text);
  g_variant_lookup (run->details, "size", "t", &self->installed_size);

  changed (self);

  start_run (self, "GetFilesLocal", g_variant_new ("(^as)", paths), FALSE, files_done);
}

static void
bus_ready_cb (GObject      *source,
              GAsyncResult *res,
              gpointer      user_data)
{
  g_autoptr (BzDebInstaller) self = user_data;
  g_autoptr (GError) local_error  = NULL;
  const char *paths[]             = { self->path, NULL };

  self->bus = g_bus_get_finish (res, &local_error);
  if (self->bus == NULL)
    {
      self->state = BZ_DEB_STATE_FAILED;
      set_error (self, local_error->message, NULL);
      changed (self);
      return;
    }

  start_run (self, "GetDetailsLocal", g_variant_new ("(^as)", paths), FALSE, details_done);
}

/* ------------------------------------------------------------------------ */
/* The app's launcher and icon, read from the file with dpkg-deb            */
/* ------------------------------------------------------------------------ */

typedef void (*MemberDone) (BzDebInstaller *self,
                            const char     *member,
                            GBytes         *bytes);

typedef struct
{
  BzDebInstaller *self;
  char           *member;
  MemberDone      done;
} MemberRead;

static void
member_read_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  MemberRead *read               = user_data;
  g_autoptr (GBytes) bytes       = NULL;
  g_autoptr (GError) local_error = NULL;

  if (!g_subprocess_communicate_finish (G_SUBPROCESS (source), res, &bytes, NULL, &local_error))
    g_debug ("Could not read %s from %s: %s", read->member, read->self->path, local_error->message);

  if (bytes != NULL && (g_bytes_get_size (bytes) == 0 || g_bytes_get_size (bytes) > MAX_MEMBER_SIZE))
    g_clear_pointer (&bytes, g_bytes_unref);

  read->done (read->self, read->member, bytes);

  g_object_unref (read->self);
  g_free (read->member);
  g_free (read);
}

/* Reads one file of the package (e.g. "/usr/share/applications/x.desktop")
 * to memory. Nothing is written to disk, so a hostile package cannot place
 * files anywhere. A symbolic link or a missing member gives NULL. */
static void
read_member (BzDebInstaller *self,
             const char     *member,
             MemberDone      done)
{
  g_autoptr (GSubprocess) process = NULL;
  g_autoptr (GError) local_error  = NULL;
  g_autofree char *tar_member     = g_strconcat (".", member, NULL);
  MemberRead      *read           = NULL;

  process = g_subprocess_new (
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      &local_error,
      "sh", "-c", "dpkg-deb --fsys-tarfile \"$1\" | tar -xOf - -- \"$2\"",
      "sh", self->path, tar_member, NULL);
  if (process == NULL)
    {
      g_debug ("Could not run dpkg-deb: %s", local_error->message);
      done (self, member, NULL);
      return;
    }

  read         = g_new0 (MemberRead, 1);
  read->self   = g_object_ref (self);
  read->member = g_strdup (member);
  read->done   = done;
  g_subprocess_communicate_async (process, NULL, NULL, member_read_cb, read);
}

static gboolean
has_file (BzDebInstaller *self,
          const char     *path)
{
  return self->files != NULL && g_strv_contains ((const char *const *) self->files, path);
}

static void
icon_read_cb (BzDebInstaller *self,
              const char     *member,
              GBytes         *bytes)
{
  if (bytes == NULL)
    return;

  g_clear_object (&self->icon);
  self->icon = g_bytes_icon_new (bytes);
  changed (self);
}

/* The best icon file for an icon name: hicolor's scalable SVG, then its
 * largest PNG, then /usr/share/pixmaps */
static char *
find_icon_file (BzDebInstaller *self,
                const char     *icon)
{
  g_autofree char *svg      = g_strdup_printf ("/%s.svg", icon);
  g_autofree char *png      = g_strdup_printf ("/%s.png", icon);
  const char      *best_png = NULL;
  int              best     = 0;

  if (g_path_is_absolute (icon))
    return has_file (self, icon) ? g_strdup (icon) : NULL;

  for (char **file = self->files; file != NULL && *file != NULL; file++)
    {
      const char *rest = NULL;
      int         size = 0;

      if (!g_str_has_prefix (*file, "/usr/share/icons/hicolor/"))
        continue;
      rest = *file + strlen ("/usr/share/icons/hicolor/");

      if (g_str_has_prefix (rest, "scalable/apps/") && g_str_has_suffix (rest, svg))
        return g_strdup (*file);
      if (g_str_has_suffix (rest, png) && sscanf (rest, "%dx", &size) == 1 && size > best)
        {
          best     = size;
          best_png = *file;
        }
    }
  if (best_png != NULL)
    return g_strdup (best_png);

  for (const char *const *ext = (const char *const[]) { ".svg", ".png", NULL }; *ext != NULL; ext++)
    {
      g_autofree char *pixmap = g_strconcat ("/usr/share/pixmaps/", icon, *ext, NULL);

      if (has_file (self, pixmap))
        return g_steal_pointer (&pixmap);
    }
  return NULL;
}

static void
desktop_read_cb (BzDebInstaller *self,
                 const char     *member,
                 GBytes         *bytes)
{
  g_autoptr (GKeyFile) key_file = g_key_file_new ();
  g_autofree char *type         = NULL;
  g_autofree char *name         = NULL;
  g_autofree char *icon         = NULL;
  g_autofree char *icon_file    = NULL;

  /* The first launcher shown in menus wins */
  if (bytes == NULL || self->desktop_id != NULL ||
      !g_key_file_load_from_data (key_file, g_bytes_get_data (bytes, NULL),
                                  g_bytes_get_size (bytes), G_KEY_FILE_NONE, NULL))
    return;

  type = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, NULL);
  name = g_key_file_get_locale_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
  if (g_strcmp0 (type, "Application") != 0 || name == NULL ||
      g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NO_DISPLAY, NULL) ||
      g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_HIDDEN, NULL))
    return;

  self->desktop_id = g_path_get_basename (member);
  self->app_name   = g_steal_pointer (&name);

  icon = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
  if (icon != NULL && *icon != '\0')
    {
      icon_file = find_icon_file (self, icon);
      if (icon_file != NULL)
        read_member (self, icon_file, icon_read_cb);
      else if (!g_path_is_absolute (icon))
        /* maybe from the icon theme, or the already installed version */
        self->icon = g_themed_icon_new (icon);
    }

  changed (self);
}

static void
load_app_info (BzDebInstaller *self)
{
  guint n = 0;

  for (char **file = self->files; file != NULL && *file != NULL && n < 8; file++)
    {
      const char *rest = NULL;

      if (!g_str_has_prefix (*file, "/usr/share/applications/") ||
          !g_str_has_suffix (*file, ".desktop"))
        continue;
      rest = *file + strlen ("/usr/share/applications/");
      if (strchr (rest, '/') != NULL)
        continue;

      read_member (self, *file, desktop_read_cb);
      n++;
    }
}

/* ------------------------------------------------------------------------ */
/* Installing                                                               */
/* ------------------------------------------------------------------------ */

static void
install_done (BzDebInstaller *self,
              Run            *run)
{
  g_clear_pointer (&self->status, g_free);
  self->progress   = -1.0;
  self->can_cancel = FALSE;

  if (run->exit == PK_EXIT_SUCCESS)
    {
      g_free (self->installed_version);
      self->installed_version = g_strdup (self->version);
      self->restart_required  = run->restart == PK_RESTART_SYSTEM ||
                               run->restart == PK_RESTART_SECURITY_SYSTEM;
      self->state             = BZ_DEB_STATE_INSTALLED;
    }
  else
    {
      self->state = BZ_DEB_STATE_READY;
      /* Cancelling and closing the password dialog are not errors */
      if (run->exit != PK_EXIT_CANCELLED &&
          run->error_code != PK_ERROR_NOT_AUTHORIZED &&
          run->error_code != PK_ERROR_TRANSACTION_CANCELLED)
        set_error (self, run->error != NULL ? run->error : _ ("Installation failed"), NULL);
    }

  changed (self);
}

void
bz_deb_installer_install (BzDebInstaller *self)
{
  const char *paths[] = { self->path, NULL };
  guint64     flags   = 0;

  g_return_if_fail (BZ_IS_DEB_INSTALLER (self));

  if (self->state != BZ_DEB_STATE_READY || self->action == BZ_DEB_ACTION_NONE)
    return;

  if (self->action == BZ_DEB_ACTION_DOWNGRADE)
    flags |= PK_TRANSACTION_FLAG_ALLOW_DOWNGRADE;

  self->state = BZ_DEB_STATE_INSTALLING;
  set_error (self, NULL, NULL);
  g_free (self->status);
  self->status   = g_strdup (_ ("Starting"));
  self->progress = -1.0;
  changed (self);

  start_run (self, "InstallFiles", g_variant_new ("(t^as)", flags, paths), TRUE, install_done);
}

void
bz_deb_installer_cancel (BzDebInstaller *self)
{
  g_return_if_fail (BZ_IS_DEB_INSTALLER (self));

  if (self->run == NULL || self->run->path == NULL || !self->run->interactive)
    return;

  g_dbus_connection_call (
      self->bus, PK_NAME, self->run->path, PK_IFACE_TXN, "Cancel",
      NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------------ */
/* Running a transaction                                                    */
/* ------------------------------------------------------------------------ */

static Run *
run_ref (Run *run)
{
  g_atomic_int_inc (&run->ref_count);
  return run;
}

static void
run_unref (Run *run)
{
  if (!g_atomic_int_dec_and_test (&run->ref_count))
    return;

  g_clear_pointer (&run->method, g_free);
  g_clear_pointer (&run->params, g_variant_unref);
  g_clear_pointer (&run->path, g_free);
  g_clear_pointer (&run->error, g_free);
  g_clear_pointer (&run->packages, g_ptr_array_unref);
  g_clear_pointer (&run->details, g_variant_unref);
  g_clear_pointer (&run->files, g_strfreev);
  g_clear_object (&run->self);
  g_free (run);
}

static void
run_finish (Run *run)
{
  BzDebInstaller *self = run->self;

  if (run->finished)
    return;
  run->finished = TRUE;

  if (run->signals_id != 0)
    g_dbus_connection_signal_unsubscribe (self->bus, run->signals_id);
  if (run->props_id != 0)
    g_dbus_connection_signal_unsubscribe (self->bus, run->props_id);
  run->signals_id = 0;
  run->props_id   = 0;

  if (self->run == run)
    self->run = NULL;

  g_debug ("%s finished: exit %u, error %u %s", run->method, run->exit,
           run->error_code, run->error != NULL ? run->error : "");
  run->done (self, run);

  /* self->run's reference */
  run_unref (run);
}

static void
run_fail (Run        *run,
          const char *message)
{
  if (run->finished)
    return;

  if (run->error == NULL)
    run->error = g_strdup (message);
  run->exit = 0;
  run_finish (run);
}

static void
run_fail_with_error (Run    *run,
                     GError *error)
{
  if (g_dbus_error_is_remote_error (error))
    {
      g_autofree char *name = g_dbus_error_get_remote_error (error);

      if (g_strcmp0 (name, "org.freedesktop.DBus.Error.ServiceUnknown") == 0 ||
          g_strcmp0 (name, "org.freedesktop.DBus.Error.NameHasNoOwner") == 0)
        {
          run_fail (run, _ ("Installing packages needs PackageKit, which is not installed."));
          return;
        }
      g_dbus_error_strip_remote_error (error);
    }

  run_fail (run, error->message);
}

static void
run_signal_cb (GDBusConnection *connection,
               const char      *sender_name,
               const char      *object_path,
               const char      *interface_name,
               const char      *signal_name,
               GVariant        *parameters,
               gpointer         user_data)
{
  Run *run = user_data;

  if (run->finished)
    return;

  if (g_strcmp0 (signal_name, "Package") == 0)
    {
      RunPackage *package = g_new0 (RunPackage, 1);

      g_variant_get (parameters, "(uss)", &package->info, &package->id, &package->summary);
      g_ptr_array_add (run->packages, package);
    }
  else if (g_strcmp0 (signal_name, "Packages") == 0)
    {
      g_autoptr (GVariantIter) iter = NULL;
      guint       info              = 0;
      const char *id                = NULL;
      const char *summary           = NULL;

      g_variant_get (parameters, "(a(uss))", &iter);
      while (g_variant_iter_next (iter, "(u&s&s)", &info, &id, &summary))
        {
          RunPackage *package = g_new0 (RunPackage, 1);

          package->info    = info;
          package->id      = g_strdup (id);
          package->summary = g_strdup (summary);
          g_ptr_array_add (run->packages, package);
        }
    }
  else if (g_strcmp0 (signal_name, "Details") == 0)
    {
      if (run->details == NULL)
        g_variant_get (parameters, "(@a{sv})", &run->details);
    }
  else if (g_strcmp0 (signal_name, "Files") == 0)
    {
      if (run->files == NULL)
        g_variant_get (parameters, "(&s^as)", NULL, &run->files);
    }
  else if (g_strcmp0 (signal_name, "ErrorCode") == 0)
    {
      const char *details = NULL;

      g_variant_get (parameters, "(u&s)", &run->error_code, &details);
      if (run->error == NULL)
        run->error = g_strstrip (g_strdup (details));
    }
  else if (g_strcmp0 (signal_name, "RequireRestart") == 0)
    {
      guint restart = 0;

      g_variant_get (parameters, "(u&s)", &restart, NULL);
      run->restart = MAX (run->restart, restart);
    }
  else if (g_strcmp0 (signal_name, "Finished") == 0)
    {
      g_variant_get (parameters, "(uu)", &run->exit, NULL);
      run_finish (run);
    }
  else if (g_strcmp0 (signal_name, "Destroy") == 0)
    run_fail (run, _ ("The package manager stopped unexpectedly"));
}

static void
run_props_cb (GDBusConnection *connection,
              const char      *sender_name,
              const char      *object_path,
              const char      *interface_name,
              const char      *signal_name,
              GVariant        *parameters,
              gpointer         user_data)
{
  Run            *run        = user_data;
  BzDebInstaller *self       = run->self;
  const char     *iface      = NULL;
  g_autoptr (GVariant) dict  = NULL;
  guint           percentage = 0;
  guint           status     = 0;
  gboolean        allow      = FALSE;

  if (run->finished || !run->interactive)
    return;

  g_variant_get (parameters, "(&s@a{sv}@as)", &iface, &dict, NULL);
  if (g_strcmp0 (iface, PK_IFACE_TXN) != 0)
    return;

  if (g_variant_lookup (dict, "Percentage", "u", &percentage))
    self->progress = percentage >= PK_PERCENTAGE_UNKNOWN ? -1.0 : percentage / 100.0;
  if (g_variant_lookup (dict, "Status", "u", &status) && status_to_string (status) != NULL)
    {
      g_free (self->status);
      self->status = g_strdup (status_to_string (status));
    }
  if (g_variant_lookup (dict, "AllowCancel", "b", &allow))
    self->can_cancel = allow;

  changed (self);
}

static void
run_method_cb (GObject      *source,
               GAsyncResult *res,
               gpointer      user_data)
{
  Run *run                       = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    run_fail_with_error (run, local_error);

  run_unref (run);
}

static void
run_hints_cb (GObject      *source,
              GAsyncResult *res,
              gpointer      user_data)
{
  Run *run                       = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    g_debug ("Could not set transaction hints: %s", local_error->message);

  if (!run->finished)
    g_dbus_connection_call (
        run->self->bus, PK_NAME, run->path, PK_IFACE_TXN, run->method,
        run->params, NULL,
        run->interactive
            ? G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION
            : G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, run_method_cb, run_ref (run));

  run_unref (run);
}

static void
run_created_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  Run                *run        = user_data;
  BzDebInstaller     *self       = run->self;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;
  GVariantBuilder     hints      = { 0 };
  const char *const  *languages  = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    {
      run_fail_with_error (run, local_error);
      run_unref (run);
      return;
    }

  g_variant_get (ret, "(o)", &run->path);

  run->signals_id = g_dbus_connection_signal_subscribe (
      self->bus, NULL, PK_IFACE_TXN, NULL, run->path, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, run_signal_cb, run, NULL);
  run->props_id = g_dbus_connection_signal_subscribe (
      self->bus, NULL, DBUS_IFACE_PROPS, "PropertiesChanged", run->path,
      PK_IFACE_TXN, G_DBUS_SIGNAL_FLAGS_NONE, run_props_cb, run, NULL);

  languages = g_get_language_names ();

  g_variant_builder_init (&hints, G_VARIANT_TYPE ("as"));
  g_variant_builder_add (&hints, "s", "interactive=true");
  g_variant_builder_add (&hints, "s", "background=false");
  if (languages != NULL && languages[0] != NULL)
    {
      g_autofree char *locale = g_strdup_printf ("locale=%s", languages[0]);

      g_variant_builder_add (&hints, "s", locale);
    }

  g_dbus_connection_call (
      self->bus, PK_NAME, run->path, PK_IFACE_TXN, "SetHints",
      g_variant_new ("(as)", &hints), NULL, G_DBUS_CALL_FLAGS_NONE,
      -1, NULL, run_hints_cb, run_ref (run));

  run_unref (run);
}

static void
start_run (BzDebInstaller *self,
           const char     *method,
           GVariant       *params,
           gboolean        interactive,
           RunDone         done)
{
  Run *run = NULL;

  g_assert (self->run == NULL);

  run              = g_new0 (Run, 1);
  run->ref_count   = 1;
  run->self        = g_object_ref (self);
  run->method      = g_strdup (method);
  run->params      = g_variant_ref_sink (params);
  run->interactive = interactive;
  run->done        = done;
  run->packages    = g_ptr_array_new_with_free_func ((GDestroyNotify) run_package_free);

  /* self->run holds the initial reference */
  self->run = run;

  g_dbus_connection_call (
      self->bus, PK_NAME, PK_PATH, PK_IFACE, "CreateTransaction",
      NULL, G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE,
      -1, NULL, run_created_cb, run_ref (run));
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

/* PackageKit runs as root and reads the file itself, so it must be a path
 * on a local disk. Anything else (a network share, or a GVfs FUSE path
 * root cannot read) is copied to the user's cache first. */
static char *
dup_local_path (GFile   *file,
                GError **error)
{
  g_autofree char *basename = NULL;
  g_autofree char *dir      = NULL;
  g_autofree char *dest     = NULL;
  g_autoptr (GFile) copy    = NULL;
  g_autoptr (GFileInfo) fs  = NULL;
  const char *fs_type       = NULL;

  if (g_file_is_native (file))
    {
      fs = g_file_query_filesystem_info (file, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, NULL, NULL);
      fs_type = fs != NULL ? g_file_info_get_attribute_string (fs, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE) : NULL;
      if (fs_type == NULL || !g_str_has_prefix (fs_type, "fuse"))
        return g_file_get_path (file);
    }

  basename = g_file_get_basename (file);
  dir      = g_build_filename (g_get_user_cache_dir (), g_get_prgname (), "deb-staging", NULL);
  if (g_mkdir_with_parents (dir, 0755) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "%s: %s", dir, g_strerror (errno));
      return NULL;
    }
  dest = g_build_filename (dir, basename, NULL);
  copy = g_file_new_for_path (dest);

  if (!g_file_copy (file, copy, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS,
                    NULL, NULL, NULL, error))
    return NULL;

  return g_steal_pointer (&dest);
}

BzDebInstaller *
bz_deb_installer_new (GFile *file)
{
  BzDebInstaller *self           = NULL;
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (G_IS_FILE (file), NULL);

  self            = g_object_new (BZ_TYPE_DEB_INSTALLER, NULL);
  self->file_name = g_file_get_basename (file);
  self->path      = dup_local_path (file, &local_error);

  if (self->path == NULL)
    {
      self->state = BZ_DEB_STATE_FAILED;
      set_error (self, local_error->message, NULL);
      return self;
    }

  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, bus_ready_cb, g_object_ref (self));
  return self;
}

BzDebState
bz_deb_installer_get_state (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), BZ_DEB_STATE_FAILED);
  return self->state;
}

BzDebAction
bz_deb_installer_get_action (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), BZ_DEB_ACTION_NONE);
  return self->action;
}

const char *
bz_deb_installer_get_file_name (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->file_name;
}

const char *
bz_deb_installer_get_name (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->name;
}

const char *
bz_deb_installer_get_version (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->version;
}

const char *
bz_deb_installer_get_summary (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->summary;
}

const char *
bz_deb_installer_get_description (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->description;
}

const char *
bz_deb_installer_get_license (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->license;
}

const char *
bz_deb_installer_get_url (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->url;
}

guint64
bz_deb_installer_get_installed_size (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), 0);
  return self->installed_size;
}

const char *
bz_deb_installer_get_installed_version (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->installed_version;
}

const char *
bz_deb_installer_get_app_name (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->app_name;
}

GIcon *
bz_deb_installer_get_icon (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->icon;
}

const char *
bz_deb_installer_get_desktop_id (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->desktop_id;
}

GPtrArray *
bz_deb_installer_get_extra_packages (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->extra;
}

GPtrArray *
bz_deb_installer_get_removed_packages (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->removed;
}

const char *
bz_deb_installer_get_status (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->status;
}

double
bz_deb_installer_get_progress (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), -1.0);
  return self->progress;
}

gboolean
bz_deb_installer_get_can_cancel (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), FALSE);
  return self->can_cancel;
}

const char *
bz_deb_installer_get_error (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->error;
}

const char *
bz_deb_installer_get_error_details (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), NULL);
  return self->error_details;
}

gboolean
bz_deb_installer_get_restart_required (BzDebInstaller *self)
{
  g_return_val_if_fail (BZ_IS_DEB_INSTALLER (self), FALSE);
  return self->restart_required;
}

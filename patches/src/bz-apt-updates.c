/* bz-apt-updates.c
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

/* DOWNSTREAM PATCH (APT system updates): see ai-patches/PLAN.md
 *
 * Everything runs on the main context with plain GDBus async calls; one
 * PackageKit transaction runs at a time and further requests are queued.
 */

#define G_LOG_DOMAIN "BAZAAR::APT"

#include "config.h"

#include <glib/gi18n.h>

#include "bz-apt-updates.h"

#define PK_NAME          "org.freedesktop.PackageKit"
#define PK_PATH          "/org/freedesktop/PackageKit"
#define PK_IFACE         "org.freedesktop.PackageKit"
#define PK_IFACE_TXN     "org.freedesktop.PackageKit.Transaction"
#define PK_IFACE_OFFLINE "org.freedesktop.PackageKit.Offline"
#define DBUS_IFACE_PROPS "org.freedesktop.DBus.Properties"

/* Values from PackageKit's pk-enum.h. These are part of PackageKit's
 * stable D-Bus ABI. Bitfields are (1 << enum value). */
#define PK_FILTER_NONE                    (G_GUINT64_CONSTANT (1) << 1)
#define PK_TRANSACTION_FLAG_ONLY_TRUSTED  (G_GUINT64_CONSTANT (1) << 1)
#define PK_TRANSACTION_FLAG_ONLY_DOWNLOAD (G_GUINT64_CONSTANT (1) << 3)
#define PK_ROLE_REFRESH_CACHE             13
#define PK_INFO_SECURITY                  8
#define PK_INFO_BLOCKED                   9
#define PK_INFO_CRITICAL                  26
#define PK_INFO_INSTALL                   27
#define PK_INFO_REMOVE                    28
#define PK_INFO_OBSOLETE                  29
#define PK_EXIT_SUCCESS                   1
#define PK_EXIT_FAILED                    2
#define PK_EXIT_CANCELLED                 3
#define PK_EXIT_NEED_UNTRUSTED            8
#define PK_EXIT_CANCELLED_PRIORITY        9
#define PK_RESTART_SYSTEM                 4
#define PK_RESTART_SECURITY_SYSTEM        6
#define PK_PERCENTAGE_UNKNOWN             101

/* Download fresh package lists automatically when they are older than this */
#define AUTO_REFRESH_SECONDS (60 * 60 * 24)

/* ------------------------------------------------------------------------ */
/* BzAptPackage                                                             */
/* ------------------------------------------------------------------------ */

struct _BzAptPackage
{
  GObject parent_instance;

  char *id;
  char *name;
  char *version;
  char *summary;
  guint info;
};

G_DEFINE_FINAL_TYPE (BzAptPackage, bz_apt_package, G_TYPE_OBJECT)

static void
bz_apt_package_finalize (GObject *object)
{
  BzAptPackage *self = BZ_APT_PACKAGE (object);

  g_clear_pointer (&self->id, g_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->version, g_free);
  g_clear_pointer (&self->summary, g_free);

  G_OBJECT_CLASS (bz_apt_package_parent_class)->finalize (object);
}

static void
bz_apt_package_class_init (BzAptPackageClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bz_apt_package_finalize;
}

static void
bz_apt_package_init (BzAptPackage *self)
{
}

static BzAptPackage *
bz_apt_package_new (guint       info,
                    const char *package_id,
                    const char *summary)
{
  BzAptPackage *self       = NULL;
  g_auto (GStrv) parts     = NULL;

  /* package_id is "name;version;arch;data" */
  parts = g_strsplit (package_id, ";", 4);

  self          = g_object_new (BZ_TYPE_APT_PACKAGE, NULL);
  self->id      = g_strdup (package_id);
  self->name    = g_strdup (parts[0] != NULL ? parts[0] : package_id);
  self->version = g_strdup (parts[0] != NULL && parts[1] != NULL ? parts[1] : "");
  self->summary = g_strdup (summary != NULL ? summary : "");
  self->info    = info;

  return self;
}

const char *
bz_apt_package_get_id (BzAptPackage *self)
{
  g_return_val_if_fail (BZ_IS_APT_PACKAGE (self), NULL);
  return self->id;
}

const char *
bz_apt_package_get_name (BzAptPackage *self)
{
  g_return_val_if_fail (BZ_IS_APT_PACKAGE (self), NULL);
  return self->name;
}

const char *
bz_apt_package_get_version (BzAptPackage *self)
{
  g_return_val_if_fail (BZ_IS_APT_PACKAGE (self), NULL);
  return self->version;
}

const char *
bz_apt_package_get_summary (BzAptPackage *self)
{
  g_return_val_if_fail (BZ_IS_APT_PACKAGE (self), NULL);
  return self->summary;
}

gboolean
bz_apt_package_is_security (BzAptPackage *self)
{
  g_return_val_if_fail (BZ_IS_APT_PACKAGE (self), FALSE);
  return self->info == PK_INFO_SECURITY || self->info == PK_INFO_CRITICAL;
}

gboolean
bz_apt_package_is_new (BzAptPackage *self)
{
  g_return_val_if_fail (BZ_IS_APT_PACKAGE (self), FALSE);
  return self->info == PK_INFO_INSTALL;
}

gboolean
bz_apt_package_is_removal (BzAptPackage *self)
{
  g_return_val_if_fail (BZ_IS_APT_PACKAGE (self), FALSE);
  return self->info == PK_INFO_REMOVE || self->info == PK_INFO_OBSOLETE;
}

/* ------------------------------------------------------------------------ */
/* BzAptUpdates                                                             */
/* ------------------------------------------------------------------------ */

typedef enum
{
  TXN_GET_UPDATES,
  TXN_REFRESH_CACHE,
  TXN_INSTALL_NOW,
  TXN_DOWNLOAD,
} TxnKind;

typedef struct
{
  TxnKind  kind;
  gboolean background;
} Request;

typedef struct
{
  gint          ref_count;
  BzAptUpdates *self;
  TxnKind       kind;
  gboolean      background;
  gboolean      finished;
  char         *path;
  guint         signals_id;
  guint         props_id;
  GHashTable   *seen;
  GPtrArray    *found;
  guint         exit;
  guint         restart;
  char         *error;
} Txn;

struct _BzAptUpdates
{
  GObject parent_instance;

  GDBusConnection *bus;
  gboolean         available;
  gboolean         loaded;
  char            *backend;
  GListStore      *packages;
  guint            n_security;
  guint            n_upgrades;
  guint            n_new;
  guint            n_removals;
  char            *status;
  double           progress;
  gboolean         can_cancel;
  char            *error;
  gboolean         offline_triggered;
  gboolean         restart_required;

  guint updates_changed_id;
  guint offline_props_id;

  Txn    *txn;
  GQueue *queue;
};

G_DEFINE_FINAL_TYPE (BzAptUpdates, bz_apt_updates, G_TYPE_OBJECT)

enum
{
  PROP_0,

  PROP_AVAILABLE,
  PROP_LOADED,
  PROP_BACKEND,
  PROP_PACKAGES,
  PROP_N_SECURITY,
  PROP_N_UPGRADES,
  PROP_N_NEW,
  PROP_N_REMOVALS,
  PROP_BUSY,
  PROP_STATUS,
  PROP_PROGRESS,
  PROP_CAN_CANCEL,
  PROP_ERROR,
  PROP_OFFLINE_TRIGGERED,
  PROP_RESTART_REQUIRED,

  LAST_PROP
};
static GParamSpec *props[LAST_PROP] = { 0 };

static void
bus_ready_cb (GObject      *source,
              GAsyncResult *res,
              gpointer      user_data);

static void
enqueue (BzAptUpdates *self,
         TxnKind       kind,
         gboolean      background);

static void
run_next (BzAptUpdates *self);

static void
set_error (BzAptUpdates *self,
           const char   *error);

static void
set_loaded (BzAptUpdates *self);

static void
set_status (BzAptUpdates *self,
            const char   *status,
            double        progress);

static Txn *
txn_ref (Txn *txn);

static void
txn_unref (Txn *txn);

static void
txn_finish (Txn *txn);

static void
bz_apt_updates_dispose (GObject *object)
{
  BzAptUpdates *self = BZ_APT_UPDATES (object);

  if (self->bus != NULL)
    {
      if (self->updates_changed_id != 0)
        g_dbus_connection_signal_unsubscribe (self->bus, self->updates_changed_id);
      if (self->offline_props_id != 0)
        g_dbus_connection_signal_unsubscribe (self->bus, self->offline_props_id);
      self->updates_changed_id = 0;
      self->offline_props_id   = 0;
    }

  if (self->queue != NULL)
    g_queue_free_full (g_steal_pointer (&self->queue), g_free);

  g_clear_object (&self->bus);
  g_clear_object (&self->packages);
  g_clear_pointer (&self->backend, g_free);
  g_clear_pointer (&self->status, g_free);
  g_clear_pointer (&self->error, g_free);

  G_OBJECT_CLASS (bz_apt_updates_parent_class)->dispose (object);
}

static void
bz_apt_updates_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  BzAptUpdates *self = BZ_APT_UPDATES (object);

  switch (prop_id)
    {
    case PROP_AVAILABLE:
      g_value_set_boolean (value, self->available);
      break;
    case PROP_LOADED:
      g_value_set_boolean (value, self->loaded);
      break;
    case PROP_BACKEND:
      g_value_set_string (value, self->backend);
      break;
    case PROP_PACKAGES:
      g_value_set_object (value, self->packages);
      break;
    case PROP_N_SECURITY:
      g_value_set_uint (value, self->n_security);
      break;
    case PROP_N_UPGRADES:
      g_value_set_uint (value, self->n_upgrades);
      break;
    case PROP_N_NEW:
      g_value_set_uint (value, self->n_new);
      break;
    case PROP_N_REMOVALS:
      g_value_set_uint (value, self->n_removals);
      break;
    case PROP_BUSY:
      g_value_set_boolean (value, bz_apt_updates_get_busy (self));
      break;
    case PROP_STATUS:
      g_value_set_string (value, self->status);
      break;
    case PROP_PROGRESS:
      g_value_set_double (value, self->progress);
      break;
    case PROP_CAN_CANCEL:
      g_value_set_boolean (value, self->can_cancel);
      break;
    case PROP_ERROR:
      g_value_set_string (value, self->error);
      break;
    case PROP_OFFLINE_TRIGGERED:
      g_value_set_boolean (value, self->offline_triggered);
      break;
    case PROP_RESTART_REQUIRED:
      g_value_set_boolean (value, self->restart_required);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
bz_apt_updates_class_init (BzAptUpdatesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose      = bz_apt_updates_dispose;
  object_class->get_property = bz_apt_updates_get_property;

  props[PROP_AVAILABLE] =
      g_param_spec_boolean ("available", NULL, NULL, FALSE,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_LOADED] =
      g_param_spec_boolean ("loaded", NULL, NULL, FALSE,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_BACKEND] =
      g_param_spec_string ("backend", NULL, NULL, NULL,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_PACKAGES] =
      g_param_spec_object ("packages", NULL, NULL, G_TYPE_LIST_MODEL,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_N_SECURITY] =
      g_param_spec_uint ("n-security", NULL, NULL, 0, G_MAXUINT, 0,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_N_UPGRADES] =
      g_param_spec_uint ("n-upgrades", NULL, NULL, 0, G_MAXUINT, 0,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_N_NEW] =
      g_param_spec_uint ("n-new", NULL, NULL, 0, G_MAXUINT, 0,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_N_REMOVALS] =
      g_param_spec_uint ("n-removals", NULL, NULL, 0, G_MAXUINT, 0,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_BUSY] =
      g_param_spec_boolean ("busy", NULL, NULL, FALSE,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_STATUS] =
      g_param_spec_string ("status", NULL, NULL, NULL,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_PROGRESS] =
      g_param_spec_double ("progress", NULL, NULL, -1.0, 1.0, -1.0,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_CAN_CANCEL] =
      g_param_spec_boolean ("can-cancel", NULL, NULL, FALSE,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_ERROR] =
      g_param_spec_string ("error", NULL, NULL, NULL,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_OFFLINE_TRIGGERED] =
      g_param_spec_boolean ("offline-triggered", NULL, NULL, FALSE,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_RESTART_REQUIRED] =
      g_param_spec_boolean ("restart-required", NULL, NULL, FALSE,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, LAST_PROP, props);
}

static void
bz_apt_updates_init (BzAptUpdates *self)
{
  self->packages = g_list_store_new (BZ_TYPE_APT_PACKAGE);
  self->queue    = g_queue_new ();
  self->progress = -1.0;

  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, bus_ready_cb, g_object_ref (self));
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

BzAptUpdates *
bz_apt_updates_get_default (void)
{
  static BzAptUpdates *instance = NULL;

  if (instance == NULL)
    instance = g_object_new (BZ_TYPE_APT_UPDATES, NULL);

  return instance;
}

gboolean
bz_apt_updates_get_available (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), FALSE);
  return self->available;
}

gboolean
bz_apt_updates_get_loaded (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), FALSE);
  return self->loaded;
}

const char *
bz_apt_updates_get_backend (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), NULL);
  return self->backend;
}

GListModel *
bz_apt_updates_get_packages (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), NULL);
  return G_LIST_MODEL (self->packages);
}

guint
bz_apt_updates_get_n_security (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), 0);
  return self->n_security;
}

guint
bz_apt_updates_get_n_upgrades (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), 0);
  return self->n_upgrades;
}

guint
bz_apt_updates_get_n_new (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), 0);
  return self->n_new;
}

guint
bz_apt_updates_get_n_removals (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), 0);
  return self->n_removals;
}

gboolean
bz_apt_updates_get_busy (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), FALSE);
  return self->txn != NULL && !self->txn->background;
}

const char *
bz_apt_updates_get_status (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), NULL);
  return self->status;
}

double
bz_apt_updates_get_progress (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), -1.0);
  return self->progress;
}

gboolean
bz_apt_updates_get_can_cancel (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), FALSE);
  return self->can_cancel;
}

const char *
bz_apt_updates_get_error (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), NULL);
  return self->error;
}

void
bz_apt_updates_clear_error (BzAptUpdates *self)
{
  g_return_if_fail (BZ_IS_APT_UPDATES (self));
  set_error (self, NULL);
}

gboolean
bz_apt_updates_get_offline_triggered (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), FALSE);
  return self->offline_triggered;
}

gboolean
bz_apt_updates_get_restart_required (BzAptUpdates *self)
{
  g_return_val_if_fail (BZ_IS_APT_UPDATES (self), FALSE);
  return self->restart_required;
}

void
bz_apt_updates_check (BzAptUpdates *self,
                      gboolean      refresh_cache)
{
  g_return_if_fail (BZ_IS_APT_UPDATES (self));

  if (!self->available)
    return;

  set_error (self, NULL);
  enqueue (self, refresh_cache ? TXN_REFRESH_CACHE : TXN_GET_UPDATES, FALSE);
}

void
bz_apt_updates_prepare_offline (BzAptUpdates *self)
{
  g_return_if_fail (BZ_IS_APT_UPDATES (self));

  if (!self->available)
    return;

  set_error (self, NULL);
  enqueue (self, TXN_DOWNLOAD, FALSE);
}

void
bz_apt_updates_install_now (BzAptUpdates *self)
{
  g_return_if_fail (BZ_IS_APT_UPDATES (self));

  if (!self->available)
    return;

  set_error (self, NULL);
  enqueue (self, TXN_INSTALL_NOW, FALSE);
}

void
bz_apt_updates_cancel (BzAptUpdates *self)
{
  g_return_if_fail (BZ_IS_APT_UPDATES (self));

  /* Drop everything that has not started yet */
  while (!g_queue_is_empty (self->queue))
    g_free (g_queue_pop_head (self->queue));

  if (self->bus == NULL || self->txn == NULL || self->txn->path == NULL)
    return;

  g_dbus_connection_call (
      self->bus, PK_NAME, self->txn->path, PK_IFACE_TXN, "Cancel",
      NULL, NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
      -1, NULL, NULL, NULL);
}

static void
cancel_offline_cb (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autoptr (BzAptUpdates) self  = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    {
      set_error (self, local_error->message);
      return;
    }

  self->offline_triggered = FALSE;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OFFLINE_TRIGGERED]);
}

void
bz_apt_updates_cancel_offline (BzAptUpdates *self)
{
  g_return_if_fail (BZ_IS_APT_UPDATES (self));

  if (self->bus == NULL)
    return;

  g_dbus_connection_call (
      self->bus, PK_NAME, PK_PATH, PK_IFACE_OFFLINE, "Cancel",
      NULL, NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
      -1, NULL, cancel_offline_cb, g_object_ref (self));
}

static void
logind_reboot_cb (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  g_autoptr (BzAptUpdates) self  = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    set_error (self, local_error->message);
}

static void
logind_reboot (BzAptUpdates *self)
{
  if (self->bus == NULL)
    return;

  g_dbus_connection_call (
      self->bus, "org.freedesktop.login1", "/org/freedesktop/login1",
      "org.freedesktop.login1.Manager", "Reboot",
      g_variant_new ("(b)", TRUE), NULL,
      G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
      -1, NULL, logind_reboot_cb, g_object_ref (self));
}

static void
session_reboot_cb (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autoptr (BzAptUpdates) self  = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    {
      g_debug ("Session manager reboot failed (%s), falling back to logind",
               local_error->message);
      logind_reboot (self);
    }
}

void
bz_apt_updates_reboot (BzAptUpdates *self)
{
  g_autoptr (GDBusConnection) session = NULL;

  g_return_if_fail (BZ_IS_APT_UPDATES (self));

  /* Prefer the desktop session: GNOME shows its own confirmation dialog,
   * which also knows about the prepared offline update. */
  session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session == NULL)
    {
      logind_reboot (self);
      return;
    }

  g_dbus_connection_call (
      session, "org.gnome.SessionManager", "/org/gnome/SessionManager",
      "org.gnome.SessionManager", "Reboot",
      NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
      -1, NULL, session_reboot_cb, g_object_ref (self));
}

/* ------------------------------------------------------------------------ */
/* Initialization                                                           */
/* ------------------------------------------------------------------------ */

static void
updates_changed_cb (GDBusConnection *connection,
                    const char      *sender_name,
                    const char      *object_path,
                    const char      *interface_name,
                    const char      *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  BzAptUpdates *self = user_data;

  g_debug ("PackageKit reported that updates changed");
  enqueue (self, TXN_GET_UPDATES, TRUE);
}

static void
apply_offline_props (BzAptUpdates *self,
                     GVariant     *dict)
{
  gboolean triggered = FALSE;

  if (!g_variant_lookup (dict, "UpdateTriggered", "b", &triggered))
    return;

  if (triggered != self->offline_triggered)
    {
      self->offline_triggered = triggered;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OFFLINE_TRIGGERED]);
    }
}

static void
offline_props_changed_cb (GDBusConnection *connection,
                          const char      *sender_name,
                          const char      *object_path,
                          const char      *interface_name,
                          const char      *signal_name,
                          GVariant        *parameters,
                          gpointer         user_data)
{
  BzAptUpdates *self          = user_data;
  const char   *iface         = NULL;
  g_autoptr (GVariant) dict   = NULL;

  g_variant_get (parameters, "(&s@a{sv}@as)", &iface, &dict, NULL);
  if (g_strcmp0 (iface, PK_IFACE_OFFLINE) == 0)
    apply_offline_props (self, dict);
}

static void
offline_get_all_cb (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  g_autoptr (BzAptUpdates) self  = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GVariant) dict      = NULL;
  g_autoptr (GError) local_error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    {
      g_debug ("Could not read PackageKit offline state: %s", local_error->message);
      return;
    }

  g_variant_get (ret, "(@a{sv})", &dict);
  apply_offline_props (self, dict);
}

static void
time_since_refresh_cb (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr (BzAptUpdates) self  = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;
  guint seconds                  = G_MAXUINT;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret != NULL)
    g_variant_get (ret, "(u)", &seconds);

  g_debug ("Package lists were refreshed %u seconds ago", seconds);

  /* A background refresh-cache is followed by get-updates automatically */
  if (seconds > AUTO_REFRESH_SECONDS)
    enqueue (self, TXN_REFRESH_CACHE, TRUE);
  else
    enqueue (self, TXN_GET_UPDATES, TRUE);
}

static void
pk_get_all_cb (GObject      *source,
               GAsyncResult *res,
               gpointer      user_data)
{
  g_autoptr (BzAptUpdates) self  = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GVariant) dict      = NULL;
  g_autoptr (GError) local_error = NULL;
  const char *description        = NULL;
  const char *name               = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    {
      g_info ("PackageKit is not available, system updates are disabled: %s",
              local_error->message);
      set_loaded (self);
      return;
    }

  g_variant_get (ret, "(@a{sv})", &dict);
  g_variant_lookup (dict, "BackendDescription", "&s", &description);
  g_variant_lookup (dict, "BackendName", "&s", &name);

  if (name == NULL || *name == '\0' || g_strcmp0 (name, "dummy") == 0)
    {
      g_info ("PackageKit has no usable backend, system updates are disabled");
      set_loaded (self);
      return;
    }

  g_clear_pointer (&self->backend, g_free);
  /* Shown as "Provided by %s". PackageKit describes its apt backend as just
   * "APT"; name the distribution family the packages come from as well. */
  if (g_strcmp0 (name, "apt") == 0)
    self->backend = g_strdup (_ ("Debian APT"));
  else
    self->backend = g_strdup (description != NULL && *description != '\0' ? description : name);
  self->available = TRUE;

  g_debug ("Using PackageKit backend '%s'", self->backend);

  self->updates_changed_id = g_dbus_connection_signal_subscribe (
      self->bus, NULL, PK_IFACE, "UpdatesChanged", PK_PATH, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, updates_changed_cb, self, NULL);

  self->offline_props_id = g_dbus_connection_signal_subscribe (
      self->bus, NULL, DBUS_IFACE_PROPS, "PropertiesChanged", PK_PATH,
      PK_IFACE_OFFLINE, G_DBUS_SIGNAL_FLAGS_NONE,
      offline_props_changed_cb, self, NULL);

  g_dbus_connection_call (
      self->bus, PK_NAME, PK_PATH, DBUS_IFACE_PROPS, "GetAll",
      g_variant_new ("(s)", PK_IFACE_OFFLINE), G_VARIANT_TYPE ("(a{sv})"),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, offline_get_all_cb, g_object_ref (self));

  g_dbus_connection_call (
      self->bus, PK_NAME, PK_PATH, PK_IFACE, "GetTimeSinceAction",
      g_variant_new ("(u)", PK_ROLE_REFRESH_CACHE), G_VARIANT_TYPE ("(u)"),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, time_since_refresh_cb, g_object_ref (self));

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BACKEND]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AVAILABLE]);
}

static void
bus_ready_cb (GObject      *source,
              GAsyncResult *res,
              gpointer      user_data)
{
  g_autoptr (BzAptUpdates) self  = user_data;
  g_autoptr (GError) local_error = NULL;

  self->bus = g_bus_get_finish (res, &local_error);
  if (self->bus == NULL)
    {
      g_info ("No system bus, system updates are disabled: %s", local_error->message);
      set_loaded (self);
      return;
    }

  /* Reading the properties also D-Bus-activates the daemon */
  g_dbus_connection_call (
      self->bus, PK_NAME, PK_PATH, DBUS_IFACE_PROPS, "GetAll",
      g_variant_new ("(s)", PK_IFACE), G_VARIANT_TYPE ("(a{sv})"),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, pk_get_all_cb, g_object_ref (self));
}

/* ------------------------------------------------------------------------ */
/* State helpers                                                            */
/* ------------------------------------------------------------------------ */

static void
set_loaded (BzAptUpdates *self)
{
  if (self->loaded)
    return;
  self->loaded = TRUE;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LOADED]);
}

static void
set_error (BzAptUpdates *self,
           const char   *error)
{
  if (g_strcmp0 (self->error, error) == 0)
    return;

  g_clear_pointer (&self->error, g_free);
  self->error = g_strdup (error);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ERROR]);
}

static void
set_status (BzAptUpdates *self,
            const char   *status,
            double        progress)
{
  if (g_strcmp0 (self->status, status) != 0)
    {
      g_clear_pointer (&self->status, g_free);
      self->status = g_strdup (status);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_STATUS]);
    }

  if (self->progress != progress)
    {
      self->progress = progress;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PROGRESS]);
    }
}

static void
set_can_cancel (BzAptUpdates *self,
                gboolean      can_cancel)
{
  if (self->can_cancel == can_cancel)
    return;

  self->can_cancel = can_cancel;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CAN_CANCEL]);
}

static const char *
status_to_string (guint status)
{
  /* PkStatusEnum */
  switch (status)
    {
    case 1: return _ ("Waiting in queue");
    case 2: return _ ("Starting");
    case 3: return _ ("Running");
    case 4: return _ ("Querying");
    case 5: return _ ("Getting information");
    case 7: return _ ("Refreshing package lists");
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
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25: return _ ("Downloading package lists");
    case 27: return _ ("Loading cache");
    case 30: return _ ("Waiting for package manager lock");
    case 31: return _ ("Waiting for authentication");
    case 36: return _ ("Running hooks");
    default: return NULL;
    }
}

/* Sort rank: removals first (the user should notice them), then security
 * updates, other updates, and new packages last */
static int
package_rank (BzAptPackage *package)
{
  if (bz_apt_package_is_removal (package))
    return 0;
  if (bz_apt_package_is_security (package))
    return 1;
  if (bz_apt_package_is_new (package))
    return 3;
  return 2;
}

static int
cmp_packages (gconstpointer a,
              gconstpointer b)
{
  BzAptPackage *pa = *(BzAptPackage **) a;
  BzAptPackage *pb = *(BzAptPackage **) b;
  int           ra = package_rank (pa);
  int           rb = package_rank (pb);

  if (ra != rb)
    return ra - rb;

  return g_strcmp0 (pa->name, pb->name);
}

static void
set_count (BzAptUpdates *self,
           guint        *field,
           guint         value,
           guint         prop)
{
  if (*field == value)
    return;
  *field = value;
  g_object_notify_by_pspec (G_OBJECT (self), props[prop]);
}

static GVariant *
dup_package_ids (BzAptUpdates *self)
{
  GVariantBuilder builder = { 0 };
  guint           n_items = 0;

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->packages));
  if (n_items == 0)
    return NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (BzAptPackage) package = NULL;

      package = g_list_model_get_item (G_LIST_MODEL (self->packages), i);
      g_variant_builder_add (&builder, "s", package->id);
    }

  return g_variant_builder_end (&builder);
}

/* ------------------------------------------------------------------------ */
/* Transactions                                                             */
/* ------------------------------------------------------------------------ */

static void
enqueue (BzAptUpdates *self,
         TxnKind       kind,
         gboolean      background)
{
  Request *request = NULL;

  for (GList *l = self->queue->head; l != NULL; l = l->next)
    {
      Request *queued = l->data;

      if (queued->kind == kind)
        {
          /* Already queued: just promote it if the user asked for it */
          queued->background = queued->background && background;
          return;
        }
    }

  request             = g_new0 (Request, 1);
  request->kind       = kind;
  request->background = background;
  g_queue_push_tail (self->queue, request);

  if (self->txn == NULL)
    run_next (self);
}

static Txn *
txn_ref (Txn *txn)
{
  g_atomic_int_inc (&txn->ref_count);
  return txn;
}

static void
txn_unref (Txn *txn)
{
  if (!g_atomic_int_dec_and_test (&txn->ref_count))
    return;

  g_clear_pointer (&txn->path, g_free);
  g_clear_pointer (&txn->seen, g_hash_table_unref);
  g_clear_pointer (&txn->found, g_ptr_array_unref);
  g_clear_pointer (&txn->error, g_free);
  g_clear_object (&txn->self);
  g_free (txn);
}

static void
txn_fail (Txn        *txn,
          const char *message)
{
  if (txn->finished)
    return;

  if (txn->error == NULL)
    txn->error = g_strdup (message);
  txn->exit = PK_EXIT_FAILED;
  txn_finish (txn);
}

static void
txn_add_package (Txn        *txn,
                 guint       info,
                 const char *package_id,
                 const char *summary)
{
  if (txn->kind != TXN_GET_UPDATES)
    return;
  if (info == PK_INFO_BLOCKED)
    return;
  /* PackageKit may send both "Package" and "Packages" */
  if (!g_hash_table_add (txn->seen, g_strdup (package_id)))
    return;

  g_ptr_array_add (txn->found, bz_apt_package_new (info, package_id, summary));
}

static void
txn_signal_cb (GDBusConnection *connection,
               const char      *sender_name,
               const char      *object_path,
               const char      *interface_name,
               const char      *signal_name,
               GVariant        *parameters,
               gpointer         user_data)
{
  Txn *txn = user_data;

  if (txn->finished)
    return;

  if (g_strcmp0 (signal_name, "Package") == 0)
    {
      guint       info       = 0;
      const char *package_id = NULL;
      const char *summary    = NULL;

      g_variant_get (parameters, "(u&s&s)", &info, &package_id, &summary);
      txn_add_package (txn, info, package_id, summary);
    }
  else if (g_strcmp0 (signal_name, "Packages") == 0)
    {
      g_autoptr (GVariantIter) iter = NULL;
      guint       info              = 0;
      const char *package_id        = NULL;
      const char *summary           = NULL;

      g_variant_get (parameters, "(a(uss))", &iter);
      while (g_variant_iter_next (iter, "(u&s&s)", &info, &package_id, &summary))
        txn_add_package (txn, info, package_id, summary);
    }
  else if (g_strcmp0 (signal_name, "ErrorCode") == 0)
    {
      guint       code    = 0;
      const char *details = NULL;

      g_variant_get (parameters, "(u&s)", &code, &details);
      g_debug ("PackageKit error %u: %s", code, details);
      if (txn->error == NULL)
        txn->error = g_strdup (details);
    }
  else if (g_strcmp0 (signal_name, "RequireRestart") == 0)
    {
      guint       restart    = 0;
      const char *package_id = NULL;

      g_variant_get (parameters, "(u&s)", &restart, &package_id);
      txn->restart = MAX (txn->restart, restart);
    }
  else if (g_strcmp0 (signal_name, "Finished") == 0)
    {
      guint exit    = 0;
      guint runtime = 0;

      g_variant_get (parameters, "(uu)", &exit, &runtime);
      txn->exit = exit;
      txn_finish (txn);
    }
  else if (g_strcmp0 (signal_name, "Destroy") == 0)
    txn_fail (txn, _ ("The package manager stopped unexpectedly"));
}

static void
txn_props_cb (GDBusConnection *connection,
              const char      *sender_name,
              const char      *object_path,
              const char      *interface_name,
              const char      *signal_name,
              GVariant        *parameters,
              gpointer         user_data)
{
  Txn               *txn         = user_data;
  BzAptUpdates      *self        = txn->self;
  const char        *iface       = NULL;
  g_autoptr (GVariant) dict      = NULL;
  guint              percentage  = 0;
  guint              status      = 0;
  gboolean           allow       = FALSE;
  const char        *status_text = NULL;
  double             progress    = 0.0;

  if (txn->finished || self->txn != txn)
    return;

  g_variant_get (parameters, "(&s@a{sv}@as)", &iface, &dict, NULL);
  if (g_strcmp0 (iface, PK_IFACE_TXN) != 0)
    return;

  progress    = self->progress;
  status_text = self->status;

  if (g_variant_lookup (dict, "Percentage", "u", &percentage))
    progress = percentage >= PK_PERCENTAGE_UNKNOWN ? -1.0 : percentage / 100.0;
  if (g_variant_lookup (dict, "Status", "u", &status) &&
      status_to_string (status) != NULL)
    status_text = status_to_string (status);
  if (g_variant_lookup (dict, "AllowCancel", "b", &allow))
    set_can_cancel (self, allow);

  /* status_text may point into self->status */
  {
    g_autofree char *copy = g_strdup (status_text);
    set_status (self, copy, progress);
  }
}

static void
txn_method_cb (GObject      *source,
               GAsyncResult *res,
               gpointer      user_data)
{
  Txn *txn                       = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    {
      g_dbus_error_strip_remote_error (local_error);
      txn_fail (txn, local_error->message);
    }

  txn_unref (txn);
}

static void
txn_call_method (Txn *txn)
{
  BzAptUpdates *self      = txn->self;
  const char   *method    = NULL;
  GVariant     *params    = NULL;
  GVariant     *ids       = NULL;
  guint64       flags     = PK_TRANSACTION_FLAG_ONLY_TRUSTED;

  switch (txn->kind)
    {
    case TXN_GET_UPDATES:
      method = "GetUpdates";
      params = g_variant_new ("(t)", PK_FILTER_NONE);
      break;
    case TXN_REFRESH_CACHE:
      method = "RefreshCache";
      params = g_variant_new ("(b)", FALSE);
      break;
    case TXN_DOWNLOAD:
      flags |= PK_TRANSACTION_FLAG_ONLY_DOWNLOAD;
      /* fall through */
    case TXN_INSTALL_NOW:
      ids = dup_package_ids (self);
      if (ids == NULL)
        {
          txn->exit = PK_EXIT_SUCCESS;
          txn_finish (txn);
          return;
        }
      method = "UpdatePackages";
      params = g_variant_new ("(t@as)", flags, ids);
      break;
    default:
      g_assert_not_reached ();
    }

  g_dbus_connection_call (
      self->bus, PK_NAME, txn->path, PK_IFACE_TXN, method,
      params, NULL,
      txn->background
          ? G_DBUS_CALL_FLAGS_NONE
          : G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
      -1, NULL, txn_method_cb, txn_ref (txn));
}

static void
txn_hints_cb (GObject      *source,
              GAsyncResult *res,
              gpointer      user_data)
{
  Txn *txn                       = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    g_debug ("Could not set transaction hints: %s", local_error->message);

  if (!txn->finished)
    txn_call_method (txn);

  txn_unref (txn);
}

static void
txn_created_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  Txn                *txn         = user_data;
  BzAptUpdates       *self        = txn->self;
  g_autoptr (GVariant) ret        = NULL;
  g_autoptr (GError) local_error  = NULL;
  GVariantBuilder     hints       = { 0 };
  const char *const  *languages   = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    {
      g_dbus_error_strip_remote_error (local_error);
      txn_fail (txn, local_error->message);
      txn_unref (txn);
      return;
    }

  g_variant_get (ret, "(o)", &txn->path);

  txn->signals_id = g_dbus_connection_signal_subscribe (
      self->bus, NULL, PK_IFACE_TXN, NULL, txn->path, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, txn_signal_cb, txn, NULL);
  txn->props_id = g_dbus_connection_signal_subscribe (
      self->bus, NULL, DBUS_IFACE_PROPS, "PropertiesChanged", txn->path,
      PK_IFACE_TXN, G_DBUS_SIGNAL_FLAGS_NONE, txn_props_cb, txn, NULL);

  languages = g_get_language_names ();

  g_variant_builder_init (&hints, G_VARIANT_TYPE ("as"));
  g_variant_builder_add (&hints, "s", txn->background ? "interactive=false" : "interactive=true");
  g_variant_builder_add (&hints, "s", txn->background ? "background=true" : "background=false");
  if (languages != NULL && languages[0] != NULL)
    {
      g_autofree char *locale = NULL;

      locale = g_strdup_printf ("locale=%s", languages[0]);
      g_variant_builder_add (&hints, "s", locale);
    }

  g_dbus_connection_call (
      self->bus, PK_NAME, txn->path, PK_IFACE_TXN, "SetHints",
      g_variant_new ("(as)", &hints), NULL, G_DBUS_CALL_FLAGS_NONE,
      -1, NULL, txn_hints_cb, txn_ref (txn));

  txn_unref (txn);
}

static void
run_next (BzAptUpdates *self)
{
  g_autofree Request *request = NULL;
  Txn                *txn     = NULL;

  if (self->txn != NULL || self->bus == NULL)
    return;

  request = g_queue_pop_head (self->queue);
  if (request == NULL)
    return;

  txn             = g_new0 (Txn, 1);
  txn->ref_count  = 1;
  txn->self       = g_object_ref (self);
  txn->kind       = request->kind;
  txn->background = request->background;
  txn->seen       = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  txn->found      = g_ptr_array_new_with_free_func (g_object_unref);

  /* self->txn holds the initial reference */
  self->txn = txn;

  set_can_cancel (self, FALSE);
  if (!txn->background)
    set_status (self, _ ("Starting"), -1.0);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BUSY]);

  g_dbus_connection_call (
      self->bus, PK_NAME, PK_PATH, PK_IFACE, "CreateTransaction",
      NULL, G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE,
      -1, NULL, txn_created_cb, txn_ref (txn));
}

static void
trigger_cb (GObject      *source,
            GAsyncResult *res,
            gpointer      user_data)
{
  g_autoptr (BzAptUpdates) self  = user_data;
  g_autoptr (GVariant) ret       = NULL;
  g_autoptr (GError) local_error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &local_error);
  if (ret == NULL)
    {
      g_dbus_error_strip_remote_error (local_error);
      set_error (self, local_error->message);
      return;
    }

  if (!self->offline_triggered)
    {
      self->offline_triggered = TRUE;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OFFLINE_TRIGGERED]);
    }
}

static void
txn_finish (Txn *txn)
{
  BzAptUpdates *self      = txn->self;
  gboolean      success   = FALSE;
  gboolean      cancelled = FALSE;
  const char   *message   = NULL;

  if (txn->finished)
    return;
  txn->finished = TRUE;

  if (txn->signals_id != 0)
    g_dbus_connection_signal_unsubscribe (self->bus, txn->signals_id);
  if (txn->props_id != 0)
    g_dbus_connection_signal_unsubscribe (self->bus, txn->props_id);
  txn->signals_id = 0;
  txn->props_id   = 0;

  success   = txn->exit == PK_EXIT_SUCCESS;
  cancelled = txn->exit == PK_EXIT_CANCELLED || txn->exit == PK_EXIT_CANCELLED_PRIORITY;

  if (!success && !cancelled)
    {
      if (txn->exit == PK_EXIT_NEED_UNTRUSTED)
        message = _ ("Some updates come from an untrusted source and were not installed");
      else if (txn->error != NULL && *txn->error != '\0')
        message = txn->error;
      else
        message = _ ("The package manager reported an unknown error");

      g_warning ("PackageKit transaction failed (exit %u): %s", txn->exit, message);
    }

  if (self->txn == txn)
    self->txn = NULL;

  set_can_cancel (self, FALSE);
  set_status (self, NULL, -1.0);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BUSY]);

  switch (txn->kind)
    {
    case TXN_GET_UPDATES:
      if (success)
        {
          guint n_old      = 0;
          guint n_security = 0;
          guint n_upgrades = 0;
          guint n_new      = 0;
          guint n_removals = 0;

          g_ptr_array_sort (txn->found, cmp_packages);
          for (guint i = 0; i < txn->found->len; i++)
            {
              BzAptPackage *package = g_ptr_array_index (txn->found, i);

              if (bz_apt_package_is_removal (package))
                n_removals++;
              else if (bz_apt_package_is_new (package))
                n_new++;
              else
                {
                  n_upgrades++;
                  if (bz_apt_package_is_security (package))
                    n_security++;
                }
            }

          /* Parsed by ai-patches/patches/harness/check-counts.py; keep the format */
          g_debug ("GetUpdates: %u upgrades (%u security), %u new, %u removals",
                   n_upgrades, n_security, n_new, n_removals);

          /* Update the counts before the list so "items-changed"
           * handlers already see the new numbers */
          set_count (self, &self->n_security, n_security, PROP_N_SECURITY);
          set_count (self, &self->n_upgrades, n_upgrades, PROP_N_UPGRADES);
          set_count (self, &self->n_new, n_new, PROP_N_NEW);
          set_count (self, &self->n_removals, n_removals, PROP_N_REMOVALS);

          n_old = g_list_model_get_n_items (G_LIST_MODEL (self->packages));
          g_list_store_splice (self->packages, 0, n_old,
                               txn->found->pdata, txn->found->len);
        }
      else if (message != NULL && !txn->background)
        set_error (self, message);
      /* The first update list (or the failure to get one) is in */
      set_loaded (self);
      break;

    case TXN_REFRESH_CACHE:
      if (message != NULL && !txn->background)
        set_error (self, message);
      if (!cancelled)
        enqueue (self, TXN_GET_UPDATES, txn->background);
      else
        set_loaded (self);
      break;

    case TXN_INSTALL_NOW:
      if (success && (txn->restart == PK_RESTART_SYSTEM ||
                      txn->restart == PK_RESTART_SECURITY_SYSTEM))
        {
          self->restart_required = TRUE;
          g_object_notify_by_pspec (G_OBJECT (self), props[PROP_RESTART_REQUIRED]);
        }
      if (message != NULL)
        set_error (self, message);
      enqueue (self, TXN_GET_UPDATES, TRUE);
      break;

    case TXN_DOWNLOAD:
      if (success)
        g_dbus_connection_call (
            self->bus, PK_NAME, PK_PATH, PK_IFACE_OFFLINE, "Trigger",
            g_variant_new ("(s)", "reboot"), NULL,
            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
            -1, NULL, trigger_cb, g_object_ref (self));
      else if (message != NULL)
        set_error (self, message);
      break;

    default:
      g_assert_not_reached ();
    }

  /* Drop the reference self->txn was holding */
  txn_unref (txn);

  run_next (self);
}

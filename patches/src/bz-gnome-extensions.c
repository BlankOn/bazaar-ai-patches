/* bz-gnome-extensions.c
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

/* DOWNSTREAM PATCH (GNOME Extensions): see ai-patches/PLAN.md §13 and
 * bz-gnome-extensions.h.
 *
 * extensions.gnome.org (no API key, public):
 *   GET /extension-query/?search=TEXT&sort=downloads|popularity|recent
 *       &page=N&n_per_page=24&shell_version=MAJOR
 *     -> {"extensions": [{uuid, name, creator, description, link, icon,
 *         screenshot, url, downloads, ...}], "numpages": N, ...}
 *   GET /extension-info/?uuid=UUID   -> one such object (404 if unknown)
 *   Icons and screenshots are paths on the same host.
 *
 * GNOME Shell: interface org.gnome.Shell.Extensions on bus name
 * org.gnome.Shell, object /org/gnome/Shell. (The bus name
 * org.gnome.Shell.Extensions is the Extensions app, which only runs on
 * demand, so it is not used.)
 *   ListExtensions, InstallRemoteExtension (Shell downloads the zip for its
 *   own version from extensions.gnome.org and asks the user first),
 *   EnableExtension, DisableExtension, UninstallExtension,
 *   OpenExtensionPrefs, the ExtensionStateChanged signal and the
 *   UserExtensionsEnabled and ShellVersion properties.
 */

#define G_LOG_DOMAIN "BAZAAR::GNOME-EXTENSIONS"

#include "config.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "bz-gnome-extensions.h"

#define EGO_HOST       "https://extensions.gnome.org"
#define EGO_PER_PAGE   24
#define SHELL_NAME     "org.gnome.Shell"
#define SHELL_PATH     "/org/gnome/Shell"
#define SHELL_IFACE    "org.gnome.Shell.Extensions"
#define DEFAULT_ICON   "/static/images/plugin.png"
#define TYPE_PER_USER  2

/* ------------------------------------------------------------------ */
/* BzGnomeExtension                                                    */
/* ------------------------------------------------------------------ */

struct _BzGnomeExtension
{
  GObject parent_instance;

  char         *uuid;
  char         *name;
  char         *creator;
  char         *description;
  char         *link;
  char         *homepage;
  char         *icon_uri;
  char         *screenshot_uri;
  GdkPaintable *icon;
  GdkPaintable *screenshot;
  gboolean      screenshot_requested;
  gboolean      info_requested;
  guint64       downloads;

  BzGnomeExtensionState state;
  gboolean              enabled;
  gboolean              has_prefs;
  gboolean              can_uninstall;
  gboolean              can_change;
  gboolean              busy;
  char                 *shell_error;
  char                 *error;
};

G_DEFINE_FINAL_TYPE (BzGnomeExtension, bz_gnome_extension, G_TYPE_OBJECT)

enum
{
  EXT_PROP_0,
  EXT_PROP_UUID,
  EXT_PROP_NAME,
  EXT_PROP_CREATOR,
  EXT_PROP_DESCRIPTION,
  EXT_PROP_LINK,
  EXT_PROP_HOMEPAGE,
  EXT_PROP_ICON,
  EXT_PROP_HAS_ICON,
  EXT_PROP_SCREENSHOT,
  EXT_PROP_HAS_SCREENSHOT,
  EXT_PROP_DOWNLOADS,
  EXT_PROP_STATE,
  EXT_PROP_INSTALLED,
  EXT_PROP_ENABLED,
  EXT_PROP_HAS_PREFS,
  EXT_PROP_CAN_UNINSTALL,
  EXT_PROP_CAN_CHANGE,
  EXT_PROP_STATUS_LABEL,
  EXT_PROP_ACTIVE,
  EXT_PROP_SHOW_STATUS_PILL,
  EXT_PROP_BUSY,
  EXT_PROP_ERROR,
  EXT_PROP_HAS_ERROR,
  EXT_N_PROPS,
};

static GParamSpec *ext_props[EXT_N_PROPS];

static void
bz_gnome_extension_finalize (GObject *object)
{
  BzGnomeExtension *self = BZ_GNOME_EXTENSION (object);

  g_free (self->uuid);
  g_free (self->name);
  g_free (self->creator);
  g_free (self->description);
  g_free (self->link);
  g_free (self->homepage);
  g_free (self->icon_uri);
  g_free (self->screenshot_uri);
  g_clear_object (&self->icon);
  g_clear_object (&self->screenshot);
  g_free (self->shell_error);
  g_free (self->error);

  G_OBJECT_CLASS (bz_gnome_extension_parent_class)->finalize (object);
}

static void
bz_gnome_extension_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  BzGnomeExtension *self = BZ_GNOME_EXTENSION (object);

  switch (prop_id)
    {
    case EXT_PROP_UUID: g_value_set_string (value, self->uuid); break;
    case EXT_PROP_NAME: g_value_set_string (value, self->name); break;
    case EXT_PROP_CREATOR: g_value_set_string (value, self->creator); break;
    case EXT_PROP_DESCRIPTION: g_value_set_string (value, self->description); break;
    case EXT_PROP_LINK: g_value_set_string (value, self->link); break;
    case EXT_PROP_HOMEPAGE: g_value_set_string (value, self->homepage); break;
    case EXT_PROP_ICON: g_value_set_object (value, self->icon); break;
    case EXT_PROP_HAS_ICON: g_value_set_boolean (value, self->icon != NULL); break;
    case EXT_PROP_SCREENSHOT: g_value_set_object (value, self->screenshot); break;
    case EXT_PROP_HAS_SCREENSHOT: g_value_set_boolean (value, self->screenshot != NULL); break;
    case EXT_PROP_DOWNLOADS: g_value_set_uint64 (value, self->downloads); break;
    case EXT_PROP_STATE: g_value_set_int (value, self->state); break;
    case EXT_PROP_INSTALLED: g_value_set_boolean (value, bz_gnome_extension_get_installed (self)); break;
    case EXT_PROP_ENABLED: g_value_set_boolean (value, self->enabled); break;
    case EXT_PROP_HAS_PREFS: g_value_set_boolean (value, self->has_prefs); break;
    case EXT_PROP_CAN_UNINSTALL: g_value_set_boolean (value, self->can_uninstall); break;
    case EXT_PROP_CAN_CHANGE: g_value_set_boolean (value, self->can_change); break;
    case EXT_PROP_STATUS_LABEL: g_value_set_string (value, bz_gnome_extension_get_status_label (self)); break;
    case EXT_PROP_ACTIVE: g_value_set_boolean (value, bz_gnome_extension_get_active (self)); break;
    case EXT_PROP_SHOW_STATUS_PILL:
      g_value_set_boolean (value, bz_gnome_extension_get_installed (self) &&
                                      !bz_gnome_extension_get_active (self));
      break;
    case EXT_PROP_BUSY: g_value_set_boolean (value, self->busy); break;
    case EXT_PROP_ERROR: g_value_set_string (value, bz_gnome_extension_get_error (self)); break;
    case EXT_PROP_HAS_ERROR: g_value_set_boolean (value, bz_gnome_extension_get_error (self) != NULL); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
bz_gnome_extension_class_init (BzGnomeExtensionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize     = bz_gnome_extension_finalize;
  object_class->get_property = bz_gnome_extension_get_property;

#define STR(id, n)  ext_props[id] = g_param_spec_string (n, NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY)
#define BOOL(id, n) ext_props[id] = g_param_spec_boolean (n, NULL, NULL, FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY)
#define OBJ(id, n)  ext_props[id] = g_param_spec_object (n, NULL, NULL, GDK_TYPE_PAINTABLE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY)
  STR (EXT_PROP_UUID, "uuid");
  STR (EXT_PROP_NAME, "name");
  STR (EXT_PROP_CREATOR, "creator");
  STR (EXT_PROP_DESCRIPTION, "description");
  STR (EXT_PROP_LINK, "link");
  STR (EXT_PROP_HOMEPAGE, "homepage");
  OBJ (EXT_PROP_ICON, "icon");
  BOOL (EXT_PROP_HAS_ICON, "has-icon");
  OBJ (EXT_PROP_SCREENSHOT, "screenshot");
  BOOL (EXT_PROP_HAS_SCREENSHOT, "has-screenshot");
  ext_props[EXT_PROP_DOWNLOADS] = g_param_spec_uint64 ("downloads", NULL, NULL, 0, G_MAXUINT64, 0,
                                                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  ext_props[EXT_PROP_STATE] = g_param_spec_int ("state", NULL, NULL, 0, G_MAXINT, 0,
                                                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  BOOL (EXT_PROP_INSTALLED, "installed");
  BOOL (EXT_PROP_ENABLED, "enabled");
  BOOL (EXT_PROP_HAS_PREFS, "has-prefs");
  BOOL (EXT_PROP_CAN_UNINSTALL, "can-uninstall");
  BOOL (EXT_PROP_CAN_CHANGE, "can-change");
  STR (EXT_PROP_STATUS_LABEL, "status-label");
  BOOL (EXT_PROP_ACTIVE, "active");
  BOOL (EXT_PROP_SHOW_STATUS_PILL, "show-status-pill");
  BOOL (EXT_PROP_BUSY, "busy");
  STR (EXT_PROP_ERROR, "error");
  BOOL (EXT_PROP_HAS_ERROR, "has-error");
#undef STR
#undef BOOL
#undef OBJ

  g_object_class_install_properties (object_class, EXT_N_PROPS, ext_props);
}

static void
bz_gnome_extension_init (BzGnomeExtension *self)
{
  self->can_change = TRUE;
}

static void
ext_notify (BzGnomeExtension *self,
            ...)
{
  va_list args;
  int     id;

  va_start (args, self);
  while ((id = va_arg (args, int)) != 0)
    g_object_notify_by_pspec (G_OBJECT (self), ext_props[id]);
  va_end (args);
}

static void
ext_notify_state (BzGnomeExtension *self)
{
  g_object_freeze_notify (G_OBJECT (self));
  ext_notify (self, EXT_PROP_STATE, EXT_PROP_INSTALLED, EXT_PROP_ENABLED, EXT_PROP_HAS_PREFS,
              EXT_PROP_CAN_UNINSTALL, EXT_PROP_CAN_CHANGE, EXT_PROP_STATUS_LABEL, EXT_PROP_ACTIVE,
              EXT_PROP_SHOW_STATUS_PILL, EXT_PROP_ERROR, EXT_PROP_HAS_ERROR, 0);
  g_object_thaw_notify (G_OBJECT (self));
}

static void
ext_set_busy (BzGnomeExtension *self,
              gboolean          busy)
{
  if (self->busy == busy)
    return;
  self->busy = busy;
  ext_notify (self, EXT_PROP_BUSY, 0);
}

static void
ext_set_error (BzGnomeExtension *self,
               const char       *error)
{
  if (g_strcmp0 (self->error, error) == 0)
    return;
  g_free (self->error);
  self->error = g_strdup (error);
  ext_notify (self, EXT_PROP_ERROR, EXT_PROP_HAS_ERROR, 0);
}

const char *
bz_gnome_extension_get_uuid (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);
  return self->uuid;
}

const char *
bz_gnome_extension_get_name (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);
  return self->name;
}

const char *
bz_gnome_extension_get_creator (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);
  return self->creator;
}

const char *
bz_gnome_extension_get_description (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);
  return self->description;
}

const char *
bz_gnome_extension_get_link (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);
  return self->link;
}

const char *
bz_gnome_extension_get_homepage (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);
  return self->homepage;
}

GdkPaintable *
bz_gnome_extension_get_icon (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);
  return self->icon;
}

GdkPaintable *
bz_gnome_extension_get_screenshot (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);
  return self->screenshot;
}

gboolean
bz_gnome_extension_get_has_screenshot (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), FALSE);
  return self->screenshot != NULL;
}

guint64
bz_gnome_extension_get_downloads (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), 0);
  return self->downloads;
}

BzGnomeExtensionState
bz_gnome_extension_get_state (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), 0);
  return self->state;
}

gboolean
bz_gnome_extension_get_installed (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), FALSE);
  return self->state != BZ_GNOME_EXTENSION_STATE_NOT_INSTALLED &&
         self->state != BZ_GNOME_EXTENSION_STATE_UNINSTALLED;
}

gboolean
bz_gnome_extension_get_enabled (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), FALSE);
  return self->enabled;
}

gboolean
bz_gnome_extension_get_has_prefs (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), FALSE);
  return self->has_prefs;
}

gboolean
bz_gnome_extension_get_can_uninstall (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), FALSE);
  return self->can_uninstall;
}

gboolean
bz_gnome_extension_get_can_change (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), FALSE);
  return self->can_change;
}

gboolean
bz_gnome_extension_get_active (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), FALSE);
  return self->state == BZ_GNOME_EXTENSION_STATE_ACTIVE;
}

gboolean
bz_gnome_extension_get_busy (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), FALSE);
  return self->busy;
}

const char *
bz_gnome_extension_get_error (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);
  if (self->error != NULL)
    return self->error;
  if (self->state == BZ_GNOME_EXTENSION_STATE_ERROR && self->shell_error != NULL && *self->shell_error != '\0')
    return self->shell_error;
  if (self->state == BZ_GNOME_EXTENSION_STATE_ERROR)
    return _ ("This extension failed to start.");
  if (self->state == BZ_GNOME_EXTENSION_STATE_OUT_OF_DATE)
    return _ ("This extension does not support this version of GNOME.");
  return NULL;
}

const char *
bz_gnome_extension_get_status_label (BzGnomeExtension *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (self), NULL);

  switch (self->state)
    {
    case BZ_GNOME_EXTENSION_STATE_ACTIVE:
      return _ ("Enabled");
    case BZ_GNOME_EXTENSION_STATE_INACTIVE:
    case BZ_GNOME_EXTENSION_STATE_INITIALIZED:
      return _ ("Disabled");
    case BZ_GNOME_EXTENSION_STATE_ERROR:
      return _ ("Error");
    case BZ_GNOME_EXTENSION_STATE_OUT_OF_DATE:
      /* Translators: a GNOME Shell extension that does not support this GNOME version */
      return _ ("Incompatible");
    case BZ_GNOME_EXTENSION_STATE_DOWNLOADING:
      return _ ("Installing…");
    case BZ_GNOME_EXTENSION_STATE_ACTIVATING:
      return _ ("Enabling…");
    case BZ_GNOME_EXTENSION_STATE_DEACTIVATING:
      return _ ("Disabling…");
    case BZ_GNOME_EXTENSION_STATE_NOT_INSTALLED:
    case BZ_GNOME_EXTENSION_STATE_UNINSTALLED:
    default:
      return NULL;
    }
}

static BzGnomeExtension *
ext_new (const char *uuid)
{
  BzGnomeExtension *self = g_object_new (BZ_TYPE_GNOME_EXTENSION, NULL);

  self->uuid = g_strdup (uuid);
  return self;
}

static char *
ego_uri (const char *path)
{
  if (path == NULL || *path == '\0')
    return NULL;
  if (g_str_has_prefix (path, "https://"))
    return g_strdup (path);
  if (path[0] != '/')
    return NULL;
  return g_strconcat (EGO_HOST, path, NULL);
}

static const char *
json_str (JsonObject *object,
          const char *member)
{
  JsonNode *node = json_object_get_member (object, member);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string (node);
}

static guint64
json_uint (JsonObject *object,
           const char *member)
{
  JsonNode *node = json_object_get_member (object, member);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return 0;
  if (json_node_get_value_type (node) == G_TYPE_STRING)
    return g_ascii_strtoull (json_node_get_string (node), NULL, 10);
  if (json_node_get_value_type (node) == G_TYPE_INT64)
    return MAX (json_node_get_int (node), 0);
  return 0;
}

static void
set_str (char      **field,
         const char *value,
         gboolean   *changed)
{
  if (value == NULL || g_strcmp0 (*field, value) == 0)
    return;
  g_free (*field);
  *field   = g_strdup (value);
  *changed = TRUE;
}

/* Fills in what extensions.gnome.org says. Returns TRUE if the icon URI
 * changed (to be fetched). */
static gboolean
ext_apply_ego (BzGnomeExtension *self,
               JsonObject       *object)
{
  gboolean         changed    = FALSE;
  gboolean         new_icon   = FALSE;
  g_autofree char *link       = NULL;
  g_autofree char *icon       = NULL;
  g_autofree char *screenshot = NULL;
  const char      *homepage   = NULL;
  guint64          downloads  = 0;

  g_object_freeze_notify (G_OBJECT (self));

  set_str (&self->name, json_str (object, "name"), &changed);
  set_str (&self->creator, json_str (object, "creator"), &changed);
  set_str (&self->description, json_str (object, "description"), &changed);
  link = ego_uri (json_str (object, "link"));
  set_str (&self->link, link, &changed);
  homepage = json_str (object, "url");
  if (homepage != NULL && (g_str_has_prefix (homepage, "https://") || g_str_has_prefix (homepage, "http://")))
    set_str (&self->homepage, homepage, &changed);

  if (g_strcmp0 (json_str (object, "icon"), DEFAULT_ICON) != 0)
    icon = ego_uri (json_str (object, "icon"));
  if (icon != NULL && g_strcmp0 (icon, self->icon_uri) != 0)
    {
      g_free (self->icon_uri);
      self->icon_uri = g_steal_pointer (&icon);
      new_icon       = TRUE;
    }
  screenshot = ego_uri (json_str (object, "screenshot"));
  if (screenshot != NULL && self->screenshot_uri == NULL)
    self->screenshot_uri = g_steal_pointer (&screenshot);

  downloads = json_uint (object, "downloads");
  if (downloads != 0 && downloads != self->downloads)
    {
      self->downloads = downloads;
      ext_notify (self, EXT_PROP_DOWNLOADS, 0);
    }

  if (changed)
    ext_notify (self, EXT_PROP_NAME, EXT_PROP_CREATOR, EXT_PROP_DESCRIPTION, EXT_PROP_LINK,
                EXT_PROP_HOMEPAGE, 0);

  g_object_thaw_notify (G_OBJECT (self));
  return new_icon;
}

static double
info_double (GVariant   *info,
             const char *key,
             double      fallback)
{
  double value = fallback;

  if (!g_variant_lookup (info, key, "d", &value))
    {
      gint32 i = 0;
      if (g_variant_lookup (info, key, "i", &i))
        value = i;
    }
  return value;
}

static gboolean
info_bool (GVariant   *info,
           const char *key,
           gboolean    fallback)
{
  gboolean value = fallback;

  g_variant_lookup (info, key, "b", &value);
  return value;
}

/* Fills in GNOME Shell's state; info NULL means not installed. */
static void
ext_apply_info (BzGnomeExtension *self,
                GVariant         *info)
{
  BzGnomeExtensionState state = BZ_GNOME_EXTENSION_STATE_NOT_INSTALLED;
  gboolean              changed = FALSE;
  const char           *str     = NULL;

  if (info != NULL)
    {
      state = (BzGnomeExtensionState) info_double (info, "state", BZ_GNOME_EXTENSION_STATE_INACTIVE);
      if (state == BZ_GNOME_EXTENSION_STATE_UNINSTALLED)
        info = NULL;
    }

  if (info == NULL)
    {
      self->state         = BZ_GNOME_EXTENSION_STATE_NOT_INSTALLED;
      self->enabled       = FALSE;
      self->has_prefs     = FALSE;
      self->can_uninstall = FALSE;
      self->can_change    = TRUE;
      g_clear_pointer (&self->shell_error, g_free);
      ext_notify_state (self);
      return;
    }

  self->state         = state;
  self->enabled       = info_bool (info, "enabled", state == BZ_GNOME_EXTENSION_STATE_ACTIVE);
  self->has_prefs     = info_bool (info, "hasPrefs", FALSE);
  self->can_change    = info_bool (info, "canChange", TRUE);
  self->can_uninstall = info_double (info, "type", 0) == TYPE_PER_USER;
  g_free (self->shell_error);
  self->shell_error = NULL;
  g_variant_lookup (info, "error", "s", &self->shell_error);

  /* An installed extension not (or not yet) known to extensions.gnome.org:
   * show what its metadata.json says. */
  g_object_freeze_notify (G_OBJECT (self));
  if (self->name == NULL && g_variant_lookup (info, "name", "&s", &str))
    set_str (&self->name, str, &changed);
  if (self->description == NULL && g_variant_lookup (info, "description", "&s", &str))
    set_str (&self->description, str, &changed);
  if (self->homepage == NULL && g_variant_lookup (info, "url", "&s", &str) &&
      (g_str_has_prefix (str, "https://") || g_str_has_prefix (str, "http://")))
    set_str (&self->homepage, str, &changed);
  if (changed)
    ext_notify (self, EXT_PROP_NAME, EXT_PROP_DESCRIPTION, EXT_PROP_HOMEPAGE, 0);
  ext_notify_state (self);
  g_object_thaw_notify (G_OBJECT (self));
}

/* ------------------------------------------------------------------ */
/* BzGnomeExtensions                                                   */
/* ------------------------------------------------------------------ */

struct _BzGnomeExtensions
{
  GObject parent_instance;

  GCancellable *cancellable;
  SoupSession  *session;
  GDBusProxy   *proxy;
  gboolean      available;
  GHashTable   *installed; /* uuid -> GVariant a{sv} */
  gboolean      listed;    /* ListExtensions answered */

  GListStore             *results;
  GHashTable             *by_uuid; /* uuid -> BzGnomeExtension, the results */
  char                   *text;
  BzGnomeExtensionsSort   sort;
  gboolean                searched;
  guint                   page;
  guint                   numpages;
  guint                   generation;
  BzGnomeExtensionsStatus status;
  gboolean                loading_more;
  char                   *error;
};

G_DEFINE_FINAL_TYPE (BzGnomeExtensions, bz_gnome_extensions, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_AVAILABLE,
  PROP_SHELL_VERSION,
  PROP_USER_EXTENSIONS_ENABLED,
  PROP_RESULTS,
  PROP_STATUS,
  PROP_LOADING_MORE,
  PROP_HAS_MORE,
  PROP_ERROR,
  N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void fetch_page (BzGnomeExtensions *self);
static void list_installed_results (BzGnomeExtensions *self);
static void list_extensions (BzGnomeExtensions *self);

static void
set_status (BzGnomeExtensions      *self,
            BzGnomeExtensionsStatus status,
            const char             *error)
{
  g_free (self->error);
  self->error  = g_strdup (error);
  self->status = status;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_STATUS]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ERROR]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_MORE]);
}

static void
set_loading_more (BzGnomeExtensions *self,
                  gboolean           loading_more)
{
  if (self->loading_more == loading_more)
    return;
  self->loading_more = loading_more;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LOADING_MORE]);
}

static void
clear_results (BzGnomeExtensions *self)
{
  g_hash_table_remove_all (self->by_uuid);
  g_list_store_remove_all (self->results);
}

static void
add_result (BzGnomeExtensions *self,
            BzGnomeExtension  *extension)
{
  g_hash_table_replace (self->by_uuid, g_strdup (extension->uuid), g_object_ref (extension));
  g_list_store_append (self->results, extension);
}

/* --- HTTP -------------------------------------------------------- */

typedef void (*FetchDone) (BzGnomeExtensions *self,
                           GBytes            *bytes,
                           const GError      *error,
                           gpointer           data);

typedef struct
{
  GWeakRef       self;
  FetchDone      done;
  gpointer       data;
  GDestroyNotify destroy;
} Fetch;

static void
fetch_free (Fetch *fetch)
{
  g_weak_ref_clear (&fetch->self);
  if (fetch->destroy != NULL)
    fetch->destroy (fetch->data);
  g_free (fetch);
}

static void
fetch_cb (GObject      *source,
          GAsyncResult *result,
          gpointer      user_data)
{
  Fetch *fetch                    = user_data;
  g_autoptr (BzGnomeExtensions) self = g_weak_ref_get (&fetch->self);
  g_autoptr (GError) local_error  = NULL;
  g_autoptr (GBytes) bytes        = NULL;
  SoupMessage *message            = NULL;

  bytes   = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &local_error);
  message = soup_session_get_async_result_message (SOUP_SESSION (source), result);
  if (bytes != NULL && message != NULL &&
      !SOUP_STATUS_IS_SUCCESSFUL (soup_message_get_status (message)))
    {
      g_clear_pointer (&bytes, g_bytes_unref);
      local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u %s",
                                 soup_message_get_status (message),
                                 soup_message_get_reason_phrase (message));
    }

  if (self != NULL && !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    fetch->done (self, bytes, local_error, fetch->data);
  fetch_free (fetch);
}

static void
fetch (BzGnomeExtensions *self,
       const char        *uri,
       FetchDone          done,
       gpointer           data,
       GDestroyNotify     destroy)
{
  g_autoptr (SoupMessage) message = NULL;
  Fetch *fetch_data               = NULL;

  message = soup_message_new (SOUP_METHOD_GET, uri);
  if (message == NULL)
    {
      g_autoptr (GError) local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "bad URI %s", uri);
      done (self, NULL, local_error, data);
      if (destroy != NULL)
        destroy (data);
      return;
    }

  fetch_data = g_new0 (Fetch, 1);
  g_weak_ref_init (&fetch_data->self, self);
  fetch_data->done    = done;
  fetch_data->data    = data;
  fetch_data->destroy = destroy;
  soup_session_send_and_read_async (self->session, message, G_PRIORITY_DEFAULT,
                                    self->cancellable, fetch_cb, fetch_data);
}

static JsonNode *
parse_json (GBytes  *bytes,
            GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new_immutable ();
  gsize         size            = 0;
  gconstpointer data            = g_bytes_get_data (bytes, &size);

  if (!json_parser_load_from_data (parser, data, size, error))
    return NULL;
  return json_node_ref (json_parser_get_root (parser));
}

typedef struct
{
  BzGnomeExtension *extension;
  gboolean          screenshot;
} TextureJob;

static void
texture_job_free (TextureJob *job)
{
  g_object_unref (job->extension);
  g_free (job);
}

static void
texture_done (BzGnomeExtensions *self,
              GBytes            *bytes,
              const GError      *error,
              gpointer           data)
{
  TextureJob *job                 = data;
  g_autoptr (GError) local_error  = NULL;
  g_autoptr (GdkTexture) texture  = NULL;
  BzGnomeExtension *extension     = job->extension;

  if (bytes == NULL)
    {
      g_debug ("%s: %s", extension->uuid, error->message);
      return;
    }
  texture = gdk_texture_new_from_bytes (bytes, &local_error);
  if (texture == NULL)
    {
      g_debug ("%s: %s", extension->uuid, local_error->message);
      return;
    }

  if (job->screenshot)
    {
      g_set_object (&extension->screenshot, GDK_PAINTABLE (texture));
      ext_notify (extension, EXT_PROP_SCREENSHOT, EXT_PROP_HAS_SCREENSHOT, 0);
    }
  else
    {
      g_set_object (&extension->icon, GDK_PAINTABLE (texture));
      ext_notify (extension, EXT_PROP_ICON, EXT_PROP_HAS_ICON, 0);
    }
}

static void
fetch_texture (BzGnomeExtensions *self,
               BzGnomeExtension  *extension,
               gboolean           screenshot)
{
  TextureJob *job  = NULL;
  const char *uri  = screenshot ? extension->screenshot_uri : extension->icon_uri;

  if (uri == NULL)
    return;
  job             = g_new0 (TextureJob, 1);
  job->extension  = g_object_ref (extension);
  job->screenshot = screenshot;
  fetch (self, uri, texture_done, job, (GDestroyNotify) texture_job_free);
}

static GVariant *
lookup_installed (BzGnomeExtensions *self,
                  const char        *uuid)
{
  return g_hash_table_lookup (self->installed, uuid);
}

typedef struct
{
  guint    generation;
  gboolean append;
} PageJob;

static void
page_done (BzGnomeExtensions *self,
           GBytes            *bytes,
           const GError      *error,
           gpointer           data)
{
  PageJob *job                   = data;
  g_autoptr (GError) local_error = NULL;
  g_autoptr (JsonNode) root      = NULL;
  JsonObject *object             = NULL;
  JsonArray  *extensions         = NULL;

  if (job->generation != self->generation)
    return;
  set_loading_more (self, FALSE);

  if (bytes != NULL)
    root = parse_json (bytes, &local_error);
  if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
    {
      object = json_node_get_object (root);
      if (json_object_has_member (object, "extensions") &&
          JSON_NODE_HOLDS_ARRAY (json_object_get_member (object, "extensions")))
        extensions = json_object_get_array_member (object, "extensions");
    }

  if (extensions == NULL)
    {
      const char *message = error != NULL ? error->message
                            : local_error != NULL ? local_error->message
                                                  : "unexpected answer";
      g_warning ("extensions.gnome.org: %s", message);
      if (job->append)
        {
          /* Keep what is shown; scrolling down again retries. */
          self->page--;
          return;
        }
      set_status (self, BZ_GNOME_EXTENSIONS_STATUS_ERROR, message);
      return;
    }

  self->numpages = json_object_get_int_member_with_default (object, "numpages", self->page);
  if (!job->append)
    clear_results (self);

  for (guint i = 0; i < json_array_get_length (extensions); i++)
    {
      JsonObject *item                     = json_array_get_object_element (extensions, i);
      const char *uuid                     = item != NULL ? json_str (item, "uuid") : NULL;
      g_autoptr (BzGnomeExtension) extension = NULL;

      if (uuid == NULL || g_hash_table_contains (self->by_uuid, uuid))
        continue;
      extension = ext_new (uuid);
      if (ext_apply_ego (extension, item))
        fetch_texture (self, extension, FALSE);
      ext_apply_info (extension, lookup_installed (self, uuid));
      add_result (self, extension);
    }

  set_status (self,
              g_list_model_get_n_items (G_LIST_MODEL (self->results)) > 0
                  ? BZ_GNOME_EXTENSIONS_STATUS_RESULTS
                  : BZ_GNOME_EXTENSIONS_STATUS_EMPTY,
              NULL);
}

static char *
shell_major (BzGnomeExtensions *self)
{
  const char *version = bz_gnome_extensions_get_shell_version (self);
  const char *dot     = NULL;

  if (version == NULL)
    return NULL;
  dot = strchr (version, '.');
  return dot != NULL ? g_strndup (version, dot - version) : g_strdup (version);
}

static void
fetch_page (BzGnomeExtensions *self)
{
  static const char *sorts[] = { "downloads", "popularity", "recent" };
  g_autofree char   *major   = shell_major (self);
  g_autofree char   *search  = NULL;
  g_autofree char   *uri     = NULL;
  PageJob           *job     = NULL;

  g_return_if_fail (self->sort < G_N_ELEMENTS (sorts));

  search = g_uri_escape_string (self->text != NULL ? self->text : "", NULL, FALSE);
  uri    = g_strdup_printf (EGO_HOST "/extension-query/?search=%s&sort=%s&page=%u&n_per_page=%d%s%s",
                            search, sorts[self->sort], self->page, EGO_PER_PAGE,
                            major != NULL ? "&shell_version=" : "", major != NULL ? major : "");

  job             = g_new0 (PageJob, 1);
  job->generation = self->generation;
  job->append     = self->page > 1;
  fetch (self, uri, page_done, job, g_free);
}

/* An installed extension's icon, link and creator from extensions.gnome.org. */
static void
info_done (BzGnomeExtensions *self,
           GBytes            *bytes,
           const GError      *error,
           gpointer           data)
{
  BzGnomeExtension *extension = data;
  g_autoptr (JsonNode) root   = NULL;

  if (bytes == NULL)
    return; /* not on extensions.gnome.org (e.g. a system extension) */
  root = parse_json (bytes, NULL);
  if (root != NULL && JSON_NODE_HOLDS_OBJECT (root) &&
      ext_apply_ego (extension, json_node_get_object (root)))
    fetch_texture (self, extension, FALSE);
}

static int
compare_by_name (gconstpointer a,
                 gconstpointer b,
                 gpointer      data)
{
  BzGnomeExtension *ea = (BzGnomeExtension *) a;
  BzGnomeExtension *eb = (BzGnomeExtension *) b;
  g_autofree char  *ka = g_utf8_collate_key (ea->name != NULL ? ea->name : ea->uuid, -1);
  g_autofree char  *kb = g_utf8_collate_key (eb->name != NULL ? eb->name : eb->uuid, -1);

  return strcmp (ka, kb);
}

static gboolean
matches (BzGnomeExtension *extension,
         const char       *needle)
{
  const char *fields[] = { extension->name, extension->description, extension->uuid };

  if (needle == NULL || *needle == '\0')
    return TRUE;
  for (guint i = 0; i < G_N_ELEMENTS (fields); i++)
    {
      g_autofree char *hay = fields[i] != NULL ? g_utf8_casefold (fields[i], -1) : NULL;
      if (hay != NULL && strstr (hay, needle) != NULL)
        return TRUE;
    }
  return FALSE;
}

/* The "Installed" view: the user's extensions, from GNOME Shell. */
static void
list_installed_results (BzGnomeExtensions *self)
{
  g_autofree char *needle = NULL;
  g_autoptr (GPtrArray) found = g_ptr_array_new_with_free_func (g_object_unref);
  GHashTableIter iter;
  gpointer       key   = NULL;
  gpointer       value = NULL;

  if (!self->listed)
    {
      set_status (self, BZ_GNOME_EXTENSIONS_STATUS_LOADING, NULL);
      return;
    }

  needle = self->text != NULL ? g_utf8_casefold (self->text, -1) : NULL;
  g_hash_table_iter_init (&iter, self->installed);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      BzGnomeExtension *extension = g_hash_table_lookup (self->by_uuid, key);

      if (extension != NULL)
        g_object_ref (extension);
      else
        extension = ext_new (key);
      ext_apply_info (extension, value);
      if (matches (extension, needle))
        g_ptr_array_add (found, extension);
      else
        g_object_unref (extension);
    }
  g_ptr_array_sort_values_with_data (found, compare_by_name, NULL);

  clear_results (self);
  for (guint i = 0; i < found->len; i++)
    {
      BzGnomeExtension *extension = g_ptr_array_index (found, i);
      g_autofree char  *uri       = NULL;

      add_result (self, extension);
      if (!extension->info_requested)
        {
          g_autofree char *uuid = g_uri_escape_string (extension->uuid, NULL, FALSE);

          extension->info_requested = TRUE;
          uri = g_strdup_printf (EGO_HOST "/extension-info/?uuid=%s", uuid);
          fetch (self, uri, info_done, g_object_ref (extension), g_object_unref);
        }
    }

  set_status (self,
              found->len > 0 ? BZ_GNOME_EXTENSIONS_STATUS_RESULTS : BZ_GNOME_EXTENSIONS_STATUS_EMPTY,
              NULL);
}

static void
run_search (BzGnomeExtensions *self)
{
  self->generation++;
  self->page     = 1;
  self->numpages = 0;
  set_loading_more (self, FALSE);

  if (self->sort == BZ_GNOME_EXTENSIONS_SORT_INSTALLED)
    {
      list_installed_results (self);
      return;
    }

  set_status (self, BZ_GNOME_EXTENSIONS_STATUS_LOADING, NULL);
  /* Wait for GNOME Shell's version, so only compatible extensions show. */
  if (bz_gnome_extensions_get_shell_version (self) != NULL)
    fetch_page (self);
}

/* --- GNOME Shell ------------------------------------------------- */

static void
update_installed (BzGnomeExtensions *self,
                  const char        *uuid,
                  GVariant          *info)
{
  BzGnomeExtension *extension = NULL;
  gboolean          removed   = info == NULL ||
                       info_double (info, "state", 0) == BZ_GNOME_EXTENSION_STATE_UNINSTALLED;

  if (removed)
    g_hash_table_remove (self->installed, uuid);
  else
    g_hash_table_replace (self->installed, g_strdup (uuid), g_variant_ref (info));

  extension = g_hash_table_lookup (self->by_uuid, uuid);
  if (extension != NULL)
    ext_apply_info (extension, removed ? NULL : info);

  if (self->sort == BZ_GNOME_EXTENSIONS_SORT_INSTALLED && self->searched &&
      (removed || extension == NULL))
    list_installed_results (self);
}

static void
list_extensions_cb (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (BzGnomeExtensions) self = user_data;
  g_autoptr (GError) local_error     = NULL;
  g_autoptr (GVariant) reply         = NULL;
  g_autoptr (GVariant) all           = NULL;
  GVariantIter iter;
  const char  *uuid = NULL;
  GVariant    *info = NULL;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &local_error);
  if (reply == NULL)
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("ListExtensions: %s", local_error->message);
      return;
    }

  g_hash_table_remove_all (self->installed);
  all = g_variant_get_child_value (reply, 0);
  g_variant_iter_init (&iter, all);
  while (g_variant_iter_next (&iter, "{&s@a{sv}}", &uuid, &info))
    {
      g_hash_table_replace (self->installed, g_strdup (uuid), info);
    }
  self->listed = TRUE;

  /* Refresh every shown extension's state. */
  for (guint i = 0; i < g_list_model_get_n_items (G_LIST_MODEL (self->results)); i++)
    {
      g_autoptr (BzGnomeExtension) extension = g_list_model_get_item (G_LIST_MODEL (self->results), i);
      ext_apply_info (extension, lookup_installed (self, extension->uuid));
    }

  if (self->searched && self->sort == BZ_GNOME_EXTENSIONS_SORT_INSTALLED)
    list_installed_results (self);
}

static void
list_extensions (BzGnomeExtensions *self)
{
  g_dbus_proxy_call (self->proxy, "ListExtensions", NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                     self->cancellable, list_extensions_cb, g_object_ref (self));
}

static void
update_available (BzGnomeExtensions *self)
{
  g_autofree char *owner     = NULL;
  gboolean         available = FALSE;

  owner     = self->proxy != NULL ? g_dbus_proxy_get_name_owner (self->proxy) : NULL;
  available = owner != NULL;
  if (available == self->available)
    return;

  self->available = available;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AVAILABLE]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SHELL_VERSION]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_USER_EXTENSIONS_ENABLED]);

  if (available)
    {
      list_extensions (self);
      if (self->searched && self->status == BZ_GNOME_EXTENSIONS_STATUS_LOADING &&
          self->sort != BZ_GNOME_EXTENSIONS_SORT_INSTALLED)
        fetch_page (self);
    }
}

static void
name_owner_cb (GObject    *proxy,
               GParamSpec *pspec,
               gpointer    data)
{
  update_available (BZ_GNOME_EXTENSIONS (data));
}

static void
properties_changed_cb (GDBusProxy *proxy,
                       GVariant   *changed,
                       GStrv       invalidated,
                       gpointer    data)
{
  BzGnomeExtensions *self      = BZ_GNOME_EXTENSIONS (data);
  g_autoptr (GVariant) enabled = g_variant_lookup_value (changed, "UserExtensionsEnabled", NULL);
  g_autoptr (GVariant) version = g_variant_lookup_value (changed, "ShellVersion", NULL);

  if (enabled != NULL)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_USER_EXTENSIONS_ENABLED]);
  if (version != NULL)
    {
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SHELL_VERSION]);
      if (self->searched && self->status == BZ_GNOME_EXTENSIONS_STATUS_LOADING &&
          self->sort != BZ_GNOME_EXTENSIONS_SORT_INSTALLED)
        fetch_page (self);
    }
}

static void
signal_cb (GDBusProxy *proxy,
           const char *sender,
           const char *signal,
           GVariant   *parameters,
           gpointer    data)
{
  BzGnomeExtensions *self = BZ_GNOME_EXTENSIONS (data);
  const char        *uuid = NULL;
  g_autoptr (GVariant) info = NULL;

  if (g_strcmp0 (signal, "ExtensionStateChanged") != 0 ||
      !g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(sa{sv})")))
    return;

  g_variant_get (parameters, "(&s@a{sv})", &uuid, &info);
  update_installed (self, uuid, info);
}

static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr (BzGnomeExtensions) self = user_data;
  g_autoptr (GError) local_error     = NULL;
  GDBusProxy *proxy                  = NULL;

  proxy = g_dbus_proxy_new_for_bus_finish (result, &local_error);
  if (proxy == NULL)
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("no session bus: %s", local_error->message);
      return;
    }

  self->proxy = proxy;
  g_signal_connect_object (proxy, "notify::g-name-owner", G_CALLBACK (name_owner_cb), self, 0);
  g_signal_connect_object (proxy, "g-properties-changed", G_CALLBACK (properties_changed_cb), self, 0);
  g_signal_connect_object (proxy, "g-signal", G_CALLBACK (signal_cb), self, 0);
  update_available (self);
}

/* --- GObject ----------------------------------------------------- */

static void
bz_gnome_extensions_dispose (GObject *object)
{
  BzGnomeExtensions *self = BZ_GNOME_EXTENSIONS (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->proxy);

  G_OBJECT_CLASS (bz_gnome_extensions_parent_class)->dispose (object);
}

static void
bz_gnome_extensions_finalize (GObject *object)
{
  BzGnomeExtensions *self = BZ_GNOME_EXTENSIONS (object);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->session);
  g_clear_pointer (&self->installed, g_hash_table_unref);
  g_clear_pointer (&self->by_uuid, g_hash_table_unref);
  g_clear_object (&self->results);
  g_free (self->text);
  g_free (self->error);

  G_OBJECT_CLASS (bz_gnome_extensions_parent_class)->finalize (object);
}

static void
bz_gnome_extensions_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  BzGnomeExtensions *self = BZ_GNOME_EXTENSIONS (object);

  switch (prop_id)
    {
    case PROP_AVAILABLE: g_value_set_boolean (value, self->available); break;
    case PROP_SHELL_VERSION: g_value_set_string (value, bz_gnome_extensions_get_shell_version (self)); break;
    case PROP_USER_EXTENSIONS_ENABLED: g_value_set_boolean (value, bz_gnome_extensions_get_user_extensions_enabled (self)); break;
    case PROP_RESULTS: g_value_set_object (value, self->results); break;
    case PROP_STATUS: g_value_set_int (value, self->status); break;
    case PROP_LOADING_MORE: g_value_set_boolean (value, self->loading_more); break;
    case PROP_HAS_MORE: g_value_set_boolean (value, bz_gnome_extensions_get_has_more (self)); break;
    case PROP_ERROR: g_value_set_string (value, self->error); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
bz_gnome_extensions_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  BzGnomeExtensions *self = BZ_GNOME_EXTENSIONS (object);

  switch (prop_id)
    {
    case PROP_USER_EXTENSIONS_ENABLED:
      bz_gnome_extensions_set_user_extensions_enabled (self, g_value_get_boolean (value));
      break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
bz_gnome_extensions_class_init (BzGnomeExtensionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose      = bz_gnome_extensions_dispose;
  object_class->finalize     = bz_gnome_extensions_finalize;
  object_class->get_property = bz_gnome_extensions_get_property;
  object_class->set_property = bz_gnome_extensions_set_property;

  props[PROP_AVAILABLE] = g_param_spec_boolean ("available", NULL, NULL, FALSE,
                                                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_SHELL_VERSION] = g_param_spec_string ("shell-version", NULL, NULL, NULL,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_USER_EXTENSIONS_ENABLED] = g_param_spec_boolean ("user-extensions-enabled", NULL, NULL, TRUE,
                                                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_RESULTS] = g_param_spec_object ("results", NULL, NULL, G_TYPE_LIST_MODEL,
                                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_STATUS] = g_param_spec_int ("status", NULL, NULL, 0, G_MAXINT, 0,
                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_LOADING_MORE] = g_param_spec_boolean ("loading-more", NULL, NULL, FALSE,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_HAS_MORE] = g_param_spec_boolean ("has-more", NULL, NULL, FALSE,
                                               G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_ERROR] = g_param_spec_string ("error", NULL, NULL, NULL,
                                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
bz_gnome_extensions_init (BzGnomeExtensions *self)
{
  g_autofree char *user_agent = NULL;

  self->cancellable = g_cancellable_new ();
  self->installed   = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_variant_unref);
  self->by_uuid     = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->results     = g_list_store_new (BZ_TYPE_GNOME_EXTENSION);

  user_agent    = g_strdup_printf ("%s (GNOME Shell extension browser)",
                                   g_get_prgname () != NULL ? g_get_prgname () : "bazaar");
  self->session = g_object_new (SOUP_TYPE_SESSION,
                                "user-agent", user_agent,
                                "timeout", 30,
                                NULL);

  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                            NULL, SHELL_NAME, SHELL_PATH, SHELL_IFACE,
                            self->cancellable, proxy_ready_cb, g_object_ref (self));
}

BzGnomeExtensions *
bz_gnome_extensions_new (void)
{
  return g_object_new (BZ_TYPE_GNOME_EXTENSIONS, NULL);
}

gboolean
bz_gnome_extensions_get_available (BzGnomeExtensions *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS (self), FALSE);
  return self->available;
}

const char *
bz_gnome_extensions_get_shell_version (BzGnomeExtensions *self)
{
  g_autoptr (GVariant) value = NULL;

  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS (self), NULL);
  if (!self->available)
    return NULL;
  value = g_dbus_proxy_get_cached_property (self->proxy, "ShellVersion");
  if (value == NULL || !g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
    return NULL;
  /* The cached value lives as long as the proxy's cache entry. */
  return g_intern_string (g_variant_get_string (value, NULL));
}

gboolean
bz_gnome_extensions_get_user_extensions_enabled (BzGnomeExtensions *self)
{
  g_autoptr (GVariant) value = NULL;

  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS (self), TRUE);
  if (!self->available)
    return TRUE;
  value = g_dbus_proxy_get_cached_property (self->proxy, "UserExtensionsEnabled");
  if (value == NULL || !g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN))
    return TRUE;
  return g_variant_get_boolean (value);
}

static void
set_property_cb (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) reply     = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &local_error);
  if (reply == NULL && !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("UserExtensionsEnabled: %s", local_error->message);
}

void
bz_gnome_extensions_set_user_extensions_enabled (BzGnomeExtensions *self,
                                                 gboolean           enabled)
{
  g_return_if_fail (BZ_IS_GNOME_EXTENSIONS (self));
  if (!self->available || bz_gnome_extensions_get_user_extensions_enabled (self) == enabled)
    return;

  g_dbus_connection_call (g_dbus_proxy_get_connection (self->proxy),
                          SHELL_NAME, SHELL_PATH, "org.freedesktop.DBus.Properties", "Set",
                          g_variant_new ("(ssv)", SHELL_IFACE, "UserExtensionsEnabled",
                                         g_variant_new_boolean (enabled)),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          self->cancellable, set_property_cb, NULL);
}

GListModel *
bz_gnome_extensions_get_results (BzGnomeExtensions *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS (self), NULL);
  return G_LIST_MODEL (self->results);
}

BzGnomeExtensionsStatus
bz_gnome_extensions_get_status (BzGnomeExtensions *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS (self), 0);
  return self->status;
}

gboolean
bz_gnome_extensions_get_loading_more (BzGnomeExtensions *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS (self), FALSE);
  return self->loading_more;
}

gboolean
bz_gnome_extensions_get_has_more (BzGnomeExtensions *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS (self), FALSE);
  return self->sort != BZ_GNOME_EXTENSIONS_SORT_INSTALLED &&
         self->status == BZ_GNOME_EXTENSIONS_STATUS_RESULTS &&
         self->page < self->numpages;
}

const char *
bz_gnome_extensions_get_error (BzGnomeExtensions *self)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS (self), NULL);
  return self->error;
}

void
bz_gnome_extensions_search (BzGnomeExtensions    *self,
                            const char           *text,
                            BzGnomeExtensionsSort sort)
{
  g_autofree char *stripped = NULL;

  g_return_if_fail (BZ_IS_GNOME_EXTENSIONS (self));
  g_return_if_fail (sort <= BZ_GNOME_EXTENSIONS_SORT_INSTALLED);

  stripped = g_strstrip (g_strdup (text != NULL ? text : ""));
  if (self->searched && self->sort == sort && g_strcmp0 (self->text, stripped) == 0 &&
      self->status != BZ_GNOME_EXTENSIONS_STATUS_ERROR)
    return;

  g_free (self->text);
  self->text     = g_steal_pointer (&stripped);
  self->sort     = sort;
  self->searched = TRUE;
  run_search (self);
}

void
bz_gnome_extensions_reload (BzGnomeExtensions *self)
{
  g_return_if_fail (BZ_IS_GNOME_EXTENSIONS (self));
  self->searched = TRUE;
  if (self->available)
    list_extensions (self);
  run_search (self);
}

void
bz_gnome_extensions_load_more (BzGnomeExtensions *self)
{
  g_return_if_fail (BZ_IS_GNOME_EXTENSIONS (self));

  if (self->loading_more || !bz_gnome_extensions_get_has_more (self))
    return;
  self->page++;
  set_loading_more (self, TRUE);
  fetch_page (self);
}

void
bz_gnome_extensions_ensure_screenshot (BzGnomeExtensions *self,
                                       BzGnomeExtension  *extension)
{
  g_return_if_fail (BZ_IS_GNOME_EXTENSIONS (self));
  g_return_if_fail (BZ_IS_GNOME_EXTENSION (extension));

  if (extension->screenshot_requested)
    return;
  extension->screenshot_requested = TRUE;
  fetch_texture (self, extension, TRUE);
}

/* --- Actions ----------------------------------------------------- */

typedef struct
{
  BzGnomeExtensions *self;
  BzGnomeExtension  *extension;
  char              *method;
} Call;

static void
call_free (Call *call)
{
  g_object_unref (call->self);
  g_object_unref (call->extension);
  g_free (call->method);
  g_free (call);
}

static void
call_cb (GObject      *source,
         GAsyncResult *result,
         gpointer      user_data)
{
  Call *call                     = user_data;
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) reply     = NULL;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &local_error);
  ext_set_busy (call->extension, FALSE);

  if (reply == NULL)
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_autofree char *remote = NULL;

          g_dbus_error_strip_remote_error (local_error);
          g_warning ("%s %s: %s", call->method, call->extension->uuid, local_error->message);
          remote = g_strdup_printf (_ ("GNOME Shell could not do this: %s"), local_error->message);
          ext_set_error (call->extension, remote);
        }
      /* Put a switch the user flipped back where it was. */
      ext_notify_state (call->extension);
    }
  else if (g_variant_is_of_type (reply, G_VARIANT_TYPE ("(b)")))
    {
      gboolean ok = FALSE;

      g_variant_get (reply, "(b)", &ok);
      if (!ok)
        {
          ext_set_error (call->extension, _ ("GNOME Shell could not do this."));
          ext_notify_state (call->extension);
        }
    }
  else if (g_variant_is_of_type (reply, G_VARIANT_TYPE ("(s)")))
    {
      const char *outcome = NULL;

      /* InstallRemoteExtension: "successful" or "cancelled". */
      g_variant_get (reply, "(&s)", &outcome);
      g_debug ("%s %s: %s", call->method, call->extension->uuid, outcome);
    }

  /* The state normally arrives with ExtensionStateChanged; ask anyway in
   * case a signal was missed. */
  if (call->self->available)
    list_extensions (call->self);
  call_free (call);
}

static void
call_shell (BzGnomeExtensions *self,
            BzGnomeExtension  *extension,
            const char        *method,
            GVariant          *parameters,
            int                timeout_msec)
{
  Call *call = NULL;

  g_return_if_fail (BZ_IS_GNOME_EXTENSIONS (self));
  g_return_if_fail (BZ_IS_GNOME_EXTENSION (extension));

  if (!self->available)
    {
      g_variant_unref (g_variant_ref_sink (parameters));
      ext_set_error (extension, _ ("GNOME Shell is not running."));
      return;
    }

  ext_set_error (extension, NULL);
  ext_set_busy (extension, TRUE);
  call            = g_new0 (Call, 1);
  call->self      = g_object_ref (self);
  call->extension = g_object_ref (extension);
  call->method    = g_strdup (method);
  g_dbus_proxy_call (self->proxy, method, parameters, G_DBUS_CALL_FLAGS_NONE,
                     timeout_msec, self->cancellable, call_cb, call);
}

void
bz_gnome_extensions_install (BzGnomeExtensions *self,
                             BzGnomeExtension  *extension)
{
  /* GNOME Shell asks the user first; wait for as long as that takes. */
  call_shell (self, extension, "InstallRemoteExtension",
              g_variant_new ("(s)", bz_gnome_extension_get_uuid (extension)), G_MAXINT);
}

void
bz_gnome_extensions_set_enabled (BzGnomeExtensions *self,
                                 BzGnomeExtension  *extension,
                                 gboolean           enabled)
{
  if (bz_gnome_extension_get_enabled (extension) == enabled)
    return;
  call_shell (self, extension, enabled ? "EnableExtension" : "DisableExtension",
              g_variant_new ("(s)", bz_gnome_extension_get_uuid (extension)), -1);
}

void
bz_gnome_extensions_uninstall (BzGnomeExtensions *self,
                               BzGnomeExtension  *extension)
{
  call_shell (self, extension, "UninstallExtension",
              g_variant_new ("(s)", bz_gnome_extension_get_uuid (extension)), -1);
}

void
bz_gnome_extensions_open_prefs (BzGnomeExtensions *self,
                                BzGnomeExtension  *extension,
                                const char        *parent_handle)
{
  call_shell (self, extension, "OpenExtensionPrefs",
              g_variant_new ("(ss@a{sv})", bz_gnome_extension_get_uuid (extension),
                             parent_handle != NULL ? parent_handle : "",
                             g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0)),
              -1);
}

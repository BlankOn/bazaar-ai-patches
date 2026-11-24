/* bz-gnome-extensions.h
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
 * BzGnomeExtensions browses extensions.gnome.org (its JSON query API) and
 * manages the user's GNOME Shell extensions through GNOME Shell's own D-Bus
 * service, org.gnome.Shell.Extensions: Shell downloads, verifies and asks
 * before installing (InstallRemoteExtension), so no extension file ever
 * passes through this code. Only libsoup, json-glib, GIO and GDK (for the
 * icon textures); no other Bazaar code, so a test program can use it.
 *
 * BzGnomeExtension is one extension: what extensions.gnome.org says about
 * it, and its state in GNOME Shell if it is installed.
 */

#pragma once

#include <gdk/gdk.h>

G_BEGIN_DECLS

/* GNOME Shell's ExtensionState (js/misc/extensionUtils.js). */
typedef enum
{
  BZ_GNOME_EXTENSION_STATE_NOT_INSTALLED = 0,
  BZ_GNOME_EXTENSION_STATE_ACTIVE        = 1,
  BZ_GNOME_EXTENSION_STATE_INACTIVE      = 2,
  BZ_GNOME_EXTENSION_STATE_ERROR         = 3,
  BZ_GNOME_EXTENSION_STATE_OUT_OF_DATE   = 4,
  BZ_GNOME_EXTENSION_STATE_DOWNLOADING   = 5,
  BZ_GNOME_EXTENSION_STATE_INITIALIZED   = 6,
  BZ_GNOME_EXTENSION_STATE_DEACTIVATING  = 7,
  BZ_GNOME_EXTENSION_STATE_ACTIVATING    = 8,
  BZ_GNOME_EXTENSION_STATE_UNINSTALLED   = 99,
} BzGnomeExtensionState;

typedef enum
{
  BZ_GNOME_EXTENSIONS_SORT_DOWNLOADS = 0, /* "Popular" */
  BZ_GNOME_EXTENSIONS_SORT_POPULARITY,    /* "Trending" */
  BZ_GNOME_EXTENSIONS_SORT_RECENT,
  BZ_GNOME_EXTENSIONS_SORT_INSTALLED,     /* the user's, from GNOME Shell */
} BzGnomeExtensionsSort;

/* What the list shows. */
typedef enum
{
  BZ_GNOME_EXTENSIONS_STATUS_LOADING = 0,
  BZ_GNOME_EXTENSIONS_STATUS_RESULTS,
  BZ_GNOME_EXTENSIONS_STATUS_EMPTY,
  BZ_GNOME_EXTENSIONS_STATUS_ERROR,
} BzGnomeExtensionsStatus;

#define BZ_TYPE_GNOME_EXTENSION (bz_gnome_extension_get_type ())
G_DECLARE_FINAL_TYPE (BzGnomeExtension, bz_gnome_extension, BZ, GNOME_EXTENSION, GObject)

const char *bz_gnome_extension_get_uuid (BzGnomeExtension *self);
const char *bz_gnome_extension_get_name (BzGnomeExtension *self);
const char *bz_gnome_extension_get_creator (BzGnomeExtension *self);
const char *bz_gnome_extension_get_description (BzGnomeExtension *self);
/* The extension's page on extensions.gnome.org, or NULL. */
const char *bz_gnome_extension_get_link (BzGnomeExtension *self);
/* Its own website (often a git repository), or NULL. */
const char *bz_gnome_extension_get_homepage (BzGnomeExtension *self);
/* NULL until loaded, or when it has none. */
GdkPaintable *bz_gnome_extension_get_icon (BzGnomeExtension *self);
GdkPaintable *bz_gnome_extension_get_screenshot (BzGnomeExtension *self);
gboolean      bz_gnome_extension_get_has_screenshot (BzGnomeExtension *self);
guint64       bz_gnome_extension_get_downloads (BzGnomeExtension *self);

BzGnomeExtensionState bz_gnome_extension_get_state (BzGnomeExtension *self);
gboolean              bz_gnome_extension_get_installed (BzGnomeExtension *self);
gboolean              bz_gnome_extension_get_enabled (BzGnomeExtension *self);
gboolean              bz_gnome_extension_get_has_prefs (BzGnomeExtension *self);
/* Installed for this user (not a system extension), so it can be removed. */
gboolean              bz_gnome_extension_get_can_uninstall (BzGnomeExtension *self);
/* Shell may change its state (FALSE when locked down by an administrator). */
gboolean              bz_gnome_extension_get_can_change (BzGnomeExtension *self);
/* "Enabled", "Disabled", "Error", "Incompatible", ...; NULL when not installed. */
const char           *bz_gnome_extension_get_status_label (BzGnomeExtension *self);
/* Installed and running. */
gboolean              bz_gnome_extension_get_active (BzGnomeExtension *self);
/* A D-Bus call for it is running. */
gboolean              bz_gnome_extension_get_busy (BzGnomeExtension *self);
/* The last failure (GNOME Shell's own error for an extension in error), or NULL. */
const char           *bz_gnome_extension_get_error (BzGnomeExtension *self);

#define BZ_TYPE_GNOME_EXTENSIONS (bz_gnome_extensions_get_type ())
G_DECLARE_FINAL_TYPE (BzGnomeExtensions, bz_gnome_extensions, BZ, GNOME_EXTENSIONS, GObject)

BzGnomeExtensions *bz_gnome_extensions_new (void);

/* GNOME Shell's extension service is on the session bus. */
gboolean bz_gnome_extensions_get_available (BzGnomeExtensions *self);
/* "50.4", or NULL while unavailable. */
const char *bz_gnome_extensions_get_shell_version (BzGnomeExtensions *self);
/* GNOME Shell's "Use Extensions" switch (UserExtensionsEnabled). */
gboolean bz_gnome_extensions_get_user_extensions_enabled (BzGnomeExtensions *self);
void     bz_gnome_extensions_set_user_extensions_enabled (BzGnomeExtensions *self,
                                                          gboolean           enabled);

/* The current results, a list of BzGnomeExtension. */
GListModel             *bz_gnome_extensions_get_results (BzGnomeExtensions *self);
BzGnomeExtensionsStatus bz_gnome_extensions_get_status (BzGnomeExtensions *self);
/* Another page is being fetched. */
gboolean                bz_gnome_extensions_get_loading_more (BzGnomeExtensions *self);
gboolean                bz_gnome_extensions_get_has_more (BzGnomeExtensions *self);
const char             *bz_gnome_extensions_get_error (BzGnomeExtensions *self);

/* Starts a new search (empty text lists everything) and replaces the results. */
void bz_gnome_extensions_search (BzGnomeExtensions    *self,
                                 const char           *text,
                                 BzGnomeExtensionsSort sort);
/* Repeats the last search (after an error). */
void bz_gnome_extensions_reload (BzGnomeExtensions *self);
/* Appends the next page of results, if there is one. */
void bz_gnome_extensions_load_more (BzGnomeExtensions *self);

/* Fetches the screenshot (only the details dialog shows it). */
void bz_gnome_extensions_ensure_screenshot (BzGnomeExtensions *self,
                                            BzGnomeExtension  *extension);
/* Asks GNOME Shell to install it: Shell shows its own confirmation. */
void bz_gnome_extensions_install (BzGnomeExtensions *self,
                                  BzGnomeExtension  *extension);
void bz_gnome_extensions_set_enabled (BzGnomeExtensions *self,
                                      BzGnomeExtension  *extension,
                                      gboolean           enabled);
void bz_gnome_extensions_uninstall (BzGnomeExtensions *self,
                                    BzGnomeExtension  *extension);
/* Opens its preferences window. parent_handle: an xdg-foreign handle or "". */
void bz_gnome_extensions_open_prefs (BzGnomeExtensions *self,
                                     BzGnomeExtension  *extension,
                                     const char        *parent_handle);

G_END_DECLS

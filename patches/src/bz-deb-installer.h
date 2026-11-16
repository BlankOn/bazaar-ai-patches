/* bz-deb-installer.h
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
 * Reads one local .deb file and installs it through PackageKit over plain
 * GDBus (no build dependency). No GTK and no other Bazaar code. Emits
 * "changed" whenever anything below changes.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum
{
  BZ_DEB_STATE_LOADING,    /* reading the file and checking dependencies */
  BZ_DEB_STATE_READY,      /* see the action; error is set after a failed install */
  BZ_DEB_STATE_BLOCKED,    /* cannot be installed; error says why (dependencies) */
  BZ_DEB_STATE_INSTALLING, /* status, progress, can-cancel */
  BZ_DEB_STATE_INSTALLED,  /* the install just finished */
  BZ_DEB_STATE_FAILED,     /* the file could not be read; error says why */
} BzDebState;

typedef enum
{
  BZ_DEB_ACTION_INSTALL,   /* not installed */
  BZ_DEB_ACTION_UPGRADE,   /* an older version is installed */
  BZ_DEB_ACTION_DOWNGRADE, /* a newer version is installed */
  BZ_DEB_ACTION_NONE,      /* this version is installed */
} BzDebAction;

/* Another package the install adds, upgrades or removes */
typedef struct
{
  char *name;
  char *version;
  char *summary;
} BzDebPackage;

#define BZ_TYPE_DEB_INSTALLER (bz_deb_installer_get_type ())
G_DECLARE_FINAL_TYPE (BzDebInstaller, bz_deb_installer, BZ, DEB_INSTALLER, GObject)

/* Starts reading the file right away. A file that is not on a local disk
 * (e.g. a network share) is copied to the user's cache first, so call this
 * while the file is still reachable. */
BzDebInstaller *
bz_deb_installer_new (GFile *file);

BzDebState
bz_deb_installer_get_state (BzDebInstaller *self);

BzDebAction
bz_deb_installer_get_action (BzDebInstaller *self);

/* The file's name, for titles while nothing else is known */
const char *
bz_deb_installer_get_file_name (BzDebInstaller *self);

/* Package fields; NULL (size 0) until known */
const char *
bz_deb_installer_get_name (BzDebInstaller *self);

const char *
bz_deb_installer_get_version (BzDebInstaller *self);

const char *
bz_deb_installer_get_summary (BzDebInstaller *self);

const char *
bz_deb_installer_get_description (BzDebInstaller *self);

const char *
bz_deb_installer_get_license (BzDebInstaller *self);

const char *
bz_deb_installer_get_url (BzDebInstaller *self);

/* Size once installed, in bytes */
guint64
bz_deb_installer_get_installed_size (BzDebInstaller *self);

/* Version of the package already installed, or NULL */
const char *
bz_deb_installer_get_installed_version (BzDebInstaller *self);

/* From the package's own launcher (.desktop file), when it has one:
 * its translated name, its icon (NULL if not found), and its desktop
 * file ID, which opens the app once installed */
const char *
bz_deb_installer_get_app_name (BzDebInstaller *self);

GIcon *
bz_deb_installer_get_icon (BzDebInstaller *self);

const char *
bz_deb_installer_get_desktop_id (BzDebInstaller *self);

/* Other packages the install adds or upgrades, and packages it removes:
 * GPtrArray of BzDebPackage */
GPtrArray *
bz_deb_installer_get_extra_packages (BzDebInstaller *self);

GPtrArray *
bz_deb_installer_get_removed_packages (BzDebInstaller *self);

const char *
bz_deb_installer_get_status (BzDebInstaller *self);

/* 0.0 - 1.0, or a negative value when unknown */
double
bz_deb_installer_get_progress (BzDebInstaller *self);

gboolean
bz_deb_installer_get_can_cancel (BzDebInstaller *self);

/* What went wrong, for people */
const char *
bz_deb_installer_get_error (BzDebInstaller *self);

/* The package manager's own message behind the error, or NULL */
const char *
bz_deb_installer_get_error_details (BzDebInstaller *self);

/* The finished install asked for a reboot */
gboolean
bz_deb_installer_get_restart_required (BzDebInstaller *self);

/* In state READY with an action other than NONE: install the file
 * (asks for authentication) */
void
bz_deb_installer_install (BzDebInstaller *self);

void
bz_deb_installer_cancel (BzDebInstaller *self);

G_END_DECLS

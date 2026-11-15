/* bz-apt-updates.h
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
 * Minimal PackageKit client used to list and apply system (APT/deb)
 * package updates. It talks to the PackageKit daemon over plain GDBus so
 * it adds no build dependency, and it does not depend on any other Bazaar
 * code, so it survives upstream refactors untouched.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* One pending system package update */
#define BZ_TYPE_APT_PACKAGE (bz_apt_package_get_type ())
G_DECLARE_FINAL_TYPE (BzAptPackage, bz_apt_package, BZ, APT_PACKAGE, GObject)

const char *
bz_apt_package_get_id (BzAptPackage *self);

const char *
bz_apt_package_get_name (BzAptPackage *self);

const char *
bz_apt_package_get_version (BzAptPackage *self);

const char *
bz_apt_package_get_summary (BzAptPackage *self);

gboolean
bz_apt_package_is_security (BzAptPackage *self);

/* PackageKit's GetUpdates also lists packages that a full upgrade newly
 * installs (e.g. renamed libraries) or removes; these are not updates. */
gboolean
bz_apt_package_is_new (BzAptPackage *self);

gboolean
bz_apt_package_is_removal (BzAptPackage *self);

/* The PackageKit client. Use bz_apt_updates_get_default () so every
 * widget shares the same state. */
#define BZ_TYPE_APT_UPDATES (bz_apt_updates_get_type ())
G_DECLARE_FINAL_TYPE (BzAptUpdates, bz_apt_updates, BZ, APT_UPDATES, GObject)

BzAptUpdates *
bz_apt_updates_get_default (void);

/* TRUE once PackageKit answered on the system bus */
gboolean
bz_apt_updates_get_available (BzAptUpdates *self);

/* TRUE once the first update list arrived, or once it is known that there
 * will be none (PackageKit unavailable, or the query failed). Lets a
 * process without a running main loop (the background update worker)
 * iterate the main context until the counts are valid. */
gboolean
bz_apt_updates_get_loaded (BzAptUpdates *self);

/* Human readable backend name, e.g. "Debian APT" */
const char *
bz_apt_updates_get_backend (BzAptUpdates *self);

/* GListModel of BzAptPackage */
GListModel *
bz_apt_updates_get_packages (BzAptUpdates *self);

guint
bz_apt_updates_get_n_security (BzAptUpdates *self);

/* The package list split by kind: upgrades of installed packages, new
 * packages pulled in by the upgrade, and packages it removes. Their sum is
 * the number of items in bz_apt_updates_get_packages (). */
guint
bz_apt_updates_get_n_upgrades (BzAptUpdates *self);

guint
bz_apt_updates_get_n_new (BzAptUpdates *self);

guint
bz_apt_updates_get_n_removals (BzAptUpdates *self);

/* TRUE while a user visible operation runs */
gboolean
bz_apt_updates_get_busy (BzAptUpdates *self);

const char *
bz_apt_updates_get_status (BzAptUpdates *self);

/* 0.0 - 1.0, or a negative value when unknown */
double
bz_apt_updates_get_progress (BzAptUpdates *self);

gboolean
bz_apt_updates_get_can_cancel (BzAptUpdates *self);

/* Last error message, or NULL */
const char *
bz_apt_updates_get_error (BzAptUpdates *self);

void
bz_apt_updates_clear_error (BzAptUpdates *self);

/* An offline update is downloaded and will be installed on next boot */
gboolean
bz_apt_updates_get_offline_triggered (BzAptUpdates *self);

/* A live update finished and asked for a reboot */
gboolean
bz_apt_updates_get_restart_required (BzAptUpdates *self);

/* Re-query the update list. When refresh_cache is TRUE the package lists
 * are downloaded first (like "apt update"). */
void
bz_apt_updates_check (BzAptUpdates *self,
                      gboolean      refresh_cache);

/* Download every update now and install it on next reboot (recommended) */
void
bz_apt_updates_prepare_offline (BzAptUpdates *self);

/* Install every update right now, in the running system */
void
bz_apt_updates_install_now (BzAptUpdates *self);

/* Cancel the running operation, if possible */
void
bz_apt_updates_cancel (BzAptUpdates *self);

/* Un-schedule a prepared offline update */
void
bz_apt_updates_cancel_offline (BzAptUpdates *self);

/* Ask the session (or logind) to reboot the machine */
void
bz_apt_updates_reboot (BzAptUpdates *self);

G_END_DECLS

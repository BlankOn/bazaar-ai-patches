/* bz-update-notifier.h
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
 * "Updates available" desktop notifications for both Flatpak apps and APT
 * system packages, sent from Bazaar's background update worker.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Called by the background update worker (update-worker.c) on every run,
 * i.e. about once an hour while bazaar-daemon is alive. Sends at most one
 * notification a day, and only when updates are pending.
 *
 * flatpak_auto_updated: TRUE when upstream's automatic updates are on. Then
 * Flatpak updates get installed (and announced) by upstream, so only APT
 * updates are counted here.
 *
 * Synchronous; iterates the thread-default main context while waiting for
 * PackageKit. */
void
bz_update_notifier_run (GSettings *settings,
                        gboolean   flatpak_auto_updated);

/* Adds "app.open-library" to the application: presents a window and shows
 * its Library page. The notification's button and default action use it. */
void
bz_update_notifier_add_app_actions (GApplication *app);

G_END_DECLS

/* bz-deb-install-dialog.h
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
 * ai-patches/PLAN.md §12 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define BZ_TYPE_DEB_INSTALL_DIALOG (bz_deb_install_dialog_get_type ())
G_DECLARE_FINAL_TYPE (BzDebInstallDialog, bz_deb_install_dialog, BZ, DEB_INSTALL_DIALOG, AdwDialog)

/* TRUE for a file name (or URI) that names a Debian package (.deb) */
gboolean
bz_deb_install_dialog_handles (const char *name);

/* Reads the .deb file and presents the dialog over parent */
void
bz_deb_install_dialog_open (GFile     *file,
                            GtkWidget *parent);

G_END_DECLS

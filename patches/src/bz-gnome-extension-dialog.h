/* bz-gnome-extension-dialog.h
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

/* DOWNSTREAM PATCH (GNOME Extensions): one extension's details, with
 * Install, Enabled, Settings and Remove. See ai-patches/PLAN.md §13. */

#pragma once

#include <adwaita.h>

#include "bz-gnome-extensions.h"

G_BEGIN_DECLS

#define BZ_TYPE_GNOME_EXTENSION_DIALOG (bz_gnome_extension_dialog_get_type ())
G_DECLARE_FINAL_TYPE (BzGnomeExtensionDialog, bz_gnome_extension_dialog, BZ, GNOME_EXTENSION_DIALOG, AdwDialog)

AdwDialog *bz_gnome_extension_dialog_new (BzGnomeExtensions *extensions,
                                          BzGnomeExtension  *extension);

G_END_DECLS

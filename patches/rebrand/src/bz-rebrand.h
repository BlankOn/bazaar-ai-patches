/* bz-rebrand.h
 *
 * Copyright 2026 BlankOn Linux
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

/* DOWNSTREAM PATCH (Warsi rebrand): see ai-patches/PLAN.md, "Rebrand".
 *
 * Code the rebrand adds to Bazaar. Everything else in the rebrand is
 * renaming, done by ai-patches/patches/rebrand/rebrand.py. */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Adds "Originally Bazaar, heavily patched by BlankOn Linux" to the About
 * dialog, under the application name and above the developer name, so the
 * Bazaar section below it (developer, website, issues, credits, legal)
 * stays as upstream has it. Call after the dialog's properties are set. */
void
bz_rebrand_about_dialog (AdwAboutDialog *dialog);

G_END_DECLS

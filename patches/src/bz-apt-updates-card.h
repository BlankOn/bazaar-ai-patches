/* bz-apt-updates-card.h
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
 * Self-contained "System Updates" card. It hides itself unless PackageKit
 * is reachable, so it is safe to place anywhere in the UI.
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define BZ_TYPE_APT_UPDATES_CARD (bz_apt_updates_card_get_type ())
G_DECLARE_FINAL_TYPE (BzAptUpdatesCard, bz_apt_updates_card, BZ, APT_UPDATES_CARD, AdwBin)

GtkWidget *
bz_apt_updates_card_new (void);

G_END_DECLS

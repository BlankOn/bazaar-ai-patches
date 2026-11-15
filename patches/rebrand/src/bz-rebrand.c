/* bz-rebrand.c
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

/* DOWNSTREAM PATCH (Warsi rebrand): see ai-patches/PLAN.md, "Rebrand". */

#define G_LOG_DOMAIN "BAZAAR::REBRAND"

#include "config.h"

#include <glib/gi18n.h>

#include "bz-rebrand.h"

/* First GtkLabel under widget (depth first) showing exactly text. */
static GtkWidget *
find_label (GtkWidget  *widget,
            const char *text)
{
  if (GTK_IS_LABEL (widget) &&
      g_strcmp0 (gtk_label_get_label (GTK_LABEL (widget)), text) == 0)
    return widget;

  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = NULL;

      found = find_label (child, text);
      if (found != NULL)
        return found;
    }

  return NULL;
}

void
bz_rebrand_about_dialog (AdwAboutDialog *dialog)
{
  const char *note       = NULL;
  GtkWidget  *name_label = NULL;
  GtkWidget  *parent     = NULL;
  GtkWidget  *label      = NULL;

  g_return_if_fail (ADW_IS_ABOUT_DIALOG (dialog));

  // Translators: Please treat "Bazaar" and "BlankOn" as brand names and avoid translating.
  note = _ ("Originally Bazaar, patched by BlankOn Linux");

  /* AdwAboutDialog has no slot for a line under the name, so find the name
   * label in its header (icon, name, developer, version) and add one after
   * it. If libadwaita ever changes that layout, fall back to the comments,
   * which it shows on a "Details" page. */
  /* Search the dialog's child: until it is presented, an AdwDialog itself
   * has no child widgets. */
  name_label = find_label (adw_dialog_get_child (ADW_DIALOG (dialog)),
                           adw_about_dialog_get_application_name (dialog));
  if (name_label != NULL)
    parent = gtk_widget_get_parent (name_label);

  if (parent == NULL || !GTK_IS_BOX (parent))
    {
      g_debug ("About dialog layout not recognised, using the comments");
      adw_about_dialog_set_comments (dialog, note);
      return;
    }

  label = gtk_label_new (note);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (label, "dimmed");
  gtk_box_insert_child_after (GTK_BOX (parent), label, name_label);
}

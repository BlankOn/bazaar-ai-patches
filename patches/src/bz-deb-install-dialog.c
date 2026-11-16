/* bz-deb-install-dialog.c
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
 * ai-patches/PLAN.md §12. A pure view of BzDebInstaller. */

#define G_LOG_DOMAIN "BAZAAR::DEB"

#include "config.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

#include "bz-apt-updates.h"
#include "bz-deb-install-dialog.h"
#include "bz-deb-installer.h"

/* Listing hundreds of rows is slow and useless; show the first few */
#define MAX_PACKAGE_ROWS 100

struct _BzDebInstallDialog
{
  AdwDialog parent_instance;

  BzDebInstaller *installer;
  GPtrArray      *package_rows;
  guint           pulse_id;
  gboolean        rows_built;

  /* Template widgets */
  GtkImage          *icon_image;
  GtkLabel          *title_label;
  GtkLabel          *summary_label;
  AdwSpinner        *spinner;
  GtkButton         *action_button;
  GtkLabel          *state_label;
  GtkBox            *progress_box;
  GtkProgressBar    *progress_bar;
  GtkLabel          *status_label;
  GtkButton         *cancel_button;
  GtkListBox        *message_list;
  AdwActionRow      *error_row;
  AdwExpanderRow    *error_details_row;
  GtkLabel          *error_details_label;
  AdwActionRow      *restart_row;
  AdwActionRow      *trust_row;
  AdwPreferencesGroup *details_group;
  AdwActionRow      *package_row;
  AdwActionRow      *version_row;
  AdwActionRow      *installed_row;
  AdwActionRow      *size_row;
  AdwActionRow      *license_row;
  AdwActionRow      *website_row;
  AdwExpanderRow    *extra_row;
  AdwExpanderRow    *removed_row;
  AdwPreferencesGroup *description_group;
  GtkLabel          *description_label;
};

G_DEFINE_FINAL_TYPE (BzDebInstallDialog, bz_deb_install_dialog, ADW_TYPE_DIALOG)

static void
sync_ui (BzDebInstallDialog *self);

/* ------------------------------------------------------------------------ */

static GDesktopAppInfo *
dup_app_info (BzDebInstallDialog *self)
{
  const char *desktop_id = bz_deb_installer_get_desktop_id (self->installer);
  g_autofree char *path  = NULL;
  GDesktopAppInfo *info  = NULL;

  if (desktop_id == NULL)
    return NULL;

  info = g_desktop_app_info_new (desktop_id);
  if (info != NULL)
    return info;

  /* GIO may not have noticed the new launcher yet */
  path = g_build_filename ("/usr/share/applications", desktop_id, NULL);
  return g_desktop_app_info_new_from_filename (path);
}

static void
set_row (AdwActionRow *row,
         const char   *value)
{
  gtk_widget_set_visible (GTK_WIDGET (row), value != NULL && *value != '\0');
  adw_action_row_set_subtitle (row, value != NULL ? value : "");
}

static void
add_package_rows (BzDebInstallDialog *self,
                  AdwExpanderRow     *expander,
                  GPtrArray          *packages)
{
  for (guint i = 0; i < packages->len && i < MAX_PACKAGE_ROWS; i++)
    {
      BzDebPackage    *package = g_ptr_array_index (packages, i);
      GtkWidget       *row     = adw_action_row_new ();
      g_autofree char *title   = g_strdup_printf ("%s %s", package->name, package->version);

      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), package->summary != NULL ? package->summary : "");
      adw_expander_row_add_row (expander, row);
      g_ptr_array_add (self->package_rows, row);
    }

  if (packages->len > MAX_PACKAGE_ROWS)
    {
      GtkWidget       *row   = adw_action_row_new ();
      g_autofree char *title = NULL;
      guint            more  = packages->len - MAX_PACKAGE_ROWS;

      title = g_strdup_printf (ngettext ("And %u more package", "And %u more packages", more), more);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_expander_row_add_row (expander, row);
      g_ptr_array_add (self->package_rows, row);
    }
}

static void
build_package_rows (BzDebInstallDialog *self)
{
  GPtrArray *extra   = bz_deb_installer_get_extra_packages (self->installer);
  GPtrArray *removed = bz_deb_installer_get_removed_packages (self->installer);

  if (self->rows_built)
    return;
  self->rows_built = TRUE;

  if (extra->len > 0)
    {
      g_autofree char *title = g_strdup_printf (
          ngettext ("Also installs or updates %u other package",
                    "Also installs or updates %u other packages", extra->len),
          extra->len);

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->extra_row), title);
      add_package_rows (self, self->extra_row, extra);
      gtk_widget_set_visible (GTK_WIDGET (self->extra_row), TRUE);
    }

  if (removed->len > 0)
    {
      g_autofree char *title = g_strdup_printf (
          ngettext ("Removes %u package", "Removes %u packages", removed->len),
          removed->len);

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->removed_row), title);
      add_package_rows (self, self->removed_row, removed);
      gtk_widget_set_visible (GTK_WIDGET (self->removed_row), TRUE);
      adw_expander_row_set_expanded (self->removed_row, TRUE);
    }
}

static gboolean
pulse_cb (gpointer user_data)
{
  BzDebInstallDialog *self = user_data;

  gtk_progress_bar_pulse (self->progress_bar);
  return G_SOURCE_CONTINUE;
}

static void
set_pulsing (BzDebInstallDialog *self,
             gboolean            pulsing)
{
  if (pulsing && self->pulse_id == 0)
    self->pulse_id = g_timeout_add (150, pulse_cb, self);
  else if (!pulsing)
    g_clear_handle_id (&self->pulse_id, g_source_remove);
}

static void
sync_icon (BzDebInstallDialog *self)
{
  GIcon *icon = bz_deb_installer_get_icon (self->installer);

  if (icon != NULL && G_IS_THEMED_ICON (icon) &&
      !gtk_icon_theme_has_gicon (gtk_icon_theme_get_for_display (gtk_widget_get_display (GTK_WIDGET (self))), icon))
    icon = NULL;

  if (icon != NULL)
    gtk_image_set_from_gicon (self->icon_image, icon);
  else
    gtk_image_set_from_icon_name (self->icon_image, "package-x-generic");
}

static void
sync_ui (BzDebInstallDialog *self)
{
  BzDebInstaller  *installer   = self->installer;
  BzDebState       state       = bz_deb_installer_get_state (installer);
  BzDebAction      action      = bz_deb_installer_get_action (installer);
  const char      *name        = bz_deb_installer_get_name (installer);
  const char      *app_name    = bz_deb_installer_get_app_name (installer);
  const char      *version     = bz_deb_installer_get_version (installer);
  const char      *installed   = bz_deb_installer_get_installed_version (installer);
  const char      *summary     = bz_deb_installer_get_summary (installer);
  const char      *description = bz_deb_installer_get_description (installer);
  const char      *error       = bz_deb_installer_get_error (installer);
  const char      *details     = bz_deb_installer_get_error_details (installer);
  guint64          size        = bz_deb_installer_get_installed_size (installer);
  double           progress    = bz_deb_installer_get_progress (installer);
  g_autofree char *size_text   = NULL;
  g_autofree char *state_text  = NULL;
  const char      *button      = NULL;
  const char      *error_title = NULL;
  gboolean         known       = name != NULL;

  /* Header */
  gtk_label_set_label (self->title_label,
                       app_name != NULL ? app_name
                       : name != NULL   ? name
                                        : bz_deb_installer_get_file_name (installer));
  gtk_label_set_label (self->summary_label, summary != NULL ? summary : "");
  gtk_widget_set_visible (GTK_WIDGET (self->summary_label), summary != NULL);
  sync_icon (self);

  /* Main button and the line under it */
  switch (state)
    {
    case BZ_DEB_STATE_READY:
      switch (action)
        {
        case BZ_DEB_ACTION_INSTALL:
          button = _ ("Install");
          break;
        case BZ_DEB_ACTION_UPGRADE:
          button     = _ ("Update");
          state_text = g_strdup_printf (_ ("Version %s is installed"), installed);
          break;
        case BZ_DEB_ACTION_DOWNGRADE:
          button     = _ ("Install Older Version");
          state_text = g_strdup_printf (_ ("A newer version, %s, is installed"), installed);
          break;
        case BZ_DEB_ACTION_NONE:
        default:
          state_text = g_strdup (_ ("This version is installed"));
          break;
        }
      if (error != NULL)
        error_title = _ ("Installation Failed");
      break;
    case BZ_DEB_STATE_INSTALLED:
      state_text = g_strdup (_ ("Installed"));
      break;
    case BZ_DEB_STATE_BLOCKED:
      error_title = _ ("This Package Cannot Be Installed");
      break;
    case BZ_DEB_STATE_FAILED:
      error_title = _ ("Cannot Open This Package");
      break;
    case BZ_DEB_STATE_LOADING:
    case BZ_DEB_STATE_INSTALLING:
    default:
      break;
    }

  /* An installed app can be opened */
  if (button == NULL &&
      (state == BZ_DEB_STATE_INSTALLED ||
       (state == BZ_DEB_STATE_READY && action == BZ_DEB_ACTION_NONE)))
    {
      g_autoptr (GDesktopAppInfo) info = dup_app_info (self);

      if (info != NULL)
        button = _ ("Open");
    }

  gtk_widget_set_visible (GTK_WIDGET (self->spinner), state == BZ_DEB_STATE_LOADING);
  gtk_widget_set_visible (GTK_WIDGET (self->action_button), button != NULL);
  if (button != NULL)
    gtk_button_set_label (self->action_button, button);
  gtk_widget_set_visible (GTK_WIDGET (self->state_label), state_text != NULL);
  gtk_label_set_label (self->state_label, state_text != NULL ? state_text : "");

  /* Progress */
  gtk_widget_set_visible (GTK_WIDGET (self->progress_box), state == BZ_DEB_STATE_INSTALLING);
  if (state == BZ_DEB_STATE_INSTALLING)
    {
      const char *status = bz_deb_installer_get_status (installer);

      gtk_label_set_label (self->status_label, status != NULL ? status : "");
      gtk_widget_set_sensitive (GTK_WIDGET (self->cancel_button),
                                bz_deb_installer_get_can_cancel (installer));
      if (progress >= 0.0)
        gtk_progress_bar_set_fraction (self->progress_bar, progress);
      set_pulsing (self, progress < 0.0);
    }
  else
    set_pulsing (self, FALSE);

  /* A running install keeps the dialog open */
  adw_dialog_set_can_close (ADW_DIALOG (self), state != BZ_DEB_STATE_INSTALLING);

  /* Messages */
  gtk_widget_set_visible (GTK_WIDGET (self->error_row), error_title != NULL);
  if (error_title != NULL)
    {
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->error_row), error_title);
      adw_action_row_set_subtitle (self->error_row, error != NULL ? error : "");
    }
  gtk_widget_set_visible (GTK_WIDGET (self->error_details_row), error_title != NULL && details != NULL);
  gtk_label_set_label (self->error_details_label, details != NULL ? details : "");
  gtk_widget_set_visible (GTK_WIDGET (self->restart_row),
                          state == BZ_DEB_STATE_INSTALLED &&
                              bz_deb_installer_get_restart_required (installer));
  gtk_widget_set_visible (GTK_WIDGET (self->trust_row),
                          state == BZ_DEB_STATE_READY && action != BZ_DEB_ACTION_NONE);
  gtk_widget_set_visible (GTK_WIDGET (self->message_list),
                          gtk_widget_get_visible (GTK_WIDGET (self->error_row)) ||
                              gtk_widget_get_visible (GTK_WIDGET (self->restart_row)) ||
                              gtk_widget_get_visible (GTK_WIDGET (self->trust_row)));

  /* Details */
  gtk_widget_set_visible (GTK_WIDGET (self->details_group), known);
  set_row (self->package_row, name);
  set_row (self->version_row, version);
  set_row (self->installed_row,
           g_strcmp0 (installed, version) != 0 ? installed : NULL);
  if (size > 0)
    size_text = g_format_size (size);
  set_row (self->size_row, size_text);
  set_row (self->license_row, bz_deb_installer_get_license (installer));
  set_row (self->website_row, bz_deb_installer_get_url (installer));

  if (state == BZ_DEB_STATE_READY || state == BZ_DEB_STATE_INSTALLING)
    build_package_rows (self);

  gtk_widget_set_visible (GTK_WIDGET (self->description_group), description != NULL);
  gtk_label_set_label (self->description_label, description != NULL ? description : "");
}

/* ------------------------------------------------------------------------ */
/* Template callbacks                                                       */
/* ------------------------------------------------------------------------ */

static void
action_cb (BzDebInstallDialog *self,
           GtkButton          *button)
{
  BzDebState state = bz_deb_installer_get_state (self->installer);

  if (state == BZ_DEB_STATE_READY &&
      bz_deb_installer_get_action (self->installer) != BZ_DEB_ACTION_NONE)
    {
      bz_deb_installer_install (self->installer);
      return;
    }

  /* Open */
  {
    g_autoptr (GDesktopAppInfo) info         = dup_app_info (self);
    g_autoptr (GdkAppLaunchContext) context  = NULL;
    g_autoptr (GError) local_error           = NULL;

    if (info == NULL)
      return;

    context = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (self)));
    if (!g_app_info_launch (G_APP_INFO (info), NULL, G_APP_LAUNCH_CONTEXT (context), &local_error))
      {
        g_warning ("Could not open %s: %s",
                   bz_deb_installer_get_desktop_id (self->installer), local_error->message);
        return;
      }
    adw_dialog_close (ADW_DIALOG (self));
  }
}

static void
cancel_cb (BzDebInstallDialog *self,
           GtkButton          *button)
{
  bz_deb_installer_cancel (self->installer);
}

static void
restart_cb (BzDebInstallDialog *self,
            GtkButton          *button)
{
  bz_apt_updates_reboot (bz_apt_updates_get_default ());
}

static void
website_cb (BzDebInstallDialog *self,
            AdwActionRow       *row)
{
  g_autoptr (GtkUriLauncher) launcher = NULL;
  const char *url                     = bz_deb_installer_get_url (self->installer);

  if (url == NULL)
    return;

  launcher = gtk_uri_launcher_new (url);
  gtk_uri_launcher_launch (launcher,
                           GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))),
                           NULL, NULL, NULL);
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                  */
/* ------------------------------------------------------------------------ */

static void
bz_deb_install_dialog_dispose (GObject *object)
{
  BzDebInstallDialog *self = BZ_DEB_INSTALL_DIALOG (object);

  set_pulsing (self, FALSE);
  if (self->installer != NULL)
    g_signal_handlers_disconnect_by_data (self->installer, self);
  g_clear_object (&self->installer);
  g_clear_pointer (&self->package_rows, g_ptr_array_unref);

  gtk_widget_dispose_template (GTK_WIDGET (self), BZ_TYPE_DEB_INSTALL_DIALOG);

  G_OBJECT_CLASS (bz_deb_install_dialog_parent_class)->dispose (object);
}

static void
bz_deb_install_dialog_class_init (BzDebInstallDialogClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = bz_deb_install_dialog_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/io/github/kolunmi/Bazaar/bz-deb-install-dialog.ui");
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, icon_image);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, title_label);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, summary_label);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, spinner);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, action_button);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, state_label);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, progress_box);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, progress_bar);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, status_label);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, message_list);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, error_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, error_details_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, error_details_label);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, restart_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, trust_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, details_group);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, package_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, version_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, installed_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, size_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, license_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, website_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, extra_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, removed_row);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, description_group);
  gtk_widget_class_bind_template_child (widget_class, BzDebInstallDialog, description_label);
  gtk_widget_class_bind_template_callback (widget_class, action_cb);
  gtk_widget_class_bind_template_callback (widget_class, cancel_cb);
  gtk_widget_class_bind_template_callback (widget_class, restart_cb);
  gtk_widget_class_bind_template_callback (widget_class, website_cb);
}

static void
bz_deb_install_dialog_init (BzDebInstallDialog *self)
{
  g_type_ensure (ADW_TYPE_SPINNER);
  gtk_widget_init_template (GTK_WIDGET (self));

  self->package_rows = g_ptr_array_new ();
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

gboolean
bz_deb_install_dialog_handles (const char *name)
{
  g_autofree char *lower = NULL;

  if (name == NULL)
    return FALSE;

  lower = g_ascii_strdown (name, -1);
  return g_str_has_suffix (lower, ".deb");
}

void
bz_deb_install_dialog_open (GFile     *file,
                            GtkWidget *parent)
{
  BzDebInstallDialog *self = NULL;

  g_return_if_fail (G_IS_FILE (file));

  self            = g_object_new (BZ_TYPE_DEB_INSTALL_DIALOG, NULL);
  self->installer = bz_deb_installer_new (file);
  g_signal_connect_swapped (self->installer, "changed", G_CALLBACK (sync_ui), self);
  sync_ui (self);

  adw_dialog_present (ADW_DIALOG (self), parent);
}

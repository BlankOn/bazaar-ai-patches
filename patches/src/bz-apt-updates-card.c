/* bz-apt-updates-card.c
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

/* DOWNSTREAM PATCH (APT system updates): see ai-patches/PLAN.md */

#define G_LOG_DOMAIN "BAZAAR::APT"

#include "config.h"

#include <glib/gi18n.h>

#include "bz-apt-updates-card.h"
#include "bz-apt-updates.h"

/* Listing thousands of rows is slow and useless; show the first few */
#define MAX_PACKAGE_ROWS 100

struct _BzAptUpdatesCard
{
  AdwBin parent_instance;

  BzAptUpdates       *updates;
  GPtrArray          *package_rows;
  GSimpleActionGroup *actions;
  guint               pulse_id;

  /* Template widgets */
  AdwActionRow   *progress_row;
  GtkProgressBar *progress_bar;
  GtkButton      *cancel_button;
  AdwActionRow   *error_row;
  AdwActionRow   *ready_row;
  AdwActionRow   *restart_row;
  AdwExpanderRow *expander_row;
  GtkButton      *refresh_button;
  AdwSplitButton *update_button;
};

G_DEFINE_FINAL_TYPE (BzAptUpdatesCard, bz_apt_updates_card, ADW_TYPE_BIN)

static void
sync_ui (BzAptUpdatesCard *self);

static void
rebuild_rows (BzAptUpdatesCard *self);

static void
updates_notify_cb (BzAptUpdatesCard *self,
                   GParamSpec       *pspec,
                   BzAptUpdates     *updates);

static void
packages_changed_cb (BzAptUpdatesCard *self,
                     guint             position,
                     guint             removed,
                     guint             added,
                     GListModel       *model);

static void
install_now_action (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data);

static void
check_action (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data);

static void
bz_apt_updates_card_dispose (GObject *object)
{
  BzAptUpdatesCard *self = BZ_APT_UPDATES_CARD (object);

  if (self->updates != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->updates, self);
      g_signal_handlers_disconnect_by_data (bz_apt_updates_get_packages (self->updates), self);
    }

  g_clear_handle_id (&self->pulse_id, g_source_remove);
  g_clear_object (&self->updates);
  g_clear_object (&self->actions);
  g_clear_pointer (&self->package_rows, g_ptr_array_unref);

  G_OBJECT_CLASS (bz_apt_updates_card_parent_class)->dispose (object);
}

static void
cancel_cb (BzAptUpdatesCard *self,
           GtkButton        *button)
{
  bz_apt_updates_cancel (self->updates);
}

static void
dismiss_error_cb (BzAptUpdatesCard *self,
                  GtkButton        *button)
{
  bz_apt_updates_clear_error (self->updates);
}

static void
cancel_offline_cb (BzAptUpdatesCard *self,
                   GtkButton        *button)
{
  bz_apt_updates_cancel_offline (self->updates);
}

static void
reboot_cb (BzAptUpdatesCard *self,
           GtkButton        *button)
{
  bz_apt_updates_reboot (self->updates);
}

static void
check_cb (BzAptUpdatesCard *self,
          GtkButton        *button)
{
  bz_apt_updates_check (self->updates, TRUE);
}

static void
prepare_offline_cb (BzAptUpdatesCard *self,
                    AdwSplitButton   *button)
{
  bz_apt_updates_prepare_offline (self->updates);
}

static void
bz_apt_updates_card_class_init (BzAptUpdatesCardClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = bz_apt_updates_card_dispose;

  g_type_ensure (BZ_TYPE_APT_UPDATES);

  gtk_widget_class_set_template_from_resource (widget_class, "/io/github/kolunmi/Bazaar/bz-apt-updates-card.ui");
  gtk_widget_class_bind_template_child (widget_class, BzAptUpdatesCard, progress_row);
  gtk_widget_class_bind_template_child (widget_class, BzAptUpdatesCard, progress_bar);
  gtk_widget_class_bind_template_child (widget_class, BzAptUpdatesCard, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, BzAptUpdatesCard, error_row);
  gtk_widget_class_bind_template_child (widget_class, BzAptUpdatesCard, ready_row);
  gtk_widget_class_bind_template_child (widget_class, BzAptUpdatesCard, restart_row);
  gtk_widget_class_bind_template_child (widget_class, BzAptUpdatesCard, expander_row);
  gtk_widget_class_bind_template_child (widget_class, BzAptUpdatesCard, refresh_button);
  gtk_widget_class_bind_template_child (widget_class, BzAptUpdatesCard, update_button);
  gtk_widget_class_bind_template_callback (widget_class, cancel_cb);
  gtk_widget_class_bind_template_callback (widget_class, dismiss_error_cb);
  gtk_widget_class_bind_template_callback (widget_class, cancel_offline_cb);
  gtk_widget_class_bind_template_callback (widget_class, reboot_cb);
  gtk_widget_class_bind_template_callback (widget_class, check_cb);
  gtk_widget_class_bind_template_callback (widget_class, prepare_offline_cb);
}

static void
bz_apt_updates_card_init (BzAptUpdatesCard *self)
{
  static const GActionEntry entries[] = {
    { "install-now", install_now_action, NULL, NULL, NULL, { 0 } },
    {       "check",       check_action, NULL, NULL, NULL, { 0 } },
  };

  gtk_widget_init_template (GTK_WIDGET (self));

  self->package_rows = g_ptr_array_new ();

  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions),
                                   entries, G_N_ELEMENTS (entries), self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "apt-updates",
                                  G_ACTION_GROUP (self->actions));

  self->updates = g_object_ref (bz_apt_updates_get_default ());
  g_signal_connect_swapped (self->updates, "notify",
                            G_CALLBACK (updates_notify_cb), self);
  g_signal_connect_swapped (bz_apt_updates_get_packages (self->updates), "items-changed",
                            G_CALLBACK (packages_changed_cb), self);

  rebuild_rows (self);
  sync_ui (self);
}

GtkWidget *
bz_apt_updates_card_new (void)
{
  return g_object_new (BZ_TYPE_APT_UPDATES_CARD, NULL);
}

static gboolean
pulse_cb (BzAptUpdatesCard *self)
{
  gtk_progress_bar_pulse (self->progress_bar);
  return G_SOURCE_CONTINUE;
}

static void
set_action_enabled (BzAptUpdatesCard *self,
                    const char       *name,
                    gboolean          enabled)
{
  GAction *action = NULL;

  action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), name);
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

static void
sync_ui (BzAptUpdatesCard *self)
{
  gboolean    available         = FALSE;
  gboolean    busy              = FALSE;
  gboolean    offline_triggered = FALSE;
  gboolean    restart_required  = FALSE;
  const char *error             = NULL;
  const char *status            = NULL;
  const char *backend           = NULL;
  double      progress          = 0.0;
  guint       n_updates         = 0;
  guint       n_upgrades        = 0;
  guint       n_new             = 0;
  guint       n_removals        = 0;
  guint       n_security        = 0;

  available         = bz_apt_updates_get_available (self->updates);
  busy              = bz_apt_updates_get_busy (self->updates);
  offline_triggered = bz_apt_updates_get_offline_triggered (self->updates);
  restart_required  = bz_apt_updates_get_restart_required (self->updates);
  error             = bz_apt_updates_get_error (self->updates);
  status            = bz_apt_updates_get_status (self->updates);
  backend           = bz_apt_updates_get_backend (self->updates);
  progress          = bz_apt_updates_get_progress (self->updates);
  n_updates         = g_list_model_get_n_items (bz_apt_updates_get_packages (self->updates));
  n_upgrades        = bz_apt_updates_get_n_upgrades (self->updates);
  n_new             = bz_apt_updates_get_n_new (self->updates);
  n_removals        = bz_apt_updates_get_n_removals (self->updates);
  n_security        = bz_apt_updates_get_n_security (self->updates);

  gtk_widget_set_visible (GTK_WIDGET (self), available);

  /* Progress */
  gtk_widget_set_visible (GTK_WIDGET (self->progress_row), busy);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->progress_row),
                                 status != NULL ? status : _ ("Working"));
  gtk_widget_set_sensitive (GTK_WIDGET (self->cancel_button),
                            bz_apt_updates_get_can_cancel (self->updates));
  if (busy && progress < 0.0)
    {
      if (self->pulse_id == 0)
        self->pulse_id = g_timeout_add (150, (GSourceFunc) pulse_cb, self);
    }
  else
    {
      g_clear_handle_id (&self->pulse_id, g_source_remove);
      gtk_progress_bar_set_fraction (self->progress_bar, MAX (progress, 0.0));
    }

  /* Error */
  gtk_widget_set_visible (GTK_WIDGET (self->error_row), error != NULL);
  adw_action_row_set_subtitle (self->error_row, error != NULL ? error : "");

  /* Offline update scheduled / restart needed */
  gtk_widget_set_visible (GTK_WIDGET (self->ready_row), offline_triggered);
  gtk_widget_set_visible (GTK_WIDGET (self->restart_row), restart_required && !offline_triggered);

  /* Update list */
  gtk_widget_set_visible (GTK_WIDGET (self->expander_row), !offline_triggered);
  gtk_widget_set_sensitive (GTK_WIDGET (self->refresh_button), !busy);
  gtk_widget_set_visible (GTK_WIDGET (self->update_button), n_updates > 0);
  gtk_widget_set_sensitive (GTK_WIDGET (self->update_button), !busy);
  if (adw_expander_row_get_enable_expansion (self->expander_row) != (n_updates > 0))
    {
      adw_expander_row_set_enable_expansion (self->expander_row, n_updates > 0);
      /* enabling expansion also expands the row; start collapsed instead */
      adw_expander_row_set_expanded (self->expander_row, FALSE);
    }

  if (n_updates > 0)
    {
      g_autofree char *title    = NULL;
      g_autoptr (GPtrArray) parts = NULL;
      g_autofree char *subtitle = NULL;

      /* The title counts real updates only. New and removed packages are
       * part of the upgrade but are not updates, so they go in the
       * subtitle; removals are also listed first in the expander. */
      if (n_upgrades > 0)
        title = g_strdup_printf (ngettext ("%u System Package Update",
                                           "%u System Package Updates",
                                           n_upgrades),
                                 n_upgrades);
      else
        title = g_strdup (_ ("System Package Changes Pending"));
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->expander_row), title);

      parts = g_ptr_array_new_with_free_func (g_free);
      if (n_security > 0)
        g_ptr_array_add (parts, g_strdup_printf (ngettext ("Includes %u security update",
                                                           "Includes %u security updates",
                                                           n_security),
                                                 n_security));
      else if (backend != NULL && n_new == 0 && n_removals == 0)
        /* Translators: %s is the package manager, e.g. "APT" */
        g_ptr_array_add (parts, g_strdup_printf (_ ("Provided by %s"), backend));
      if (n_new > 0)
        g_ptr_array_add (parts, g_strdup_printf (ngettext ("%u new package",
                                                           "%u new packages",
                                                           n_new),
                                                 n_new));
      if (n_removals > 0)
        g_ptr_array_add (parts, g_strdup_printf (ngettext ("%u package will be removed",
                                                           "%u packages will be removed",
                                                           n_removals),
                                                 n_removals));
      g_ptr_array_add (parts, NULL);

      /* Translators: separator between subtitle parts, e.g. "3 new packages · 1 package will be removed" */
      subtitle = g_strjoinv (_ (" · "), (char **) parts->pdata);
      adw_expander_row_set_subtitle (self->expander_row, subtitle);
    }
  else
    {
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->expander_row),
                                     _ ("System Is Up to Date"));
      if (backend != NULL)
        {
          g_autofree char *subtitle = NULL;

          subtitle = g_strdup_printf (_ ("Provided by %s"), backend);
          adw_expander_row_set_subtitle (self->expander_row, subtitle);
        }
    }

  set_action_enabled (self, "install-now", n_updates > 0 && !busy);
  set_action_enabled (self, "check", !busy);
}

static void
rebuild_rows (BzAptUpdatesCard *self)
{
  GListModel *packages = NULL;
  guint       n_items  = 0;
  guint       n_shown  = 0;

  for (guint i = 0; i < self->package_rows->len; i++)
    adw_expander_row_remove (self->expander_row, g_ptr_array_index (self->package_rows, i));
  g_ptr_array_set_size (self->package_rows, 0);

  packages = bz_apt_updates_get_packages (self->updates);
  n_items  = g_list_model_get_n_items (packages);
  n_shown  = MIN (n_items, MAX_PACKAGE_ROWS);

  for (guint i = 0; i < n_shown; i++)
    {
      g_autoptr (BzAptPackage) package = NULL;
      GtkWidget *row                   = NULL;

      package = g_list_model_get_item (packages, i);

      row = adw_action_row_new ();
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), bz_apt_package_get_name (package));
      gtk_widget_set_tooltip_text (row, bz_apt_package_get_summary (package));

      if (bz_apt_package_is_removal (package))
        {
          GtkWidget *icon = NULL;

          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _ ("Will be removed"));
          icon = gtk_image_new_from_icon_name ("user-trash-symbolic");
          gtk_widget_set_tooltip_text (icon, _ ("This package will be removed"));
          gtk_widget_add_css_class (icon, "error");
          adw_action_row_add_suffix (ADW_ACTION_ROW (row), icon);
        }
      else if (bz_apt_package_is_new (package))
        {
          g_autofree char *subtitle = NULL;

          /* Translators: %s is a package version */
          subtitle = g_strdup_printf (_ ("New package, %s"), bz_apt_package_get_version (package));
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
        }
      else
        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), bz_apt_package_get_version (package));

      if (bz_apt_package_is_security (package))
        {
          GtkWidget *icon = NULL;

          icon = gtk_image_new_from_icon_name ("security-low-symbolic");
          gtk_widget_set_tooltip_text (icon, _ ("Security update"));
          gtk_widget_add_css_class (icon, "warning");
          adw_action_row_add_suffix (ADW_ACTION_ROW (row), icon);
        }

      adw_expander_row_add_row (self->expander_row, row);
      g_ptr_array_add (self->package_rows, row);
    }

  if (n_items > n_shown)
    {
      GtkWidget       *row   = NULL;
      g_autofree char *title = NULL;

      title = g_strdup_printf (ngettext ("And %u more package",
                                         "And %u more packages",
                                         n_items - n_shown),
                               n_items - n_shown);

      row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      gtk_widget_add_css_class (row, "dim-label");
      adw_expander_row_add_row (self->expander_row, row);
      g_ptr_array_add (self->package_rows, row);
    }
}

static void
updates_notify_cb (BzAptUpdatesCard *self,
                   GParamSpec       *pspec,
                   BzAptUpdates     *updates)
{
  sync_ui (self);
}

static void
packages_changed_cb (BzAptUpdatesCard *self,
                     guint             position,
                     guint             removed,
                     guint             added,
                     GListModel       *model)
{
  rebuild_rows (self);
  sync_ui (self);
}

static void
install_now_response_cb (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  g_autoptr (BzAptUpdatesCard) self = user_data;
  const char *response              = NULL;

  response = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res);
  if (g_strcmp0 (response, "install") == 0)
    bz_apt_updates_install_now (self->updates);
}

static void
install_now_action (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
  BzAptUpdatesCard *self = user_data;
  AdwDialog        *alert = NULL;

  alert = adw_alert_dialog_new (
      _ ("Install Updates Now?"),
      _ ("Updating the running system can make open apps misbehave until "
         "they are restarted. Installing updates on restart is safer."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "cancel", _ ("_Cancel"),
                                  "install", _ ("_Install Now"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert),
                                            "install", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");

  adw_alert_dialog_choose (ADW_ALERT_DIALOG (alert), GTK_WIDGET (self), NULL,
                           install_now_response_cb, g_object_ref (self));
}

static void
check_action (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data)
{
  BzAptUpdatesCard *self = user_data;

  bz_apt_updates_check (self->updates, TRUE);
}

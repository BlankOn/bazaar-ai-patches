/* bz-update-notifier.c
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

#define G_LOG_DOMAIN "BAZAAR::NOTIFY"

#include "config.h"

#include <adwaita.h>
#include <flatpak/flatpak.h>
#include <glib/gi18n.h>

#include "bz-apt-updates.h"
#include "bz-flatpak-private.h"
#include "bz-update-notifier.h"

/* Notify at most this often; the worker itself runs about hourly */
#define NOTIFY_INTERVAL_SECONDS (60 * 60 * 23)
/* PackageKit may refresh its package lists first; give it time */
#define APT_TIMEOUT_SECONDS 300
/* How long "app.open-library" keeps trying while the window loads */
#define OPEN_LIBRARY_TRIES       240
#define OPEN_LIBRARY_INTERVAL_MS 250

#define NOTIFICATION_ID "bazaar-updates-available"

static gboolean
notification_due (GSettings *settings);

static guint
count_flatpak_updates (void);

static guint
count_installation_updates (FlatpakInstallation *installation);

static void
count_apt_updates (guint       *n_upgrades,
                   guint       *n_security,
                   const char **backend);

static gboolean
timeout_cb (gpointer user_data);

static GVariant *
build_notification (guint       n_apps,
                    guint       n_system,
                    guint       n_security,
                    const char *backend);

static gboolean
send_notification (GVariant *notification);

static void
open_library_action (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data);

static gboolean
open_library_cb (gpointer user_data);

static GtkWidget *
find_stack_with_page (GtkWidget  *widget,
                      GType       type,
                      const char *page);

/* ------------------------------------------------------------------------ */
/* Worker side                                                              */
/* ------------------------------------------------------------------------ */

void
bz_update_notifier_run (GSettings *settings,
                        gboolean   flatpak_auto_updated)
{
  g_autoptr (GVariant) notification = NULL;
  g_autoptr (GDateTime) now         = NULL;
  const char *backend               = NULL;
  guint       n_apps                = 0;
  guint       n_system              = 0;
  guint       n_security            = 0;

  g_return_if_fail (G_IS_SETTINGS (settings));

  if (!g_settings_get_boolean (settings, "update-notifications"))
    {
      g_debug ("Update notifications are disabled");
      return;
    }

  if (!notification_due (settings))
    return;

  /* Listing Flatpak updates downloads remote metadata; skip that on a
   * metered network. PackageKit's list is local (it refreshes it by itself
   * at most daily and honours metered networks). */
  if (flatpak_auto_updated)
    g_debug ("Automatic updates are on: Flatpak updates are announced by them");
  else if (g_network_monitor_get_network_metered (g_network_monitor_get_default ()))
    g_debug ("Metered network: not checking for Flatpak updates");
  else
    n_apps = count_flatpak_updates ();

  count_apt_updates (&n_system, &n_security, &backend);

  /* Parsed by the patch's tests; keep the format */
  g_debug ("Pending updates: %u apps, %u system (%u security)", n_apps, n_system, n_security);

  if (n_apps == 0 && n_system == 0)
    return;

  notification = g_variant_ref_sink (build_notification (n_apps, n_system, n_security, backend));
  if (!send_notification (notification))
    return;

  now = g_date_time_new_now_local ();
  g_settings_set_int64 (settings, "last-update-notification", g_date_time_to_unix (now));
  g_settings_sync ();
}

static gboolean
notification_due (GSettings *settings)
{
  g_autoptr (GDateTime) now = NULL;
  gint64 last               = 0;

  /* For testing: notify now, whatever the last notification was */
  if (g_getenv ("BAZAAR_NOTIFY_NOW") != NULL)
    return TRUE;

  last = g_settings_get_int64 (settings, "last-update-notification");
  now  = g_date_time_new_now_local ();

  if (last > 0 && g_date_time_to_unix (now) - last < NOTIFY_INTERVAL_SECONDS)
    {
      g_debug ("Skipping: notified %" G_GINT64_FORMAT " seconds ago",
               g_date_time_to_unix (now) - last);
      return FALSE;
    }

  return TRUE;
}

static guint
count_flatpak_updates (void)
{
  g_autoptr (GError) local_error              = NULL;
  g_autoptr (FlatpakInstallation) system_inst = NULL;
  guint n_updates                             = 0;

  system_inst = flatpak_installation_new_system (NULL, &local_error);
  if (system_inst != NULL)
    n_updates += count_installation_updates (system_inst);
  else
    {
      g_debug ("No system installation: %s", local_error->message);
      g_clear_error (&local_error);
    }

  if (bz_flatpak_setup_host_environment ())
    {
      g_autoptr (FlatpakInstallation) user_inst = NULL;

      user_inst = bz_flatpak_new_user_installation (&local_error);
      if (user_inst != NULL)
        n_updates += count_installation_updates (user_inst);
      else
        g_debug ("No user installation: %s", local_error->message);
    }

  return n_updates;
}

static guint
count_installation_updates (FlatpakInstallation *installation)
{
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GPtrArray) refs     = NULL;
  guint n_updates                = 0;

  refs = flatpak_installation_list_installed_refs_for_update (installation, NULL, &local_error);
  if (refs == NULL)
    {
      g_debug ("Failed to list Flatpak updates: %s", local_error->message);
      return 0;
    }

  for (guint i = 0; i < refs->len; i++)
    {
      const char *name = NULL;

      name = flatpak_ref_get_name (FLATPAK_REF (g_ptr_array_index (refs, i)));
      /* Same filter as upstream's update worker */
      if (g_str_has_suffix (name, ".Locale") ||
          g_str_has_suffix (name, ".Debug") ||
          g_str_has_suffix (name, ".Sources"))
        continue;
      n_updates++;
    }

  return n_updates;
}

static void
count_apt_updates (guint       *n_upgrades,
                   guint       *n_security,
                   const char **backend)
{
  BzAptUpdates *apt       = NULL;
  gboolean      timed_out = FALSE;
  guint         timeout   = 0;

  /* The same PackageKit client the Library page uses. This process has no
   * running main loop, so iterate the context until the list is in. */
  apt     = bz_apt_updates_get_default ();
  timeout = g_timeout_add_seconds (APT_TIMEOUT_SECONDS, timeout_cb, &timed_out);

  while (!bz_apt_updates_get_loaded (apt) && !timed_out)
    g_main_context_iteration (NULL, TRUE);

  if (!timed_out)
    g_source_remove (timeout);
  else
    g_debug ("Timed out waiting for PackageKit");

  if (!bz_apt_updates_get_available (apt))
    return;

  *n_upgrades = bz_apt_updates_get_n_upgrades (apt);
  *n_security = bz_apt_updates_get_n_security (apt);
  *backend    = bz_apt_updates_get_backend (apt);
}

static gboolean
timeout_cb (gpointer user_data)
{
  gboolean *timed_out = user_data;

  *timed_out = TRUE;
  return G_SOURCE_REMOVE;
}

static GVariant *
build_notification (guint       n_apps,
                    guint       n_system,
                    guint       n_security,
                    const char *backend)
{
  g_autoptr (GString) body = NULL;
  GVariantBuilder builder  = { 0 };
  GVariantBuilder buttons  = { 0 };
  GVariantBuilder button   = { 0 };

  body = g_string_new (NULL);

  if (n_system > 0)
    {
      /* Translators: %s is the package manager, e.g. "Debian APT" */
      g_string_append_printf (body,
                              ngettext ("%u system update from %s is available.",
                                        "%u system updates from %s are available.",
                                        n_system),
                              n_system, backend != NULL ? backend : "PackageKit");
      if (n_security > 0)
        {
          g_string_append_c (body, ' ');
          g_string_append_printf (body,
                                  ngettext ("This includes %u security update.",
                                            "This includes %u security updates.",
                                            n_security),
                                  n_security);
        }
    }

  if (n_apps > 0)
    {
      if (body->len > 0)
        g_string_append_c (body, ' ');
      g_string_append_printf (body,
                              ngettext ("%u app update is available.",
                                        "%u app updates are available.",
                                        n_apps),
                              n_apps);
    }

  g_variant_builder_init (&button, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&button, "{sv}", "label", g_variant_new_string (_ ("View Updates")));
  g_variant_builder_add (&button, "{sv}", "action", g_variant_new_string ("app.open-library"));
  g_variant_builder_init (&buttons, G_VARIANT_TYPE ("aa{sv}"));
  g_variant_builder_add (&buttons, "a{sv}", &button);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "title",
                         g_variant_new_string (n_security > 0
                                                   ? _ ("Security Updates Available")
                                                   : _ ("Updates Available")));
  g_variant_builder_add (&builder, "{sv}", "body", g_variant_new_string (body->str));
  g_variant_builder_add (&builder, "{sv}", "priority",
                         g_variant_new_string (n_security > 0 ? "high" : "normal"));
  g_variant_builder_add (&builder, "{sv}", "default-action", g_variant_new_string ("app.open-library"));
  g_variant_builder_add (&builder, "{sv}", "buttons", g_variant_builder_end (&buttons));

  return g_variant_builder_end (&builder);
}

/* GNOME Shell's org.gtk.Notifications takes the application ID explicitly,
 * so the notification belongs to Bazaar and clicking it D-Bus-activates
 * Bazaar with the action, even from this short-lived worker process. That
 * is what GApplication uses natively. Elsewhere (a Flatpak sandbox, other
 * desktops) fall back to the notification portal, like upstream. */
static gboolean
send_notification (GVariant *notification)
{
  g_autoptr (GDBusConnection) bus = NULL;
  g_autoptr (GVariant) reply      = NULL;
  g_autoptr (GError) local_error  = NULL;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &local_error);
  if (bus == NULL)
    {
      g_warning ("No session bus for the update notification: %s", local_error->message);
      return FALSE;
    }

  reply = g_dbus_connection_call_sync (
      bus, "org.gtk.Notifications", "/org/gtk/Notifications",
      "org.gtk.Notifications", "AddNotification",
      g_variant_new ("(ss@a{sv})", APPLICATION_ID, NOTIFICATION_ID, notification),
      NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, &local_error);
  if (reply != NULL)
    {
      g_debug ("Sent the update notification through org.gtk.Notifications");
      return TRUE;
    }

  g_debug ("org.gtk.Notifications failed (%s), using the portal", local_error->message);
  g_clear_error (&local_error);

  reply = g_dbus_connection_call_sync (
      bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Notification", "AddNotification",
      g_variant_new ("(s@a{sv})", NOTIFICATION_ID, notification),
      NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &local_error);
  if (reply == NULL)
    {
      g_warning ("Failed to send the update notification: %s", local_error->message);
      return FALSE;
    }

  g_debug ("Sent the update notification through the portal");
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Application side                                                         */
/* ------------------------------------------------------------------------ */

void
bz_update_notifier_add_app_actions (GApplication *app)
{
  static const GActionEntry entries[] = {
    { "open-library", open_library_action, NULL, NULL, NULL, { 0 } },
  };

  g_return_if_fail (G_IS_APPLICATION (app));

  g_action_map_add_action_entries (G_ACTION_MAP (app), entries, G_N_ELEMENTS (entries), app);
}

static void
open_library_action (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
  GApplication *app    = user_data;
  GtkWindow    *window = NULL;
  guint        *tries  = NULL;

  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window == NULL)
    {
      GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));

      window = windows != NULL ? windows->data : NULL;
    }

  /* Bazaar's "activate" always opens a new window; only use it when there
   * is none */
  if (window != NULL)
    gtk_window_present (window);
  else
    g_application_activate (app);

  /* A new window may not exist yet, or still show its loading page, and
   * ignore the request. Keep asking until the Library page is shown. */
  tries  = g_new0 (guint, 1);
  *tries = OPEN_LIBRARY_TRIES;
  g_application_hold (app);
  g_timeout_add_full (G_PRIORITY_DEFAULT, OPEN_LIBRARY_INTERVAL_MS, open_library_cb,
                      tries, g_free);
}

static gboolean
open_library_cb (gpointer user_data)
{
  guint        *tries     = user_data;
  GApplication *app       = g_application_get_default ();
  GtkWindow    *window    = NULL;
  GtkWidget    *pages     = NULL;
  GtkWidget    *loading   = NULL;
  gboolean      in_place  = FALSE;

  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window == NULL)
    {
      GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));

      window = windows != NULL ? windows->data : NULL;
    }

  if (window != NULL)
    {
      gtk_widget_activate_action (GTK_WIDGET (window), "window.open-library", NULL);

      pages   = find_stack_with_page (GTK_WIDGET (window), ADW_TYPE_VIEW_STACK, "installed");
      loading = find_stack_with_page (GTK_WIDGET (window), GTK_TYPE_STACK, "loading");
      in_place =
          pages != NULL &&
          g_strcmp0 (adw_view_stack_get_visible_child_name (ADW_VIEW_STACK (pages)), "installed") == 0 &&
          (loading == NULL ||
           g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (loading)), "loading") != 0);
    }

  if (in_place || --(*tries) == 0)
    {
      g_application_release (app);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static GtkWidget *
find_stack_with_page (GtkWidget  *widget,
                      GType       type,
                      const char *page)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (widget, type))
    {
      if (type == ADW_TYPE_VIEW_STACK &&
          adw_view_stack_get_child_by_name (ADW_VIEW_STACK (widget), page) != NULL)
        return widget;
      if (type == GTK_TYPE_STACK &&
          gtk_stack_get_child_by_name (GTK_STACK (widget), page) != NULL)
        return widget;
    }

  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_stack_with_page (child, type, page);

      if (found != NULL)
        return found;
    }

  return NULL;
}

/* bz-gnome-extension-dialog.c
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

/* DOWNSTREAM PATCH (GNOME Extensions): see ai-patches/PLAN.md §13.
 *
 * Install asks GNOME Shell, which shows its own confirmation; Remove asks
 * here first. */

#define G_LOG_DOMAIN "BAZAAR::GNOME-EXTENSIONS"

#include "config.h"

#include <glib/gi18n.h>

#include "bz-gnome-extension-dialog.h"

struct _BzGnomeExtensionDialog
{
  AdwDialog parent_instance;

  BzGnomeExtensions *extensions;
  BzGnomeExtension  *extension;

  /* Template widgets */
  AdwSwitchRow *enabled_row;
};

G_DEFINE_FINAL_TYPE (BzGnomeExtensionDialog, bz_gnome_extension_dialog, ADW_TYPE_DIALOG)

enum
{
  PROP_0,
  PROP_EXTENSIONS,
  PROP_EXTENSION,
  N_PROPS,
};

static GParamSpec *props[N_PROPS];

static gboolean
invert_boolean (gpointer object,
                gboolean value)
{
  return !value;
}

static gboolean
is_set (gpointer    object,
        const char *value)
{
  return value != NULL && *value != '\0';
}

static gboolean
is_nonzero (gpointer object,
            guint64  value)
{
  return value != 0;
}

static gboolean
can_toggle (gpointer object,
            gboolean can_change,
            gboolean busy)
{
  return can_change && !busy;
}

static char *
format_creator (gpointer    object,
                const char *creator)
{
  if (creator == NULL || *creator == '\0')
    return g_strdup ("");
  /* Translators: %s is the name of the person who made a GNOME Shell extension */
  return g_strdup_printf (_ ("by %s"), creator);
}

static char *
format_downloads (gpointer object,
                  guint64  downloads)
{
  return g_strdup_printf ("%'" G_GUINT64_FORMAT, downloads);
}

static void
install_cb (BzGnomeExtensionDialog *self)
{
  bz_gnome_extensions_install (self->extensions, self->extension);
}

static void
enabled_changed_cb (BzGnomeExtensionDialog *self)
{
  gboolean active = adw_switch_row_get_active (self->enabled_row);

  /* Also called when the binding follows GNOME Shell: then nothing changes. */
  if (self->extension == NULL || active == bz_gnome_extension_get_enabled (self->extension))
    return;
  bz_gnome_extensions_set_enabled (self->extensions, self->extension, active);
}

static void
prefs_cb (BzGnomeExtensionDialog *self)
{
  bz_gnome_extensions_open_prefs (self->extensions, self->extension, "");
}

static void
remove_response_cb (AdwAlertDialog         *alert,
                    const char             *response,
                    BzGnomeExtensionDialog *self)
{
  if (g_strcmp0 (response, "remove") == 0)
    bz_gnome_extensions_uninstall (self->extensions, self->extension);
}

static void
remove_cb (BzGnomeExtensionDialog *self)
{
  AdwDialog *alert = NULL;

  alert = adw_alert_dialog_new (NULL, _ ("The extension is deleted from this computer."));
  /* Translators: %s is the name of a GNOME Shell extension */
  adw_alert_dialog_format_heading (ADW_ALERT_DIALOG (alert), _ ("Remove “%s”?"),
                                   bz_gnome_extension_get_name (self->extension));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "cancel", _ ("_Cancel"),
                                  "remove", _ ("_Remove"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");
  g_signal_connect_object (alert, "response", G_CALLBACK (remove_response_cb), self, 0);
  adw_dialog_present (alert, GTK_WIDGET (self));
}

static void
open_uri (BzGnomeExtensionDialog *self,
          const char             *uri)
{
  g_autoptr (GtkUriLauncher) launcher = NULL;
  GtkRoot *root                       = NULL;

  if (uri == NULL)
    return;
  launcher = gtk_uri_launcher_new (uri);
  root     = gtk_widget_get_root (GTK_WIDGET (self));
  gtk_uri_launcher_launch (launcher, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, NULL, NULL);
}

static void
open_link_cb (BzGnomeExtensionDialog *self)
{
  open_uri (self, bz_gnome_extension_get_link (self->extension));
}

static void
open_homepage_cb (BzGnomeExtensionDialog *self)
{
  open_uri (self, bz_gnome_extension_get_homepage (self->extension));
}

static void
bz_gnome_extension_dialog_dispose (GObject *object)
{
  BzGnomeExtensionDialog *self = BZ_GNOME_EXTENSION_DIALOG (object);

  g_clear_object (&self->extensions);
  g_clear_object (&self->extension);

  G_OBJECT_CLASS (bz_gnome_extension_dialog_parent_class)->dispose (object);
}

static void
bz_gnome_extension_dialog_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  BzGnomeExtensionDialog *self = BZ_GNOME_EXTENSION_DIALOG (object);

  switch (prop_id)
    {
    case PROP_EXTENSIONS: g_value_set_object (value, self->extensions); break;
    case PROP_EXTENSION: g_value_set_object (value, self->extension); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
bz_gnome_extension_dialog_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  BzGnomeExtensionDialog *self = BZ_GNOME_EXTENSION_DIALOG (object);

  switch (prop_id)
    {
    case PROP_EXTENSIONS: g_set_object (&self->extensions, g_value_get_object (value)); break;
    case PROP_EXTENSION: g_set_object (&self->extension, g_value_get_object (value)); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
bz_gnome_extension_dialog_class_init (BzGnomeExtensionDialogClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose      = bz_gnome_extension_dialog_dispose;
  object_class->get_property = bz_gnome_extension_dialog_get_property;
  object_class->set_property = bz_gnome_extension_dialog_set_property;

  props[PROP_EXTENSIONS] = g_param_spec_object ("extensions", NULL, NULL, BZ_TYPE_GNOME_EXTENSIONS,
                                                G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  props[PROP_EXTENSION] = g_param_spec_object ("extension", NULL, NULL, BZ_TYPE_GNOME_EXTENSION,
                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);

  g_type_ensure (BZ_TYPE_GNOME_EXTENSION);

  gtk_widget_class_set_template_from_resource (widget_class, "/io/github/kolunmi/Bazaar/bz-gnome-extension-dialog.ui");
  gtk_widget_class_bind_template_child (widget_class, BzGnomeExtensionDialog, enabled_row);
  gtk_widget_class_bind_template_callback (widget_class, invert_boolean);
  gtk_widget_class_bind_template_callback (widget_class, is_set);
  gtk_widget_class_bind_template_callback (widget_class, is_nonzero);
  gtk_widget_class_bind_template_callback (widget_class, can_toggle);
  gtk_widget_class_bind_template_callback (widget_class, format_creator);
  gtk_widget_class_bind_template_callback (widget_class, format_downloads);
  gtk_widget_class_bind_template_callback (widget_class, install_cb);
  gtk_widget_class_bind_template_callback (widget_class, enabled_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, prefs_cb);
  gtk_widget_class_bind_template_callback (widget_class, remove_cb);
  gtk_widget_class_bind_template_callback (widget_class, open_link_cb);
  gtk_widget_class_bind_template_callback (widget_class, open_homepage_cb);
}

static void
bz_gnome_extension_dialog_init (BzGnomeExtensionDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

AdwDialog *
bz_gnome_extension_dialog_new (BzGnomeExtensions *extensions,
                               BzGnomeExtension  *extension)
{
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSIONS (extensions), NULL);
  g_return_val_if_fail (BZ_IS_GNOME_EXTENSION (extension), NULL);

  bz_gnome_extensions_ensure_screenshot (extensions, extension);
  return g_object_new (BZ_TYPE_GNOME_EXTENSION_DIALOG,
                       "extensions", extensions,
                       "extension", extension,
                       NULL);
}

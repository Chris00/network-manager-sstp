/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/***************************************************************************
 * nm-sstp.c : GNOME UI dialogs for configuring SSTP VPN connections
 *
 * Copyright (C) 2008 Dan Williams, <dcbw@redhat.com>
 * Copyright (C) 2008 - 2011 Red Hat, Inc.
 * Based on work by David Zeuthen, <davidz@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <glib/gi18n-lib.h>
#include <string.h>
#include <gtk/gtk.h>
#include <fcntl.h>

#define NM_VPN_API_SUBJECT_TO_CHANGE

#include <nm-vpn-plugin-ui-interface.h>
#include <nm-setting-vpn.h>
#include <nm-setting-connection.h>
#include <nm-setting-ip4-config.h>

#include "src/nm-sstp-service.h"
#include "nm-sstp.h"
#include "import-export.h"
#include "advanced-dialog.h"

#define SSTP_PLUGIN_NAME    _("Secure Socket Tunneling Protocol (SSTP)")
#define SSTP_PLUGIN_DESC    _("Compatible with Microsoft and other SSTP VPN servers.")
#define SSTP_PLUGIN_SERVICE NM_DBUS_SERVICE_SSTP


typedef void (*ChangedCallback) (GtkWidget *widget, gpointer user_data);

/************** plugin class **************/

static void sstp_plugin_ui_interface_init (NMVpnPluginUiInterface *iface_class);

G_DEFINE_TYPE_EXTENDED (SstpPluginUi, sstp_plugin_ui, G_TYPE_OBJECT, 0,
						G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_PLUGIN_UI_INTERFACE,
											   sstp_plugin_ui_interface_init))

/************** UI widget class **************/

static void sstp_plugin_ui_widget_interface_init (NMVpnPluginUiWidgetInterface *iface_class);

G_DEFINE_TYPE_EXTENDED (SstpPluginUiWidget, sstp_plugin_ui_widget, G_TYPE_OBJECT, 0,
						G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_PLUGIN_UI_WIDGET_INTERFACE,
											   sstp_plugin_ui_widget_interface_init))

#define SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SSTP_TYPE_PLUGIN_UI_WIDGET, SstpPluginUiWidgetPrivate))

typedef struct {
	GtkBuilder *builder;
	GtkWidget *widget;
	GtkSizeGroup *group;
	GtkWindowGroup *window_group;
	gboolean window_added;
	GHashTable *advanced;
	gboolean new_connection;
} SstpPluginUiWidgetPrivate;


GQuark
sstp_plugin_ui_error_quark (void)
{
	static GQuark error_quark = 0;

	if (G_UNLIKELY (error_quark == 0))
		error_quark = g_quark_from_static_string ("sstp-plugin-ui-error-quark");

	return error_quark;
}

/* This should really be standard. */
#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }

GType
sstp_plugin_ui_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			/* Unknown error. */
			ENUM_ENTRY (SSTP_PLUGIN_UI_ERROR_UNKNOWN, "UnknownError"),
			/* The connection was missing invalid. */
			ENUM_ENTRY (SSTP_PLUGIN_UI_ERROR_INVALID_CONNECTION, "InvalidConnection"),
			/* The specified property was invalid. */
			ENUM_ENTRY (SSTP_PLUGIN_UI_ERROR_INVALID_PROPERTY, "InvalidProperty"),
			/* The specified property was missing and is required. */
			ENUM_ENTRY (SSTP_PLUGIN_UI_ERROR_MISSING_PROPERTY, "MissingProperty"),
			/* The file to import could not be read. */
			ENUM_ENTRY (SSTP_PLUGIN_UI_ERROR_FILE_NOT_READABLE, "FileNotReadable"),
			/* The file to import could was not an SSTP client file. */
			ENUM_ENTRY (SSTP_PLUGIN_UI_ERROR_FILE_NOT_SSTP, "FileNotSSTP"),
			{ 0, 0, 0 }
		};
		etype = g_enum_register_static ("SstpPluginUiError", values);
	}
	return etype;
}

static gboolean
check_validity (SstpPluginUiWidget *self, GError **error)
{
	SstpPluginUiWidgetPrivate *priv = SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *widget;
	const char *str;

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (!str || !strlen (str)) {
		g_set_error (error,
		             SSTP_PLUGIN_UI_ERROR,
		             SSTP_PLUGIN_UI_ERROR_INVALID_PROPERTY,
		             NM_SSTP_KEY_GATEWAY);
		return FALSE;
	}

	return TRUE;
}

static void
stuff_changed_cb (GtkWidget *widget, gpointer user_data)
{
	g_signal_emit_by_name (SSTP_PLUGIN_UI_WIDGET (user_data), "changed");
}

static void
advanced_dialog_close_cb (GtkWidget *dialog, gpointer user_data)
{
	gtk_widget_hide (dialog);
	/* gtk_widget_destroy() will remove the window from the window group */
	gtk_widget_destroy (dialog);
}


static const char *
find_tag (const char *tag, const char *buf, gsize len)
{
	gsize i, taglen;

	taglen = strlen (tag);
	if (len < taglen)
		return NULL;

	for (i = 0; i < len - taglen + 1; i++) {
		if (memcmp (buf + i, tag, taglen) == 0)
			return buf + i;
	}
	return NULL;
}

static gboolean
default_filter (const GtkFileFilterInfo *filter_info, gpointer data)
{
	static const char *key_begin = "-----BEGIN CERTIFICATE-----";
	int fd;
	unsigned char buffer[1024];
	ssize_t bytes_read;
	gboolean show = FALSE;
	char *p;
	char *ext;

	/* No nane, bail .. */
	if (!filter_info->filename)
		return FALSE;

	/* Must have a '.' */
	p = strrchr (filter_info->filename, '.');
	if (!p)
		return FALSE;

	/* Check extention, make lower case */
	ext = g_ascii_strdown (p, -1);
	if (!ext)
		return FALSE;
	if (strcmp (ext, ".pem")) {
		g_free (ext);
		return FALSE;
	}
	g_free (ext);

	/* Extention is OK, check if file is a certificate */
	fd = open (filter_info->filename, O_RDONLY);
	if (fd < 0)
		return FALSE;

	/* Read the first 400 bytes */
	bytes_read = read (fd, buffer, sizeof (buffer) - 1);
	if (bytes_read < 400)  /* needs to be lower? */
		goto out;
	buffer[bytes_read] = '\0';

	/* Check for PEM signatures */
	if (find_tag (key_begin, (const char *) buffer, bytes_read)) {
		show = TRUE;
		goto out;
	}

out:
	close (fd);
	return show;
}


static GtkFileFilter *
file_chooser_filter_new(void)
{
	GtkFileFilter *filter;

	filter = gtk_file_filter_new ();
	gtk_file_filter_add_custom (filter, GTK_FILE_FILTER_FILENAME, 
							    default_filter, NULL, NULL);
	gtk_file_filter_set_name (filter, _("Certificate in PEM (*.pem)"));
	return filter;
}


static void
advanced_dialog_response_cb (GtkWidget *dialog, gint response, gpointer user_data)
{
	SstpPluginUiWidget *self = SSTP_PLUGIN_UI_WIDGET (user_data);
	SstpPluginUiWidgetPrivate *priv = SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GError *error = NULL;

	if (response != GTK_RESPONSE_OK) {
		advanced_dialog_close_cb (dialog, self);
		return;
	}

	if (priv->advanced)
		g_hash_table_destroy (priv->advanced);
	priv->advanced = advanced_dialog_new_hash_from_dialog (dialog, &error);
	if (!priv->advanced) {
		g_message ("%s: error reading advanced settings: %s", __func__, error->message);
		g_error_free (error);
	}
	advanced_dialog_close_cb (dialog, self);

	stuff_changed_cb (NULL, self);
}

static void
advanced_button_clicked_cb (GtkWidget *button, gpointer user_data)
{
	SstpPluginUiWidget *self = SSTP_PLUGIN_UI_WIDGET (user_data);
	SstpPluginUiWidgetPrivate *priv = SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *dialog, *toplevel;

	toplevel = gtk_widget_get_toplevel (priv->widget);
	g_return_if_fail (gtk_widget_is_toplevel (toplevel));

	dialog = advanced_dialog_new (priv->advanced);
	if (!dialog) {
		g_warning ("%s: failed to create the Advanced dialog!", __func__);
		return;
	}

	gtk_window_group_add_window (priv->window_group, GTK_WINDOW (dialog));
	if (!priv->window_added) {
		gtk_window_group_add_window (priv->window_group, GTK_WINDOW (toplevel));
		priv->window_added = TRUE;
	}

	gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (toplevel));
	g_signal_connect (G_OBJECT (dialog), "response", G_CALLBACK (advanced_dialog_response_cb), self);
	g_signal_connect (G_OBJECT (dialog), "close", G_CALLBACK (advanced_dialog_close_cb), self);

	gtk_widget_show_all (dialog);
}

static void
show_toggled_cb (GtkCheckButton *button, SstpPluginUiWidget *self)
{
	SstpPluginUiWidgetPrivate *priv = SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *widget;
	gboolean visible;

	visible = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button));

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_password_entry"));
	g_assert (widget);
	gtk_entry_set_visibility (GTK_ENTRY (widget), visible);
}

static gboolean
init_plugin_ui (SstpPluginUiWidget *self, NMConnection *connection, GError **error)
{
	SstpPluginUiWidgetPrivate *priv = SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	NMSettingVPN *s_vpn;
	GtkWidget *widget;
	GtkFileFilter *filter;
	const char *value;
	NMSettingSecretFlags pw_flags = NM_SETTING_SECRET_FLAG_NONE;

	s_vpn = (NMSettingVPN *) nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN);

	priv->group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	if (!widget)
		return FALSE;
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_SSTP_KEY_GATEWAY);
		if (value && strlen (value))
			gtk_entry_set_text (GTK_ENTRY (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_entry"));
	if (!widget)
		return FALSE;
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_SSTP_KEY_USER);
		if (value && strlen (value))
			gtk_entry_set_text (GTK_ENTRY (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "ca_cert_chooser"));
	if (!widget)
		return FALSE;
	gtk_size_group_add_widget (priv->group, widget);
	filter = file_chooser_filter_new ();
	gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (widget), filter);
	gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (widget), TRUE);
	gtk_file_chooser_button_set_title (GTK_FILE_CHOOSER_BUTTON (widget),
									  _("Choose a CA Certificate"));
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_SSTP_KEY_CA_CERT);
		if (value && strlen (value))
			gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (widget), value);
	}
	
	g_signal_connect (G_OBJECT (widget), "selection-changed", G_CALLBACK (stuff_changed_cb), self);
	
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "domain_entry"));
	if (!widget)
		return FALSE;
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_SSTP_KEY_DOMAIN);
		if (value && strlen (value))
			gtk_entry_set_text (GTK_ENTRY (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "advanced_button"));
	g_signal_connect (G_OBJECT (widget), "clicked", G_CALLBACK (advanced_button_clicked_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "show_passwords_checkbutton"));
	g_return_val_if_fail (widget != NULL, FALSE);
	g_signal_connect (G_OBJECT (widget), "toggled",
	                  (GCallback) show_toggled_cb,
	                  self);

	widget = (GtkWidget *) gtk_builder_get_object (priv->builder, "user_password_entry");
	g_assert (widget);
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_secret (s_vpn, NM_SSTP_KEY_PASSWORD);
		if (value)
			gtk_entry_set_text (GTK_ENTRY (widget), value);

		/* Default to agent-owned for new connections */
		if (priv->new_connection)
			pw_flags = NM_SETTING_SECRET_FLAG_AGENT_OWNED;
		else
			nm_setting_get_secret_flags (NM_SETTING (s_vpn), NM_SSTP_KEY_PASSWORD, &pw_flags, NULL);

		g_object_set_data (G_OBJECT (widget), "flags", GUINT_TO_POINTER (pw_flags));
	}
	g_signal_connect (widget, "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "cert_warn_checkbutton"));
	g_return_val_if_fail (widget != NULL, FALSE);
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_SSTP_KEY_IGN_CERT_WARN);
		if (value && !strcmp(value, "yes")) {
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);
		}
	}
	g_signal_connect (widget, "toggled", G_CALLBACK (stuff_changed_cb), self);

	return TRUE;
}

static GObject *
get_widget (NMVpnPluginUiWidgetInterface *iface)
{
	SstpPluginUiWidget *self = SSTP_PLUGIN_UI_WIDGET (iface);
	SstpPluginUiWidgetPrivate *priv = SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);

	return G_OBJECT (priv->widget);
}

static void
hash_copy_advanced (gpointer key, gpointer data, gpointer user_data)
{
	NMSettingVPN *s_vpn = NM_SETTING_VPN (user_data);

	/* Special handling of the secrets */
	if (!strcmp(NM_SSTP_KEY_PROXY_PASSWORD, (const char *) key))
	{
		nm_setting_vpn_add_secret (s_vpn, (const char *) key, 
								  (const char *) data);
		return;
	}

	nm_setting_vpn_add_data_item (s_vpn, (const char *) key, (const char *) data);
}

static gboolean
update_connection (NMVpnPluginUiWidgetInterface *iface,
				   NMConnection *connection,
				   GError **error)
{
	SstpPluginUiWidget *self = SSTP_PLUGIN_UI_WIDGET (iface);
	SstpPluginUiWidgetPrivate *priv = SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	NMSettingVPN *s_vpn;
	GtkWidget *widget;
	const char *str;
	char *tmp;
	gboolean valid = FALSE;
	NMSettingSecretFlags pw_flags;

	if (!check_validity (self, error))
		return FALSE;

	s_vpn = NM_SETTING_VPN (nm_setting_vpn_new ());
	g_object_set (s_vpn, NM_SETTING_VPN_SERVICE_TYPE, NM_DBUS_SERVICE_SSTP, NULL);

	/* Gateway */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_SSTP_KEY_GATEWAY, str);

	/* Username */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_SSTP_KEY_USER, str);

	/* User password */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_password_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_secret (s_vpn, NM_SSTP_KEY_PASSWORD, str);

	/* And password flags */
	pw_flags = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (widget), "flags"));
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_SSTP_KEY_PASSWORD, pw_flags, NULL);

	/* Domain */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "domain_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_SSTP_KEY_DOMAIN, str);

	/* CA Certificate */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "ca_cert_chooser"));
	tmp = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (widget));
	if (tmp && strlen(tmp)) {
		nm_setting_vpn_add_data_item (s_vpn, NM_SSTP_KEY_CA_CERT, tmp);
		g_free (tmp);
	}

	/* Ignore Certificate Warnings */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "cert_warn_checkbutton"));
	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget))) {
		nm_setting_vpn_add_data_item (s_vpn, NM_SSTP_KEY_IGN_CERT_WARN, "yes");
	}

	/* Update the NM_SETTING object */
	if (priv->advanced)
		g_hash_table_foreach (priv->advanced, hash_copy_advanced, s_vpn);

	/* Default to agent owned secret for new connections */
	// if (priv->new_connection) {
		if (nm_setting_vpn_get_secret (s_vpn, NM_SSTP_KEY_PROXY_PASSWORD)) {
			nm_setting_set_secret_flags (NM_SETTING(s_vpn),
										 NM_SSTP_KEY_PROXY_PASSWORD,
										 NM_SETTING_SECRET_FLAG_NONE,
										 // NM_SETTING_SECRET_FLAG_AGENT_OWNED,
										 NULL);
		}
	//}

	nm_connection_add_setting (connection, NM_SETTING (s_vpn));
	valid = TRUE;

	return valid;
}

static void
is_new_func (const char *key, const char *value, gpointer user_data)
{
	gboolean *is_new = user_data;

	/* If there are any VPN data items the connection isn't new */
	*is_new = FALSE;
}

static NMVpnPluginUiWidgetInterface *
nm_vpn_plugin_ui_widget_interface_new (NMConnection *connection, GError **error)
{
	NMVpnPluginUiWidgetInterface *object;
	SstpPluginUiWidgetPrivate *priv;
	char *ui_file;
	gboolean new = TRUE;
	NMSettingVPN *s_vpn;

	if (error)
		g_return_val_if_fail (*error == NULL, NULL);

	object = NM_VPN_PLUGIN_UI_WIDGET_INTERFACE (g_object_new (SSTP_TYPE_PLUGIN_UI_WIDGET, NULL));
	if (!object) {
		g_set_error (error, SSTP_PLUGIN_UI_ERROR, 0, "could not create sstp object");
		return NULL;
	}

	priv = SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE (object);

	ui_file = g_strdup_printf ("%s/%s", UIDIR, "nm-sstp-dialog.ui");
	priv->builder = gtk_builder_new ();

	gtk_builder_set_translation_domain (priv->builder, GETTEXT_PACKAGE);

	if (!gtk_builder_add_from_file (priv->builder, ui_file, error)) {
		g_warning ("Couldn't load builder file: %s",
		           error && *error ? (*error)->message : "(unknown)");
		g_clear_error (error);
		g_set_error (error, SSTP_PLUGIN_UI_ERROR, 0,
		             "could not load required resources at %s", ui_file);
		g_free (ui_file);
		g_object_unref (object);
		return NULL;
	}
	g_free (ui_file);

	priv->widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "sstp-vbox"));
	if (!priv->widget) {
		g_set_error (error, SSTP_PLUGIN_UI_ERROR, 0, "could not load UI widget");
		g_object_unref (object);
		return NULL;
	}
	g_object_ref_sink (priv->widget);

	priv->window_group = gtk_window_group_new ();

	s_vpn = nm_connection_get_setting_vpn (connection);
	if (s_vpn)
		nm_setting_vpn_foreach_data_item (s_vpn, is_new_func, &new);
	priv->new_connection = new;

	if (!init_plugin_ui (SSTP_PLUGIN_UI_WIDGET (object), connection, error)) {
		g_object_unref (object);
		return NULL;
	}

	priv->advanced = advanced_dialog_new_hash_from_connection (connection, error);
	if (!priv->advanced) {
		g_object_unref (object);
		return NULL;
	}

	return object;
}

static void
dispose (GObject *object)
{
	SstpPluginUiWidget *plugin = SSTP_PLUGIN_UI_WIDGET (object);
	SstpPluginUiWidgetPrivate *priv = SSTP_PLUGIN_UI_WIDGET_GET_PRIVATE (plugin);

	if (priv->group)
		g_object_unref (priv->group);

	if (priv->window_group)
		g_object_unref (priv->window_group);

	if (priv->widget)
		g_object_unref (priv->widget);

	if (priv->builder)
		g_object_unref (priv->builder);

	if (priv->advanced)
		g_hash_table_destroy (priv->advanced);

	G_OBJECT_CLASS (sstp_plugin_ui_widget_parent_class)->dispose (object);
}

static void
sstp_plugin_ui_widget_class_init (SstpPluginUiWidgetClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	g_type_class_add_private (req_class, sizeof (SstpPluginUiWidgetPrivate));

	object_class->dispose = dispose;
}

static void
sstp_plugin_ui_widget_init (SstpPluginUiWidget *plugin)
{
}

static void
sstp_plugin_ui_widget_interface_init (NMVpnPluginUiWidgetInterface *iface_class)
{
	/* interface implementation */
	iface_class->get_widget = get_widget;
	iface_class->update_connection = update_connection;
}

static NMConnection *
import (NMVpnPluginUiInterface *iface, const char *path, GError **error)
{
	NMConnection *connection = NULL;
	char *contents = NULL;
	char **lines = NULL;
	char *ext;

	ext = strrchr (path, '.');
	if (!ext) {
		g_set_error (error,
		             SSTP_PLUGIN_UI_ERROR,
		             SSTP_PLUGIN_UI_ERROR_FILE_NOT_SSTP,
		             "unknown SSTP file extension");
		goto out;
	}

	if (strcmp (ext, ".conf") && strcmp (ext, ".cnf")) {
		g_set_error (error,
		             SSTP_PLUGIN_UI_ERROR,
		             SSTP_PLUGIN_UI_ERROR_FILE_NOT_SSTP,
		             "unknown SSTP file extension");
		goto out;
	}

	if (!g_file_get_contents (path, &contents, NULL, error))
		return NULL;

	lines = g_strsplit_set (contents, "\r\n", 0);
	if (g_strv_length (lines) <= 1) {
		g_set_error (error,
		             SSTP_PLUGIN_UI_ERROR,
		             SSTP_PLUGIN_UI_ERROR_FILE_NOT_READABLE,
		             "not a valid SSTP configuration file");
		goto out;
	}

	connection = do_import (path, lines, error);

out:
	if (lines)
		g_strfreev (lines);
	g_free (contents);
	return connection;
}

static gboolean
export (NMVpnPluginUiInterface *iface,
		const char *path,
		NMConnection *connection,
		GError **error)
{
	return do_export (path, connection, error);
}

static char *
get_suggested_name (NMVpnPluginUiInterface *iface, NMConnection *connection)
{
	NMSettingConnection *s_con;
	const char *id;

	g_return_val_if_fail (connection != NULL, NULL);

	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
	g_return_val_if_fail (s_con != NULL, NULL);

	id = nm_setting_connection_get_id (s_con);
	g_return_val_if_fail (id != NULL, NULL);

	return g_strdup_printf ("%s (sstp).conf", id);
}

static guint32
get_capabilities (NMVpnPluginUiInterface *iface)
{
	return (NM_VPN_PLUGIN_UI_CAPABILITY_IMPORT | NM_VPN_PLUGIN_UI_CAPABILITY_EXPORT);
}

static NMVpnPluginUiWidgetInterface *
ui_factory (NMVpnPluginUiInterface *iface, NMConnection *connection, GError **error)
{
	return nm_vpn_plugin_ui_widget_interface_new (connection, error);
}

static void
get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	case NM_VPN_PLUGIN_UI_INTERFACE_PROP_NAME:
		g_value_set_string (value, SSTP_PLUGIN_NAME);
		break;
	case NM_VPN_PLUGIN_UI_INTERFACE_PROP_DESC:
		g_value_set_string (value, SSTP_PLUGIN_DESC);
		break;
	case NM_VPN_PLUGIN_UI_INTERFACE_PROP_SERVICE:
		g_value_set_string (value, SSTP_PLUGIN_SERVICE);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
sstp_plugin_ui_class_init (SstpPluginUiClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	object_class->get_property = get_property;

	g_object_class_override_property (object_class,
									  NM_VPN_PLUGIN_UI_INTERFACE_PROP_NAME,
									  NM_VPN_PLUGIN_UI_INTERFACE_NAME);

	g_object_class_override_property (object_class,
									  NM_VPN_PLUGIN_UI_INTERFACE_PROP_DESC,
									  NM_VPN_PLUGIN_UI_INTERFACE_DESC);

	g_object_class_override_property (object_class,
									  NM_VPN_PLUGIN_UI_INTERFACE_PROP_SERVICE,
									  NM_VPN_PLUGIN_UI_INTERFACE_SERVICE);
}

static void
sstp_plugin_ui_init (SstpPluginUi *plugin)
{
}

static void
sstp_plugin_ui_interface_init (NMVpnPluginUiInterface *iface_class)
{
	/* interface implementation */
	iface_class->ui_factory = ui_factory;
	iface_class->get_capabilities = get_capabilities;
	iface_class->import_from_file = import;
	iface_class->export_to_file = export;
	iface_class->get_suggested_name = get_suggested_name;
}


G_MODULE_EXPORT NMVpnPluginUiInterface *
nm_vpn_plugin_ui_factory (GError **error)
{
	if (error)
		g_return_val_if_fail (*error == NULL, NULL);

	return NM_VPN_PLUGIN_UI_INTERFACE (g_object_new (SSTP_TYPE_PLUGIN_UI, NULL));
}


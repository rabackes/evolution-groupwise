/*
 * module-groupwise-configuration.c: GroupWise in Evolution's account assistant
 *
 * Copyright (C) 2026 bond Software Entwicklung GmbH
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 * License for more details.
 */

/*
 * "GroupWise" as server type in the account assistant and editor: server,
 * port and user of the POA. Like Exchange Web Services, a GroupWise account
 * is a collection: mail, address books, calendar, tasks and notes with one
 * login. The settings live in the collection ("Groupwise Backend"), which
 * the registry module of this package serves. The provider sends mail
 * itself, so Evolution shows no page for sending.
 *
 * The module also brings the GroupWise actions of the mail view
 * (e-groupwise-mail-ui.c: Restore in the Trash, Junk Mail, the GroupWise
 * Settings window) and the properties dialog of GroupWise calendars and
 * address books (e-groupwise-source-config.c).
 */

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>
#include <mail/e-mail-config-receiving-page.h>
#include <mail/e-mail-config-service-backend.h>

#include "e-groupwise-mail-ui.h"
#include "e-groupwise-source-config.h"
#include "e-groupwise-travel-page.h"

#define GW_DEFAULT_PORT 7191

typedef struct {
	EMailConfigServiceBackend parent;

	GtkWidget *host_entry;	/* not referenced */
	GtkWidget *user_entry;	/* not referenced */
} EMailConfigGroupwiseBackend;

typedef struct {
	EMailConfigServiceBackendClass parent_class;
} EMailConfigGroupwiseBackendClass;

GType e_mail_config_groupwise_backend_get_type (void);
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

G_DEFINE_DYNAMIC_TYPE (EMailConfigGroupwiseBackend, e_mail_config_groupwise_backend, E_TYPE_MAIL_CONFIG_SERVICE_BACKEND)

/* The backend serves the receiving page and the (hidden) sending page; only
 * the receiving page has anything to show and keep. */
static gboolean
is_receiving (EMailConfigServiceBackend *backend)
{
	return E_IS_MAIL_CONFIG_RECEIVING_PAGE (e_mail_config_service_backend_get_page (backend));
}

static ESource *
groupwise_backend_new_collection (EMailConfigServiceBackend *backend)
{
	EMailConfigServiceBackendClass *class = E_MAIL_CONFIG_SERVICE_BACKEND_GET_CLASS (backend);
	ESource *source = e_source_new (NULL, NULL, NULL);

	/* Also for the sending page: so that no [Groupwise Backend] lands in
	 * the transport source */
	e_source_backend_set_backend_name (e_source_get_extension (source, E_SOURCE_EXTENSION_COLLECTION),
		class->backend_name);

	return source;
}

static GtkWidget *
add_row (GtkGrid *grid,
	 gint row,
	 const gchar *mnemonic,
	 GtkWidget *widget)
{
	GtkWidget *label = gtk_label_new_with_mnemonic (mnemonic);

	gtk_label_set_xalign (GTK_LABEL (label), 1.0);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), widget);
	gtk_grid_attach (grid, label, 0, row, 1, 1);
	gtk_grid_attach (grid, widget, 1, row, 1, 1);
	gtk_widget_show (label);
	gtk_widget_show (widget);

	return widget;
}

static void
groupwise_backend_insert_widgets (EMailConfigServiceBackend *backend,
				  GtkBox *parent)
{
	EMailConfigGroupwiseBackend *gw_backend = (EMailConfigGroupwiseBackend *) backend;
	CamelSettings *settings;
	ESource *collection;
	GtkWidget *widget, *grid;
	gchar *markup;

	if (!is_receiving (backend))
		return;

	settings = e_mail_config_service_backend_get_settings (backend);

	markup = g_markup_printf_escaped ("<b>%s</b>", _("Configuration"));
	widget = gtk_label_new (markup);
	gtk_label_set_use_markup (GTK_LABEL (widget), TRUE);
	gtk_label_set_xalign (GTK_LABEL (widget), 0.0);
	gtk_box_pack_start (parent, widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);
	g_free (markup);

	grid = gtk_grid_new ();
	gtk_widget_set_margin_start (grid, 12);
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
	gtk_box_pack_start (parent, grid, FALSE, FALSE, 0);
	gtk_widget_show (grid);

	widget = gtk_entry_new ();
	gtk_widget_set_hexpand (widget, TRUE);
	gw_backend->host_entry = add_row (GTK_GRID (grid), 0, _("_Server (POA):"), widget);
	e_binding_bind_object_text_property (settings, "host", widget, "text",
		G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

	widget = gtk_spin_button_new_with_range (1, 65535, 1);
	gtk_widget_set_halign (widget, GTK_ALIGN_START);
	add_row (GTK_GRID (grid), 1, _("SOAP _port:"), widget);
	e_binding_bind_property (settings, "port", widget, "value",
		G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

	widget = gtk_entry_new ();
	gtk_widget_set_hexpand (widget, TRUE);
	gw_backend->user_entry = add_row (GTK_GRID (grid), 2, _("User_name:"), widget);
	e_binding_bind_object_text_property (settings, "user", widget, "text",
		G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

	widget = gtk_label_new (_("The connection is encrypted (TLS) when the post office offers it. "
		"A certificate of an internal certificate authority is trusted once it is confirmed, or "
		"permanently when the authority is in ~/.config/evolution-groupwise/ca/."));
	gtk_label_set_line_wrap (GTK_LABEL (widget), TRUE);
	gtk_label_set_xalign (GTK_LABEL (widget), 0.0);
	gtk_widget_set_margin_top (widget, 6);
	gtk_grid_attach (GTK_GRID (grid), widget, 0, 3, 2, 1);
	gtk_widget_show (widget);

	/* The collection identity is the user name */
	collection = e_mail_config_service_backend_get_collection (backend);
	if (collection) {
		e_binding_bind_property (settings, "user",
			e_source_get_extension (collection, E_SOURCE_EXTENSION_COLLECTION), "identity",
			G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	}
}

/* From the e-mail address: erika@example.com -> user erika, mail.example.com */
static void
groupwise_backend_setup_defaults (EMailConfigServiceBackend *backend)
{
	EMailConfigServicePage *page;
	CamelNetworkSettings *network_settings;
	const gchar *email_address;
	gchar **parts = NULL;

	if (!is_receiving (backend))
		return;

	page = e_mail_config_service_backend_get_page (backend);
	network_settings = CAMEL_NETWORK_SETTINGS (e_mail_config_service_backend_get_settings (backend));

	camel_network_settings_set_port (network_settings, GW_DEFAULT_PORT);

	email_address = e_mail_config_service_page_get_email_address (page);
	if (email_address)
		parts = g_strsplit (email_address, "@", 2);
	if (parts && g_strv_length (parts) == 2) {
		gchar *host = g_strconcat ("mail.", g_strstrip (parts[1]), NULL);

		camel_network_settings_set_user (network_settings, g_strstrip (parts[0]));
		camel_network_settings_set_host (network_settings, host);
		g_free (host);
	}
	g_strfreev (parts);
}

static gboolean
groupwise_backend_check_complete (EMailConfigServiceBackend *backend)
{
	EMailConfigGroupwiseBackend *gw_backend = (EMailConfigGroupwiseBackend *) backend;
	CamelNetworkSettings *network_settings;
	gchar *host, *user;
	gboolean complete;

	if (!is_receiving (backend))
		return TRUE;

	network_settings = CAMEL_NETWORK_SETTINGS (e_mail_config_service_backend_get_settings (backend));
	host = camel_network_settings_dup_host (network_settings);
	user = camel_network_settings_dup_user (network_settings);
	if (host)
		g_strstrip (host);
	if (user)
		g_strstrip (user);

	complete = host && *host && user && *user;

	if (gw_backend->host_entry)
		e_util_set_entry_issue_hint (gw_backend->host_entry, host && *host ? NULL : _("Server cannot be empty"));
	if (gw_backend->user_entry)
		e_util_set_entry_issue_hint (gw_backend->user_entry, user && *user ? NULL : _("User name cannot be empty"));

	g_free (host);
	g_free (user);

	return complete;
}

/* The collection logs in: it needs server and user (the password prompt
 * and the children read them from there) */
static void
groupwise_backend_commit_changes (EMailConfigServiceBackend *backend)
{
	CamelNetworkSettings *network_settings;
	ESourceAuthentication *auth;
	ESource *collection;
	gchar *host, *user;

	if (!is_receiving (backend))
		return;

	collection = e_mail_config_service_backend_get_collection (backend);
	if (!collection)
		return;

	network_settings = CAMEL_NETWORK_SETTINGS (e_mail_config_service_backend_get_settings (backend));
	host = camel_network_settings_dup_host (network_settings);
	user = camel_network_settings_dup_user (network_settings);

	auth = e_source_get_extension (collection, E_SOURCE_EXTENSION_AUTHENTICATION);
	e_source_authentication_set_host (auth, host ? g_strstrip (host) : NULL);
	e_source_authentication_set_port (auth, camel_network_settings_get_port (network_settings));
	e_source_authentication_set_user (auth, user ? g_strstrip (user) : NULL);
	e_source_collection_set_identity (e_source_get_extension (collection, E_SOURCE_EXTENSION_COLLECTION), user);

	/* Where Evolution keeps the trust in the server certificate for the account */
	e_source_webdav_set_ssl_trust (e_source_get_extension (collection, E_SOURCE_EXTENSION_WEBDAV_BACKEND), "");

	g_free (host);
	g_free (user);
}

static void
e_mail_config_groupwise_backend_class_init (EMailConfigGroupwiseBackendClass *class)
{
	EMailConfigServiceBackendClass *backend_class = E_MAIL_CONFIG_SERVICE_BACKEND_CLASS (class);

	backend_class->backend_name = "groupwise";
	backend_class->new_collection = groupwise_backend_new_collection;
	backend_class->insert_widgets = groupwise_backend_insert_widgets;
	backend_class->setup_defaults = groupwise_backend_setup_defaults;
	backend_class->check_complete = groupwise_backend_check_complete;
	backend_class->commit_changes = groupwise_backend_commit_changes;
}

static void
e_mail_config_groupwise_backend_class_finalize (EMailConfigGroupwiseBackendClass *class)
{
}

static void
e_mail_config_groupwise_backend_init (EMailConfigGroupwiseBackend *backend)
{
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

	e_mail_config_groupwise_backend_register_type (type_module);
	e_groupwise_source_config_type_register (type_module);
#ifdef GW_HAVE_MAIL_UI
	e_groupwise_mail_ui_type_register (type_module);
	e_groupwise_travel_page_type_register (type_module);
#endif
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
}

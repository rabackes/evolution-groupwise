/*
 * camel-groupwise-settings.c: settings of a GroupWise mail account
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

#include "camel-groupwise-settings.h"

struct _CamelGroupwiseSettings {
	CamelOfflineSettings parent;

	gboolean read_only;
	gboolean filter_inbox;
	gboolean run_startup_rules;
	gboolean run_folder_rules;
	GMutex lock;
	gchar *proxy;
};

enum {
	PROP_0,
	PROP_AUTH_MECHANISM,
	PROP_HOST,
	PROP_PORT,
	PROP_SECURITY_METHOD,
	PROP_USER,
	PROP_READ_ONLY,
	PROP_FILTER_INBOX,
	PROP_RUN_STARTUP_RULES,
	PROP_RUN_FOLDER_RULES,
	PROP_PROXY
};

G_DEFINE_TYPE_WITH_CODE (CamelGroupwiseSettings, camel_groupwise_settings, CAMEL_TYPE_OFFLINE_SETTINGS,
	G_IMPLEMENT_INTERFACE (CAMEL_TYPE_NETWORK_SETTINGS, NULL))

/* The CamelNetworkSettings accessors keep the values; the properties only forward to them. */
static void
groupwise_settings_set_property (GObject *object,
				 guint property_id,
				 const GValue *value,
				 GParamSpec *pspec)
{
	CamelNetworkSettings *settings = CAMEL_NETWORK_SETTINGS (object);

	switch (property_id) {
	case PROP_AUTH_MECHANISM:
		camel_network_settings_set_auth_mechanism (settings, g_value_get_string (value));
		return;
	case PROP_HOST:
		camel_network_settings_set_host (settings, g_value_get_string (value));
		return;
	case PROP_PORT:
		camel_network_settings_set_port (settings, g_value_get_uint (value));
		return;
	case PROP_SECURITY_METHOD:
		camel_network_settings_set_security_method (settings, g_value_get_enum (value));
		return;
	case PROP_USER:
		camel_network_settings_set_user (settings, g_value_get_string (value));
		return;
	case PROP_READ_ONLY:
		camel_groupwise_settings_set_read_only (CAMEL_GROUPWISE_SETTINGS (object), g_value_get_boolean (value));
		return;
	case PROP_FILTER_INBOX:
		camel_groupwise_settings_set_filter_inbox (CAMEL_GROUPWISE_SETTINGS (object), g_value_get_boolean (value));
		return;
	case PROP_RUN_STARTUP_RULES:
		camel_groupwise_settings_set_run_startup_rules (CAMEL_GROUPWISE_SETTINGS (object), g_value_get_boolean (value));
		return;
	case PROP_RUN_FOLDER_RULES:
		camel_groupwise_settings_set_run_folder_rules (CAMEL_GROUPWISE_SETTINGS (object), g_value_get_boolean (value));
		return;
	case PROP_PROXY:
		camel_groupwise_settings_set_proxy (CAMEL_GROUPWISE_SETTINGS (object), g_value_get_string (value));
		return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
groupwise_settings_get_property (GObject *object,
				 guint property_id,
				 GValue *value,
				 GParamSpec *pspec)
{
	CamelNetworkSettings *settings = CAMEL_NETWORK_SETTINGS (object);

	switch (property_id) {
	case PROP_AUTH_MECHANISM:
		g_value_take_string (value, camel_network_settings_dup_auth_mechanism (settings));
		return;
	case PROP_HOST:
		g_value_take_string (value, camel_network_settings_dup_host (settings));
		return;
	case PROP_PORT:
		g_value_set_uint (value, camel_network_settings_get_port (settings));
		return;
	case PROP_SECURITY_METHOD:
		g_value_set_enum (value, camel_network_settings_get_security_method (settings));
		return;
	case PROP_USER:
		g_value_take_string (value, camel_network_settings_dup_user (settings));
		return;
	case PROP_READ_ONLY:
		g_value_set_boolean (value, camel_groupwise_settings_get_read_only (CAMEL_GROUPWISE_SETTINGS (object)));
		return;
	case PROP_FILTER_INBOX:
		g_value_set_boolean (value, camel_groupwise_settings_get_filter_inbox (CAMEL_GROUPWISE_SETTINGS (object)));
		return;
	case PROP_RUN_STARTUP_RULES:
		g_value_set_boolean (value, camel_groupwise_settings_get_run_startup_rules (CAMEL_GROUPWISE_SETTINGS (object)));
		return;
	case PROP_RUN_FOLDER_RULES:
		g_value_set_boolean (value, camel_groupwise_settings_get_run_folder_rules (CAMEL_GROUPWISE_SETTINGS (object)));
		return;
	case PROP_PROXY:
		g_value_take_string (value, camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (object)));
		return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
groupwise_settings_finalize (GObject *object)
{
	CamelGroupwiseSettings *settings = CAMEL_GROUPWISE_SETTINGS (object);

	g_free (settings->proxy);
	g_mutex_clear (&settings->lock);

	G_OBJECT_CLASS (camel_groupwise_settings_parent_class)->finalize (object);
}

static void
camel_groupwise_settings_class_init (CamelGroupwiseSettingsClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);

	object_class->set_property = groupwise_settings_set_property;
	object_class->get_property = groupwise_settings_get_property;
	object_class->finalize = groupwise_settings_finalize;

	/* Inherited from CamelNetworkSettings */
	g_object_class_override_property (object_class, PROP_AUTH_MECHANISM, "auth-mechanism");
	g_object_class_override_property (object_class, PROP_HOST, "host");
	g_object_class_override_property (object_class, PROP_PORT, "port");
	g_object_class_override_property (object_class, PROP_SECURITY_METHOD, "security-method");
	g_object_class_override_property (object_class, PROP_USER, "user");

	g_object_class_install_property (object_class, PROP_READ_ONLY,
		g_param_spec_boolean ("read-only", "Read Only",
			"Never change anything on the server",
			FALSE, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

	/* Named like the IMAP setting, off like there: GroupWise filters on the server */
	g_object_class_install_property (object_class, PROP_FILTER_INBOX,
		g_param_spec_boolean ("filter-inbox", "Filter Inbox",
			"Run Evolution's filters and junk test on new messages in the Mailbox",
			FALSE, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

	/* The rules of the GroupWise client's events: Evolution runs them
	 * (executeRule) when the user wants it */
	g_object_class_install_property (object_class, PROP_RUN_STARTUP_RULES,
		g_param_spec_boolean ("run-startup-rules", "Run Startup Rules",
			"Run the rules of the events Startup and Exit when Evolution starts and quits",
			FALSE, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class, PROP_RUN_FOLDER_RULES,
		g_param_spec_boolean ("run-folder-rules", "Run Folder Rules",
			"Run the rules of the events Open Folder and Close Folder when a folder is opened or left",
			FALSE, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

	/* A proxy account: the mailbox of this user, with the login of the
	 * account's own user (its password is the one of the main account) */
	g_object_class_install_property (object_class, PROP_PROXY,
		g_param_spec_string ("proxy", "Proxy",
			"E-mail address of the user whose mailbox a proxy account shows",
			NULL, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));
}

static void
camel_groupwise_settings_init (CamelGroupwiseSettings *settings)
{
	g_mutex_init (&settings->lock);
}

gboolean
camel_groupwise_settings_get_read_only (CamelGroupwiseSettings *settings)
{
	g_return_val_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings), FALSE);

	return settings->read_only;
}

void
camel_groupwise_settings_set_read_only (CamelGroupwiseSettings *settings,
					gboolean read_only)
{
	g_return_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings));

	if (settings->read_only == read_only)
		return;

	settings->read_only = read_only;
	g_object_notify (G_OBJECT (settings), "read-only");
}

gboolean
camel_groupwise_settings_get_filter_inbox (CamelGroupwiseSettings *settings)
{
	g_return_val_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings), FALSE);

	return settings->filter_inbox;
}

void
camel_groupwise_settings_set_filter_inbox (CamelGroupwiseSettings *settings,
					   gboolean filter_inbox)
{
	g_return_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings));

	if (settings->filter_inbox == filter_inbox)
		return;

	settings->filter_inbox = filter_inbox;
	g_object_notify (G_OBJECT (settings), "filter-inbox");
}

gchar *
camel_groupwise_settings_dup_proxy (CamelGroupwiseSettings *settings)
{
	gchar *proxy;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings), NULL);

	g_mutex_lock (&settings->lock);
	proxy = g_strdup (settings->proxy);
	g_mutex_unlock (&settings->lock);

	return proxy;
}

void
camel_groupwise_settings_set_proxy (CamelGroupwiseSettings *settings,
				    const gchar *proxy)
{
	g_return_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings));

	if (proxy && !*proxy)
		proxy = NULL;

	g_mutex_lock (&settings->lock);
	if (g_strcmp0 (settings->proxy, proxy) == 0) {
		g_mutex_unlock (&settings->lock);
		return;
	}
	g_free (settings->proxy);
	settings->proxy = g_strdup (proxy);
	g_mutex_unlock (&settings->lock);

	g_object_notify (G_OBJECT (settings), "proxy");
}

gboolean
camel_groupwise_settings_get_run_startup_rules (CamelGroupwiseSettings *settings)
{
	g_return_val_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings), FALSE);

	return settings->run_startup_rules;
}

void
camel_groupwise_settings_set_run_startup_rules (CamelGroupwiseSettings *settings,
						gboolean run_startup_rules)
{
	g_return_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings));

	if (settings->run_startup_rules == run_startup_rules)
		return;

	settings->run_startup_rules = run_startup_rules;
	g_object_notify (G_OBJECT (settings), "run-startup-rules");
}

gboolean
camel_groupwise_settings_get_run_folder_rules (CamelGroupwiseSettings *settings)
{
	g_return_val_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings), FALSE);

	return settings->run_folder_rules;
}

void
camel_groupwise_settings_set_run_folder_rules (CamelGroupwiseSettings *settings,
					       gboolean run_folder_rules)
{
	g_return_if_fail (CAMEL_IS_GROUPWISE_SETTINGS (settings));

	if (settings->run_folder_rules == run_folder_rules)
		return;

	settings->run_folder_rules = run_folder_rules;
	g_object_notify (G_OBJECT (settings), "run-folder-rules");
}

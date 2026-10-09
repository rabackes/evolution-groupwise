/*
 * e-groupwise-ui-utils.c: helpers of the Evolution module
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

#include <glib/gi18n-lib.h>

#include "e-gw-backend-utils.h"

#include "e-groupwise-ui-utils.h"

ESource *
e_groupwise_ui_ref_account_source (ESourceRegistry *registry,
				   CamelService *service)
{
	CamelProvider *provider = service ? camel_service_get_provider (service) : NULL;

	if (!provider || g_strcmp0 (provider->protocol, "groupwise") != 0)
		return NULL;

	return e_source_registry_ref_source (registry, camel_service_get_uid (service));
}

gchar *
e_groupwise_ui_dup_proxy (ESource *source)
{
	const gchar *extension_name;

	g_return_val_if_fail (E_IS_SOURCE (source), NULL);

	e_gw_backend_ensure_types ();
	extension_name = e_source_camel_get_extension_name ("groupwise");
	if (!e_source_has_extension (source, extension_name))
		return NULL;

	return camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (
		e_source_camel_get_settings (e_source_get_extension (source, extension_name))));
}

gboolean
e_groupwise_ui_is_proxy_store (CamelService *service)
{
	CamelProvider *provider = service ? camel_service_get_provider (service) : NULL;
	CamelSettings *settings;
	gchar *proxy = NULL;
	gboolean is_proxy;

	if (!provider || g_strcmp0 (provider->protocol, "groupwise") != 0)
		return FALSE;

	settings = camel_service_ref_settings (service);
	if (CAMEL_IS_GROUPWISE_SETTINGS (settings))
		proxy = camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (settings));
	g_clear_object (&settings);
	is_proxy = proxy != NULL;
	g_free (proxy);

	return is_proxy;
}

static EGwConnection *
connect_account_sync (ESourceRegistry *registry,
		      ESource *account_source,
		      gboolean for_settings,
		      GCancellable *cancellable,
		      GError **error)
{
	CamelGroupwiseSettings *settings;
	ENamedParameters *credentials;
	ESourceAuthenticationResult result = E_SOURCE_AUTHENTICATION_ERROR;
	ESource *collection, *source;
	EGwConnection *cnc;
	gchar *password = NULL, *proxy;

	/* The password is kept with the collection of the account (or with a
	 * single mail account, like a proxy account) */
	collection = e_source_registry_find_extension (registry, account_source, E_SOURCE_EXTENSION_COLLECTION);
	source = collection ? collection : account_source;
	if (!e_source_lookup_password_sync (source, cancellable, &password, NULL) || !password) {
		g_clear_pointer (&password, e_util_safe_free_string);
		if (source != account_source)
			e_source_lookup_password_sync (account_source, cancellable, &password, NULL);
	}
	if (!password || !*password) {
		e_util_safe_free_string (password);
		g_clear_object (&collection);
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			_("The password of the account is not in the keyring; let Evolution remember it"));
		return NULL;
	}

	settings = e_gw_backend_ref_settings (registry, account_source);
	if (!settings) {
		e_util_safe_free_string (password);
		g_clear_object (&collection);
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The account has no GroupWise settings"));
		return NULL;
	}

	credentials = e_named_parameters_new ();
	e_named_parameters_set (credentials, E_SOURCE_CREDENTIAL_PASSWORD, password);
	e_util_safe_free_string (password);

	/* A proxy account: the settings of the other mailbox, in a proxy login.
	 * The trust in the server certificate is kept in the collection. */
	proxy = camel_groupwise_settings_dup_proxy (settings);
	cnc = e_gw_backend_connect_sync (registry, source, settings, proxy, credentials, &result,
		NULL, NULL, cancellable, error);
	e_named_parameters_free (credentials);
	g_object_unref (settings);
	g_clear_object (&collection);

	if (!cnc && error && !*error)
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			result == E_SOURCE_AUTHENTICATION_REJECTED ? _("The password was not accepted") :
			_("Cannot connect to the GroupWise server"));

	/* Its settings only with the right to change them */
	if (for_settings && cnc && proxy && !(e_gw_connection_get_proxy_rights (cnc) & E_GW_PROXY_SETUP)) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			_("%s has not granted you the right to change settings, rules and folders"), proxy);
		e_gw_connection_logout_sync (cnc, NULL);
		g_clear_object (&cnc);
	}
	g_free (proxy);

	return cnc;
}

EGwConnection *
e_groupwise_ui_connect_sync (ESourceRegistry *registry,
			     ESource *account_source,
			     GCancellable *cancellable,
			     GError **error)
{
	return connect_account_sync (registry, account_source, TRUE, cancellable, error);
}

EGwConnection *
e_groupwise_ui_connect_items_sync (ESourceRegistry *registry,
				   ESource *account_source,
				   GCancellable *cancellable,
				   GError **error)
{
	return connect_account_sync (registry, account_source, FALSE, cancellable, error);
}

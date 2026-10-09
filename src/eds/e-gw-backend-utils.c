/*
 * e-gw-backend-utils.c: GroupWise connections for EDS backends
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

#include "e-source-groupwise-folder.h"
#include "e-gw-backend-utils.h"

void
e_gw_backend_ensure_types (void)
{
	e_source_camel_generate_subtype ("groupwise", CAMEL_TYPE_GROUPWISE_SETTINGS);
	g_type_ensure (E_TYPE_SOURCE_GROUPWISE_FOLDER);
}

CamelGroupwiseSettings *
e_gw_backend_ref_settings (ESourceRegistry *registry,
			   ESource *source)
{
	const gchar *extension_name;
	ESource *collection;
	CamelSettings *settings;

	e_gw_backend_ensure_types ();
	extension_name = e_source_camel_get_extension_name ("groupwise");

	collection = registry ? e_source_registry_find_extension (registry, source, extension_name) : NULL;
	if (!collection && e_source_has_extension (source, extension_name))
		collection = g_object_ref (source);
	if (!collection)
		return NULL;

	settings = e_source_camel_get_settings (e_source_get_extension (collection, extension_name));
	g_object_ref (settings);
	g_object_unref (collection);

	return CAMEL_GROUPWISE_SETTINGS (settings);
}

typedef struct {
	ESourceRegistry *registry;
	ESource *source;
	gchar *host;
} TrustData;

static void
trust_data_free (TrustData *data)
{
	g_clear_object (&data->registry);
	g_clear_object (&data->source);
	g_free (data->host);
	g_free (data);
}

static gboolean
trusted_for (ESource *source,
	     const gchar *host,
	     GTlsCertificate *certificate,
	     GTlsCertificateFlags errors)
{
	ETrustPromptResponse response;

	response = e_source_webdav_verify_ssl_trust (e_source_get_extension (source, E_SOURCE_EXTENSION_WEBDAV_BACKEND),
		host, certificate, errors);

	return response == E_TRUST_PROMPT_RESPONSE_ACCEPT || response == E_TRUST_PROMPT_RESPONSE_ACCEPT_TEMPORARILY;
}

/* The user's decision from the trust prompt of EDS, kept in the source or
 * in its collection (where the account's prompt put it) */
static gboolean
accept_certificate (EGwConnection *cnc,
		    GTlsCertificate *certificate,
		    GTlsCertificateFlags errors,
		    gpointer user_data)
{
	TrustData *data = user_data;
	ESource *collection = NULL;
	gboolean trusted;

	trusted = trusted_for (data->source, data->host, certificate, errors);
	if (!trusted && data->registry)
		collection = e_source_registry_find_extension (data->registry, data->source, E_SOURCE_EXTENSION_COLLECTION);
	if (!trusted && collection)
		trusted = trusted_for (collection, data->host, certificate, errors);
	g_clear_object (&collection);

	return trusted;
}

EGwConnection *
e_gw_backend_connect_sync (ESourceRegistry *registry,
			   ESource *source,
			   CamelGroupwiseSettings *settings,
			   const gchar *proxy,
			   const ENamedParameters *credentials,
			   ESourceAuthenticationResult *out_result,
			   gchar **out_certificate_pem,
			   GTlsCertificateFlags *out_certificate_errors,
			   GCancellable *cancellable,
			   GError **error)
{
	CamelNetworkSettings *network = CAMEL_NETWORK_SETTINGS (settings);
	EGwConnection *cnc;
	TrustData *trust;
	GError *local_error = NULL;
	const gchar *password;
	gchar *host, *user;

	g_return_val_if_fail (out_result != NULL, NULL);

	password = credentials ? e_named_parameters_get (credentials, E_SOURCE_CREDENTIAL_PASSWORD) : NULL;
	if (!password || !*password) {
		*out_result = E_SOURCE_AUTHENTICATION_REQUIRED;
		return NULL;
	}

	host = camel_network_settings_dup_host (network);
	user = camel_network_settings_dup_user (network);

	cnc = e_gw_connection_new (host, camel_network_settings_get_port (network),
		camel_network_settings_get_security_method (network) != CAMEL_NETWORK_SECURITY_METHOD_NONE);

	trust = g_new0 (TrustData, 1);
	trust->registry = registry ? g_object_ref (registry) : NULL;
	trust->source = g_object_ref (source);
	trust->host = g_strdup (host);
	e_gw_connection_set_certificate_func (cnc, accept_certificate, trust, (GDestroyNotify) trust_data_free);
	e_gw_connection_set_proxy (cnc, proxy);

	if (e_gw_connection_login_sync (cnc, user, password, cancellable, &local_error)) {
		*out_result = E_SOURCE_AUTHENTICATION_ACCEPTED;
	} else {
		gchar *pem = NULL;
		GTlsCertificateFlags errors = 0;

		if (g_error_matches (local_error, E_GW_ERROR, E_GW_ERROR_INVALID_PASSWORD)) {
			*out_result = E_SOURCE_AUTHENTICATION_REJECTED;
			g_clear_error (&local_error);
		} else if (e_gw_connection_get_refused_certificate (cnc, &pem, &errors)) {
			/* EDS asks the user and keeps the answer in the source */
			*out_result = E_SOURCE_AUTHENTICATION_ERROR_SSL_FAILED;
			if (out_certificate_pem)
				*out_certificate_pem = pem;
			else
				g_free (pem);
			if (out_certificate_errors)
				*out_certificate_errors = errors;
		} else {
			*out_result = E_SOURCE_AUTHENTICATION_ERROR;
		}

		if (local_error)
			g_propagate_error (error, local_error);
		g_clear_object (&cnc);
	}

	g_free (host);
	g_free (user);

	return cnc;
}

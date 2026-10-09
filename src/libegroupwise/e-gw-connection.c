/*
 * e-gw-connection.c: a session with the SOAP interface of a GroupWise POA
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
 *
 * The requests mirror gwsoap.pas and gw_soap.py of the gwapi project:
 * envelope layout, login variants and element names.
 */

#include <string.h>

#include <glib/gi18n-lib.h>
#include <libsoup/soup.h>
#include <libxml/parser.h>

#include "e-gw-connection.h"
#include "e-gw-proxy.h"
#include "e-gw-xml.h"

#define ENVELOPE_START \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>" \
	"<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\"" \
	" xmlns:SOAP-ENC=\"http://schemas.xmlsoap.org/soap/encoding/\"" \
	" xmlns:xsd=\"http://www.w3.org/1999/XMLSchema\"" \
	" xmlns:xsi=\"http://www.w3.org/1999/XMLSchema-instance\">" \
	"<SOAP-ENV:Header SOAP-ENV:encodingStyle=\"\">"
#define ENVELOPE_BODY \
	"</SOAP-ENV:Header>" \
	"<SOAP-ENV:Body xmlns:types=\"http://schemas.novell.com/2003/10/NCSP/types.xsd\"" \
	" SOAP-ENV:encodingStyle=\"\">"
#define ENVELOPE_END \
	"</SOAP-ENV:Body></SOAP-ENV:Envelope>"

#define DEFAULT_TIMEOUT 60
#define APPLICATION "Evolution"

struct _EGwResponse {
	xmlDoc *doc;
	xmlNode *node;
};

struct _EGwConnection {
	GObject parent;

	/* Serializes requests: one POA session is one conversation */
	GRecMutex lock;
	SoupSession *soup;

	gchar *host;
	guint16 port;
	gboolean use_ssl;
	gboolean verify_ssl;

	EGwCertificateFunc certificate_func;
	gpointer certificate_data;
	GDestroyNotify certificate_destroy;
	GTlsCertificate *accepted_certificate;
	gchar *refused_pem;
	GTlsCertificateFlags refused_errors;

	gchar *session;
	gchar *login_user;
	gchar *login_password;
	gchar *proxy;		/* login as this user's proxy */
	EGwProxyRights proxy_rights;

	gchar *user_name;
	gchar *user_email;
	gchar *user_id;
	gchar *user_uuid;
	gchar *server_version;
};

G_DEFINE_QUARK (e-gw-error-quark, e_gw_error)

G_DEFINE_TYPE (EGwConnection, e_gw_connection, G_TYPE_OBJECT)

xmlNode *
e_gw_response_get_node (EGwResponse *response)
{
	g_return_val_if_fail (response != NULL, NULL);

	return response->node;
}

void
e_gw_response_free (EGwResponse *response)
{
	if (response) {
		xmlFreeDoc (response->doc);
		g_free (response);
	}
}

static void
clear_user_info (EGwConnection *cnc)
{
	g_clear_pointer (&cnc->session, g_free);
	g_clear_pointer (&cnc->user_name, g_free);
	g_clear_pointer (&cnc->user_email, g_free);
	g_clear_pointer (&cnc->user_id, g_free);
	g_clear_pointer (&cnc->user_uuid, g_free);
	g_clear_pointer (&cnc->server_version, g_free);
}

static void
clear_credentials (EGwConnection *cnc)
{
	if (cnc->login_password)
		memset (cnc->login_password, 0, strlen (cnc->login_password));
	g_clear_pointer (&cnc->login_password, g_free);
	g_clear_pointer (&cnc->login_user, g_free);
}

static gchar *
build_url (EGwConnection *cnc,
	   const gchar *path)
{
	return g_strdup_printf ("%s://%s:%u/%s", cnc->use_ssl ? "https" : "http", cnc->host, cnc->port, path);
}

/* The CA directory of the user: certificates of GroupWise agents signed by
 * a CA there count as trusted, without root rights and across renewals of
 * the agent certificates ($GROUPWISE_CA_DIR replaces the directory). */
static gchar *
user_ca_directory (void)
{
	const gchar *dir = g_getenv ("GROUPWISE_CA_DIR");

	return dir ? g_strdup (dir) : g_build_filename (g_get_user_config_dir (), "evolution-groupwise", "ca", NULL);
}

static gboolean
signed_by_user_ca (GTlsCertificate *certificate,
		   const gchar *host)
{
	gchar *path = user_ca_directory ();
	GDir *dir = g_dir_open (path, 0, NULL);
	GSocketConnectable *identity = g_network_address_new (host, 0);
	const gchar *name;
	gboolean trusted = FALSE;

	while (dir && !trusted && (name = g_dir_read_name (dir)) != NULL) {
		gchar *file = g_build_filename (path, name, NULL);
		GList *cas = g_tls_certificate_list_new_from_file (file, NULL), *link;

		for (link = cas; link && !trusted; link = g_list_next (link)) {
			if (g_tls_certificate_verify (certificate, identity, link->data) == 0) {
				g_debug ("certificate of %s signed by the CA in %s", host, file);
				trusted = TRUE;
			}
		}

		g_list_free_full (cas, g_object_unref);
		g_free (file);
	}

	if (dir)
		g_dir_close (dir);
	g_object_unref (identity);
	g_free (path);

	return trusted;
}

/* Only called for certificates GIO does not trust */
static gboolean
accept_certificate_cb (SoupMessage *message,
		       GTlsCertificate *certificate,
		       GTlsCertificateFlags errors,
		       EGwConnection *cnc)
{
	if (!cnc->verify_ssl)
		return TRUE;

	/* Asked once per connection, not for every request */
	if (cnc->accepted_certificate && g_tls_certificate_is_same (cnc->accepted_certificate, certificate))
		return TRUE;

	if (signed_by_user_ca (certificate, cnc->host)) {
		g_set_object (&cnc->accepted_certificate, certificate);
		return TRUE;
	}

	g_debug ("certificate not trusted by the system (flags 0x%x), asking", errors);
	if (cnc->certificate_func && cnc->certificate_func (cnc, certificate, errors, cnc->certificate_data)) {
		g_set_object (&cnc->accepted_certificate, certificate);
		return TRUE;
	}

	/* For the trust prompt of EDS, which wants the certificate as PEM */
	g_clear_pointer (&cnc->refused_pem, g_free);
	g_object_get (certificate, "certificate-pem", &cnc->refused_pem, NULL);
	cnc->refused_errors = errors;

	return FALSE;
}

static SoupMessage *
new_message (EGwConnection *cnc,
	     const gchar *method,
	     const gchar *url)
{
	SoupMessage *message = soup_message_new (method, url);

	if (message)
		g_signal_connect (message, "accept-certificate", G_CALLBACK (accept_certificate_cb), cnc);

	return message;
}

static gboolean
bytes_contain_xml (GBytes *bytes)
{
	gsize size;
	const gchar *data = g_bytes_get_data (bytes, &size);

	return data && memchr (data, '<', size);
}

/* Parses a SOAP response. Returns NULL with @error set when the document is
 * unusable; otherwise the response and its status in @code / @description. */
static EGwResponse *
parse_response (GBytes *bytes,
		gint64 *code,
		gchar **description,
		GError **error)
{
	gsize size;
	const gchar *data = g_bytes_get_data (bytes, &size);
	const gchar *start = data ? memchr (data, '<', size) : NULL;
	EGwResponse *response;
	xmlNode *body, *node;
	gchar *code_text;

	if (!start) {
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_EMPTY, _("Empty document"));
		return NULL;
	}

	response = g_new0 (EGwResponse, 1);
	response->doc = xmlReadMemory (start, size - (start - data), NULL, "UTF-8", XML_PARSE_NONET);
	body = response->doc ? e_gw_xml_find (xmlDocGetRootElement (response->doc), "Body") : NULL;
	node = e_gw_xml_first_child (body, NULL);
	if (!node) {
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_EMPTY, _("XML decoding error: no SOAP body"));
		e_gw_response_free (response);
		return NULL;
	}
	response->node = node;

	if (g_strcmp0 ((const gchar *) node->name, "Fault") == 0) {
		gchar *fault = e_gw_xml_dup_text (node, "faultstring");

		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, fault ? fault : _("SOAP fault"));
		g_free (fault);
		e_gw_response_free (response);
		return NULL;
	}

	code_text = e_gw_xml_dup_text (node, "status/code");
	if (!code_text || !*code_text || !g_ascii_string_to_signed (code_text, 10, G_MININT32, G_MAXINT32, code, NULL)) {
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, _("XML decoding error: no status"));
		g_free (code_text);
		e_gw_response_free (response);
		return NULL;
	}
	g_free (code_text);

	*description = e_gw_xml_dup_text (node, "status/description");

	return response;
}

/* The POA leaves the description empty for some common errors */
static const gchar *
describe_status (gint64 code)
{
	switch (code) {
	case E_GW_ERROR_INVALID_PASSWORD:
		return _("The password is not correct");
	case E_GW_ERROR_UNKNOWN_USER:
		return _("The user was not found on the post office");
	case E_GW_ERROR_INVALID_SESSION:
		return _("The session with the server is no longer valid");
	default:
		return _("**No Error Description given**");
	}
}

/* One request without session recovery. A GroupWise status other than 0
 * fails; the response is then handed out in @failed_response if requested. */

static EGwResponse *
post_request_once (EGwConnection *cnc,
	      const gchar *action,
	      const gchar *inner_xml,
	      gboolean with_session,
	      EGwResponse **failed_response,
	      GCancellable *cancellable,
	      GError **error)
{
	GString *envelope;
	GBytes *payload, *bytes = NULL;
	gchar *soap_action;
	gboolean flipped = FALSE;
	EGwResponse *response;
	gint64 code = 0;
	gchar *description = NULL;

	envelope = g_string_new (ENVELOPE_START);
	if (with_session && cnc->session)
		e_gw_xml_add_leaf (envelope, "session", cnc->session);
	g_string_append (envelope, ENVELOPE_BODY);
	g_string_append_printf (envelope, "<%sRequest>%s</%sRequest>", action, inner_xml ? inner_xml : "", action);
	g_string_append (envelope, ENVELOPE_END);
	payload = g_string_free_to_bytes (envelope);
	soap_action = g_strconcat (action, "Request", NULL);

	while (TRUE) {
		gchar *url = build_url (cnc, "soap");
		SoupMessage *message = new_message (cnc, SOUP_METHOD_POST, url);
		GError *local_error = NULL;
		guint status;

		if (!message) {
			g_set_error (error, E_GW_ERROR, E_GW_ERROR_CONNECTION, _("Invalid server address %s"), url);
			g_free (url);
			break;
		}

		/* A POST must not silently turn into a GET on redirects */
		soup_message_add_flags (message, SOUP_MESSAGE_NO_REDIRECT);
		soup_message_headers_append (soup_message_get_request_headers (message), "SOAPAction", soap_action);
		soup_message_set_request_body_from_bytes (message, "text/xml; charset=utf-8", payload);

		bytes = soup_session_send_and_read (cnc->soup, message, cancellable, &local_error);
		status = soup_message_get_status (message);

		if (!bytes && !cnc->use_ssl && !flipped &&
		    g_error_matches (local_error, SOUP_SESSION_ERROR, SOUP_SESSION_ERROR_PARSING)) {
			/* A POA that requires SSL answers plain http with a TLS alert
			 * in front of its redirect, which is no valid HTTP response */
			cnc->use_ssl = TRUE;
			flipped = TRUE;
			g_clear_error (&local_error);
			g_object_unref (message);
			g_free (url);
			continue;
		} else if (!bytes) {
			if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
				g_propagate_error (error, local_error);
			else {
				g_set_error (error, E_GW_ERROR, E_GW_ERROR_CONNECTION,
					_("Error connecting to %s: %s"), url, local_error->message);
				g_error_free (local_error);
			}
		} else if (SOUP_STATUS_IS_REDIRECTION (status) && !flipped) {
			/* The POA answers plain http with a redirect if it wants SSL (and vice versa) */
			cnc->use_ssl = !cnc->use_ssl;
			flipped = TRUE;
			g_clear_pointer (&bytes, g_bytes_unref);
			g_object_unref (message);
			g_free (url);
			continue;
		} else if (status != SOUP_STATUS_OK && !bytes_contain_xml (bytes)) {
			g_set_error (error, E_GW_ERROR, E_GW_ERROR_CONNECTION, _("HTTP %u %s"),
				status, soup_message_get_reason_phrase (message));
			g_clear_pointer (&bytes, g_bytes_unref);
		}

		g_object_unref (message);
		g_free (url);
		break;
	}

	g_bytes_unref (payload);
	g_free (soap_action);

	if (!bytes)
		return NULL;

	response = parse_response (bytes, &code, &description, error);
	g_bytes_unref (bytes);

	if (response && code != 0) {
		g_set_error_literal (error, E_GW_ERROR, (gint) code,
			description && *description ? description : describe_status (code));
		if (failed_response)
			*failed_response = response;
		else
			e_gw_response_free (response);
		response = NULL;
	}
	g_free (description);

	return response;
}

/* The POA now and then answers with an empty or truncated document; the
 * request is simply sent again (gwapi/gac do the same, up to 5 times). */
#define MAX_ATTEMPTS 5

static EGwResponse *
post_request (EGwConnection *cnc,
	      const gchar *action,
	      const gchar *inner_xml,
	      gboolean with_session,
	      EGwResponse **failed_response,
	      GCancellable *cancellable,
	      GError **error)
{
	GError *local_error = NULL;
	EGwResponse *response = NULL;
	guint attempt;

	for (attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
		g_clear_error (&local_error);
		response = post_request_once (cnc, action, inner_xml, with_session, failed_response, cancellable, &local_error);

		if (response || !g_error_matches (local_error, E_GW_ERROR, E_GW_ERROR_EMPTY) ||
		    g_cancellable_is_cancelled (cancellable))
			break;
	}

	/* G_MESSAGES_DEBUG=evolution-groupwise; never shows request contents (passwords) */
	if (local_error)
		g_debug ("%s %s://%s:%u: error %d: %s (%u attempts)", action, cnc->use_ssl ? "https" : "http",
			cnc->host, cnc->port, local_error->code, local_error->message, MIN (attempt, MAX_ATTEMPTS));
	else
		g_debug ("%s %s://%s:%u: ok", action, cnc->use_ssl ? "https" : "http", cnc->host, cnc->port);

	if (local_error)
		g_propagate_error (error, local_error);

	return response;
}

static gboolean
login_locked (EGwConnection *cnc,
	      GCancellable *cancellable,
	      GError **error)
{
	GString *inner;
	EGwResponse *response = NULL;
	gboolean redirected = FALSE;
	xmlNode *node;

	clear_user_info (cnc);

	inner = g_string_new (cnc->proxy ? "<types:auth type=\"types:Proxy\">" : "<types:auth type=\"types:PlainText\">");
	e_gw_xml_add_leaf (inner, "types:username", cnc->login_user);
	e_gw_xml_add_leaf (inner, "types:password", cnc->login_password);
	if (cnc->proxy)
		e_gw_xml_add_leaf (inner, "types:proxy", cnc->proxy);
	g_string_append (inner, "</types:auth>");
	/* The API version the POA answers in (without one: the oldest): the
	 * one GroupWise 26 uses itself (GroupWise Web). $GROUPWISE_SOAP_VERSION
	 * sets another one for experiments, an empty value sends none. */
	{
		const gchar *version = g_getenv ("GROUPWISE_SOAP_VERSION");

		if (!version)
			version = E_GW_SOAP_VERSION;
		if (*version)
			e_gw_xml_add_leaf (inner, "version", version);
	}
	e_gw_xml_add_leaf (inner, "application", APPLICATION);
	e_gw_xml_add_bool (inner, "userid", TRUE);
	e_gw_xml_add_bool (inner, "system", TRUE);

	while (TRUE) {
		EGwResponse *failed = NULL;
		GError *local_error = NULL;
		gchar *ip;

		response = post_request (cnc, "login", inner->str, FALSE, &failed, cancellable, &local_error);
		if (response)
			break;

		/* The user lives on another post office: its POA is named in the response */
		ip = failed ? e_gw_xml_dup_text (e_gw_response_get_node (failed), "redirectToHost/ipAddress") : NULL;
		if (ip && *ip && !redirected) {
			gint64 port = e_gw_xml_get_int (e_gw_response_get_node (failed), "redirectToHost/port", 0);

			g_free (cnc->host);
			cnc->host = ip;
			if (port > 0 && port <= G_MAXUINT16)
				cnc->port = (guint16) port;
			redirected = TRUE;
			e_gw_response_free (failed);
			g_error_free (local_error);
			continue;
		}

		g_free (ip);
		e_gw_response_free (failed);
		g_propagate_error (error, local_error);
		break;
	}

	/* The password is part of the request; do not leave it in freed memory */
	memset (inner->str, 0, inner->len);
	g_string_free (inner, TRUE);

	if (!response)
		return FALSE;

	node = e_gw_response_get_node (response);
	cnc->session = e_gw_xml_dup_text (node, "session");
	if (!cnc->session || !*cnc->session) {
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, _("The server returned no session"));
		clear_user_info (cnc);
		e_gw_response_free (response);
		return FALSE;
	}

	cnc->user_name = e_gw_xml_dup_text (node, "userinfo/name");
	cnc->user_email = e_gw_xml_dup_text (node, "userinfo/email");
	cnc->user_id = e_gw_xml_dup_text (node, "userinfo/userid");
	cnc->user_uuid = e_gw_xml_dup_text (node, "userinfo/uuid");
	cnc->server_version = e_gw_xml_dup_text (node, "gwVersion");
	cnc->proxy_rights = cnc->proxy ? e_gw_proxy_rights_from_entry (e_gw_xml_find (node, "entry")) : 0;
	if (cnc->proxy && !cnc->user_email)
		cnc->user_email = e_gw_xml_dup_text (node, "entry/email");
	if (cnc->proxy && !cnc->user_name)
		cnc->user_name = e_gw_xml_dup_text (node, "entry/displayName");

	e_gw_response_free (response);

	return TRUE;
}

/* A lost or timed out session: 59910 on GroupWise 26 (without description);
 * other POAs name the session in the description */
static gboolean
is_session_error (const GError *error)
{
	gchar *lower;
	gboolean result;

	if (!error || error->domain != E_GW_ERROR || error->code < 10000)
		return FALSE;

	if (error->code == E_GW_ERROR_INVALID_SESSION)
		return TRUE;

	lower = g_utf8_strdown (error->message, -1);
	result = strstr (lower, "session") != NULL;
	g_free (lower);

	return result;
}

static void
e_gw_connection_finalize (GObject *object)
{
	EGwConnection *cnc = E_GW_CONNECTION (object);

	clear_user_info (cnc);
	clear_credentials (cnc);
	if (cnc->certificate_destroy)
		cnc->certificate_destroy (cnc->certificate_data);
	g_clear_object (&cnc->accepted_certificate);
	g_free (cnc->refused_pem);
	g_free (cnc->proxy);
	g_free (cnc->host);
	g_clear_object (&cnc->soup);
	g_rec_mutex_clear (&cnc->lock);

	G_OBJECT_CLASS (e_gw_connection_parent_class)->finalize (object);
}

static void
e_gw_connection_class_init (EGwConnectionClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);

	object_class->finalize = e_gw_connection_finalize;
}

static void
e_gw_connection_init (EGwConnection *cnc)
{
	g_rec_mutex_init (&cnc->lock);
	cnc->verify_ssl = TRUE;
	cnc->soup = soup_session_new_with_options (
		"timeout", DEFAULT_TIMEOUT,
		"user-agent", "evolution-groupwise/" PACKAGE_VERSION,
		NULL);
}

EGwConnection *
e_gw_connection_new (const gchar *host,
		     guint16 port,
		     gboolean use_ssl)
{
	EGwConnection *cnc;

	g_return_val_if_fail (host != NULL, NULL);

	cnc = g_object_new (E_TYPE_GW_CONNECTION, NULL);
	cnc->host = g_strdup (host);
	cnc->port = port ? port : E_GW_DEFAULT_PORT;
	cnc->use_ssl = use_ssl;

	return cnc;
}

void
e_gw_connection_set_verify_ssl (EGwConnection *cnc,
				gboolean verify)
{
	g_return_if_fail (E_IS_GW_CONNECTION (cnc));

	cnc->verify_ssl = verify;
}

void
e_gw_connection_set_certificate_func (EGwConnection *cnc,
				      EGwCertificateFunc func,
				      gpointer user_data,
				      GDestroyNotify destroy)
{
	g_return_if_fail (E_IS_GW_CONNECTION (cnc));

	if (cnc->certificate_destroy)
		cnc->certificate_destroy (cnc->certificate_data);

	cnc->certificate_func = func;
	cnc->certificate_data = user_data;
	cnc->certificate_destroy = destroy;
}

void
e_gw_connection_set_timeout (EGwConnection *cnc,
			     guint seconds)
{
	g_return_if_fail (E_IS_GW_CONNECTION (cnc));

	g_object_set (cnc->soup, "timeout", seconds, NULL);
}

const gchar *
e_gw_connection_get_host (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	return cnc->host;
}

guint16
e_gw_connection_get_port (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), 0);

	return cnc->port;
}

gboolean
e_gw_connection_get_use_ssl (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);

	return cnc->use_ssl;
}

void
e_gw_connection_set_proxy (EGwConnection *cnc,
			   const gchar *email)
{
	g_return_if_fail (E_IS_GW_CONNECTION (cnc));

	g_rec_mutex_lock (&cnc->lock);
	g_free (cnc->proxy);
	cnc->proxy = email && *email ? g_strdup (email) : NULL;
	g_rec_mutex_unlock (&cnc->lock);
}

const gchar *
e_gw_connection_get_proxy (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	return cnc->proxy;
}

EGwProxyRights
e_gw_connection_get_proxy_rights (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), 0);

	return cnc->proxy_rights;
}

void
e_gw_proxy_user_free (EGwProxyUser *user)
{
	if (!user)
		return;

	g_free (user->uuid);
	g_free (user->email);
	g_free (user->display_name);
	g_free (user);
}

GPtrArray *
e_gw_connection_get_proxy_users_sync (EGwConnection *cnc,
				      GCancellable *cancellable,
				      GError **error)
{
	EGwResponse *response;
	GPtrArray *users;
	xmlNode *node;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	response = e_gw_connection_call_sync (cnc, "getProxyList", NULL, cancellable, error);
	if (!response)
		return NULL;

	users = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_proxy_user_free);
	node = e_gw_xml_find (e_gw_response_get_node (response), "proxies");
	for (node = e_gw_xml_first_child (node, "proxy"); node; node = e_gw_xml_next_sibling (node, "proxy")) {
		EGwProxyUser *user = g_new0 (EGwProxyUser, 1);

		user->uuid = e_gw_xml_dup_text (node, "uuid");
		user->email = e_gw_xml_dup_text (node, "email");
		user->display_name = e_gw_xml_dup_text (node, "displayName");
		if (user->uuid && *user->uuid && user->email && *user->email) {
			if (!user->display_name || !*user->display_name) {
				g_free (user->display_name);
				user->display_name = g_strdup (user->email);
			}
			g_ptr_array_add (users, user);
		} else {
			e_gw_proxy_user_free (user);
		}
	}
	e_gw_response_free (response);

	return users;
}

GHashTable *
e_gw_connection_get_proxy_list_sync (EGwConnection *cnc,
				     GCancellable *cancellable,
				     GError **error)
{
	GPtrArray *users;
	GHashTable *proxies;
	guint ii;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	users = e_gw_connection_get_proxy_users_sync (cnc, cancellable, error);
	if (!users)
		return NULL;

	proxies = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	for (ii = 0; ii < users->len; ii++) {
		EGwProxyUser *user = g_ptr_array_index (users, ii);

		g_hash_table_insert (proxies, g_strdup (user->uuid), g_strdup (user->email));
	}
	g_ptr_array_unref (users);

	return proxies;
}

gboolean
e_gw_connection_login_sync (EGwConnection *cnc,
			    const gchar *user,
			    const gchar *password,
			    GCancellable *cancellable,
			    GError **error)
{
	gboolean success;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (user != NULL, FALSE);

	/* No trusted-application login: it would open every mailbox of the post office */
	if (!password || !*password) {
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_NO_PASSWORD, _("A password is required"));
		return FALSE;
	}

	g_rec_mutex_lock (&cnc->lock);

	clear_credentials (cnc);
	cnc->login_user = g_strdup (user);
	cnc->login_password = g_strdup (password);

	success = login_locked (cnc, cancellable, error);
	if (!success)
		clear_credentials (cnc);

	g_rec_mutex_unlock (&cnc->lock);

	return success;
}

void
e_gw_connection_logout_sync (EGwConnection *cnc,
			     GCancellable *cancellable)
{
	g_return_if_fail (E_IS_GW_CONNECTION (cnc));

	g_rec_mutex_lock (&cnc->lock);

	if (cnc->session) {
		EGwResponse *response = post_request (cnc, "logout", NULL, TRUE, NULL, cancellable, NULL);

		e_gw_response_free (response);
	}
	clear_user_info (cnc);
	clear_credentials (cnc);

	g_rec_mutex_unlock (&cnc->lock);
}

gboolean
e_gw_connection_is_logged_in (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);

	return cnc->session != NULL;
}

const gchar *
e_gw_connection_get_session (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	return cnc->session;
}

const gchar *
e_gw_connection_get_user_name (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	return cnc->user_name;
}

const gchar *
e_gw_connection_get_user_email (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	return cnc->user_email;
}

const gchar *
e_gw_connection_get_user_id (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	return cnc->user_id;
}

const gchar *
e_gw_connection_get_user_uuid (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	return cnc->user_uuid;
}

const gchar *
e_gw_connection_get_server_version (EGwConnection *cnc)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	return cnc->server_version;
}

EGwResponse *
e_gw_connection_call_sync (EGwConnection *cnc,
			   const gchar *action,
			   const gchar *inner_xml,
			   GCancellable *cancellable,
			   GError **error)
{
	EGwResponse *response;
	GError *local_error = NULL;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (action != NULL, NULL);

	g_rec_mutex_lock (&cnc->lock);

	if (!cnc->session) {
		g_rec_mutex_unlock (&cnc->lock);
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_NOT_LOGGED_IN, _("Not logged in"));
		return NULL;
	}

	response = post_request (cnc, action, inner_xml, TRUE, NULL, cancellable, &local_error);

	/* One new login if the POA dropped the session */
	if (!response && cnc->login_password && is_session_error (local_error)) {
		g_clear_error (&local_error);
		if (login_locked (cnc, cancellable, &local_error))
			response = post_request (cnc, action, inner_xml, TRUE, NULL, cancellable, &local_error);
	}

	g_rec_mutex_unlock (&cnc->lock);

	if (local_error)
		g_propagate_error (error, local_error);

	return response;
}

/* One streaming download; @retry is set when a new login may help. */
static gboolean
download_once (EGwConnection *cnc,
	       const gchar *id,
	       gboolean as_mime,
	       GOutputStream *output,
	       gboolean *retry,
	       GCancellable *cancellable,
	       GError **error)
{
	gchar *url, *escaped_session, *escaped_id;
	SoupMessage *message;
	GInputStream *input;
	gboolean success = FALSE;
	guint status;

	*retry = FALSE;

	escaped_session = g_uri_escape_string (cnc->session, NULL, FALSE);
	escaped_id = g_uri_escape_string (id, NULL, FALSE);
	url = g_strdup_printf ("%s://%s:%u/attachment?session=%s&id=%s%s",
		cnc->use_ssl ? "https" : "http", cnc->host, cnc->port,
		escaped_session, escaped_id, as_mime ? "&mime=1" : "");
	g_free (escaped_session);
	g_free (escaped_id);

	message = new_message (cnc, SOUP_METHOD_GET, url);
	if (!message) {
		g_set_error (error, E_GW_ERROR, E_GW_ERROR_CONNECTION, _("Invalid server address %s"), url);
		g_free (url);
		return FALSE;
	}

	input = soup_session_send (cnc->soup, message, cancellable, error);
	status = soup_message_get_status (message);
	g_debug ("download %s%s: HTTP %u%s%s", id, as_mime ? " (mime)" : "", status,
		!input && error && *error ? ", " : "", !input && error && *error ? (*error)->message : "");

	if (!input) {
		/* the error from libsoup is descriptive enough */
	} else if (status == SOUP_STATUS_OK) {
		success = g_output_stream_splice (output, input, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
			cancellable, error) >= 0;
	} else {
		/* GroupWise errors come as HTTP 400 with the code in a header */
		const gchar *gw_code = soup_message_headers_get_one (
			soup_message_get_response_headers (message), "X-GWError-Code");
		gchar *end = NULL;
		gint64 code = 0;

		/* Real POAs send it in hex ("0xEA02"); base 0 accepts decimal as well */
		if (gw_code)
			code = g_ascii_strtoll (gw_code, &end, 0);

		if (gw_code && end && end != gw_code && *end == '\0' && code > 0 && code <= G_MAXINT32) {
			g_set_error (error, E_GW_ERROR, (gint) code, _("Cannot download %s"), id);
			*retry = code == E_GW_ERROR_INVALID_SESSION;
		} else {
			g_set_error (error, E_GW_ERROR, E_GW_ERROR_CONNECTION, _("HTTP %u %s"),
				status, soup_message_get_reason_phrase (message));
		}
		g_input_stream_close (input, NULL, NULL);
	}

	g_clear_object (&input);
	g_object_unref (message);
	g_free (url);

	return success;
}

gboolean
e_gw_connection_download_sync (EGwConnection *cnc,
			       const gchar *id,
			       gboolean as_mime,
			       GOutputStream *output,
			       GCancellable *cancellable,
			       GError **error)
{
	GError *local_error = NULL;
	gboolean success, retry;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (id != NULL, FALSE);
	g_return_val_if_fail (G_IS_OUTPUT_STREAM (output), FALSE);

	g_rec_mutex_lock (&cnc->lock);

	if (!cnc->session) {
		g_rec_mutex_unlock (&cnc->lock);
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_NOT_LOGGED_IN, _("Not logged in"));
		return FALSE;
	}

	success = download_once (cnc, id, as_mime, output, &retry, cancellable, &local_error);

	/* A lost session: log in again once */
	if (!success && retry && cnc->login_password) {
		GError *login_error = NULL;

		if (login_locked (cnc, cancellable, &login_error)) {
			g_clear_error (&local_error);
			success = download_once (cnc, id, as_mime, output, &retry, cancellable, &local_error);
		} else {
			g_clear_error (&local_error);
			local_error = login_error;
		}
	}

	g_rec_mutex_unlock (&cnc->lock);

	if (local_error)
		g_propagate_error (error, local_error);

	return success;
}

EGwResponse *
e_gw_connection_get_items_sync (EGwConnection *cnc,
				const gchar *container,
				const gchar *view,
				const gchar *filter_xml,
				gint count,
				GCancellable *cancellable,
				GError **error)
{
	GString *inner = g_string_new (NULL);
	EGwResponse *response;

	e_gw_xml_add_leaf (inner, "container", container);
	if (view)
		e_gw_xml_add_leaf (inner, "view", view);
	if (filter_xml)
		g_string_append (inner, filter_xml);
	e_gw_xml_add_int (inner, "count", count < 0 ? -1 : count);

	response = e_gw_connection_call_sync (cnc, "getItems", inner->str, cancellable, error);
	g_string_free (inner, TRUE);

	return response;
}

EGwResponse *
e_gw_connection_get_item_sync (EGwConnection *cnc,
			       const gchar *id,
			       const gchar *view,
			       GCancellable *cancellable,
			       GError **error)
{
	GString *inner = g_string_new (NULL);
	EGwResponse *response;

	e_gw_xml_add_leaf (inner, "id", id);
	if (view)
		e_gw_xml_add_leaf (inner, "view", view);

	response = e_gw_connection_call_sync (cnc, "getItem", inner->str, cancellable, error);
	g_string_free (inner, TRUE);

	return response;
}

gboolean
e_gw_connection_foreach_item_sync (EGwConnection *cnc,
				   const gchar *container,
				   const gchar *view,
				   const gchar *filter_xml,
				   guint page_size,
				   EGwItemFunc func,
				   gpointer user_data,
				   GCancellable *cancellable,
				   GError **error)
{
	GString *inner = g_string_new (NULL);
	EGwResponse *response;
	gchar *cursor;
	gboolean answered, success = TRUE;

	g_return_val_if_fail (container != NULL, FALSE);
	g_return_val_if_fail (func != NULL, FALSE);

	/* The cursor lives in the POA session: no re-login may happen in between */
	g_rec_mutex_lock (&cnc->lock);

	e_gw_xml_add_leaf (inner, "container", container);
	if (view)
		e_gw_xml_add_leaf (inner, "view", view);
	if (filter_xml)
		g_string_append (inner, filter_xml);

	{
		GError *local_error = NULL;

		response = e_gw_connection_call_sync (cnc, "createCursor", inner->str, cancellable, &local_error);

		/* Search result folders have no cursors (GroupWise 26.2: 59916):
		 * all items in one response */
		if (g_error_matches (local_error, E_GW_ERROR, E_GW_ERROR_NOT_SUPPORTED)) {
			xmlNode *item;

			g_clear_error (&local_error);
			response = e_gw_connection_call_sync (cnc, "getItems", inner->str, cancellable, error);
			success = response != NULL;
			item = response ? e_gw_xml_find (e_gw_response_get_node (response), "items") : NULL;
			for (item = e_gw_xml_first_child (item, "item"); item && success; item = e_gw_xml_next_sibling (item, "item"))
				success = func (item, user_data);
			e_gw_response_free (response);
			g_string_free (inner, TRUE);
			g_rec_mutex_unlock (&cnc->lock);
			return success;
		}
		if (local_error)
			g_propagate_error (error, local_error);
	}
	answered = response != NULL;
	cursor = answered ? e_gw_xml_dup_text (e_gw_response_get_node (response), "cursor") : NULL;
	e_gw_response_free (response);

	if (!cursor || !*cursor) {
		/* Special containers (TeamWorks) accept the request without a
		 * cursor and hold no items */
		gboolean empty = answered;

		g_free (cursor);
		g_string_free (inner, TRUE);
		g_rec_mutex_unlock (&cnc->lock);
		return empty;
	}

	while (success) {
		xmlNode *item;
		guint count = 0;

		g_string_truncate (inner, 0);
		e_gw_xml_add_leaf (inner, "container", container);
		e_gw_xml_add_leaf (inner, "cursor", cursor);
		e_gw_xml_add_bool (inner, "forward", TRUE);
		e_gw_xml_add_leaf (inner, "position", "current");
		e_gw_xml_add_int (inner, "count", page_size ? page_size : 100);

		response = e_gw_connection_call_sync (cnc, "readCursor", inner->str, cancellable, error);
		if (!response) {
			success = FALSE;
			break;
		}

		item = e_gw_xml_find (e_gw_response_get_node (response), "items");
		for (item = e_gw_xml_first_child (item, "item"); item && success; item = e_gw_xml_next_sibling (item, "item")) {
			count++;
			success = func (item, user_data);
		}
		e_gw_response_free (response);

		if (count == 0)
			break;
	}

	g_string_truncate (inner, 0);
	e_gw_xml_add_leaf (inner, "container", container);
	e_gw_xml_add_leaf (inner, "cursor", cursor);
	e_gw_response_free (e_gw_connection_call_sync (cnc, "destroyCursor", inner->str, NULL, NULL));

	g_rec_mutex_unlock (&cnc->lock);

	g_free (cursor);
	g_string_free (inner, TRUE);

	return success;
}

gboolean
e_gw_connection_mark_read_sync (EGwConnection *cnc,
				const gchar * const *ids,
				gboolean read,
				GCancellable *cancellable,
				GError **error)
{
	GString *inner = g_string_new ("<items>");
	EGwResponse *response;
	guint ii;

	for (ii = 0; ids && ids[ii]; ii++)
		e_gw_xml_add_leaf (inner, "item", ids[ii]);
	g_string_append (inner, "</items>");

	response = e_gw_connection_call_sync (cnc, read ? "markRead" : "markUnRead", inner->str, cancellable, error);
	g_string_free (inner, TRUE);

	if (!response)
		return FALSE;

	e_gw_response_free (response);

	return TRUE;
}

EGwResponse *
e_gw_connection_get_quick_messages_sync (EGwConnection *cnc,
					 const gchar *list,
					 const gchar *since,
					 const gchar *container,
					 const gchar *view,
					 gchar **out_server_time,
					 GCancellable *cancellable,
					 GError **error)
{
	GString *inner = g_string_new (NULL);
	EGwResponse *response;

	g_return_val_if_fail (list != NULL, NULL);
	g_return_val_if_fail (since != NULL, NULL);
	g_return_val_if_fail (out_server_time != NULL, NULL);

	e_gw_xml_add_leaf (inner, "list", list);
	e_gw_xml_add_leaf (inner, "startDate", since);
	if (container)
		e_gw_xml_add_leaf (inner, "container", container);
	if (view)
		e_gw_xml_add_leaf (inner, "view", view);

	response = e_gw_connection_call_sync (cnc, "getQuickMessages", inner->str, cancellable, error);
	g_string_free (inner, TRUE);

	*out_server_time = response ? e_gw_xml_dup_text (e_gw_response_get_node (response), "startDate") : NULL;
	if (response && (!*out_server_time || !**out_server_time)) {
		g_clear_pointer (out_server_time, g_free);
		g_clear_pointer (&response, e_gw_response_free);
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, _("The server returned no time"));
	}

	return response;
}

gchar *
e_gw_item_id_in_container (const gchar *id,
			   const gchar *container)
{
	const gchar *colon;

	g_return_val_if_fail (id != NULL, NULL);
	g_return_val_if_fail (container != NULL, NULL);

	colon = strchr (id, ':');

	return g_strdup_printf ("%.*s:%s", (gint) (colon ? colon - id : (gssize) strlen (id)), id, container);
}

/* Sends ACTION with the body built by the caller; a call without error */
static gboolean
simple_call (EGwConnection *cnc,
	     const gchar *action,
	     GString *inner,
	     GCancellable *cancellable,
	     GError **error)
{
	EGwResponse *response = e_gw_connection_call_sync (cnc, action, inner->str, cancellable, error);

	g_string_free (inner, TRUE);
	e_gw_response_free (response);

	return response != NULL;
}

static void
add_item_list (GString *inner,
	       const gchar * const *ids)
{
	guint ii;

	g_string_append (inner, "<items>");
	for (ii = 0; ids && ids[ii]; ii++)
		e_gw_xml_add_leaf (inner, "item", ids[ii]);
	g_string_append (inner, "</items>");
}

gboolean
e_gw_connection_move_items_sync (EGwConnection *cnc,
				 const gchar * const *ids,
				 const gchar *container,
				 const gchar *from,
				 GCancellable *cancellable,
				 GError **error)
{
	GString *inner = g_string_new (NULL);
	guint ii;

	g_return_val_if_fail (container != NULL, FALSE);

	for (ii = 0; ids && ids[ii]; ii++) {
		g_string_append (inner, "<item>");
		e_gw_xml_add_leaf (inner, "id", ids[ii]);
		e_gw_xml_add_leaf (inner, "container", container);
		if (from)
			e_gw_xml_add_leaf (inner, "from", from);
		g_string_append (inner, "</item>");
	}

	return simple_call (cnc, "moveItems", inner, cancellable, error);
}

gboolean
e_gw_connection_remove_items_sync (EGwConnection *cnc,
				   const gchar * const *ids,
				   const gchar *container,
				   GCancellable *cancellable,
				   GError **error)
{
	GString *inner = g_string_new (NULL);

	g_return_val_if_fail (container != NULL, FALSE);

	/* Without a container the item would leave every folder */
	e_gw_xml_add_leaf (inner, "container", container);
	add_item_list (inner, ids);

	return simple_call (cnc, "removeItems", inner, cancellable, error);
}

gboolean
e_gw_connection_purge_sync (EGwConnection *cnc,
			    const gchar * const *ids,
			    GCancellable *cancellable,
			    GError **error)
{
	GString *inner = g_string_new (NULL);

	add_item_list (inner, ids);

	return simple_call (cnc, "purge", inner, cancellable, error);
}

gchar *
e_gw_connection_create_folder_sync (EGwConnection *cnc,
				    const gchar *parent,
				    const gchar *name,
				    GCancellable *cancellable,
				    GError **error)
{
	GString *inner = g_string_new ("<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Folder\">");
	EGwResponse *response;
	gchar *raw, *id;

	g_return_val_if_fail (parent != NULL, NULL);
	g_return_val_if_fail (name != NULL, NULL);

	e_gw_xml_add_leaf (inner, "name", name);
	e_gw_xml_add_leaf (inner, "parent", parent);
	g_string_append (inner, "</item>");

	response = e_gw_connection_call_sync (cnc, "createItem", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return NULL;

	raw = e_gw_xml_dup_text (e_gw_response_get_node (response), "id");
	id = raw && *raw ? e_gw_clean_id (raw) : NULL;
	g_free (raw);
	e_gw_response_free (response);

	if (!id)
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, _("The server returned no folder ID"));

	return id;
}

gboolean
e_gw_connection_modify_folder_sync (EGwConnection *cnc,
				    const gchar *id,
				    const gchar *new_name,
				    const gchar *new_parent,
				    GCancellable *cancellable,
				    GError **error)
{
	GString *inner = g_string_new (NULL);

	g_return_val_if_fail (id != NULL, FALSE);

	e_gw_xml_add_leaf (inner, "id", id);
	g_string_append (inner, "<updates><update>");
	if (new_name)
		e_gw_xml_add_leaf (inner, "name", new_name);
	if (new_parent)
		e_gw_xml_add_leaf (inner, "parent", new_parent);
	g_string_append (inner, "</update></updates>");

	return simple_call (cnc, "modifyItem", inner, cancellable, error);
}

gboolean
e_gw_connection_remove_folder_sync (EGwConnection *cnc,
				    const gchar *id,
				    GCancellable *cancellable,
				    GError **error)
{
	GString *inner = g_string_new (NULL);

	g_return_val_if_fail (id != NULL, FALSE);

	e_gw_xml_add_leaf (inner, "id", id);

	return simple_call (cnc, "removeItem", inner, cancellable, error);
}

gboolean
e_gw_connection_get_refused_certificate (EGwConnection *cnc,
					 gchar **out_pem,
					 GTlsCertificateFlags *out_errors)
{
	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);

	if (!cnc->refused_pem)
		return FALSE;

	if (out_pem)
		*out_pem = g_strdup (cnc->refused_pem);
	if (out_errors)
		*out_errors = cnc->refused_errors;

	return TRUE;
}

gchar *
e_gw_connection_create_item_sync (EGwConnection *cnc,
				  const gchar *item_xml,
				  GCancellable *cancellable,
				  GError **error)
{
	EGwResponse *response;
	gchar *raw, *id;

	g_return_val_if_fail (item_xml != NULL, NULL);

	response = e_gw_connection_call_sync (cnc, "createItem", item_xml, cancellable, error);
	if (!response)
		return NULL;

	raw = e_gw_xml_dup_text (e_gw_response_get_node (response), "id");
	id = raw && *raw ? e_gw_clean_id (raw) : NULL;
	g_free (raw);
	e_gw_response_free (response);

	if (!id)
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, _("The server returned no ID"));

	return id;
}

gboolean
e_gw_connection_modify_item_sync (EGwConnection *cnc,
				  const gchar *id,
				  const gchar *updates_xml,
				  GCancellable *cancellable,
				  GError **error)
{
	GString *inner = g_string_new (NULL);

	g_return_val_if_fail (id != NULL, FALSE);

	e_gw_xml_add_leaf (inner, "id", id);
	g_string_append_printf (inner, "<updates>%s</updates>", updates_xml ? updates_xml : "");
	/* What changes, short: a text part can be long */
	g_debug ("modifyItem %s: %.300s", id, updates_xml ? updates_xml : "");

	return simple_call (cnc, "modifyItem", inner, cancellable, error);
}

gchar *
e_gw_connection_send_item_sync (EGwConnection *cnc,
				const gchar *item_xml,
				GCancellable *cancellable,
				GError **error)
{
	EGwResponse *response;
	gchar *raw, *id;

	g_return_val_if_fail (item_xml != NULL, NULL);

	response = e_gw_connection_call_sync (cnc, "sendItem", item_xml, cancellable, error);
	if (!response)
		return NULL;

	/* The first ID: the item of the sender */
	raw = e_gw_xml_dup_text (e_gw_response_get_node (response), "id");
	id = raw && *raw ? e_gw_clean_id (raw) : NULL;
	g_free (raw);
	e_gw_response_free (response);

	if (!id)
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, _("The server returned no ID"));

	return id;
}

gboolean
e_gw_connection_accept_sync (EGwConnection *cnc,
			     const gchar *id,
			     const gchar *accept_level,
			     const gchar *comment,
			     GCancellable *cancellable,
			     GError **error)
{
	const gchar *ids[] = { id, NULL };
	GString *inner = g_string_new (NULL);

	g_return_val_if_fail (id != NULL, FALSE);

	/* Element order and the empty recurrence element as gwmcp sends them */
	add_item_list (inner, ids);
	e_gw_xml_add_leaf (inner, "comment", comment);
	e_gw_xml_add_leaf (inner, "acceptLevel", accept_level ? accept_level : "Busy");
	e_gw_xml_add_leaf (inner, "recurrenceAllInstances", "");

	return simple_call (cnc, "accept", inner, cancellable, error);
}

gboolean
e_gw_connection_decline_sync (EGwConnection *cnc,
			      const gchar *id,
			      const gchar *comment,
			      GCancellable *cancellable,
			      GError **error)
{
	const gchar *ids[] = { id, NULL };
	GString *inner = g_string_new (NULL);

	g_return_val_if_fail (id != NULL, FALSE);

	add_item_list (inner, ids);
	e_gw_xml_add_leaf (inner, "comment", comment);
	e_gw_xml_add_leaf (inner, "recurrenceAllInstances", "");

	return simple_call (cnc, "decline", inner, cancellable, error);
}

gboolean
e_gw_connection_retract_sync (EGwConnection *cnc,
			      const gchar *id,
			      const gchar *comment,
			      GCancellable *cancellable,
			      GError **error)
{
	const gchar *ids[] = { id, NULL };
	GString *inner = g_string_new (NULL);

	g_return_val_if_fail (id != NULL, FALSE);

	add_item_list (inner, ids);
	e_gw_xml_add_leaf (inner, "comment", comment);
	g_string_append (inner, "<retractingAllInstances>false</retractingAllInstances>"
		"<retractCausedByResend>false</retractCausedByResend>");
	e_gw_xml_add_leaf (inner, "retractType", "allMailboxes");

	return simple_call (cnc, "retract", inner, cancellable, error);
}

gboolean
e_gw_connection_retract_from_recipients_sync (EGwConnection *cnc,
					      const gchar * const *ids,
					      GCancellable *cancellable,
					      GError **error)
{
	GString *inner = g_string_new (NULL);

	g_return_val_if_fail (ids != NULL && ids[0] != NULL, FALSE);

	/* The POA takes a mail back only by its ID with the container
	 * (without, it answers success and does nothing) */
	add_item_list (inner, ids);
	e_gw_xml_add_leaf (inner, "retractType", "recipientMailboxes");

	return simple_call (cnc, "retract", inner, cancellable, error);
}

gboolean
e_gw_connection_complete_sync (EGwConnection *cnc,
			       const gchar *id,
			       gboolean completed,
			       GCancellable *cancellable,
			       GError **error)
{
	const gchar *ids[] = { id, NULL };
	GString *inner = g_string_new (NULL);

	g_return_val_if_fail (id != NULL, FALSE);

	add_item_list (inner, ids);

	return simple_call (cnc, completed ? "complete" : "uncomplete", inner, cancellable, error);
}

EGwResponse *
e_gw_connection_get_free_busy_sync (EGwConnection *cnc,
				    const gchar * const *emails,
				    const gchar *start_utc,
				    const gchar *end_utc,
				    guint wait_seconds,
				    GCancellable *cancellable,
				    GError **error)
{
	GString *inner = g_string_new ("<users>");
	EGwResponse *response, *result = NULL;
	gchar *session;
	gint64 deadline;
	guint ii;

	g_return_val_if_fail (emails != NULL, NULL);

	for (ii = 0; emails[ii]; ii++) {
		g_string_append (inner, "<user>");
		e_gw_xml_add_leaf (inner, "email", emails[ii]);
		g_string_append (inner, "</user>");
	}
	g_string_append (inner, "</users>");
	e_gw_xml_add_leaf (inner, "startDate", start_utc);
	e_gw_xml_add_leaf (inner, "endDate", end_utc);

	response = e_gw_connection_call_sync (cnc, "startFreeBusySession", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return NULL;
	session = e_gw_xml_dup_text (e_gw_response_get_node (response), "freeBusySessionId");
	e_gw_response_free (response);
	if (!session || !*session) {
		g_free (session);
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, _("The server started no free/busy search"));
		return NULL;
	}

	/* The POA asks the other post offices: wait for their answers a while */
	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "freeBusySessionId", session);
	deadline = g_get_monotonic_time () + (gint64) wait_seconds * G_USEC_PER_SEC;
	while (TRUE) {
		gint64 outstanding;

		e_gw_response_free (result);
		result = e_gw_connection_call_sync (cnc, "getFreeBusy", inner->str, cancellable, error);
		if (!result)
			break;
		outstanding = e_gw_xml_get_int (e_gw_response_get_node (result), "freeBusyStats/outstanding", -1);
		if (outstanding < 0)
			outstanding = e_gw_xml_get_int (e_gw_response_get_node (result), "freeBusyInfo/freeBusyStats/outstanding", 0);
		if (outstanding <= 0 || g_get_monotonic_time () >= deadline || g_cancellable_is_cancelled (cancellable))
			break;
		g_usleep (700 * 1000);
	}

	response = e_gw_connection_call_sync (cnc, "closeFreeBusySession", inner->str, NULL, NULL);
	e_gw_response_free (response);
	g_string_free (inner, TRUE);
	g_free (session);

	return result;
}

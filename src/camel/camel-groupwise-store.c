/*
 * camel-groupwise-store.c: Camel store backed by the GroupWise SOAP interface
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

#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-events.h"
#include "e-gw-folder.h"

#include "camel-groupwise-folder.h"
#include "camel-groupwise-labels.h"
#include "camel-groupwise-settings.h"
#include "camel-groupwise-store.h"

struct _CamelGroupwiseStore {
	CamelOfflineStore parent;

	GMutex lock;			/* guards cnc and summary */
	EGwConnection *cnc;
	CamelGroupwiseStoreSummary *summary;
	CamelGroupwiseLabels *labels;
	gboolean proxy_read_only;	/* a proxy login without the right to write mail */

	/* The events of the mailbox, asked for now and then (see below) */
	guint events_timer;
	gint events_busy;		/* atomic: a question is on its way */
	gint64 events_asked;		/* monotonic time of the last question */
	gchar *events_key;
	gboolean events_configured;	/* the POA records for the key (this session) */
	gboolean events_cleared;	/* the key was taken away while switched off */
};

enum {
	PROP_0,
	PROP_CONNECTABLE,
	PROP_HOST_REACHABLE
};

CamelServiceAuthType camel_groupwise_password_authtype = {
	N_("Password"),
	N_("This option will connect to the GroupWise server using a plaintext password."),
	"",
	TRUE
};

static void groupwise_store_network_service_init (CamelNetworkServiceInterface *iface);

G_DEFINE_TYPE_WITH_CODE (CamelGroupwiseStore, camel_groupwise_store, CAMEL_TYPE_OFFLINE_STORE,
	G_IMPLEMENT_INTERFACE (CAMEL_TYPE_NETWORK_SERVICE, groupwise_store_network_service_init))

static void
groupwise_store_set_property (GObject *object,
			      guint property_id,
			      const GValue *value,
			      GParamSpec *pspec)
{
	switch (property_id) {
	case PROP_CONNECTABLE:
		camel_network_service_set_connectable (CAMEL_NETWORK_SERVICE (object), g_value_get_object (value));
		return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
groupwise_store_get_property (GObject *object,
			      guint property_id,
			      GValue *value,
			      GParamSpec *pspec)
{
	switch (property_id) {
	case PROP_CONNECTABLE:
		g_value_take_object (value, camel_network_service_ref_connectable (CAMEL_NETWORK_SERVICE (object)));
		return;
	case PROP_HOST_REACHABLE:
		g_value_set_boolean (value, camel_network_service_get_host_reachable (CAMEL_NETWORK_SERVICE (object)));
		return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

/* Evolution-facing errors: an unreachable server is "unavailable", so
 * Evolution treats the account as offline instead of showing a failure. */
static void
propagate_gw_error (GError **error,
		    GError *gw_error)
{
	if (gw_error && gw_error->domain == E_GW_ERROR && gw_error->code == E_GW_ERROR_CONNECTION) {
		g_set_error_literal (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE, gw_error->message);
		g_error_free (gw_error);
	} else {
		g_propagate_error (error, gw_error);
	}
}

/* "ab:cd:..." over the DER data, the key of a certificate in the CamelCertDB */
static gchar *
certificate_fingerprint (GTlsCertificate *certificate,
			 GByteArray **out_der)
{
	GByteArray *der = NULL;
	GString *fingerprint;
	GChecksum *checksum;
	guint8 digest[32];
	gsize len = sizeof (digest), ii;

	g_object_get (certificate, "certificate", &der, NULL);
	if (!der)
		return NULL;

	checksum = g_checksum_new (G_CHECKSUM_SHA256);
	g_checksum_update (checksum, der->data, der->len);
	g_checksum_get_digest (checksum, digest, &len);
	g_checksum_free (checksum);

	fingerprint = g_string_new (NULL);
	for (ii = 0; ii < len; ii++)
		g_string_append_printf (fingerprint, "%s%02x", ii ? ":" : "", digest[ii]);

	*out_der = der;

	return g_string_free (fingerprint, FALSE);
}

static gboolean
trust_accepts (CamelCertTrust trust)
{
	return trust == CAMEL_CERT_TRUST_TEMPORARY || trust == CAMEL_CERT_TRUST_MARGINAL ||
	       trust == CAMEL_CERT_TRUST_FULLY || trust == CAMEL_CERT_TRUST_ULTIMATE;
}

/* A certificate GIO does not trust. Like the TLS code of Camel's own
 * network services: a decision kept in the CamelCertDB for this host and
 * certificate counts, otherwise the session asks (Evolution shows its
 * trust dialog) and a permanent answer is kept. The session itself does
 * not remember anything. */
static gboolean
groupwise_store_accept_certificate (EGwConnection *cnc,
				    GTlsCertificate *certificate,
				    GTlsCertificateFlags errors,
				    gpointer user_data)
{
	CamelService *service = user_data;
	CamelCertDB *certdb = camel_certdb_get_default ();
	CamelSession *session;
	CamelSettings *settings;
	CamelCert *cert = NULL;
	CamelCertTrust trust;
	GByteArray *der = NULL;
	gchar *host, *fingerprint;

	settings = camel_service_ref_settings (service);
	host = camel_network_settings_dup_host (CAMEL_NETWORK_SETTINGS (settings));
	g_object_unref (settings);
	fingerprint = certificate_fingerprint (certificate, &der);

	if (certdb && host && fingerprint)
		cert = camel_certdb_get_host (certdb, host, fingerprint);

	if (cert && cert->trust != CAMEL_CERT_TRUST_UNKNOWN) {
		trust = cert->trust;
		g_debug ("certificate of %s: kept decision %d", host, trust);
	} else {
		session = camel_service_ref_session (service);
		trust = session ? camel_session_trust_prompt (session, service, certificate, errors) : CAMEL_CERT_TRUST_UNKNOWN;
		g_clear_object (&session);

		/* Permanent answers are kept, temporary ones only for this connection */
		if (certdb && host && fingerprint &&
		    (trust == CAMEL_CERT_TRUST_NEVER || trust == CAMEL_CERT_TRUST_MARGINAL ||
		     trust == CAMEL_CERT_TRUST_FULLY || trust == CAMEL_CERT_TRUST_ULTIMATE)) {
			if (!cert) {
				cert = camel_cert_new ();
				cert->hostname = g_strdup (host);
				cert->fingerprint = g_strdup (fingerprint);
				g_object_get (certificate, "subject-name", &cert->subject, "issuer-name", &cert->issuer, NULL);
				camel_cert_save_cert_file (cert, der, NULL);
			}
			cert->trust = trust;
			camel_certdb_put (certdb, cert);
			camel_certdb_touch (certdb);
			camel_certdb_save (certdb);
		}
	}

	if (cert)
		camel_cert_unref (cert);
	if (der)
		g_byte_array_unref (der);
	g_free (fingerprint);
	g_free (host);

	return trust_accepts (trust);
}

CamelGroupwiseStoreSummary *
camel_groupwise_store_get_summary (CamelGroupwiseStore *store)
{
	CamelGroupwiseStoreSummary *summary;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_STORE (store), NULL);

	g_mutex_lock (&store->lock);
	if (!store->summary) {
		const gchar *dir = camel_service_get_user_data_dir (CAMEL_SERVICE (store));
		gchar *filename = g_build_filename (dir, "folders.ini", NULL);

		store->summary = camel_groupwise_store_summary_new (filename);
		g_free (filename);
	}
	summary = store->summary;
	g_mutex_unlock (&store->lock);

	return summary;
}

CamelGroupwiseLabels *
camel_groupwise_store_get_labels (CamelGroupwiseStore *store)
{
	CamelGroupwiseLabels *labels;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_STORE (store), NULL);

	g_mutex_lock (&store->lock);
	if (!store->labels) {
		const gchar *dir = camel_service_get_user_data_dir (CAMEL_SERVICE (store));
		gchar *filename = g_build_filename (dir, "categories.ini", NULL);

		store->labels = camel_groupwise_labels_new (filename);
		g_free (filename);
	}
	labels = store->labels;
	g_mutex_unlock (&store->lock);

	return labels;
}

EGwConnection *
camel_groupwise_store_ref_connection (CamelGroupwiseStore *store)
{
	EGwConnection *cnc;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_STORE (store), NULL);

	g_mutex_lock (&store->lock);
	cnc = store->cnc ? g_object_ref (store->cnc) : NULL;
	g_mutex_unlock (&store->lock);

	return cnc;
}

EGwConnection *
camel_groupwise_store_ref_connection_sync (CamelGroupwiseStore *store,
					   GCancellable *cancellable,
					   GError **error)
{
	EGwConnection *cnc = camel_groupwise_store_ref_connection (store);

	if (cnc)
		return cnc;

	if (camel_offline_store_get_online (CAMEL_OFFLINE_STORE (store)) &&
	    !camel_service_connect_sync (CAMEL_SERVICE (store), cancellable, error))
		return NULL;

	cnc = camel_groupwise_store_ref_connection (store);
	if (!cnc)
		g_set_error_literal (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
			_("You must be working online to complete this operation"));

	return cnc;
}

/* Reads the folder list of the mailbox into the store summary */
static gboolean
groupwise_store_refresh_folders (CamelGroupwiseStore *store,
				 EGwConnection *cnc,
				 GCancellable *cancellable,
				 GError **error)
{
	GPtrArray *folders, *records;
	GError *local_error = NULL;

	folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, cancellable, &local_error);
	if (!folders) {
		propagate_gw_error (error, local_error);
		return FALSE;
	}

	records = camel_groupwise_folder_records_from_folders (folders);
	g_ptr_array_unref (folders);

	return camel_groupwise_store_summary_replace (camel_groupwise_store_get_summary (store), records, error);
}

static gchar *
groupwise_store_get_name (CamelService *service,
			  gboolean brief)
{
	CamelNetworkSettings *settings;
	gchar *host, *user, *name;

	gchar *proxy;

	settings = CAMEL_NETWORK_SETTINGS (camel_service_ref_settings (service));
	host = camel_network_settings_dup_host (settings);
	user = camel_network_settings_dup_user (settings);
	proxy = camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (settings));
	g_object_unref (settings);

	if (brief)
		name = g_strdup_printf (_("GroupWise server %s"), host);
	else if (proxy)
		name = g_strdup_printf (_("GroupWise mail of %s as proxy of %s on %s"), proxy, user, host);
	else
		name = g_strdup_printf (_("GroupWise mail for %s on %s"), user, host);

	g_free (host);
	g_free (user);
	g_free (proxy);

	return name;
}

static gboolean
groupwise_store_connect_sync (CamelService *service,
			      GCancellable *cancellable,
			      GError **error)
{
	CamelSession *session;
	gboolean success;

	if (camel_service_get_connection_status (service) == CAMEL_SERVICE_CONNECTED &&
	    CAMEL_GROUPWISE_STORE (service)->cnc)
		return TRUE;

	/* Calls authenticate_sync, asking for the password as needed */
	session = camel_service_ref_session (service);
	success = session && camel_session_authenticate_sync (session, service, NULL, cancellable, error);
	g_clear_object (&session);

	return success;
}

static gboolean
groupwise_store_disconnect_sync (CamelService *service,
				 gboolean clean,
				 GCancellable *cancellable,
				 GError **error)
{
	CamelGroupwiseStore *store = CAMEL_GROUPWISE_STORE (service);
	EGwConnection *cnc;

	g_mutex_lock (&store->lock);
	cnc = store->cnc;
	store->cnc = NULL;
	g_mutex_unlock (&store->lock);

	if (cnc) {
		if (clean)
			e_gw_connection_logout_sync (cnc, cancellable);
		g_object_unref (cnc);
	}

	return TRUE;
}

static CamelAuthenticationResult
groupwise_store_authenticate_sync (CamelService *service,
				   const gchar *mechanism,
				   GCancellable *cancellable,
				   GError **error)
{
	CamelGroupwiseStore *store = CAMEL_GROUPWISE_STORE (service);
	CamelNetworkSettings *settings;
	CamelNetworkSecurityMethod method;
	EGwConnection *cnc;
	GError *local_error = NULL;
	const gchar *password;
	gchar *host, *user, *proxy;
	guint16 port;

	password = camel_service_get_password (service);
	if (!password || !*password) {
		g_debug ("authenticate: no password");
		return CAMEL_AUTHENTICATION_REJECTED;
	}

	settings = CAMEL_NETWORK_SETTINGS (camel_service_ref_settings (service));
	host = camel_network_settings_dup_host (settings);
	user = camel_network_settings_dup_user (settings);
	port = camel_network_settings_get_port (settings);
	method = camel_network_settings_get_security_method (settings);
	proxy = camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (settings));
	g_object_unref (settings);

	/* Without SSL configured, the connection switches to https by itself
	 * when the POA requires it */
	cnc = e_gw_connection_new (host, port, method != CAMEL_NETWORK_SECURITY_METHOD_NONE);
	e_gw_connection_set_certificate_func (cnc, groupwise_store_accept_certificate, service, NULL);
	/* A proxy account: the other user's mailbox with the own login */
	e_gw_connection_set_proxy (cnc, proxy);

	if (!e_gw_connection_login_sync (cnc, user, password, cancellable, &local_error)) {
		g_object_unref (cnc);
		g_free (host);
		g_free (user);
		g_free (proxy);

		g_debug ("authenticate: login failed: %s", local_error->message);
		if (g_error_matches (local_error, E_GW_ERROR, E_GW_ERROR_INVALID_PASSWORD)) {
			g_error_free (local_error);
			return CAMEL_AUTHENTICATION_REJECTED;
		}

		propagate_gw_error (error, local_error);
		return CAMEL_AUTHENTICATION_ERROR;
	}

	g_free (host);
	g_free (user);

	if (proxy) {
		EGwProxyRights rights = e_gw_connection_get_proxy_rights (cnc);

		g_debug ("authenticate: proxy of %s, rights 0x%x", proxy, rights);
		if (!(rights & E_GW_PROXY_MAIL_READ)) {
			g_set_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
				_("%s has not granted you the right to read mail"), proxy);
			g_object_unref (cnc);
			g_free (proxy);
			return CAMEL_AUTHENTICATION_ERROR;
		}
		store->proxy_read_only = !(rights & E_GW_PROXY_MAIL_WRITE);
		g_free (proxy);
	} else {
		store->proxy_read_only = FALSE;
	}

	g_mutex_lock (&store->lock);
	g_clear_object (&store->cnc);
	store->cnc = g_object_ref (cnc);
	g_mutex_unlock (&store->lock);

	/* Errors here are not authentication errors; the list is read again later */
	if (!groupwise_store_refresh_folders (store, cnc, cancellable, &local_error)) {
		g_debug ("authenticate: folder list: %s", local_error->message);
		g_clear_error (&local_error);
	}
	/* The categories, as Evolution's labels */
	{
		GPtrArray *categories = e_gw_connection_get_categories_sync (cnc, cancellable, &local_error);

		if (categories) {
			camel_groupwise_labels_set_categories (camel_groupwise_store_get_labels (store), categories);
			g_ptr_array_unref (categories);
		} else {
			g_debug ("authenticate: categories: %s", local_error->message);
			g_clear_error (&local_error);
		}
	}
	g_object_unref (cnc);

	return CAMEL_AUTHENTICATION_ACCEPTED;
}

static GList *
groupwise_store_query_auth_types_sync (CamelService *service,
				       GCancellable *cancellable,
				       GError **error)
{
	return g_list_prepend (NULL, &camel_groupwise_password_authtype);
}

static CamelFolderInfo *
groupwise_store_get_folder_info_sync (CamelStore *camel_store,
				      const gchar *top,
				      CamelStoreGetFolderInfoFlags flags,
				      GCancellable *cancellable,
				      GError **error)
{
	CamelGroupwiseStore *store = CAMEL_GROUPWISE_STORE (camel_store);
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (store);
	CamelFolderInfo *fi;

	if (camel_offline_store_get_online (CAMEL_OFFLINE_STORE (store)) &&
	    ((flags & CAMEL_STORE_FOLDER_INFO_REFRESH) != 0 || camel_groupwise_store_summary_is_empty (summary))) {
		EGwConnection *cnc;
		GError *local_error = NULL;

		cnc = camel_groupwise_store_ref_connection_sync (store, cancellable, &local_error);
		if (cnc) {
			groupwise_store_refresh_folders (store, cnc, cancellable, &local_error);
			g_object_unref (cnc);
		}

		if (local_error)
			g_debug ("get_folder_info: %s", local_error->message);

		/* Stale folders are better than none */
		if (local_error && camel_groupwise_store_summary_is_empty (summary)) {
			g_propagate_error (error, local_error);
			return NULL;
		}
		g_clear_error (&local_error);
	}

	fi = camel_groupwise_store_summary_build_folder_info (summary, top,
		(flags & CAMEL_STORE_FOLDER_INFO_RECURSIVE) != 0);
	if (!fi && top && *top)
		g_set_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER, _("No such folder: %s"), top);

	return fi;
}

static CamelFolder *
groupwise_store_get_folder_sync (CamelStore *camel_store,
				 const gchar *folder_name,
				 CamelStoreGetFolderFlags flags,
				 GCancellable *cancellable,
				 GError **error)
{
	CamelGroupwiseStore *store = CAMEL_GROUPWISE_STORE (camel_store);
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (store);
	CamelFolder *folder;
	gchar *id, *checksum, *folder_dir;

	id = camel_groupwise_store_summary_dup_id (summary, folder_name);

	/* A folder created elsewhere since the last folder list */
	if (!id && camel_offline_store_get_online (CAMEL_OFFLINE_STORE (store))) {
		EGwConnection *cnc = camel_groupwise_store_ref_connection_sync (store, cancellable, NULL);

		if (cnc) {
			groupwise_store_refresh_folders (store, cnc, cancellable, NULL);
			g_object_unref (cnc);
			id = camel_groupwise_store_summary_dup_id (summary, folder_name);
		}
	}

	if (!id) {
		g_set_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER,
			_("No such folder: %s"), folder_name);
		return NULL;
	}

	/* Keyed by the GroupWise ID, which survives renames */
	checksum = g_compute_checksum_for_string (G_CHECKSUM_SHA1, id, -1);
	folder_dir = g_build_filename (camel_service_get_user_data_dir (CAMEL_SERVICE (store)), "folders", checksum, NULL);

	folder = camel_groupwise_folder_new (camel_store, folder_name, id, folder_dir, cancellable, error);

	g_free (folder_dir);
	g_free (checksum);
	g_free (id);

	return folder;
}

static CamelFolder *
groupwise_store_get_inbox_folder_sync (CamelStore *camel_store,
				       GCancellable *cancellable,
				       GError **error)
{
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (CAMEL_GROUPWISE_STORE (camel_store));
	CamelFolder *folder;
	gchar *full_name;

	full_name = camel_groupwise_store_summary_dup_full_name_by_type (summary, E_GW_FOLDER_TYPE_MAILBOX);
	if (!full_name) {
		g_set_error_literal (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER, _("The mailbox has no Mailbox folder"));
		return NULL;
	}

	folder = camel_store_get_folder_sync (camel_store, full_name, 0, cancellable, error);
	g_free (full_name);

	return folder;
}

static CamelFolder *
groupwise_store_get_trash_folder_sync (CamelStore *camel_store,
				       GCancellable *cancellable,
				       GError **error)
{
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (CAMEL_GROUPWISE_STORE (camel_store));
	CamelFolder *folder;
	gchar *full_name;

	full_name = camel_groupwise_store_summary_dup_full_name_by_type (summary, E_GW_FOLDER_TYPE_TRASH);
	if (!full_name) {
		g_set_error_literal (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER, _("The mailbox has no Trash folder"));
		return NULL;
	}

	folder = camel_store_get_folder_sync (camel_store, full_name, 0, cancellable, error);
	g_free (full_name);

	return folder;
}

/* The Junk Mail folder of GroupWise: Evolution moves junk there */
static CamelFolder *
groupwise_store_get_junk_folder_sync (CamelStore *camel_store,
				      GCancellable *cancellable,
				      GError **error)
{
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (CAMEL_GROUPWISE_STORE (camel_store));
	CamelFolder *folder;
	gchar *full_name;

	full_name = camel_groupwise_store_summary_dup_full_name_by_type (summary, E_GW_FOLDER_TYPE_JUNK);
	if (!full_name) {
		/* GroupWise makes it when junk mail handling is switched on */
		g_set_error_literal (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER,
			_("The mailbox has no Junk Mail folder; switch on the junk mail handling of GroupWise"));
		return NULL;
	}

	folder = camel_store_get_folder_sync (camel_store, full_name, 0, cancellable, error);
	g_free (full_name);

	return folder;
}

gboolean
camel_groupwise_store_get_read_only (CamelGroupwiseStore *store)
{
	CamelSettings *settings;
	gboolean read_only;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_STORE (store), TRUE);

	settings = camel_service_ref_settings (CAMEL_SERVICE (store));
	read_only = camel_groupwise_settings_get_read_only (CAMEL_GROUPWISE_SETTINGS (settings)) ||
		store->proxy_read_only;
	g_object_unref (settings);

	return read_only;
}

static gboolean
groupwise_store_check_writable (CamelGroupwiseStore *store,
				GError **error)
{
	gboolean read_only = camel_groupwise_store_get_read_only (store);

	if (read_only)
		g_set_error_literal (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID, _("The account is read-only"));

	return !read_only;
}

/* Only the user's own folders can be renamed, moved or deleted */
static gchar *
groupwise_store_dup_own_folder_id (CamelGroupwiseStore *store,
				   const gchar *full_name,
				   GError **error)
{
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (store);
	gchar *id = camel_groupwise_store_summary_dup_id (summary, full_name);

	if (!id) {
		g_set_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER, _("No such folder: %s"), full_name);
		return NULL;
	}

	if (camel_groupwise_store_summary_get_type (summary, full_name) != E_GW_FOLDER_TYPE_NORMAL) {
		g_set_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID,
			_("%s is a system folder of GroupWise"), full_name);
		g_free (id);
		return NULL;
	}

	return id;
}

static gchar *
parent_of (const gchar *full_name)
{
	const gchar *slash = strrchr (full_name, '/');

	return slash ? g_strndup (full_name, slash - full_name) : g_strdup ("");
}

/* Reads the folder list again after a change; the result of the change itself counts */
static void
groupwise_store_reload_folders (CamelGroupwiseStore *store,
				EGwConnection *cnc,
				GCancellable *cancellable)
{
	GError *local_error = NULL;

	if (!groupwise_store_refresh_folders (store, cnc, cancellable, &local_error)) {
		g_debug ("folder list after a change: %s", local_error->message);
		g_clear_error (&local_error);
	}
}

static CamelFolderInfo *
groupwise_store_create_folder_sync (CamelStore *camel_store,
				    const gchar *parent_name,
				    const gchar *folder_name,
				    GCancellable *cancellable,
				    GError **error)
{
	CamelGroupwiseStore *store = CAMEL_GROUPWISE_STORE (camel_store);
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (store);
	CamelFolderInfo *fi = NULL;
	EGwConnection *cnc;
	GError *local_error = NULL;
	gchar *parent_id, *id, *full_name;

	if (!groupwise_store_check_writable (store, error))
		return NULL;

	if (strchr (folder_name, '/')) {
		g_set_error_literal (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID,
			_("A folder name must not contain \"/\""));
		return NULL;
	}

	parent_id = camel_groupwise_store_summary_dup_id (summary, parent_name ? parent_name : "");
	if (!parent_id) {
		g_set_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER, _("No such folder: %s"), parent_name);
		return NULL;
	}

	cnc = camel_groupwise_store_ref_connection_sync (store, cancellable, error);
	if (!cnc) {
		g_free (parent_id);
		return NULL;
	}

	id = e_gw_connection_create_folder_sync (cnc, parent_id, folder_name, cancellable, &local_error);
	if (id) {
		groupwise_store_reload_folders (store, cnc, cancellable);

		full_name = camel_groupwise_store_summary_dup_full_name (summary, id);
		if (full_name) {
			fi = camel_groupwise_store_summary_build_folder_info (summary, full_name, FALSE);
			if (fi)
				camel_store_folder_created (camel_store, fi);
		}
		g_free (full_name);
	} else {
		propagate_gw_error (error, local_error);
	}

	g_object_unref (cnc);
	g_free (parent_id);
	g_free (id);

	return fi;
}

/* The folder and its subfolders, as the summary knows them */
static GPtrArray *
folder_and_below (CamelGroupwiseStore *store,
		  const gchar *full_name)
{
	CamelFolderInfo *tree = camel_groupwise_store_summary_build_folder_info (
		camel_groupwise_store_get_summary (store), full_name, TRUE);
	GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
	GPtrArray *stack = g_ptr_array_new ();

	if (tree)
		g_ptr_array_add (stack, tree);
	while (stack->len) {
		CamelFolderInfo *fi = g_ptr_array_steal_index (stack, stack->len - 1);

		for (; fi; fi = fi->next) {
			g_ptr_array_add (names, g_strdup (fi->full_name));
			if (fi->child)
				g_ptr_array_add (stack, fi->child);
		}
	}

	g_ptr_array_unref (stack);
	camel_folder_info_free (tree);

	return names;
}

static gboolean
groupwise_store_rename_folder_sync (CamelStore *camel_store,
				    const gchar *old_name,
				    const gchar *new_name,
				    GCancellable *cancellable,
				    GError **error)
{
	CamelGroupwiseStore *store = CAMEL_GROUPWISE_STORE (camel_store);
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (store);
	EGwConnection *cnc;
	GError *local_error = NULL;
	gchar *id, *old_parent, *new_parent, *new_parent_id = NULL;
	const gchar *old_leaf, *new_leaf;
	gboolean success;

	if (!groupwise_store_check_writable (store, error))
		return FALSE;

	id = groupwise_store_dup_own_folder_id (store, old_name, error);
	if (!id)
		return FALSE;

	old_parent = parent_of (old_name);
	new_parent = parent_of (new_name);
	old_leaf = strrchr (old_name, '/') ? strrchr (old_name, '/') + 1 : old_name;
	new_leaf = strrchr (new_name, '/') ? strrchr (new_name, '/') + 1 : new_name;

	if (g_strcmp0 (old_parent, new_parent) != 0) {
		new_parent_id = camel_groupwise_store_summary_dup_id (summary, new_parent);
		if (!new_parent_id) {
			g_set_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER, _("No such folder: %s"), new_parent);
			g_free (old_parent);
			g_free (new_parent);
			g_free (id);
			return FALSE;
		}
	}

	cnc = camel_groupwise_store_ref_connection_sync (store, cancellable, error);
	if (!cnc) {
		g_free (new_parent_id);
		g_free (old_parent);
		g_free (new_parent);
		g_free (id);
		return FALSE;
	}

	/* camel_store_rename_folder_sync() renames the summaries in the database */
	success = e_gw_connection_modify_folder_sync (cnc, id, g_strcmp0 (old_leaf, new_leaf) != 0 ? new_leaf : NULL,
		new_parent_id, cancellable, &local_error);
	if (success) {
		CamelFolderInfo *fi;

		groupwise_store_reload_folders (store, cnc, cancellable);

		fi = camel_groupwise_store_summary_build_folder_info (summary, new_name, TRUE);
		if (fi) {
			camel_store_folder_renamed (camel_store, old_name, fi);
			camel_folder_info_free (fi);
		}
	} else {
		propagate_gw_error (error, local_error);
	}

	g_object_unref (cnc);
	g_free (new_parent_id);
	g_free (old_parent);
	g_free (new_parent);
	g_free (id);

	return success;
}

static gboolean
groupwise_store_delete_folder_sync (CamelStore *camel_store,
				    const gchar *folder_name,
				    GCancellable *cancellable,
				    GError **error)
{
	CamelGroupwiseStore *store = CAMEL_GROUPWISE_STORE (camel_store);
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (store);
	CamelFolderInfo *fi;
	EGwConnection *cnc;
	GError *local_error = NULL;
	GPtrArray *names;
	gchar *id;
	gboolean success;
	guint ii;

	if (!groupwise_store_check_writable (store, error))
		return FALSE;

	id = groupwise_store_dup_own_folder_id (store, folder_name, error);
	if (!id)
		return FALSE;

	cnc = camel_groupwise_store_ref_connection_sync (store, cancellable, error);
	if (!cnc) {
		g_free (id);
		return FALSE;
	}

	fi = camel_groupwise_store_summary_build_folder_info (summary, folder_name, FALSE);
	names = folder_and_below (store, folder_name);

	/* The POA moves the messages of the folder and its subfolders to the Trash */
	success = e_gw_connection_remove_folder_sync (cnc, id, cancellable, &local_error);
	if (success) {
		for (ii = 0; ii < names->len; ii++)
			camel_db_delete_folder (camel_store_get_db (camel_store), names->pdata[ii], NULL);
		groupwise_store_reload_folders (store, cnc, cancellable);
		if (fi)
			camel_store_folder_deleted (camel_store, fi);
	} else {
		propagate_gw_error (error, local_error);
	}

	if (fi)
		camel_folder_info_free (fi);
	g_ptr_array_unref (names);
	g_object_unref (cnc);
	g_free (id);

	return success;
}

static const gchar *
groupwise_store_get_service_name (CamelNetworkService *service,
				  CamelNetworkSecurityMethod method)
{
	return "groupwise";
}

static guint16
groupwise_store_get_default_port (CamelNetworkService *service,
				  CamelNetworkSecurityMethod method)
{
	/* The POA serves SOAP over http and https on the same port */
	return E_GW_DEFAULT_PORT;
}

/* ------------------------------------------------------------------ */
/* Events: the POA writes down what happens in the mailbox (new, moved,
 * deleted, read …) for a key of this installation. Asked for every so many
 * seconds (account option), they say which folders changed; those are
 * refreshed at once, with the cheap check they have. New mail and what
 * other clients do shows within the interval instead of at the next check
 * of the whole account, and a proxy account, whose folders have no cheap
 * check, need not be gone through for it. The periodic check stays: the
 * POA does not promise every event. */

#define EVENTS_TICK_SECONDS CAMEL_GROUPWISE_EVENTS_INTERVAL_MIN
#define EVENTS_PERSISTENCE_DAYS 1

/* One key per installation and login: the configuration lies in the mailbox
 * (of the other user for a proxy account), next to those of others */
static gchar *
events_dup_key (CamelGroupwiseStore *store)
{
	CamelSettings *settings = camel_service_ref_settings (CAMEL_SERVICE (store));
	gchar *user = camel_network_settings_dup_user (CAMEL_NETWORK_SETTINGS (settings));
	gchar *machine = NULL, *digest, *key;

	if (!g_file_get_contents ("/etc/machine-id", &machine, NULL, NULL) || !machine || !*machine) {
		g_free (machine);
		machine = g_strdup (g_get_host_name ());
	}
	digest = g_compute_checksum_for_string (G_CHECKSUM_SHA1, machine, -1);
	key = g_strdup_printf ("Evolution_%.8s_%s", digest, user ? user : "");

	g_free (digest);
	g_free (machine);
	g_free (user);
	g_object_unref (settings);

	return key;
}

/* What the events say goes into the folders that are open, at once: read
 * and unread, and what left a folder (deleted, purged, moved away) — the
 * cheap check of a folder does not see those. A folder that got an item
 * (new, moved in, undeleted) or whose item changed is refreshed; new mail
 * opens its folder for it. */
static void
events_refresh_folders (CamelGroupwiseStore *store,
			GPtrArray *events,
			GCancellable *cancellable)
{
	CamelGroupwiseStoreSummary *summary = camel_groupwise_store_get_summary (store);
	GHashTable *by_id = g_hash_table_new (g_str_hash, g_str_equal);	/* folder ID -> open CamelFolder */
	GHashTable *refresh = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);	/* full names */
	GPtrArray *opened = camel_store_dup_opened_folders (CAMEL_STORE (store));
	gboolean any_added = FALSE, any_trash = FALSE;
	GHashTableIter iter;
	gpointer key;
	guint ii, jj;

	for (jj = 0; opened && jj < opened->len; jj++) {
		if (CAMEL_IS_GROUPWISE_FOLDER (opened->pdata[jj]))
			g_hash_table_insert (by_id, (gpointer) camel_groupwise_folder_get_id (opened->pdata[jj]), opened->pdata[jj]);
	}

	for (ii = 0; ii < events->len; ii++) {
		EGwEvent *event = events->pdata[ii];
		gboolean read = g_str_equal (event->type, "ItemMarkRead"), unread = g_str_equal (event->type, "ItemMarkUnread");
		gboolean purged = g_str_equal (event->type, "ItemPurge");
		gboolean added = g_str_equal (event->type, "FolderItemAdd") || g_str_equal (event->type, "FolderItemMove") ||
			g_str_equal (event->type, "ItemUndelete");

		any_added = any_added || added;
		any_trash = any_trash || purged || g_str_equal (event->type, "ItemDelete") || g_str_equal (event->type, "ItemUndelete");

		if (read || unread || purged) {
			/* In whichever folders it is (the POA does not say) */
			for (jj = 0; opened && jj < opened->len; jj++) {
				if (CAMEL_IS_GROUPWISE_FOLDER (opened->pdata[jj]))
					camel_groupwise_folder_apply_event (opened->pdata[jj], event->item,
						read ? CAMEL_GROUPWISE_EVENT_READ : unread ? CAMEL_GROUPWISE_EVENT_UNREAD :
						CAMEL_GROUPWISE_EVENT_GONE);
			}
			continue;
		}
		/* Out of the folder it was in */
		if (event->from && g_hash_table_contains (by_id, event->from))
			camel_groupwise_folder_apply_event (g_hash_table_lookup (by_id, event->from), event->item,
				CAMEL_GROUPWISE_EVENT_GONE);
		if (event->container && (added || g_hash_table_contains (by_id, event->container))) {
			gchar *full_name = camel_groupwise_store_summary_dup_full_name (summary, event->container);

			/* New mail counts in a folder not looked at yet, too */
			if (full_name && !g_hash_table_contains (by_id, event->container)) {
				CamelFolder *folder = camel_store_get_folder_sync (CAMEL_STORE (store), full_name, 0, cancellable, NULL);

				if (folder && CAMEL_IS_GROUPWISE_FOLDER (folder)) {
					g_ptr_array_add (opened, folder);
					g_hash_table_insert (by_id, (gpointer) camel_groupwise_folder_get_id (CAMEL_GROUPWISE_FOLDER (folder)), folder);
				} else {
					g_clear_object (&folder);
				}
			}
			if (full_name)
				g_hash_table_add (refresh, full_name);
		}
	}

	/* Views of the POA: what was sent is "added to the Mailbox", what is
	 * deleted shows in the Trash */
	if (any_added) {
		gchar *full_name = camel_groupwise_store_summary_dup_full_name_by_type (summary, E_GW_FOLDER_TYPE_SENT_ITEMS);

		if (full_name)
			g_hash_table_add (refresh, full_name);
	}
	if (any_trash) {
		gchar *full_name = camel_groupwise_store_summary_dup_full_name_by_type (summary, E_GW_FOLDER_TYPE_TRASH);

		if (full_name)
			g_hash_table_add (refresh, full_name);
	}

	for (ii = 0; opened && ii < opened->len; ii++) {
		CamelFolder *folder = opened->pdata[ii];

		if (g_hash_table_contains (refresh, camel_folder_get_full_name (folder))) {
			GError *error = NULL;

			if (!camel_folder_refresh_info_sync (folder, cancellable, &error))
				g_debug ("events: refresh of %s: %s", camel_folder_get_full_name (folder), error ? error->message : "?");
			g_clear_error (&error);
		}
	}
	g_hash_table_iter_init (&iter, refresh);
	while (g_hash_table_iter_next (&iter, &key, NULL))
		g_debug ("events: %s", (const gchar *) key);

	if (opened)
		g_ptr_array_unref (opened);
	g_hash_table_destroy (refresh);
	g_hash_table_destroy (by_id);
}

static void
events_thread (GTask *task,
	       gpointer source_object,
	       gpointer task_data,
	       GCancellable *cancellable)
{
	static const gchar *types[] = { E_GW_EVENTS_MAIL, NULL };
	CamelGroupwiseStore *store = source_object;
	gboolean wanted = GPOINTER_TO_INT (task_data);
	EGwConnection *cnc = camel_groupwise_store_ref_connection (store);
	GError *error = NULL;

	if (cnc && !store->events_key)
		store->events_key = events_dup_key (store);

	if (cnc && !wanted) {
		/* Switched off: nothing of it stays in the mailbox */
		if (!e_gw_connection_remove_events_sync (cnc, store->events_key, cancellable, &error))
			g_debug ("events: no configuration %s to remove: %s", store->events_key, error ? error->message : "?");
		g_clear_error (&error);
		store->events_configured = FALSE;
		store->events_cleared = TRUE;
	} else if (cnc) {
		GPtrArray *events = NULL;

		if (!store->events_configured) {
			store->events_configured = e_gw_connection_configure_events_sync (cnc, store->events_key, types,
				EVENTS_PERSISTENCE_DAYS, NULL, 0, cancellable, &error);
			store->events_cleared = FALSE;
			g_debug ("events: configuration %s: %s", store->events_key,
				store->events_configured ? "ok" : error ? error->message : "?");
			g_clear_error (&error);
		}
		if (store->events_configured)
			events = e_gw_connection_get_events_sync (cnc, store->events_key, TRUE, FALSE, cancellable, &error);
		if (events) {
			if (events->len) {
				g_debug ("events: %u", events->len);
				events_refresh_folders (store, events, cancellable);
			}
			g_ptr_array_unref (events);
		} else if (store->events_configured) {
			/* Set up again the next time (the configuration may be gone) */
			g_debug ("events: %s", error ? error->message : "?");
			store->events_configured = FALSE;
		}
		g_clear_error (&error);
	}

	g_clear_object (&cnc);
	g_atomic_int_set (&store->events_busy, 0);
	g_task_return_boolean (task, TRUE);
}

static gboolean
events_tick_cb (gpointer user_data)
{
	CamelGroupwiseStore *store = g_weak_ref_get (user_data);
	CamelSettings *settings;
	gboolean wanted;
	guint interval;

	if (!store)
		return G_SOURCE_REMOVE;

	settings = camel_service_ref_settings (CAMEL_SERVICE (store));
	wanted = camel_groupwise_settings_get_use_events_interval (CAMEL_GROUPWISE_SETTINGS (settings));
	interval = camel_groupwise_settings_get_events_interval (CAMEL_GROUPWISE_SETTINGS (settings));
	/* A read-only account changes nothing on the server, not this either */
	if (camel_groupwise_settings_get_read_only (CAMEL_GROUPWISE_SETTINGS (settings))) {
		g_object_unref (settings);
		g_object_unref (store);
		return G_SOURCE_CONTINUE;
	}
	g_object_unref (settings);

	if (camel_service_get_connection_status (CAMEL_SERVICE (store)) == CAMEL_SERVICE_CONNECTED &&
	    camel_offline_store_get_online (CAMEL_OFFLINE_STORE (store)) &&
	    (wanted ? g_get_monotonic_time () - store->events_asked >= (gint64) interval * G_USEC_PER_SEC - G_USEC_PER_SEC :
		!store->events_cleared) &&
	    g_atomic_int_compare_and_exchange (&store->events_busy, 0, 1)) {
		GTask *task = g_task_new (store, NULL, NULL, NULL);

		store->events_asked = g_get_monotonic_time ();
		g_task_set_task_data (task, GINT_TO_POINTER (wanted), NULL);
		g_task_run_in_thread (task, events_thread);
		g_object_unref (task);
	}

	g_object_unref (store);

	return G_SOURCE_CONTINUE;
}

static void
events_weak_ref_free (gpointer data)
{
	g_weak_ref_clear (data);
	g_free (data);
}

static void
groupwise_store_finalize (GObject *object)
{
	CamelGroupwiseStore *store = CAMEL_GROUPWISE_STORE (object);

	if (store->events_timer)
		g_source_remove (store->events_timer);
	g_free (store->events_key);

	g_clear_object (&store->cnc);
	camel_groupwise_store_summary_free (store->summary);
	camel_groupwise_labels_free (store->labels);
	g_mutex_clear (&store->lock);

	G_OBJECT_CLASS (camel_groupwise_store_parent_class)->finalize (object);
}

static void
camel_groupwise_store_class_init (CamelGroupwiseStoreClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	CamelServiceClass *service_class = CAMEL_SERVICE_CLASS (class);
	CamelStoreClass *store_class = CAMEL_STORE_CLASS (class);

	object_class->set_property = groupwise_store_set_property;
	object_class->get_property = groupwise_store_get_property;
	object_class->finalize = groupwise_store_finalize;

	service_class->settings_type = CAMEL_TYPE_GROUPWISE_SETTINGS;
	service_class->get_name = groupwise_store_get_name;
	service_class->connect_sync = groupwise_store_connect_sync;
	service_class->disconnect_sync = groupwise_store_disconnect_sync;
	service_class->authenticate_sync = groupwise_store_authenticate_sync;
	service_class->query_auth_types_sync = groupwise_store_query_auth_types_sync;

	store_class->get_folder_info_sync = groupwise_store_get_folder_info_sync;
	store_class->get_folder_sync = groupwise_store_get_folder_sync;
	store_class->get_inbox_folder_sync = groupwise_store_get_inbox_folder_sync;
	store_class->get_trash_folder_sync = groupwise_store_get_trash_folder_sync;
	store_class->get_junk_folder_sync = groupwise_store_get_junk_folder_sync;
	store_class->create_folder_sync = groupwise_store_create_folder_sync;
	store_class->rename_folder_sync = groupwise_store_rename_folder_sync;
	store_class->delete_folder_sync = groupwise_store_delete_folder_sync;

	/* Inherited from CamelNetworkService */
	g_object_class_override_property (object_class, PROP_CONNECTABLE, "connectable");
	g_object_class_override_property (object_class, PROP_HOST_REACHABLE, "host-reachable");
}

static void
groupwise_store_network_service_init (CamelNetworkServiceInterface *iface)
{
	iface->get_service_name = groupwise_store_get_service_name;
	iface->get_default_port = groupwise_store_get_default_port;
}

static void
camel_groupwise_store_init (CamelGroupwiseStore *store)
{
	g_mutex_init (&store->lock);

	/* GroupWise has a real Trash and a real Junk Mail folder; virtual ones
	 * would show up next to them. Junk goes into the Junk Mail folder. */
	camel_store_set_flags (CAMEL_STORE (store),
		(camel_store_get_flags (CAMEL_STORE (store)) & ~(CAMEL_STORE_VTRASH | CAMEL_STORE_VJUNK)) |
		CAMEL_STORE_REAL_JUNK_FOLDER);

	/* The events of the mailbox, for accounts that want them */
	{
		GWeakRef *ref = g_new0 (GWeakRef, 1);

		g_weak_ref_init (ref, store);
		store->events_timer = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, EVENTS_TICK_SECONDS,
			events_tick_cb, ref, events_weak_ref_free);
	}
}

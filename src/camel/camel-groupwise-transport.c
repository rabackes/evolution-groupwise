/*
 * camel-groupwise-transport.c: sends mail through the GroupWise POA
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

#include "e-gw-connection.h"
#include "e-gw-proxy.h"
#include "e-gw-xml.h"

#include "camel-groupwise-mime.h"
#include "camel-groupwise-settings.h"
#include "camel-groupwise-store.h"
#include "camel-groupwise-transport.h"

struct _CamelGroupwiseTransport {
	CamelTransport parent;
};

G_DEFINE_TYPE (CamelGroupwiseTransport, camel_groupwise_transport, CAMEL_TYPE_TRANSPORT)

static gboolean
same_proxy (CamelService *a,
	    CamelService *b)
{
	CamelSettings *sa = camel_service_ref_settings (a);
	CamelSettings *sb = camel_service_ref_settings (b);
	gchar *pa = camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (sa));
	gchar *pb = camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (sb));
	gboolean same = (!pa && !pb) || (pa && pb && !g_ascii_strcasecmp (pa, pb));

	g_free (pa);
	g_free (pb);
	g_object_unref (sa);
	g_object_unref (sb);

	return same;
}

static gboolean
same_account (CamelService *a,
	      CamelService *b)
{
	CamelNetworkSettings *sa = CAMEL_NETWORK_SETTINGS (camel_service_ref_settings (a));
	CamelNetworkSettings *sb = CAMEL_NETWORK_SETTINGS (camel_service_ref_settings (b));
	gchar *ha = camel_network_settings_dup_host (sa), *hb = camel_network_settings_dup_host (sb);
	gchar *ua = camel_network_settings_dup_user (sa), *ub = camel_network_settings_dup_user (sb);
	/* A proxy account sends through the proxy session of its store */
	gboolean same = ha && hb && ua && ub && !g_ascii_strcasecmp (ha, hb) && !g_ascii_strcasecmp (ua, ub) &&
		same_proxy (a, b);

	g_free (ha);
	g_free (hb);
	g_free (ua);
	g_free (ub);
	g_object_unref (sa);
	g_object_unref (sb);

	return same;
}

/* The store of the same account: its session sends, no second login.
 * Evolution fills host and user of a transport from the Authentication
 * extension of its source; without it they are empty, then the only
 * GroupWise account there is will do (proxy accounts apart). */
static CamelGroupwiseStore *
transport_ref_store (CamelService *transport,
		     GError **error)
{
	CamelSession *session = camel_service_ref_session (transport);
	CamelGroupwiseStore *found = NULL, *only = NULL;
	GList *services, *link;
	guint stores = 0;

	if (!session) {
		g_set_error_literal (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE, _("No session"));
		return NULL;
	}

	services = camel_session_list_services (session);
	for (link = services; link && !found; link = g_list_next (link)) {
		if (!CAMEL_IS_GROUPWISE_STORE (link->data) || !same_proxy (transport, link->data))
			continue;
		stores++;
		only = link->data;
		if (same_account (transport, link->data))
			found = g_object_ref (link->data);
	}
	if (!found && stores == 1) {
		g_debug ("transport %s: no account matches host and user, using the only GroupWise account",
			camel_service_get_uid (transport));
		found = g_object_ref (only);
	}
	g_list_free_full (services, g_object_unref);
	g_object_unref (session);

	if (!found)
		g_set_error_literal (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
			_("The GroupWise account for sending was not found"));

	return found;
}

static gchar *
groupwise_transport_get_name (CamelService *service,
			      gboolean brief)
{
	CamelNetworkSettings *settings = CAMEL_NETWORK_SETTINGS (camel_service_ref_settings (service));
	gchar *host = camel_network_settings_dup_host (settings);
	gchar *name;

	name = brief ? g_strdup_printf (_("GroupWise server %s"), host) :
		g_strdup_printf (_("GroupWise mail delivery via %s"), host);

	g_free (host);
	g_object_unref (settings);

	return name;
}

static gboolean
groupwise_transport_connect_sync (CamelService *service,
				  GCancellable *cancellable,
				  GError **error)
{
	/* The store's connection is used when sending */
	return TRUE;
}

/* The XML of the first element @name below @parent */
static gchar *
dump_element (xmlNode *parent,
	      const gchar *path)
{
	xmlNode *node = e_gw_xml_find (parent, path);
	xmlBuffer *buffer;
	gchar *xml;

	if (!node)
		return NULL;

	buffer = xmlBufferCreate ();
	xmlNodeDump (buffer, node->doc, node, 0, 0);
	xml = g_strndup ((const gchar *) xmlBufferContent (buffer), xmlBufferLength (buffer));
	xmlBufferFree (buffer);

	return xml;
}

/* A message of the account forwarded as attachment goes as the item itself:
 * forwardRequest with embed gives the attachment that refers to it */
static gchar *
embed_item (const gchar *item_id,
	    gchar **out_link_xml,
	    gpointer user_data,
	    GCancellable *cancellable)
{
	EGwConnection *cnc = user_data;
	EGwResponse *response;
	GString *inner = g_string_new (NULL);
	GError *error = NULL;
	gchar *attachment = NULL;
	xmlNode *item;

	e_gw_xml_add_leaf (inner, "id", item_id);
	/* Not "attachments": then the reference comes instead */
	e_gw_xml_add_leaf (inner, "view", "id");
	e_gw_xml_add_bool (inner, "embed", TRUE);
	response = e_gw_connection_call_sync (cnc, "forward", inner->str, cancellable, &error);
	g_string_free (inner, TRUE);

	item = response ? e_gw_xml_find (e_gw_response_get_node (response), "item") : NULL;
	if (item) {
		attachment = dump_element (item, "attachments/attachment");
		*out_link_xml = dump_element (item, "link");
	}
	if (!attachment)
		g_debug ("forward %s as item failed, as MIME instead: %s", item_id, error ? error->message : "no attachment");
	g_clear_error (&error);
	e_gw_response_free (response);

	return attachment;
}

/* The mailbox to send from when the sender of the message is not the user
 * of the account but one he is proxy of (a user or a resource, an alias of
 * the identity): the address to log in as its proxy, else NULL */
static gchar *
dup_proxy_sender (CamelGroupwiseStore *store,
		  EGwConnection *cnc,
		  CamelAddress *from,
		  GCancellable *cancellable)
{
	CamelSettings *settings;
	GHashTable *proxies;
	GHashTableIter iter;
	gpointer value;
	const gchar *own = e_gw_connection_get_user_email (cnc), *email = NULL;
	gchar *proxy, *found = NULL;

	if (!from || !CAMEL_IS_INTERNET_ADDRESS (from) ||
	    !camel_internet_address_get (CAMEL_INTERNET_ADDRESS (from), 0, NULL, &email) ||
	    !email || !*email || (own && g_ascii_strcasecmp (email, own) == 0))
		return NULL;

	/* A proxy account sends from its mailbox */
	settings = camel_service_ref_settings (CAMEL_SERVICE (store));
	proxy = camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (settings));
	g_object_unref (settings);
	if (proxy) {
		g_free (proxy);
		return NULL;
	}

	proxies = e_gw_connection_get_proxy_list_sync (cnc, cancellable, NULL);
	if (!proxies)
		return NULL;
	g_hash_table_iter_init (&iter, proxies);
	while (!found && g_hash_table_iter_next (&iter, NULL, &value)) {
		if (value && g_ascii_strcasecmp (value, email) == 0)
			found = g_strdup (value);
	}
	g_hash_table_destroy (proxies);

	return found;
}

static gboolean
groupwise_transport_send_to_sync (CamelTransport *transport,
				  CamelMimeMessage *message,
				  CamelAddress *from,
				  CamelAddress *recipients,
				  gboolean *out_sent_message_saved,
				  GCancellable *cancellable,
				  GError **error)
{
	CamelGroupwiseStore *store;
	EGwConnection *cnc;
	EGwResponse *response;
	GError *local_error = NULL;
	gboolean read_only;
	gchar *item, *sender;

	store = transport_ref_store (CAMEL_SERVICE (transport), error);
	if (!store)
		return FALSE;

	read_only = camel_groupwise_store_get_read_only (store);
	if (read_only) {
		g_set_error_literal (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_INVALID, _("The account is read-only"));
		g_object_unref (store);
		return FALSE;
	}

	cnc = camel_groupwise_store_ref_connection_sync (store, cancellable, error);
	if (!cnc) {
		g_object_unref (store);
		return FALSE;
	}

	/* Another mailbox chosen as the sender: through a session as its proxy */
	sender = dup_proxy_sender (store, cnc, from, cancellable);
	if (sender) {
		EGwConnection *proxy_cnc = camel_groupwise_store_open_proxy_connection_sync (store, sender, cancellable, &local_error);

		if (proxy_cnc && !(e_gw_connection_get_proxy_rights (proxy_cnc) & E_GW_PROXY_MAIL_WRITE)) {
			g_set_error (&local_error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
				_("%s has not granted you the right to write mail"), sender);
			e_gw_connection_logout_sync (proxy_cnc, cancellable);
			g_clear_object (&proxy_cnc);
		}
		if (!proxy_cnc) {
			g_set_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
				_("Sending as %s failed: %s"), sender, local_error->message);
			g_clear_error (&local_error);
			g_free (sender);
			g_object_unref (cnc);
			g_object_unref (store);
			return FALSE;
		}
		g_debug ("send: as proxy of %s", sender);
		g_object_unref (cnc);
		cnc = proxy_cnc;
	}

	item = camel_groupwise_item_from_message (message, recipients, FALSE, embed_item, cnc, cancellable, error);
	if (!item) {
		if (sender)
			e_gw_connection_logout_sync (cnc, cancellable);
		g_free (sender);
		g_object_unref (cnc);
		g_object_unref (store);
		return FALSE;
	}

	/* The sender is the logged-in user, or in a proxy session the user
	 * of the mailbox (the POA notes the proxy); the POA keeps the copy in
	 * Sent Items of that mailbox */
	response = e_gw_connection_call_sync (cnc, "sendItem", item, cancellable, &local_error);
	if (sender)
		e_gw_connection_logout_sync (cnc, cancellable);
	g_free (sender);
	if (response) {
		e_gw_response_free (response);
		if (out_sent_message_saved)
			*out_sent_message_saved = TRUE;
	} else {
		g_set_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
			_("Sending through GroupWise failed: %s"), local_error->message);
		g_clear_error (&local_error);
	}

	g_object_unref (cnc);
	g_free (item);
	g_object_unref (store);

	return response != NULL;
}

static void
camel_groupwise_transport_class_init (CamelGroupwiseTransportClass *class)
{
	CamelServiceClass *service_class = CAMEL_SERVICE_CLASS (class);
	CamelTransportClass *transport_class = CAMEL_TRANSPORT_CLASS (class);

	service_class->settings_type = CAMEL_TYPE_GROUPWISE_SETTINGS;
	service_class->get_name = groupwise_transport_get_name;
	service_class->connect_sync = groupwise_transport_connect_sync;

	transport_class->send_to_sync = groupwise_transport_send_to_sync;
}

static void
camel_groupwise_transport_init (CamelGroupwiseTransport *transport)
{
}

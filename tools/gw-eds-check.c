/*
 * gw-eds-check.c: runs a GroupWise collection through the real
 * evolution-data-server without Evolution: keeps the password and the
 * trust in the server certificate as Evolution's prompts would, lets the
 * registry log in and lists the contacts of the address books it made.
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
 * Usage: GW_PASSWORD=.. gw-eds-check COLLECTION-UID [--trust-certificate] [--write-test BOOK-NAME]
 * --write-test creates, changes and deletes a contact in that address book
 * and tries to change the system address book: for test accounts.
 * --forget-certificate drops the kept trust of the collection first and
 * reports which sources ask for a prompt (as Evolution would show them).
 * The registry and the address book factory must see the development
 * prefix (tools/dev-account.sh sets it up).
 */

#include <stdio.h>
#include <string.h>

#include <libedataserver/libedataserver.h>
#include <libebook-contacts/libebook-contacts.h>

#define BOOK_BUS "org.gnome.evolution.dataserver.AddressBook10"
#define BOOK_INTERFACE "org.gnome.evolution.dataserver.AddressBook"

static gboolean
accept_any (GTlsConnection *connection,
	    GTlsCertificate *certificate,
	    GTlsCertificateFlags errors,
	    gpointer user_data)
{
	return TRUE;
}

/* The certificate of the server, as the trust prompt would show it */
static GTlsCertificate *
fetch_certificate (const gchar *host,
		   guint16 port)
{
	GSocketClient *client = g_socket_client_new ();
	GSocketConnection *connection;
	GIOStream *tls;
	GTlsCertificate *certificate = NULL;
	GSocketConnectable *address = g_network_address_new (host, port);

	connection = g_socket_client_connect (client, address, NULL, NULL);
	if (connection) {
		tls = g_tls_client_connection_new (G_IO_STREAM (connection), address, NULL);
		if (tls) {
			/* Only to read it: every certificate is taken here */
			g_signal_connect (tls, "accept-certificate", G_CALLBACK (accept_any), NULL);
			if (g_tls_connection_handshake (G_TLS_CONNECTION (tls), NULL, NULL))
				certificate = g_object_ref (g_tls_connection_get_peer_certificate (G_TLS_CONNECTION (tls)));
			g_object_unref (tls);
		}
		g_object_unref (connection);
	}

	g_object_unref (address);
	g_object_unref (client);

	return certificate;
}

static GList *
children_of (ESourceRegistry *registry,
	     const gchar *parent_uid)
{
	GList *all = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_ADDRESS_BOOK), *link, *children = NULL;

	for (link = all; link; link = g_list_next (link)) {
		if (g_strcmp0 (e_source_get_parent (link->data), parent_uid) == 0)
			children = g_list_append (children, g_object_ref (link->data));
	}
	g_list_free_full (all, g_object_unref);

	return children;
}

/* Opens the address book in the factory: its bus name and object path */
static gboolean
open_book (GDBusConnection *bus,
	   ESource *book,
	   gchar **out_bus_name,
	   gchar **out_path)
{
	GError *error = NULL;
	GVariant *reply;

	reply = g_dbus_connection_call_sync (bus, BOOK_BUS, "/org/gnome/evolution/dataserver/AddressBookFactory",
		"org.gnome.evolution.dataserver.AddressBookFactory", "OpenAddressBook",
		g_variant_new ("(s)", e_source_get_uid (book)), G_VARIANT_TYPE ("(ss)"),
		G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
	if (!reply) {
		printf ("    open: %s\n", error->message);
		g_clear_error (&error);
		return FALSE;
	}
	g_variant_get (reply, "(ss)", out_path, out_bus_name);
	g_variant_unref (reply);

	reply = g_dbus_connection_call_sync (bus, *out_bus_name, *out_path, BOOK_INTERFACE, "Open", NULL, NULL,
		G_DBUS_CALL_FLAGS_NONE, 60000, NULL, NULL);
	if (reply)
		g_variant_unref (reply);

	return TRUE;
}

static GVariant *
book_call (GDBusConnection *bus,
	   const gchar *bus_name,
	   const gchar *path,
	   const gchar *method,
	   GVariant *args,
	   const GVariantType *reply_type,
	   GError **error)
{
	return g_dbus_connection_call_sync (bus, bus_name, path, BOOK_INTERFACE, method, args, reply_type,
		G_DBUS_CALL_FLAGS_NONE, 60000, NULL, error);
}

static gchar *
server_contact (const gchar *bus_name,
		const gchar *path,
		GDBusConnection *bus,
		const gchar *uid)
{
	GVariant *reply = book_call (bus, bus_name, path, "GetContact", g_variant_new ("(s)", uid), G_VARIANT_TYPE ("(s)"), NULL);
	gchar *vcard = NULL;

	if (reply) {
		g_variant_get (reply, "(s)", &vcard);
		g_variant_unref (reply);
	}

	return vcard;
}

static gint failures;
static GHashTable *prompts;

static void
credentials_required_cb (ESource *source,
			 ESourceCredentialsReason reason,
			 const gchar *certificate_pem,
			 GTlsCertificateFlags certificate_errors,
			 const GError *op_error,
			 gpointer user_data)
{
	static const gchar *reasons[] = { "unknown", "required", "rejected", "ssl-failed", "error" };
	gchar *key = g_strdup_printf ("%s: %s", e_source_get_display_name (source),
		reason < G_N_ELEMENTS (reasons) ? reasons[reason] : "?");

	g_hash_table_add (prompts, key);
}

static void
watch_prompts (ESourceRegistry *registry)
{
	GList *all = e_source_registry_list_sources (registry, NULL), *link;

	for (link = all; link; link = g_list_next (link))
		g_signal_connect (link->data, "credentials-required", G_CALLBACK (credentials_required_cb), NULL);
	g_list_free_full (all, g_object_unref);
}

static void
spin (guint seconds)
{
	gint64 end = g_get_monotonic_time () + seconds * G_USEC_PER_SEC;

	while (g_get_monotonic_time () < end)
		g_main_context_iteration (NULL, FALSE), g_usleep (50000);
}

static void
check (gboolean ok,
       const gchar *what,
       GError **error)
{
	printf ("    %s  %s%s%s\n", ok ? "ok  " : "FAIL", what, error && *error ? ": " : "", error && *error ? (*error)->message : "");
	if (!ok)
		failures++;
	if (error)
		g_clear_error (error);
}

static void
write_test (GDBusConnection *bus,
	    ESource *book,
	    ESource *system_book)
{
	const gchar *vcard_new =
		"BEGIN:VCARD\r\nVERSION:3.0\r\nN:Testmann;Evolution;;;\r\nFN:Evolution Testmann\r\n"
		"EMAIL;TYPE=WORK:evo.test@example.com\r\nTEL;TYPE=CELL:+49 170 0000001\r\n"
		"ORG:Acme GmbH;Einkauf\r\nTITLE:Tester\r\nNOTE:Aus Evolution angelegt\r\nEND:VCARD";
	gchar *bus_name = NULL, *path = NULL, *uid = NULL, *stored;
	const gchar *uids[2] = { NULL, NULL };
	GError *error = NULL;
	GVariant *reply;

	if (!open_book (bus, book, &bus_name, &path))
		return;

	reply = book_call (bus, bus_name, path, "CreateContacts",
		g_variant_new ("(^asu)", (const gchar *[]) { vcard_new, NULL }, 0), G_VARIANT_TYPE ("(as)"), &error);
	if (reply) {
		gchar **new_uids = NULL;

		g_variant_get (reply, "(^as)", &new_uids);
		uid = g_strdup (new_uids ? new_uids[0] : NULL);
		g_strfreev (new_uids);
		g_variant_unref (reply);
	}
	check (uid != NULL, "create a contact", &error);
	if (!uid)
		return;
	printf ("      UID %s\n", uid);

	stored = server_contact (bus_name, path, bus, uid);
	check (stored && strstr (stored, "Evolution Testmann") && strstr (stored, "+49 170 0000001"), "read back", NULL);

	/* Change: another title, a second number, no note */
	if (stored) {
		EContact *contact = e_contact_new_from_vcard (stored);
		gchar *changed;

		e_contact_set (contact, E_CONTACT_TITLE, "Oberster Tester");
		e_contact_set (contact, E_CONTACT_PHONE_BUSINESS, "+49 6221 0000002");
		e_contact_set (contact, E_CONTACT_NOTE, NULL);
		changed = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);

		reply = book_call (bus, bus_name, path, "ModifyContacts",
			g_variant_new ("(^asu)", (const gchar *[]) { changed, NULL }, 0), NULL, &error);
		check (reply != NULL, "change it", &error);
		if (reply)
			g_variant_unref (reply);

		g_free (changed);
		g_object_unref (contact);
	}
	g_free (stored);

	/* From the server again, not from the cache */
	reply = book_call (bus, bus_name, path, "Refresh", NULL, NULL, NULL);
	if (reply)
		g_variant_unref (reply);
	g_usleep (3 * G_USEC_PER_SEC);
	stored = server_contact (bus_name, path, bus, uid);
	check (stored && strstr (stored, "Oberster Tester") && strstr (stored, "+49 6221 0000002") &&
		strstr (stored, "+49 170 0000001") && !strstr (stored, "Aus Evolution angelegt"), "the change on the server", NULL);
	g_free (stored);

	/* GW_KEEP=1 leaves it for a look on the server */
	if (!g_getenv ("GW_KEEP")) {
		uids[0] = uid;
		reply = book_call (bus, bus_name, path, "RemoveContacts", g_variant_new ("(^asu)", uids, 0), NULL, &error);
		check (reply != NULL, "delete it", &error);
		if (reply)
			g_variant_unref (reply);
	}

	g_free (uid);
	g_free (bus_name);
	g_free (path);

	/* The system address book stays as it is */
	if (system_book && open_book (bus, system_book, &bus_name, &path)) {
		reply = book_call (bus, bus_name, path, "CreateContacts",
			g_variant_new ("(^asu)", (const gchar *[]) { vcard_new, NULL }, 0), G_VARIANT_TYPE ("(as)"), &error);
		check (reply == NULL, "the system address book refuses", NULL);
		if (error)
			printf ("      (%s)\n", error->message);
		g_clear_error (&error);
		if (reply)
			g_variant_unref (reply);
		g_free (bus_name);
		g_free (path);
	}
}

static void
list_contacts (GDBusConnection *bus,
	       ESource *book)
{
	GError *error = NULL;
	GVariant *reply;
	gchar *path = NULL, *bus_name = NULL, **vcards = NULL;
	guint ii, tries;

	reply = g_dbus_connection_call_sync (bus, BOOK_BUS, "/org/gnome/evolution/dataserver/AddressBookFactory",
		"org.gnome.evolution.dataserver.AddressBookFactory", "OpenAddressBook",
		g_variant_new ("(s)", e_source_get_uid (book)), G_VARIANT_TYPE ("(ss)"),
		G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
	if (!reply) {
		printf ("    open: %s\n", error->message);
		g_clear_error (&error);
		return;
	}
	g_variant_get (reply, "(ss)", &path, &bus_name);
	g_variant_unref (reply);

	reply = g_dbus_connection_call_sync (bus, bus_name, path, BOOK_INTERFACE, "Open", NULL, NULL,
		G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
	if (reply)
		g_variant_unref (reply);
	else {
		printf ("    Open: %s\n", error->message);
		g_clear_error (&error);
	}

	/* The first sync runs in the background after Open */
	for (tries = 0; tries < 30; tries++) {
		reply = g_dbus_connection_call_sync (bus, bus_name, path, BOOK_INTERFACE, "GetContactList",
			g_variant_new ("(s)", ""), G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
		if (!reply)
			break;
		g_strfreev (vcards);
		g_variant_get (reply, "(^as)", &vcards);
		g_variant_unref (reply);
		if (g_strv_length (vcards) > 0)
			break;
		g_usleep (G_USEC_PER_SEC);
	}

	if (error) {
		printf ("    GetContactList: %s\n", error->message);
		g_clear_error (&error);
	} else {
		printf ("    %u contacts\n", vcards ? g_strv_length (vcards) : 0);
		for (ii = 0; vcards && vcards[ii] && ii < 6; ii++) {
			EVCard *vcard = e_vcard_new_from_string (vcards[ii]);
			EVCardAttribute *fn = e_vcard_get_attribute (vcard, EVC_FN);
			EVCardAttribute *email = e_vcard_get_attribute (vcard, EVC_EMAIL);

			printf ("      %-28s %s\n", fn ? e_vcard_attribute_get_value (fn) : "?",
				email ? e_vcard_attribute_get_value (email) : "");
			g_object_unref (vcard);
		}
	}

	g_strfreev (vcards);
	g_free (path);
	g_free (bus_name);
}

int
main (int argc,
      char **argv)
{
	GError *error = NULL;
	ESourceRegistry *registry;
	ESource *collection;
	ESourceAuthentication *auth;
	ENamedParameters *credentials;
	GDBusConnection *bus;
	GList *books = NULL, *link;
	const gchar *password = g_getenv ("GW_PASSWORD");
	guint tries, ii;

	if (argc < 2 || !password) {
		fprintf (stderr, "Usage: GW_PASSWORD=.. %s COLLECTION-UID [--trust-certificate]\n", argv[0]);
		return 2;
	}

	registry = e_source_registry_new_sync (NULL, &error);
	if (!registry)
		g_error ("%s", error->message);
	collection = e_source_registry_ref_source (registry, argv[1]);
	if (!collection)
		g_error ("No source %s", argv[1]);
	auth = e_source_get_extension (collection, E_SOURCE_EXTENSION_AUTHENTICATION);

	prompts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	if (g_strv_contains ((const gchar * const *) argv, "--forget-certificate")) {
		GList *children = children_of (registry, argv[1]), *child;

		e_source_webdav_set_ssl_trust (e_source_get_extension (collection, E_SOURCE_EXTENSION_WEBDAV_BACKEND), NULL);
		printf ("certificate forgotten: %s\n", e_source_write_sync (collection, NULL, &error) ? "yes" : error->message);
		g_clear_error (&error);

		/* A first start: the address books open before anything is trusted */
		watch_prompts (registry);
		bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
		for (child = children; child; child = g_list_next (child)) {
			gchar *bus_name = NULL, *path = NULL;

			if (open_book (bus, child->data, &bus_name, &path)) {
				GVariant *reply = book_call (bus, bus_name, path, "Refresh", NULL, NULL, NULL);

				if (reply)
					g_variant_unref (reply);
			}
			g_free (bus_name);
			g_free (path);
		}
		g_list_free_full (children, g_object_unref);
		g_clear_object (&bus);
	}

	if (argc > 2 && g_strv_contains ((const gchar * const *) argv, "--trust-certificate")) {
		GTlsCertificate *certificate = fetch_certificate (e_source_authentication_get_host (auth),
			e_source_authentication_get_port (auth));

		/* What "Accept permanently" in Evolution's trust prompt stores */
		if (certificate) {
			e_source_webdav_update_ssl_trust (e_source_get_extension (collection, E_SOURCE_EXTENSION_WEBDAV_BACKEND),
				e_source_authentication_get_host (auth), certificate, E_TRUST_PROMPT_RESPONSE_ACCEPT);
			printf ("certificate trusted: %s\n", e_source_write_sync (collection, NULL, &error) ? "yes" : error->message);
			g_clear_error (&error);
			g_object_unref (certificate);
		}
	}

	/* As "Remember password" in Evolution's prompt: the address books find it there */
	printf ("password stored: %s\n", e_source_store_password_sync (collection, password, TRUE, NULL, &error) ? "yes" : error->message);
	g_clear_error (&error);

	credentials = e_named_parameters_new ();
	e_named_parameters_set (credentials, E_SOURCE_CREDENTIAL_PASSWORD, password);
	printf ("authenticate: %s\n", e_source_invoke_authenticate_sync (collection, credentials, NULL, &error) ? "sent" : error->message);
	g_clear_error (&error);

	for (tries = 0; tries < 30 && !books; tries++) {
		spin (1);
		books = children_of (registry, argv[1]);
	}
	spin (3);

	if (g_hash_table_size (prompts)) {
		GList *keys = g_hash_table_get_keys (prompts), *key;

		printf ("prompts asked for:\n");
		for (key = keys; key; key = g_list_next (key))
			printf ("  %s\n", (const gchar *) key->data);
		g_list_free (keys);
	} else if (g_strv_contains ((const gchar * const *) argv, "--forget-certificate")) {
		printf ("prompts asked for: none\n");
	}
	printf ("address books: %u (%s)\n", g_list_length (books),
		e_source_get_connection_status (collection) == E_SOURCE_CONNECTION_STATUS_CONNECTED ? "connected" : "not connected");

	bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
	for (link = books; link; link = g_list_next (link)) {
		printf ("  %s (%s)\n", e_source_get_display_name (link->data), e_source_get_uid (link->data));
		list_contacts (bus, link->data);
	}

	for (ii = 2; ii + 1 < (guint) argc; ii++) {
		ESource *target = NULL, *system_book = NULL;

		if (!g_str_equal (argv[ii], "--write-test"))
			continue;
		for (link = books; link; link = g_list_next (link)) {
			if (g_str_equal (e_source_get_display_name (link->data), argv[ii + 1]))
				target = link->data;
			if (g_str_equal (e_source_get_display_name (link->data), "GroupWise Address Book"))
				system_book = link->data;
		}
		printf ("write test in %s:\n", argv[ii + 1]);
		if (target)
			write_test (bus, target, system_book);
		else
			check (FALSE, "no such address book", NULL);
		printf ("%s\n", failures ? "FAILURES" : "all ok");
	}

	g_list_free_full (books, g_object_unref);
	e_named_parameters_free (credentials);
	g_object_unref (bus);
	g_object_unref (collection);
	g_object_unref (registry);

	return failures ? 1 : 0;
}

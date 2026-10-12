/*
 * test-connection.c: libegroupwise against the mock POA (tests/mock/run-with-mock.py)
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

#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "e-gw-addressbook.h"
#include "e-gw-connection.h"
#include "e-gw-events.h"
#include "e-gw-folder.h"
#include "e-gw-junk.h"
#include "e-gw-proxy.h"
#include "e-gw-rule.h"
#include "e-gw-mailbox.h"
#include "e-gw-share.h"
#include "e-gw-signature.h"
#include "e-gw-vacation.h"
#include "e-gw-xml.h"

/* IDs of mock_poa.py */
#define INBOX "7.domain1.po1.100.0.1.0.1@16"
#define ROOT "6.domain1.po1.100.0.1.0.1@15"
#define MAIL1 "45003893.domain1.po1.100.16E3837.1.FBA.1@1:" INBOX
#define MAIL2 "4512B362.domain1.po1.100.1676834.1.798.1@1:" INBOX
#define ATTACHMENT "4512B362.domain1.po1.200.20000CA.1.671.1@45:" MAIL2

static guint16 mock_port;
static guint16 redirect_port;
static guint16 tls_port;

static guint16
port_from_env (const gchar *name)
{
	const gchar *value = g_getenv (name);

	if (!value) {
		g_printerr ("%s not set: run this test through tests/mock/run-with-mock.py\n", name);
		exit (77);
	}

	return (guint16) atoi (value);
}

/* Talks to the test extensions of the mock */
static gchar *
mock_get (const gchar *path)
{
	SoupSession *session = soup_session_new ();
	gchar *url = g_strdup_printf ("http://127.0.0.1:%u%s", mock_port, path);
	SoupMessage *message = soup_message_new (SOUP_METHOD_GET, url);
	GBytes *bytes = soup_session_send_and_read (session, message, NULL, NULL);
	gchar *text;

	g_assert_nonnull (bytes);
	text = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));

	g_bytes_unref (bytes);
	g_object_unref (message);
	g_object_unref (session);
	g_free (url);

	return text;
}

static gint
mock_logins (void)
{
	gchar *text = mock_get ("/_test/logins");
	gint count = atoi (text);

	g_free (text);

	return count;
}

static EGwConnection *
logged_in (void)
{
	EGwConnection *cnc = e_gw_connection_new ("127.0.0.1", mock_port, FALSE);
	GError *error = NULL;

	g_assert_true (e_gw_connection_login_sync (cnc, "u1", "secret", NULL, &error));
	g_assert_no_error (error);

	return cnc;
}

static void
test_login_wrong_password (void)
{
	EGwConnection *cnc = e_gw_connection_new ("127.0.0.1", mock_port, FALSE);
	GError *error = NULL;

	g_assert_false (e_gw_connection_login_sync (cnc, "u1", "wrong", NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_INVALID_PASSWORD);
	g_assert_cmpstr (error->message, ==, "The password is not correct");
	g_assert_false (e_gw_connection_is_logged_in (cnc));
	g_clear_error (&error);

	g_assert_false (e_gw_connection_login_sync (cnc, "nobody", "secret", NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_UNKNOWN_USER);
	g_assert_cmpstr (error->message, ==, "The user was not found on the post office");
	g_clear_error (&error);

	g_assert_false (e_gw_connection_login_sync (cnc, "u1", "", NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_NO_PASSWORD);
	g_clear_error (&error);

	g_object_unref (cnc);
}

static void
test_login (void)
{
	EGwConnection *cnc = logged_in ();

	g_assert_cmpstr (e_gw_connection_get_session (cnc), ==, "SESSION123");
	g_assert_cmpstr (e_gw_connection_get_user_name (cnc), ==, "User One");
	g_assert_cmpstr (e_gw_connection_get_user_email (cnc), ==, "u1@phantom.com");
	g_assert_cmpstr (e_gw_connection_get_user_id (cnc), ==, "u1");
	g_assert_cmpstr (e_gw_connection_get_user_uuid (cnc), ==, "ME1");
	g_assert_cmpstr (e_gw_connection_get_server_version (cnc), ==, "18.4");

	e_gw_connection_logout_sync (cnc, NULL);
	g_assert_false (e_gw_connection_is_logged_in (cnc));
	g_object_unref (cnc);
}

static void
test_login_redirect (void)
{
	EGwConnection *cnc = e_gw_connection_new ("localhost", redirect_port, FALSE);
	GError *error = NULL;

	g_assert_true (e_gw_connection_login_sync (cnc, "u1", "secret", NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpstr (e_gw_connection_get_host (cnc), ==, "127.0.0.1");
	g_assert_cmpuint (e_gw_connection_get_port (cnc), ==, mock_port);

	g_object_unref (cnc);
}

static gboolean
count_item_cb (xmlNode *item,
	       gpointer user_data)
{
	(*(guint *) user_data)++;
	return TRUE;
}

static void
test_foreach_item (void)
{
	EGwConnection *cnc = logged_in ();
	GError *error = NULL;
	guint count = 0;

	/* Pages of one item: every item, then the end */
	g_assert_true (e_gw_connection_foreach_item_sync (cnc, INBOX, "id", NULL, 1, count_item_cb, &count, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (count, ==, 2);

	/* A container that answers without a cursor is empty, not an error */
	count = 0;
	g_assert_true (e_gw_connection_foreach_item_sync (cnc, "TW@86", "id", NULL, 100, count_item_cb, &count, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (count, ==, 0);

	g_object_unref (cnc);
}

static void
test_mailbox (void)
{
	EGwConnection *cnc = logged_in ();
	EGwDiskUsage usage = { 0 };
	GError *error = NULL;

	g_assert_true (e_gw_connection_get_disk_usage_sync (cnc, &usage, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpint (usage.used, ==, 3);
	g_assert_cmpint (usage.max_size, ==, 100);
	g_assert_cmpint (usage.threshold, ==, 80);

	g_assert_false (e_gw_connection_modify_password_sync (cnc, "falsch", "neu", NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_WRONG_PASSWORD);
	g_clear_error (&error);
	g_assert_true (e_gw_connection_modify_password_sync (cnc, "alt", "neu", NULL, &error));
	g_assert_no_error (error);

	/* A phone message: the caller and the number are its subject */
	{
		const gchar *names[] = { "User Two", NULL, NULL }, *emails[] = { "two@example.com", "three@example.com", NULL };
		gchar *sent;

		g_assert_true (e_gw_connection_send_phone_message_sync (cnc, names, emails, "Erika Mustermann", "Beispiel & Co",
			"030 1234567", E_GW_PHONE_CALLED | E_GW_PHONE_URGENT, "Bitte R\xc3\xbc" "ckruf", NULL, &error));
		g_assert_no_error (error);
		sent = mock_get ("/_test/last-send");
		g_assert_nonnull (strstr (sent, "xsi:type=\"PhoneMessage\""));
		g_assert_nonnull (strstr (sent, "<subject>Erika Mustermann 030 1234567</subject>"));
		g_assert_nonnull (strstr (sent, "<displayName>User Two</displayName><email>two@example.com</email><distType>TO</distType>"));
		g_assert_nonnull (strstr (sent, "<displayName>three@example.com</displayName><email>three@example.com</email>"));
		g_assert_nonnull (strstr (sent, "<company>Beispiel &amp; Co</company>"));
		g_assert_nonnull (strstr (sent, "<flags><called>1</called><urgent>1</urgent></flags>"));
		g_assert_nonnull (strstr (sent, "<part contentType=\"text/plain\">Qml0dGUgUsO8Y2tydWY=</part>"));
		g_free (sent);
	}

	g_object_unref (cnc);
}

static void
test_address_books (void)
{
	EGwConnection *cnc = logged_in ();
	GError *error = NULL;
	GPtrArray *books;
	EGwAddressBook *book;

	books = e_gw_connection_get_address_books_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (books->len, ==, 7);

	book = books->pdata[0];
	g_assert_cmpstr (book->id, ==, E_GW_SYSTEM_ADDRESS_BOOK_ID);
	g_assert_true (book->is_system);
	g_assert_false (book->is_personal);

	book = books->pdata[1];
	g_assert_cmpstr (book->name, ==, "Privat");
	g_assert_true (book->is_personal);
	g_assert_false (book->is_frequent_contacts);
	g_assert_true (((EGwAddressBook *) books->pdata[2])->is_frequent_contacts);

	/* Books shared to the user and by the user */
	{
		EGwAddressBook *team = books->pdata[5], *club = books->pdata[6];
		EGwShare *share;

		g_assert_cmpstr (team->id, ==, "AB5@53");
		g_assert_true (team->is_shared_to_me);
		g_assert_cmpint (team->share_rights, ==, E_GW_SHARE_READ);
		g_assert_cmpstr (team->owner_name, ==, "Egon");
		g_assert_true (club->is_shared_by_me);
		g_assert_false (club->is_shared_to_me);

		share = e_gw_connection_get_share_sync (cnc, "AB6@53", NULL, &error);
		g_assert_no_error (error);
		g_assert_true (share->shared_by_me);
		g_assert_cmpuint (share->entries->len, ==, 1);
		g_assert_cmpint (((EGwShareEntry *) share->entries->pdata[0])->rights, ==, E_GW_SHARE_READ | E_GW_SHARE_EDIT);
		g_assert_cmpint (((EGwShareEntry *) share->entries->pdata[0])->status, ==, E_GW_SHARE_STATUS_PENDING);
		e_gw_share_free (share);

		share = e_gw_connection_get_share_sync (cnc, "AB5@53", NULL, &error);
		g_assert_no_error (error);
		g_assert_true (share->shared_to_me);
		g_assert_cmpstr (share->owner_name, ==, "Egon");
		g_assert_cmpint (e_gw_share_get_my_rights (share), ==, E_GW_SHARE_READ);
		e_gw_share_free (share);

		g_assert_true (e_gw_share_is_notification_id ("SN3@62:7.domain1.po1.100.0.1.0.1@16"));
	}

	g_ptr_array_unref (books);
	g_object_unref (cnc);
}

static void
test_empty_responses (void)
{
	EGwConnection *cnc = logged_in ();
	GError *error = NULL;
	GPtrArray *folders;

	/* A few empty documents are sent again ... */
	g_free (mock_get ("/_test/empty?n=2"));
	folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (folders);
	g_ptr_array_unref (folders);

	/* ... but not forever */
	g_free (mock_get ("/_test/empty?n=5"));
	g_assert_null (e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_EMPTY);
	g_clear_error (&error);

	folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, NULL, &error);
	g_assert_no_error (error);
	g_ptr_array_unref (folders);

	g_object_unref (cnc);
}

static gchar *
download (EGwConnection *cnc,
	  const gchar *id,
	  gboolean as_mime,
	  GError **error)
{
	GOutputStream *out = g_memory_output_stream_new_resizable ();
	gchar *data = NULL;

	if (e_gw_connection_download_sync (cnc, id, as_mime, out, NULL, error)) {
		g_output_stream_write (out, "", 1, NULL, NULL);
		g_output_stream_close (out, NULL, NULL);
		data = g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (out));
	}
	g_object_unref (out);

	return data;
}

static void
test_tls_required (void)
{
	/* Configured for plain http, but the POA only speaks TLS */
	EGwConnection *cnc = e_gw_connection_new ("127.0.0.1", tls_port, FALSE);
	GError *error = NULL;
	gchar *data;

	e_gw_connection_set_verify_ssl (cnc, FALSE);
	g_assert_true (e_gw_connection_login_sync (cnc, "u1", "secret", NULL, &error));
	g_assert_no_error (error);
	g_assert_true (e_gw_connection_get_use_ssl (cnc));

	data = download (cnc, ATTACHMENT, FALSE, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (data, ==, "attachment!");
	g_free (data);

	g_object_unref (cnc);
}

static void
test_tls_user_ca (void)
{
	gchar *dir = g_dir_make_tmp ("gw-ca-XXXXXX", NULL);
	gchar *ca = NULL, *copy = g_build_filename (dir, "groupwise-ca.pem", NULL);
	EGwConnection *cnc;
	GError *error = NULL;

	/* The CA of the mock in the user's CA directory: its server certificate counts */
	g_assert_true (g_file_get_contents (g_getenv ("GW_MOCK_CA"), &ca, NULL, NULL));
	g_assert_true (g_file_set_contents (copy, ca, -1, NULL));
	g_setenv ("GROUPWISE_CA_DIR", dir, TRUE);

	cnc = e_gw_connection_new ("127.0.0.1", tls_port, TRUE);
	g_assert_true (e_gw_connection_login_sync (cnc, "u1", "secret", NULL, &error));
	g_assert_no_error (error);
	g_object_unref (cnc);

	/* Without it the certificate is refused again */
	g_unlink (copy);
	cnc = e_gw_connection_new ("127.0.0.1", tls_port, TRUE);
	g_assert_false (e_gw_connection_login_sync (cnc, "u1", "secret", NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_CONNECTION);
	g_clear_error (&error);
	g_object_unref (cnc);

	g_setenv ("GROUPWISE_CA_DIR", "/nonexistent", TRUE);
	g_rmdir (dir);
	g_free (copy);
	g_free (ca);
	g_free (dir);
}

static void
test_tls_untrusted (void)
{
	/* The self-signed certificate of the mock must be refused by default */
	EGwConnection *cnc = e_gw_connection_new ("127.0.0.1", tls_port, TRUE);
	GError *error = NULL;

	g_assert_false (e_gw_connection_login_sync (cnc, "u1", "secret", NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_CONNECTION);
	g_clear_error (&error);

	g_object_unref (cnc);
}

static void
test_not_logged_in (void)
{
	EGwConnection *cnc = e_gw_connection_new ("127.0.0.1", mock_port, FALSE);
	GError *error = NULL;

	g_assert_null (e_gw_connection_call_sync (cnc, "getFolderList", NULL, NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_NOT_LOGGED_IN);
	g_clear_error (&error);

	g_object_unref (cnc);
}

static void
test_connection_refused (void)
{
	/* Port 1 on localhost is closed on any sane test machine */
	EGwConnection *cnc = e_gw_connection_new ("127.0.0.1", 1, FALSE);
	GError *error = NULL;

	g_assert_false (e_gw_connection_login_sync (cnc, "u1", "secret", NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_CONNECTION);
	g_clear_error (&error);

	g_object_unref (cnc);
}

static EGwFolder *
find_folder (GPtrArray *folders,
	     const gchar *name,
	     const gchar *parent_id)
{
	guint ii;

	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];

		if (g_strcmp0 (folder->name, name) == 0 && (!parent_id || g_strcmp0 (folder->parent_id, parent_id) == 0))
			return folder;
	}

	return NULL;
}

/* A proxy login with the user's credentials: the other user's mailbox and rights */
static void
test_proxy (void)
{
	EGwConnection *cnc = logged_in ();
	EGwConnection *proxy = e_gw_connection_new ("127.0.0.1", mock_port, FALSE);
	GHashTable *proxies;
	GPtrArray *users;
	GError *error = NULL;

	proxies = e_gw_connection_get_proxy_list_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (g_hash_table_size (proxies), ==, 2);
	g_assert_cmpstr (g_hash_table_lookup (proxies, "U1"), ==, "anna.mueller@example.com");
	g_hash_table_destroy (proxies);

	users = e_gw_connection_get_proxy_users_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (users->len, ==, 2);
	g_assert_cmpstr (((EGwProxyUser *) users->pdata[1])->display_name, ==, "Weber, Max");
	g_assert_cmpstr (((EGwProxyUser *) users->pdata[1])->email, ==, "max.weber@example.com");
	g_ptr_array_unref (users);

	e_gw_connection_set_proxy (proxy, "anna.mueller@example.com");
	g_assert_true (e_gw_connection_login_sync (proxy, "u1", "secret", NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpstr (e_gw_connection_get_user_email (proxy), ==, "anna.mueller@example.com");
	g_assert_cmpint (e_gw_connection_get_proxy_rights (proxy), ==,
		E_GW_PROXY_APPOINTMENT_READ | E_GW_PROXY_MAIL_READ);
	g_object_unref (proxy);

	/* The miscellaneous rights: private items, settings */
	proxy = e_gw_connection_new ("127.0.0.1", mock_port, FALSE);
	e_gw_connection_set_proxy (proxy, "chef@example.com");
	g_assert_true (e_gw_connection_login_sync (proxy, "u1", "secret", NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpint (e_gw_connection_get_proxy_rights (proxy), ==,
		E_GW_PROXY_APPOINTMENT_READ | E_GW_PROXY_MAIL_READ | E_GW_PROXY_MAIL_WRITE |
		E_GW_PROXY_READ_HIDDEN | E_GW_PROXY_SETUP);

	g_object_unref (proxy);
	g_object_unref (cnc);
}

/* Who may log in as proxy of the user: read, grant, change, take away */
static void
test_proxy_access (void)
{
	EGwConnection *cnc = logged_in ();
	GPtrArray *list;
	GError *error = NULL;
	gchar *id, *text;

	g_free (mock_get ("/_test/reset"));

	list = e_gw_connection_get_proxy_access_list_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (list->len, ==, 2);
	g_assert_true (e_gw_proxy_access_is_all_users (list->pdata[0]));
	g_assert_cmpint (((EGwProxyAccess *) list->pdata[0])->rights, ==, E_GW_PROXY_APPOINTMENT_READ);
	g_assert_cmpstr (((EGwProxyAccess *) list->pdata[1])->email, ==, "anna.mueller@example.com");
	g_assert_cmpint (((EGwProxyAccess *) list->pdata[1])->rights, ==,
		E_GW_PROXY_MAIL_READ | E_GW_PROXY_MAIL_WRITE | E_GW_PROXY_APPOINTMENT_READ | E_GW_PROXY_SETUP);
	g_ptr_array_unref (list);

	/* The POA grants only with the UUID: the address is resolved first */
	id = e_gw_connection_create_proxy_access_sync (cnc, "max.weber@example.com",
		E_GW_PROXY_MAIL_READ | E_GW_PROXY_NOTE_READ, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (id, ==, "U2@60");

	g_assert_true (e_gw_connection_modify_proxy_access_sync (cnc, id, E_GW_PROXY_MAIL_READ | E_GW_PROXY_NOTE_READ,
		E_GW_PROXY_MAIL_READ | E_GW_PROXY_TASK_READ | E_GW_PROXY_READ_HIDDEN, NULL, &error));
	g_assert_no_error (error);
	text = mock_get ("/_test/access");
	g_assert_nonnull (strstr (text, "max.weber@example.com:mail/read,misc/readHidden,task/read"));
	g_free (text);

	/* The entry for all users by its ID with the brackets */
	g_assert_true (e_gw_connection_modify_proxy_access_sync (cnc, "<All User Access>@60", E_GW_PROXY_APPOINTMENT_READ,
		0, NULL, &error));
	g_assert_no_error (error);
	text = mock_get ("/_test/access");
	g_assert_true (g_str_has_prefix (text, "<All User Access>:;"));
	g_free (text);

	g_assert_true (e_gw_connection_remove_proxy_access_sync (cnc, id, NULL, &error));
	g_assert_no_error (error);
	text = mock_get ("/_test/access");
	g_assert_null (strstr (text, "max.weber"));
	g_free (text);
	g_free (id);

	/* Nobody of that address */
	g_assert_null (e_gw_connection_create_proxy_access_sync (cnc, "niemand@example.com", E_GW_PROXY_MAIL_READ, NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_UNKNOWN_USER);
	g_clear_error (&error);

	g_object_unref (cnc);
}

/* Shared folders: who has one, share it, other rights, take it away; accept one */
static void
test_shares (void)
{
	EGwConnection *cnc = logged_in ();
	EGwShare *share;
	EGwShareEntry *entry;
	EGwShareNotification *offer;
	GPtrArray *added, *changed, *removed;
	GError *error = NULL;
	gchar *id, *text;

	g_free (mock_get ("/_test/reset"));

	/* A folder the user shares; asked for with its ID from before */
	share = e_gw_connection_get_share_sync (cnc, "F3.domain1.po1.100.0.1.0.1@14", NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (share->id, ==, "F3.domain1.po1.100.0.1.0.1@35");
	g_assert_true (share->shared_by_me);
	g_assert_false (share->shared_to_me);
	g_assert_true (share->share_tree);
	g_assert_cmpuint (share->entries->len, ==, 1);
	entry = share->entries->pdata[0];
	g_assert_cmpstr (entry->email, ==, "anna.mueller@example.com");
	g_assert_cmpint (entry->rights, ==, E_GW_SHARE_READ | E_GW_SHARE_ADD);
	g_assert_cmpint (entry->status, ==, E_GW_SHARE_STATUS_ACCEPTED);
	g_assert_cmpint (e_gw_share_get_my_rights (share), ==, E_GW_SHARE_ALL);

	/* Anna gets more rights, Max the folder too */
	changed = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);
	g_ptr_array_add (changed, e_gw_share_entry_copy (entry));
	((EGwShareEntry *) changed->pdata[0])->rights = E_GW_SHARE_ALL;
	added = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);
	g_ptr_array_add (added, e_gw_share_entry_new ("Weber, Max", "max.weber@example.com", E_GW_SHARE_EDIT));
	g_assert_true (e_gw_connection_modify_share_sync (cnc, share->id, added, changed, NULL, FALSE,
		"Ordner f\303\274r dich", "Schau <mal> rein", NULL, NULL, &error));
	g_assert_no_error (error);
	text = mock_get ("/_test/shares");
	g_assert_cmpstr (text, ==, "F3.domain1.po1.100.0.1.0.1:1:anna.mueller@example.com=raed/accepted,max.weber@example.com=re/");
	g_free (text);
	g_ptr_array_unref (added);
	g_ptr_array_unref (changed);
	e_gw_share_free (share);

	/* Max has not answered yet */
	share = e_gw_connection_get_share_sync (cnc, "F3.domain1.po1.100.0.1.0.1@35", NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (share->entries->len, ==, 2);
	entry = share->entries->pdata[1];
	g_assert_cmpstr (entry->display_name, ==, "Weber, Max");
	g_assert_cmpint (entry->status, ==, E_GW_SHARE_STATUS_PENDING);

	/* Both lose it: an ordinary folder again */
	removed = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);
	g_ptr_array_add (removed, e_gw_share_entry_copy (share->entries->pdata[0]));
	g_ptr_array_add (removed, e_gw_share_entry_copy (share->entries->pdata[1]));
	g_assert_true (e_gw_connection_modify_share_sync (cnc, share->id, NULL, NULL, removed, FALSE, NULL, NULL, NULL, NULL, &error));
	g_assert_no_error (error);
	g_ptr_array_unref (removed);
	e_gw_share_free (share);
	share = e_gw_connection_get_share_sync (cnc, "F3.domain1.po1.100.0.1.0.1@35", NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (share->id, ==, "F3.domain1.po1.100.0.1.0.1@14");
	g_assert_false (share->shared_by_me);
	g_assert_cmpuint (share->entries->len, ==, 0);
	e_gw_share_free (share);

	/* A folder not shared so far, with its subfolders */
	added = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);
	g_ptr_array_add (added, e_gw_share_entry_new ("Weber, Max", "max.weber@example.com", 0));
	g_assert_true (e_gw_connection_modify_share_sync (cnc, "F1.domain1.po1.100.0.1.0.1@14", added, NULL, NULL, TRUE,
		"Archiv", NULL, "Altes", NULL, &error));
	g_assert_no_error (error);
	g_ptr_array_unref (added);
	text = mock_get ("/_test/shares");
	g_assert_cmpstr (text, ==, "F1.domain1.po1.100.0.1.0.1:1:max.weber@example.com=r/;F3.domain1.po1.100.0.1.0.1:1:");
	g_free (text);

	/* Nothing to do, nothing sent */
	g_assert_true (e_gw_connection_modify_share_sync (cnc, "F1.domain1.po1.100.0.1.0.1@14", NULL, NULL, NULL, TRUE,
		NULL, NULL, NULL, NULL, &error));
	g_assert_no_error (error);

	/* A folder of another user */
	share = e_gw_connection_get_share_sync (cnc, "T1@14", NULL, &error);
	g_assert_no_error (error);
	g_assert_true (share->shared_to_me);
	g_assert_cmpstr (share->owner_name, ==, "Egon");
	g_assert_cmpstr (share->owner_email, ==, "egon@example.com");
	g_assert_cmpstr (share->description, ==, "Gemeinsame Ablage");
	g_assert_cmpint (e_gw_share_get_my_rights (share), ==, E_GW_SHARE_READ | E_GW_SHARE_ADD);
	g_assert_cmpint (((EGwShareEntry *) share->entries->pdata[0])->status, ==, E_GW_SHARE_STATUS_ACCEPTED);
	e_gw_share_free (share);

	/* The message offering a folder, and one that does not */
	g_assert_true (e_gw_share_is_notification_id ("SN1@37:7.domain1.po1.100.0.1.0.1@16"));
	g_assert_true (e_gw_share_is_notification_id ("SN1@37"));
	g_assert_false (e_gw_share_is_notification_id ("SN1@1:7.domain1.po1.100.0.1.0.1@37"));
	g_assert_false (e_gw_share_is_notification_id ("SN1@371"));
	g_assert_false (e_gw_share_is_notification_id (NULL));

	offer = e_gw_connection_get_share_notification_sync (cnc, "SN1@37:7.domain1.po1.100.0.1.0.1@16", NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (offer->folder_name, ==, "Projekt A");
	g_assert_cmpstr (offer->sender_name, ==, "Egon");
	g_assert_cmpstr (offer->message, ==, "Bitte annehmen");
	g_assert_cmpstr (offer->description, ==, "Unterlagen");
	g_assert_cmpint (offer->rights, ==, E_GW_SHARE_READ | E_GW_SHARE_ADD | E_GW_SHARE_EDIT);
	g_assert_cmpint (offer->kind, ==, E_GW_SHARE_KIND_FOLDER);
	{
		EGwShareNotification *calendar = e_gw_connection_get_share_notification_sync (cnc,
			"SN2@37:7.domain1.po1.100.0.1.0.1@16", NULL, &error);

		g_assert_no_error (error);
		g_assert_cmpint (calendar->kind, ==, E_GW_SHARE_KIND_CALENDAR);
		g_assert_cmpstr (calendar->folder_name, ==, "Urlaub");
		g_assert_cmpint (calendar->rights, ==, E_GW_SHARE_READ);
		e_gw_share_notification_free (calendar);
	}

	g_assert_null (e_gw_connection_get_share_notification_sync (cnc, MAIL1, NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_ITEM_NOT_FOUND);
	g_clear_error (&error);

	id = e_gw_connection_accept_share_sync (cnc, offer->id, "Projekt von Egon", NULL, "F1.domain1.po1.100.0.1.0.1@14", NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (id, ==, "MOUNT1@34:OWNERF1");
	g_free (id);
	text = mock_get ("/_test/shares");
	g_assert_true (g_str_has_suffix (text, ";accepted:Projekt von Egon:F1.domain1.po1.100.0.1.0.1@14"));
	g_free (text);

	/* Only once */
	g_assert_null (e_gw_connection_accept_share_sync (cnc, offer->id, NULL, NULL, NULL, NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_ITEM_NOT_FOUND);
	g_clear_error (&error);
	e_gw_share_notification_free (offer);

	g_object_unref (cnc);
}

/* The signatures: read, create, change (the default moves), remove */
static void
test_signatures (void)
{
	EGwConnection *cnc = logged_in ();
	EGwSignatures *signatures;
	EGwSignature *signature;
	GBytes *mime;
	GError *error = NULL;
	gchar *id, *text;
	const gchar *data;
	gsize len;

	g_free (mock_get ("/_test/reset"));

	signatures = e_gw_connection_get_signatures_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (signatures->enabled);
	g_assert_false (signatures->automatic);
	g_assert_cmpuint (signatures->signatures->len, ==, 1);
	signature = signatures->signatures->pdata[0];
	g_assert_cmpstr (signature->name, ==, "Kurz");
	g_assert_true (signature->is_default);
	data = g_bytes_get_data (signature->mime, &len);
	g_assert_nonnull (g_strstr_len (data, len, "Content-Type: text/plain"));
	g_assert_nonnull (g_strstr_len (data, len, "Grüße"));
	e_gw_signatures_free (signatures);

	data = "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nLang\r\n";
	mime = g_bytes_new_static (data, strlen (data));
	id = e_gw_connection_create_signature_sync (cnc, "Lang & breit", mime, FALSE, NULL, &error);
	g_bytes_unref (mime);
	g_assert_no_error (error);
	g_assert_nonnull (id);

	g_assert_true (e_gw_connection_modify_signature_sync (cnc, id, "Lang", NULL, TRUE, NULL, &error));
	g_assert_no_error (error);
	text = mock_get ("/_test/signatures");
	g_assert_cmpstr (text, ==, "Kurz:0:Grüße;Lang:1:Lang");
	g_free (text);

	g_assert_true (e_gw_connection_remove_signature_sync (cnc, id, NULL, &error));
	g_assert_no_error (error);
	text = mock_get ("/_test/signatures");
	g_assert_cmpstr (text, ==, "Kurz:0:Grüße");
	g_free (text);
	g_free (id);

	g_object_unref (cnc);
}

/* The rules: read, create, change (filter with types and source, actions
 * replaced), switch off, run, remove */
static void
test_rules (void)
{
	EGwConnection *cnc = logged_in ();
	GPtrArray *rules;
	EGwRule *rule, *changed;
	EGwRuleAction *action;
	GError *error = NULL;
	gchar *id, *text;

	g_free (mock_get ("/_test/reset"));

	rules = e_gw_connection_get_rules_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (rules->len, ==, 1);
	rule = rules->pdata[0];
	g_assert_cmpstr (rule->name, ==, "Heise");
	g_assert_true (rule->enabled);
	g_assert_cmpstr (rule->execution, ==, "New");
	g_assert_cmpstr (rule->types, ==, "Mail");
	g_assert_null (rule->filter->children);
	g_assert_cmpstr (rule->filter->field, ==, "from");
	g_assert_cmpstr (rule->filter->value, ==, "heise.de");
	g_assert_cmpuint (rule->actions->len, ==, 1);
	g_assert_cmpstr (((EGwRuleAction *) rule->actions->pdata[0])->type, ==, "Move");
	g_assert_cmpstr (((EGwRuleAction *) rule->actions->pdata[0])->container, ==, "F1.domain1.po1.100.0.1.0.1@14");
	g_ptr_array_unref (rules);

	/* New: an or group, an auto reply */
	rule = e_gw_rule_new ();
	rule->name = g_strdup ("Urlaub & mehr");
	rule->enabled = TRUE;
	rule->execution = g_strdup ("New");
	rule->types = g_strdup ("Mail");
	rule->source = g_strdup ("received");
	rule->sequence = 1;
	rule->filter = e_gw_filter_node_new_group ("or");
	g_ptr_array_add (rule->filter->children, e_gw_filter_node_new_entry ("subject", "contains", "Angebot"));
	g_ptr_array_add (rule->filter->children, e_gw_filter_node_new_entry ("from", "begins", "info@"));
	action = e_gw_rule_action_new ("Reply");
	action->has_item = TRUE;
	action->subject = g_strdup ("Danke");
	action->text = g_strdup ("Grüße");
	g_ptr_array_add (rule->actions, action);
	id = e_gw_connection_create_rule_sync (cnc, rule, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (id);
	g_free (rule->id);
	rule->id = id;

	rules = e_gw_connection_get_rules_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (rules->len, ==, 2);
	changed = rules->pdata[1];
	g_assert_cmpstr (changed->name, ==, "Urlaub & mehr");
	g_assert_cmpstr (changed->filter->op, ==, "or");
	g_assert_cmpuint (changed->filter->children->len, ==, 2);
	action = changed->actions->pdata[0];
	g_assert_true (action->has_item);
	g_assert_cmpstr (action->subject, ==, "Danke");
	g_assert_cmpstr (action->text, ==, "Grüße");

	/* Changed: another filter (types and source stay), other actions (not
	 * appended to the old ones), switched off */
	changed = e_gw_rule_copy (changed);
	e_gw_filter_node_free (changed->filter);
	changed->filter = e_gw_filter_node_new_entry ("created", "fieldLT", "-3");
	changed->filter->date = g_strdup ("Today");
	g_ptr_array_set_size (changed->actions, 0);
	g_ptr_array_add (changed->actions, e_gw_rule_action_new ("MarkRead"));
	action = e_gw_rule_action_new ("SimpleForward");
	action->has_item = TRUE;
	e_gw_rule_action_add_recipient (action, "bob@phantom.com", NULL, "TO");
	g_ptr_array_add (changed->actions, action);
	changed->enabled = FALSE;
	g_assert_true (e_gw_connection_modify_rule_sync (cnc, rules->pdata[1], changed, NULL, &error));
	g_assert_no_error (error);
	text = mock_get ("/_test/rules");
	g_assert_cmpstr (text, ==, "0:Heise:1:New:Mail:received:1;1:Urlaub & mehr:0:New:Mail:received:2");
	g_free (text);
	text = mock_get ("/_test/rule-xml?id=R11%4010");
	g_assert_nonnull (strstr (text, "<field>created</field><value>-3</value><date>Today</date>"));
	g_assert_nonnull (strstr (text, "<email>bob@phantom.com</email>"));
	g_free (text);

	/* Nothing changed: no call */
	g_assert_true (e_gw_connection_modify_rule_sync (cnc, changed, changed, NULL, &error));
	g_assert_no_error (error);

	g_assert_true (e_gw_connection_execute_rule_sync (cnc, "R1@10", NULL, &error));
	g_assert_no_error (error);
	text = mock_get ("/_test/executed-rules");
	g_assert_cmpstr (text, ==, "R1@10");
	g_free (text);

	g_assert_true (e_gw_connection_remove_rule_sync (cnc, rule->id, NULL, &error));
	g_assert_no_error (error);
	text = mock_get ("/_test/rules");
	g_assert_cmpstr (text, ==, "0:Heise:1:New:Mail:received:1");
	g_free (text);

	e_gw_rule_free (changed);
	e_gw_rule_free (rule);
	g_ptr_array_unref (rules);
	g_object_unref (cnc);
}

/* The events of a mailbox: configured, read once, removed */
static void
test_events (void)
{
	const gchar *types[] = { E_GW_EVENTS_MAIL, NULL };
	EGwConnection *cnc = logged_in ();
	GPtrArray *events;
	EGwEvent *event;
	GError *error = NULL;

	/* None without a configuration */
	events = e_gw_connection_get_events_sync (cnc, "test-key", TRUE, FALSE, NULL, &error);
	g_assert_null (events);
	g_assert_nonnull (error);
	g_clear_error (&error);

	g_assert_false (e_gw_connection_has_events_sync (cnc, "test-key", types, NULL, 0, NULL));
	g_assert_true (e_gw_connection_configure_events_sync (cnc, "test-key", types, 1, NULL, 0, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (e_gw_connection_has_events_sync (cnc, "test-key", types, NULL, 0, NULL));
	g_assert_false (e_gw_connection_has_events_sync (cnc, "other-key", types, NULL, 0, NULL));
	{
		const gchar *more[] = { E_GW_EVENTS_MAIL, "FolderAdd", NULL };

		g_assert_false (e_gw_connection_has_events_sync (cnc, "test-key", more, NULL, 0, NULL));
	}
	/* Told at an address: another configuration than a silent one */
	g_assert_false (e_gw_connection_has_events_sync (cnc, "test-key", types, "192.0.2.1", 5221, NULL));
	g_assert_true (e_gw_connection_configure_events_sync (cnc, "test-key", types, 1, "192.0.2.1", 5221, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (e_gw_connection_has_events_sync (cnc, "test-key", types, "192.0.2.1", 5221, NULL));
	g_assert_false (e_gw_connection_has_events_sync (cnc, "test-key", types, "192.0.2.1", 5222, NULL));
	g_assert_false (e_gw_connection_has_events_sync (cnc, "test-key", types, NULL, 0, NULL));
	events = e_gw_connection_get_events_sync (cnc, "test-key", TRUE, FALSE, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (events->len, ==, 3);
	event = events->pdata[0];
	g_assert_cmpstr (event->type, ==, "FolderItemAdd");
	g_assert_cmpstr (event->item, ==, "NEW1");
	g_assert_cmpstr (event->container, ==, "PM@16");
	g_assert_null (event->from);
	event = events->pdata[1];
	g_assert_cmpstr (event->type, ==, "ItemMarkRead");
	g_assert_null (event->container);
	event = events->pdata[2];
	g_assert_cmpstr (event->type, ==, "ItemDelete");
	g_assert_cmpstr (event->from, ==, "T1@14");
	g_ptr_array_unref (events);

	/* Read with remove: gone */
	events = e_gw_connection_get_events_sync (cnc, "test-key", TRUE, FALSE, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (events->len, ==, 0);
	g_ptr_array_unref (events);

	g_assert_true (e_gw_connection_remove_events_sync (cnc, "test-key", NULL, &error));
	g_assert_no_error (error);
	events = e_gw_connection_get_events_sync (cnc, "test-key", FALSE, FALSE, NULL, &error);
	g_assert_null (events);
	g_clear_error (&error);

	g_object_unref (cnc);
}

/* The junk lists and settings */
static void
test_junk (void)
{
	EGwConnection *cnc = logged_in ();
	GHashTable *settings, *changes;
	GPtrArray *entries;
	GError *error = NULL;
	gchar *id;
	guint ii, trust = 0;

	entries = e_gw_connection_get_junk_entries_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (entries->len, ==, 2);
	for (ii = 0; ii < entries->len; ii++) {
		EGwJunkEntry *entry = entries->pdata[ii];

		if (entry->list == E_GW_JUNK_LIST_TRUST) {
			trust++;
			g_assert_cmpstr (entry->match, ==, "bob@phantom.com");
			g_assert_false (entry->is_domain);
		} else {
			g_assert_cmpint (entry->list, ==, E_GW_JUNK_LIST_JUNK);
			g_assert_true (entry->is_domain);
		}
	}
	g_assert_cmpuint (trust, ==, 1);
	g_ptr_array_unref (entries);

	id = e_gw_connection_create_junk_entry_sync (cnc, "evil.example", TRUE, E_GW_JUNK_LIST_BLOCK, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (e_gw_connection_remove_junk_entry_sync (cnc, id, NULL, &error));
	g_assert_no_error (error);
	g_free (id);

	settings = e_gw_connection_get_junk_settings_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (g_hash_table_lookup (settings, E_GW_JUNK_SETTING_PERSISTENCE), ==, "14");
	g_hash_table_destroy (settings);

	changes = g_hash_table_new (g_str_hash, g_str_equal);
	g_hash_table_insert (changes, (gpointer) E_GW_JUNK_SETTING_USE_PAB, (gpointer) "1");
	g_hash_table_insert (changes, (gpointer) E_GW_JUNK_SETTING_PERSISTENCE, (gpointer) "7");
	g_assert_true (e_gw_connection_modify_junk_settings_sync (cnc, changes, NULL, &error));
	g_assert_no_error (error);
	g_hash_table_destroy (changes);
	settings = e_gw_connection_get_junk_settings_sync (cnc, NULL, &error);
	g_assert_cmpstr (g_hash_table_lookup (settings, E_GW_JUNK_SETTING_USE_PAB), ==, "1");
	g_assert_cmpstr (g_hash_table_lookup (settings, E_GW_JUNK_SETTING_PERSISTENCE), ==, "7");
	g_hash_table_destroy (settings);

	g_object_unref (cnc);
}

/* The out of office rule; the time zone of the test is Europe/Berlin */
static void
test_vacation (void)
{
	EGwConnection *cnc = logged_in ();
	EGwVacation *vacation;
	GError *error = NULL;
	gchar *tz;

	/* None yet: off, without text */
	vacation = e_gw_connection_get_vacation_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_false (vacation->enabled);
	g_assert_null (vacation->subject);
	e_gw_vacation_free (vacation);

	/* The zone with the months of change of Berlin, not the first one fitting the offsets */
	tz = e_gw_connection_dup_local_timezone_xml_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (strstr (tz, "<id>WET</id>"));
	g_free (tz);

	vacation = e_gw_vacation_new ();
	vacation->enabled = TRUE;
	vacation->subject = g_strdup ("Urlaub");
	vacation->message = g_strdup ("Bin im Urlaub. Grüße");
	vacation->reply_to_external = TRUE;
	vacation->my_contacts_only = TRUE;
	vacation->has_range = TRUE;
	vacation->all_day = TRUE;
	vacation->start_day = g_strdup ("2026-10-19");
	vacation->end_day = g_strdup ("2026-10-23");
	g_assert_true (e_gw_connection_set_vacation_sync (cnc, vacation, NULL, &error));
	g_assert_no_error (error);
	e_gw_vacation_free (vacation);

	vacation = e_gw_connection_get_vacation_sync (cnc, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (vacation->enabled);
	g_assert_cmpstr (vacation->subject, ==, "Urlaub");
	g_assert_cmpstr (vacation->message, ==, "Bin im Urlaub. Grüße");
	g_assert_true (vacation->reply_to_external);
	g_assert_true (vacation->my_contacts_only);
	g_assert_true (vacation->has_range);
	g_assert_true (vacation->all_day);
	g_assert_cmpstr (vacation->start_day, ==, "2026-10-19");
	g_assert_cmpstr (vacation->end_day, ==, "2026-10-23");
	e_gw_vacation_free (vacation);

	/* From and to a time; off */
	vacation = e_gw_vacation_new ();
	vacation->subject = g_strdup ("Seminar");
	vacation->has_range = TRUE;
	vacation->start = g_date_time_new_from_iso8601 ("2026-10-26T09:00:00+01:00", NULL);
	vacation->end = g_date_time_new_from_iso8601 ("2026-10-26T17:00:00+01:00", NULL);
	g_assert_true (e_gw_connection_set_vacation_sync (cnc, vacation, NULL, &error));
	e_gw_vacation_free (vacation);

	vacation = e_gw_connection_get_vacation_sync (cnc, NULL, &error);
	g_assert_false (vacation->enabled);
	g_assert_true (vacation->has_range);
	g_assert_false (vacation->all_day);
	{
		gchar *start = g_date_time_format_iso8601 (vacation->start);

		g_assert_cmpstr (start, ==, "2026-10-26T08:00:00Z");
		g_free (start);
	}
	e_gw_vacation_free (vacation);

	g_object_unref (cnc);
}

static void
test_folder_list (void)
{
	EGwConnection *cnc = logged_in ();
	GError *error = NULL;
	GPtrArray *folders;
	EGwFolder *folder, *archive;

	folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (folders);
	g_assert_cmpuint (folders->len, ==, 16);

	folder = find_folder (folders, "u1", NULL);
	g_assert_nonnull (folder);
	g_assert_cmpint (folder->type, ==, E_GW_FOLDER_TYPE_ROOT);
	g_assert_null (folder->parent_id);

	folder = find_folder (folders, "Mailbox", NULL);
	g_assert_nonnull (folder);
	g_assert_cmpstr (folder->id, ==, INBOX);
	g_assert_cmpstr (folder->parent_id, ==, ROOT);
	g_assert_cmpint (folder->type, ==, E_GW_FOLDER_TYPE_MAILBOX);
	g_assert_cmpint (folder->kind, ==, E_GW_FOLDER_KIND_SYSTEM);
	g_assert_cmpint (folder->count, ==, 2);
	g_assert_cmpint (folder->unread_count, ==, 1);

	g_assert_cmpint (find_folder (folders, "Sent Items", NULL)->type, ==, E_GW_FOLDER_TYPE_SENT_ITEMS);
	g_assert_cmpint (find_folder (folders, "Work In Progress", NULL)->type, ==, E_GW_FOLDER_TYPE_DRAFT);
	g_assert_cmpint (find_folder (folders, "Trash", NULL)->type, ==, E_GW_FOLDER_TYPE_TRASH);
	g_assert_cmpint (find_folder (folders, "Junk Mail", NULL)->type, ==, E_GW_FOLDER_TYPE_JUNK);
	g_assert_cmpint (find_folder (folders, "Alle Mails", NULL)->type, ==, E_GW_FOLDER_TYPE_QUERY);

	folder = find_folder (folders, "Team", NULL);
	g_assert_cmpint (folder->kind, ==, E_GW_FOLDER_KIND_SHARED);
	g_assert_true (folder->is_shared_to_me);
	g_assert_cmpstr (folder->owner, ==, "egon@example.com");

	/* What the owner lets the user do there */
	folder = find_folder (folders, "Nur lesen", NULL);
	g_assert_true (folder->is_shared_to_me);
	g_assert_cmpuint (folder->share_rights, ==, E_GW_SHARE_READ);
	g_assert_cmpuint (find_folder (folders, "Archiv", NULL)->share_rights, ==, 0);

	/* Two folders with the same name, told apart by their parent */
	archive = find_folder (folders, "Archiv", NULL);
	g_assert_cmpint (archive->type, ==, E_GW_FOLDER_TYPE_NORMAL);
	g_assert_cmpint (archive->kind, ==, E_GW_FOLDER_KIND_FOLDER);
	g_assert_nonnull (find_folder (folders, "Amazon", archive->id));
	g_assert_nonnull (find_folder (folders, "Amazon", ROOT));
	g_assert_cmpint (find_folder (folders, "Amazon", ROOT)->count, ==, 0);
	g_assert_cmpint (find_folder (folders, "Amazon", ROOT)->unread_count, ==, -1);

	g_ptr_array_unref (folders);
	g_object_unref (cnc);
}

static void
test_get_items (void)
{
	EGwConnection *cnc = logged_in ();
	GError *error = NULL;
	EGwResponse *response;
	xmlNode *item;
	gchar *raw_id, *id;
	guint count = 0;

	response = e_gw_connection_get_items_sync (cnc, INBOX, "default", NULL, -1, NULL, &error);
	g_assert_no_error (error);

	item = e_gw_xml_first_child (e_gw_xml_find (e_gw_response_get_node (response), "items"), "item");
	g_assert_nonnull (item);

	/* The mock wraps IDs over two lines, like the examples of the documentation */
	raw_id = e_gw_xml_dup_text (item, "id");
	g_assert_nonnull (strchr (raw_id, '\n'));
	id = e_gw_clean_id (raw_id);
	g_assert_cmpstr (id, ==, MAIL1);
	g_assert_true (e_gw_xml_get_bool (item, "status/read"));
	g_free (raw_id);
	g_free (id);

	for (; item; item = e_gw_xml_next_sibling (item, "item"))
		count++;
	g_assert_cmpuint (count, ==, 2);

	e_gw_response_free (response);
	g_object_unref (cnc);
}

static void
test_get_item (void)
{
	EGwConnection *cnc = logged_in ();
	GError *error = NULL;
	EGwResponse *response;
	xmlNode *item;
	gchar *subject, *attachment_name;

	response = e_gw_connection_get_item_sync (cnc, MAIL2, "default message attachments", NULL, &error);
	g_assert_no_error (error);

	item = e_gw_xml_find (e_gw_response_get_node (response), "item");
	subject = e_gw_xml_dup_text (item, "subject");
	attachment_name = e_gw_xml_dup_text (item, "attachments/attachment/name");
	g_assert_cmpstr (subject, ==, "Test message");
	g_assert_cmpstr (attachment_name, ==, "notes.txt");
	g_free (subject);
	g_free (attachment_name);
	e_gw_response_free (response);

	g_assert_null (e_gw_connection_get_item_sync (cnc, "NOSUCHITEM", NULL, NULL, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_ITEM_NOT_FOUND);
	g_assert_cmpstr (error->message, ==, "Item not found");
	g_clear_error (&error);

	g_object_unref (cnc);
}

static void
test_session_recovery (void)
{
	EGwConnection *cnc = logged_in ();
	GError *error = NULL;
	GPtrArray *folders;
	gint logins = mock_logins ();

	/* The POA answers 59910 without a description */
	g_free (mock_get ("/_test/expire"));

	folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (folders);
	g_assert_cmpint (mock_logins (), ==, logins + 1);
	g_ptr_array_unref (folders);

	/* ... or with a text that does not say "session": the code decides */
	g_free (mock_get ("/_test/expire?text=Sitzung%20ung%C3%BCltig"));

	folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (folders);
	g_assert_cmpint (mock_logins (), ==, logins + 2);

	g_ptr_array_unref (folders);
	g_object_unref (cnc);
}

static void
test_download (void)
{
	EGwConnection *cnc = logged_in ();
	GError *error = NULL;
	gchar *data;
	gint logins;

	data = download (cnc, MAIL2, TRUE, &error);
	g_assert_no_error (error);
	g_assert_true (g_str_has_prefix (data, "From: Bob <bob@phantom.com>\r\n"));
	g_assert_nonnull (strstr (data, "\r\nSubject: Test message\r\n"));
	g_free (data);

	data = download (cnc, ATTACHMENT, FALSE, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (data, ==, "attachment!");
	g_free (data);

	/* Not found is no session problem: no new login */
	logins = mock_logins ();
	g_assert_null (download (cnc, "NOSUCHITEM", TRUE, &error));
	g_assert_error (error, E_GW_ERROR, E_GW_ERROR_ITEM_NOT_FOUND);
	g_clear_error (&error);
	g_assert_cmpint (mock_logins (), ==, logins);

	/* A lost session is recovered by one new login */
	logins = mock_logins ();
	g_free (mock_get ("/_test/expire"));
	data = download (cnc, ATTACHMENT, FALSE, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (data, ==, "attachment!");
	g_assert_cmpint (mock_logins (), ==, logins + 1);
	g_free (data);

	g_object_unref (cnc);
}

static void
test_raw_call (void)
{
	EGwConnection *cnc = logged_in ();
	GError *error = NULL;
	EGwResponse *response;
	GString *inner = g_string_new (NULL);

	/* Escaping of request values */
	e_gw_xml_add_leaf (inner, "id", "a<b&c");
	g_assert_cmpstr (inner->str, ==, "<id>a&lt;b&amp;c</id>");
	g_string_free (inner, TRUE);

	response = e_gw_connection_call_sync (cnc, "noSuchMethod", NULL, NULL, &error);
	g_assert_null (response);
	g_assert_error (error, E_GW_ERROR, 1);
	g_clear_error (&error);

	g_object_unref (cnc);
}

int
main (int argc,
      char **argv)
{
	g_test_init (&argc, &argv, NULL);

	mock_port = port_from_env ("GW_MOCK_PORT");
	redirect_port = port_from_env ("GW_MOCK_REDIRECT_PORT");
	tls_port = port_from_env ("GW_MOCK_TLS_PORT");

	/* No CA of the user's own configuration in the tests */
	g_setenv ("GROUPWISE_CA_DIR", "/nonexistent", TRUE);

	g_test_add_func ("/connection/login-wrong-password", test_login_wrong_password);
	g_test_add_func ("/connection/login", test_login);
	g_test_add_func ("/connection/login-redirect", test_login_redirect);
	g_test_add_func ("/connection/tls-required", test_tls_required);
	g_test_add_func ("/connection/tls-untrusted", test_tls_untrusted);
	g_test_add_func ("/connection/tls-user-ca", test_tls_user_ca);
	g_test_add_func ("/connection/not-logged-in", test_not_logged_in);
	g_test_add_func ("/connection/connection-refused", test_connection_refused);
	g_test_add_func ("/connection/session-recovery", test_session_recovery);
	g_test_add_func ("/connection/raw-call", test_raw_call);
	g_test_add_func ("/connection/empty-responses", test_empty_responses);
	g_test_add_func ("/items/foreach", test_foreach_item);
	g_test_add_func ("/addressbook/list", test_address_books);
	g_test_add_func ("/connection/mailbox", test_mailbox);
	g_test_add_func ("/folders/list", test_folder_list);
	g_test_add_func ("/connection/proxy", test_proxy);
	g_test_add_func ("/connection/proxy-access", test_proxy_access);
	g_test_add_func ("/connection/shares", test_shares);
	g_test_add_func ("/connection/signatures", test_signatures);
	g_test_add_func ("/connection/rules", test_rules);
	g_test_add_func ("/connection/events", test_events);
	g_test_add_func ("/connection/junk", test_junk);
	g_test_add_func ("/connection/vacation", test_vacation);
	g_test_add_func ("/items/get-items", test_get_items);
	g_test_add_func ("/items/get-item", test_get_item);
	g_test_add_func ("/items/download", test_download);

	return g_test_run ();
}

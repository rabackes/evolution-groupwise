/*
 * test-camel.c: the groupwise Camel provider against the mock POA
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
 * Usage (through tests/mock/run-with-mock.py): test-camel MODULE-PATH
 */

#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <camel/camel.h>

#define INBOX "7.domain1.po1.100.0.1.0.1@16"
#define MAIL1 "45003893.domain1.po1.100.16E3837.1.FBA.1@1:" INBOX
#define MAIL2 "4512B362.domain1.po1.100.1676834.1.798.1@1:" INBOX
#define ARCHIV "F1.domain1.po1.100.0.1.0.1@14"
#define WIP "D.domain1.po1.100.0.1.0.1@22"
#define TRASH "8.domain1.po1.100.0.1.0.1@16"
#define IN(base, container) base "@1:" container
#define MAIL1_BASE "45003893.domain1.po1.100.16E3837.1.FBA.1"
#define MAIL2_BASE "4512B362.domain1.po1.100.1676834.1.798.1"

static guint16 mock_port;
static guint16 tls_port;
static gint trust_prompts;
static CamelCertTrust trust_answer = CAMEL_CERT_TRUST_TEMPORARY;
static const gchar *module_path;

/* Evolution's session asks for a new password when one is rejected; the
 * test session fails instead. */
typedef struct {
	CamelSession parent;
} TestSession;

typedef struct {
	CamelSessionClass parent_class;
} TestSessionClass;

static GType test_session_get_type (void);

G_DEFINE_TYPE (TestSession, test_session, CAMEL_TYPE_SESSION)

static gboolean
test_session_authenticate_sync (CamelSession *session,
				CamelService *service,
				const gchar *mechanism,
				GCancellable *cancellable,
				GError **error)
{
	switch (camel_service_authenticate_sync (service, mechanism, cancellable, error)) {
	case CAMEL_AUTHENTICATION_ACCEPTED:
		return TRUE;
	case CAMEL_AUTHENTICATION_REJECTED:
		g_set_error_literal (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE, "Password rejected");
		return FALSE;
	default:
		return FALSE;
	}
}

/* Accepts the self-signed certificate of the TLS mock for this session */
static CamelCertTrust
test_session_trust_prompt (CamelSession *session,
			   CamelService *service,
			   GTlsCertificate *certificate,
			   GTlsCertificateFlags errors)
{
	trust_prompts++;

	return trust_answer;
}

/* Like Evolution without filter rules: new mail in the Mailbox runs through it */
static CamelFilterDriver *
test_session_get_filter_driver (CamelSession *session,
				const gchar *type,
				CamelFolder *for_folder,
				GError **error)
{
	return camel_filter_driver_new (session);
}

static void
test_session_class_init (TestSessionClass *class)
{
	CAMEL_SESSION_CLASS (class)->authenticate_sync = test_session_authenticate_sync;
	CAMEL_SESSION_CLASS (class)->trust_prompt = test_session_trust_prompt;
	CAMEL_SESSION_CLASS (class)->get_filter_driver = test_session_get_filter_driver;
}

static void
test_session_init (TestSession *session)
{
}

typedef struct {
	gchar *tmp;
	CamelSession *session;
	CamelService *store;
} Fixture;

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

static void
mock_do (const gchar *format,
	 ...)
{
	va_list args;
	gchar *path;

	va_start (args, format);
	path = g_strdup_vprintf (format, args);
	va_end (args);

	g_free (mock_get (path));
	g_free (path);
}

static gchar *
mock_get_f (const gchar *format,
	    ...)
{
	va_list args;
	gchar *path, *text;

	va_start (args, format);
	path = g_strdup_vprintf (format, args);
	va_end (args);

	text = mock_get (path);
	g_free (path);

	return text;
}

static gboolean
mock_is_read (const gchar *id)
{
	gchar *path = g_strdup_printf ("/_test/read?id=%s", id);
	gchar *text = mock_get (path);
	gboolean read = g_str_equal (text, "1");

	g_free (text);
	g_free (path);

	return read;
}

static void
remove_tree (const gchar *path)
{
	GDir *dir = g_dir_open (path, 0, NULL);
	const gchar *entry;

	while (dir && (entry = g_dir_read_name (dir)) != NULL) {
		gchar *child = g_build_filename (path, entry, NULL);

		if (g_file_test (child, G_FILE_TEST_IS_DIR))
			remove_tree (child);
		else
			g_unlink (child);
		g_free (child);
	}
	if (dir)
		g_dir_close (dir);
	g_rmdir (path);
}

static CamelService *
new_store_at (Fixture *fixture,
	      const gchar *password,
	      guint16 port,
	      CamelNetworkSecurityMethod method)
{
	GError *error = NULL;
	CamelService *store;
	CamelSettings *settings;

	store = camel_session_add_service (fixture->session, "gw", "groupwise", CAMEL_PROVIDER_STORE, &error);
	g_assert_no_error (error);

	settings = camel_service_ref_settings (store);
	g_object_set (settings, "host", "127.0.0.1", "port", port, "user", "u1", "security-method", method, NULL);
	g_object_unref (settings);
	camel_service_set_password (store, password);

	return store;
}

static CamelService *
new_store (Fixture *fixture,
	   const gchar *password)
{
	return new_store_at (fixture, password, mock_port, CAMEL_NETWORK_SECURITY_METHOD_NONE);
}

static void
fixture_setup (Fixture *fixture,
	       gconstpointer data)
{
	GError *error = NULL;

	/* Every test starts with the same mailbox */
	mock_do ("/_test/reset");

	fixture->tmp = g_dir_make_tmp ("gw-camel-XXXXXX", NULL);
	fixture->session = g_object_new (test_session_get_type (),
		"user-data-dir", fixture->tmp,
		"user-cache-dir", fixture->tmp,
		"online", TRUE,
		NULL);
	fixture->store = new_store (fixture, "secret");

	g_assert_true (camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (fixture->store), TRUE, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpint (camel_service_get_connection_status (fixture->store), ==, CAMEL_SERVICE_CONNECTED);
}

static void
fixture_teardown (Fixture *fixture,
		  gconstpointer data)
{
	camel_service_disconnect_sync (fixture->store, TRUE, NULL, NULL);
	g_object_unref (fixture->store);
	g_object_unref (fixture->session);
	remove_tree (fixture->tmp);
	g_free (fixture->tmp);
}

static CamelFolderInfo *
find_info (CamelFolderInfo *fi,
	   const gchar *full_name)
{
	for (; fi; fi = fi->next) {
		CamelFolderInfo *found;

		if (g_strcmp0 (fi->full_name, full_name) == 0)
			return fi;
		found = find_info (fi->child, full_name);
		if (found)
			return found;
	}

	return NULL;
}

static void
test_wrong_password (void)
{
	Fixture fixture = { NULL };
	GError *error = NULL;
	CamelService *store;

	fixture.tmp = g_dir_make_tmp ("gw-camel-XXXXXX", NULL);
	fixture.session = g_object_new (test_session_get_type (),
		"user-data-dir", fixture.tmp, "user-cache-dir", fixture.tmp, "online", TRUE, NULL);
	store = new_store (&fixture, "wrong");

	g_assert_false (camel_service_connect_sync (store, NULL, &error));
	g_assert_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE);
	g_clear_error (&error);

	g_object_unref (store);
	g_object_unref (fixture.session);
	remove_tree (fixture.tmp);
	g_free (fixture.tmp);
}

static void
test_untrusted_certificate (void)
{
	Fixture fixture = { NULL };
	GError *error = NULL;
	CamelService *store;

	fixture.tmp = g_dir_make_tmp ("gw-camel-XXXXXX", NULL);
	fixture.session = g_object_new (test_session_get_type (),
		"user-data-dir", fixture.tmp, "user-cache-dir", fixture.tmp, "online", TRUE, NULL);
	store = new_store_at (&fixture, "secret", tls_port, CAMEL_NETWORK_SECURITY_METHOD_SSL_ON_ALTERNATE_PORT);

	/* Asked once, then remembered for the connection */
	trust_prompts = 0;
	g_assert_true (camel_service_connect_sync (store, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpint (trust_prompts, ==, 1);

	camel_service_disconnect_sync (store, TRUE, NULL, NULL);
	g_object_unref (store);
	g_object_unref (fixture.session);
	remove_tree (fixture.tmp);
	g_free (fixture.tmp);
}

/* One connection through the TLS mock in a fresh session: as after a restart of Evolution */
static gboolean
connect_tls (void)
{
	Fixture fixture = { NULL };
	GError *error = NULL;
	CamelService *store;
	gboolean connected;

	fixture.tmp = g_dir_make_tmp ("gw-camel-XXXXXX", NULL);
	fixture.session = g_object_new (test_session_get_type (),
		"user-data-dir", fixture.tmp, "user-cache-dir", fixture.tmp, "online", TRUE, NULL);
	store = new_store_at (&fixture, "secret", tls_port, CAMEL_NETWORK_SECURITY_METHOD_SSL_ON_ALTERNATE_PORT);

	connected = camel_service_connect_sync (store, NULL, &error);
	g_clear_error (&error);

	camel_service_disconnect_sync (store, TRUE, NULL, NULL);
	g_object_unref (store);
	g_object_unref (fixture.session);
	remove_tree (fixture.tmp);
	g_free (fixture.tmp);

	return connected;
}

static void
test_certificate_kept (void)
{
	gchar *dir = g_dir_make_tmp ("gw-certdb-XXXXXX", NULL);
	gchar *filename = g_build_filename (dir, "camel-cert.db", NULL);
	CamelCertDB *certdb = camel_certdb_new ();

	/* Evolution keeps accepted certificates in the default CamelCertDB */
	camel_certdb_set_filename (certdb, filename);
	camel_certdb_set_default (certdb);

	/* Accepted permanently: asked once, not again after a restart */
	trust_answer = CAMEL_CERT_TRUST_FULLY;
	trust_prompts = 0;
	g_assert_true (connect_tls ());
	g_assert_true (connect_tls ());
	g_assert_cmpint (trust_prompts, ==, 1);

	/* Accepted temporarily: asked every time */
	camel_certdb_clear (certdb);
	trust_answer = CAMEL_CERT_TRUST_TEMPORARY;
	trust_prompts = 0;
	g_assert_true (connect_tls ());
	g_assert_true (connect_tls ());
	g_assert_cmpint (trust_prompts, ==, 2);

	/* Rejected permanently: refused without asking again */
	camel_certdb_clear (certdb);
	trust_answer = CAMEL_CERT_TRUST_NEVER;
	trust_prompts = 0;
	g_assert_false (connect_tls ());
	g_assert_false (connect_tls ());
	g_assert_cmpint (trust_prompts, ==, 1);

	trust_answer = CAMEL_CERT_TRUST_TEMPORARY;
	camel_certdb_set_default (NULL);
	g_object_unref (certdb);
	remove_tree (dir);
	g_free (filename);
	g_free (dir);
}

static void
test_folder_info (Fixture *fixture,
		  gconstpointer data)
{
	GError *error = NULL;
	CamelFolderInfo *fi, *tree;

	tree = camel_store_get_folder_info_sync (CAMEL_STORE (fixture->store), NULL,
		CAMEL_STORE_FOLDER_INFO_RECURSIVE, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (tree);

	fi = find_info (tree, "Mailbox");
	g_assert_nonnull (fi);
	g_assert_cmpuint (fi->flags & CAMEL_FOLDER_TYPE_MASK, ==, CAMEL_FOLDER_TYPE_INBOX);
	g_assert_cmpint (fi->unread, ==, 1);
	g_assert_cmpint (fi->total, ==, 2);

	g_assert_cmpuint (find_info (tree, "Sent Items")->flags & CAMEL_FOLDER_TYPE_MASK, ==, CAMEL_FOLDER_TYPE_SENT);
	g_assert_cmpuint (find_info (tree, "Trash")->flags & CAMEL_FOLDER_TYPE_MASK, ==, CAMEL_FOLDER_TYPE_TRASH);
	g_assert_cmpuint (find_info (tree, "Work In Progress")->flags & CAMEL_FOLDER_TYPE_MASK, ==, CAMEL_FOLDER_TYPE_DRAFTS);

	/* Nested folders, two of them named alike */
	g_assert_nonnull (find_info (tree, "Archiv"));
	g_assert_nonnull (find_info (tree, "Archiv/Amazon"));
	g_assert_nonnull (find_info (tree, "Amazon"));
	g_assert_true (find_info (tree, "Archiv")->child != NULL);

	/* A shared folder whose parent is gone stays hidden, as in the GroupWise client */
	g_assert_null (find_info (tree, "Verwaist"));

	/* No calendars, not even shared ones below the calendar */
	g_assert_null (find_info (tree, "Calendar"));
	g_assert_null (find_info (tree, "Calendar/Team"));
	g_assert_null (find_info (tree, "Team"));
	g_assert_null (find_info (tree, "u1"));

	camel_folder_info_free (tree);
}

static CamelFolder *
open_inbox (Fixture *fixture)
{
	GError *error = NULL;
	CamelFolder *folder;

	folder = camel_store_get_inbox_folder_sync (CAMEL_STORE (fixture->store), NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (folder);
	g_assert_cmpstr (camel_folder_get_full_name (folder), ==, "Mailbox");

	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);

	return folder;
}

static void
test_refresh (Fixture *fixture,
	      gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	CamelMessageInfo *info;

	g_assert_cmpint (camel_folder_get_message_count (folder), ==, 2);
	g_assert_cmpint (camel_folder_get_unread_message_count (folder), ==, 1);

	info = camel_folder_get_message_info (folder, MAIL1);
	g_assert_nonnull (info);
	g_assert_cmpstr (camel_message_info_get_subject (info), ==, "Quarterly report");
	g_assert_true (camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN);
	g_assert_true (camel_message_info_get_flags (info) & CAMEL_MESSAGE_ATTACHMENTS);
	g_object_unref (info);

	info = camel_folder_get_message_info (folder, MAIL2);
	g_assert_nonnull (info);
	g_assert_cmpstr (camel_message_info_get_subject (info), ==, "Test message");
	g_assert_cmpstr (camel_message_info_get_from (info), ==, "Bob <bob@phantom.com>");
	g_assert_cmpint (camel_message_info_get_date_received (info), ==, 1790238600);	/* 2026-09-24T08:30:00Z */
	g_assert_false (camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN);
	g_object_unref (info);

	g_object_unref (folder);
}

static void
test_get_message (Fixture *fixture,
		  gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	GError *error = NULL;
	CamelMimeMessage *message;
	CamelMessageInfo *info;
	gchar *filename;

	/* The original from the Internet, not the MIME rebuilt by the POA */
	message = camel_folder_get_message_sync (folder, MAIL2, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (message);
	g_assert_cmpstr (camel_mime_message_get_subject (message), ==, "Test message");
	g_assert_cmpstr (camel_medium_get_header (CAMEL_MEDIUM (message), "X-Original-MIME"), ==, "yes");
	g_assert_cmpstr (camel_mime_message_get_message_id (message), ==, "orig-1@phantom.com");
	g_object_unref (message);

	/* The summary takes Message-ID and References from the message for threading */
	info = camel_folder_get_message_info (folder, MAIL2);
	g_assert_cmpuint (camel_message_info_get_message_id (info), ==,
		camel_folder_search_util_hash_message_id ("<orig-1@phantom.com>", TRUE));
	g_assert_nonnull (camel_message_info_get_references (info));
	g_assert_cmpuint (camel_message_info_get_references (info)->len, ==, 1);
	g_assert_cmpuint (g_array_index (camel_message_info_get_references (info), guint64, 0), ==,
		camel_folder_search_util_hash_message_id ("<parent-1@phantom.com>", TRUE));
	g_object_unref (info);

	filename = camel_folder_get_filename (folder, MAIL2, NULL);
	g_assert_true (g_file_test (filename, G_FILE_TEST_EXISTS));
	g_free (filename);

	/* Offline the message comes from the cache */
	g_assert_true (camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (fixture->store), FALSE, NULL, &error));
	message = camel_folder_get_message_sync (folder, MAIL2, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (message);
	g_object_unref (message);

	/* ... and an uncached one is refused */
	g_assert_null (camel_folder_get_message_sync (folder, MAIL1, NULL, &error));
	g_assert_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE);
	g_clear_error (&error);

	g_object_unref (folder);
}

static void
test_flags (Fixture *fixture,
	    gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	GError *error = NULL;
	CamelMessageInfo *info;

	/* Read in Evolution: written to the server */
	g_assert_false (mock_is_read (MAIL2));
	camel_folder_set_message_flags (folder, MAIL2, CAMEL_MESSAGE_SEEN, CAMEL_MESSAGE_SEEN);
	g_assert_true (camel_folder_synchronize_sync (folder, FALSE, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (mock_is_read (MAIL2));

	/* Unread elsewhere: taken over by the next refresh */
	mock_do ("/_test/set-read?id=%s&value=0", MAIL1);
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);
	info = camel_folder_get_message_info (folder, MAIL1);
	g_assert_false (camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN);
	g_assert_false (camel_message_info_get_folder_flagged (info));
	g_object_unref (info);

	/* ... and not written back */
	g_assert_true (camel_folder_synchronize_sync (folder, FALSE, NULL, &error));
	g_assert_false (mock_is_read (MAIL1));

	g_object_unref (folder);
}

static void
test_read_only (Fixture *fixture,
		gconstpointer data)
{
	CamelFolder *folder;
	CamelSettings *settings;
	CamelMessageInfo *info;
	GError *error = NULL;

	mock_do ("/_test/set-read?id=%s&value=0", MAIL2);
	settings = camel_service_ref_settings (fixture->store);
	g_object_set (settings, "read-only", TRUE, NULL);
	g_object_unref (settings);

	folder = open_inbox (fixture);
	camel_folder_set_message_flags (folder, MAIL2, CAMEL_MESSAGE_SEEN, CAMEL_MESSAGE_SEEN);
	g_assert_true (camel_folder_synchronize_sync (folder, FALSE, NULL, &error));
	g_assert_no_error (error);

	/* Read in Evolution, unread on the server, and it stays so after a refresh */
	g_assert_false (mock_is_read (MAIL2));
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	info = camel_folder_get_message_info (folder, MAIL2);
	g_assert_true (camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN);
	g_object_unref (info);
	g_assert_false (mock_is_read (MAIL2));

	g_object_unref (folder);
}

static void
test_flag_merge (Fixture *fixture,
		 gconstpointer data)
{
	CamelFolder *folder;
	GError *error = NULL;
	CamelMessageInfo *info;

	mock_do ("/_test/set-read?id=%s&value=1", MAIL1);
	mock_do ("/_test/set-read?id=%s&value=0", MAIL2);
	folder = open_inbox (fixture);

	/* Evolution changes a flag of its own (here: important) on an unread mail,
	 * which is then read elsewhere before the next write */
	camel_folder_set_message_flags (folder, MAIL2, CAMEL_MESSAGE_FLAGGED, CAMEL_MESSAGE_FLAGGED);
	mock_do ("/_test/set-read?id=%s&value=1", MAIL2);

	/* The user marks a read mail unread; the server still says read */
	camel_folder_set_message_flags (folder, MAIL1, CAMEL_MESSAGE_SEEN, 0);

	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);

	info = camel_folder_get_message_info (folder, MAIL2);
	g_assert_true (camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN);	/* from the server */
	g_assert_true (camel_message_info_get_flags (info) & CAMEL_MESSAGE_FLAGGED);	/* kept */
	g_object_unref (info);

	info = camel_folder_get_message_info (folder, MAIL1);
	g_assert_false (camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN);	/* the user's change */
	g_object_unref (info);

	g_assert_true (camel_folder_synchronize_sync (folder, FALSE, NULL, &error));
	g_assert_no_error (error);

	/* No stale "unread" for MAIL2, the user's "unread" for MAIL1 */
	g_assert_true (mock_is_read (MAIL2));
	g_assert_false (mock_is_read (MAIL1));

	g_object_unref (folder);
}

typedef struct {
	GHashTable *recent;
	guint signals;
} ChangedData;

static void
collect_recent_cb (CamelFolder *folder,
		   CamelFolderChangeInfo *changes,
		   ChangedData *data)
{
	GPtrArray *uids = camel_folder_change_info_get_recent_uids (changes);
	guint ii;

	for (ii = 0; uids && ii < uids->len; ii++)
		g_hash_table_add (data->recent, g_strdup (uids->pdata[ii]));
	data->signals++;
}

static gboolean
timeout_cb (gpointer user_data)
{
	*(gboolean *) user_data = TRUE;
	return G_SOURCE_REMOVE;
}

/* Camel emits "changed" from an idle callback of the session, after the
 * filters (in a thread of their own) are done with recent messages */
static void
wait_for_changed (ChangedData *data,
		  guint signals)
{
	gboolean timed_out = FALSE;
	guint id = g_timeout_add_seconds (5, timeout_cb, &timed_out);

	while (data->signals < signals && !timed_out)
		g_main_context_iteration (NULL, TRUE);

	g_assert_false (timed_out);
	g_source_remove (id);
}

static void
test_recent (Fixture *fixture,
	     gconstpointer data)
{
	ChangedData changed = { g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL), 0 };
	CamelSettings *settings;
	CamelFolder *folder;
	GError *error = NULL;

	mock_do ("/_test/set-read?id=%s&value=0", MAIL2);
	folder = camel_store_get_inbox_folder_sync (CAMEL_STORE (fixture->store), NULL, &error);
	g_assert_no_error (error);

	/* Off by default: GroupWise filters on the server */
	g_assert_false (camel_folder_get_flags (folder) & CAMEL_FOLDER_FILTER_RECENT);
	settings = camel_service_ref_settings (fixture->store);
	g_object_set (settings, "filter-inbox", TRUE, NULL);
	g_object_unref (settings);
	g_assert_true (camel_folder_get_flags (folder) & CAMEL_FOLDER_FILTER_RECENT);

	g_signal_connect (folder, "changed", G_CALLBACK (collect_recent_cb), &changed);

	/* The first fill is no new mail: "recent" would make Evolution fetch
	 * every unread message for filters and the junk test */
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);
	wait_for_changed (&changed, 1);
	g_assert_cmpuint (camel_folder_get_message_count (folder), ==, 2);
	g_assert_cmpuint (g_hash_table_size (changed.recent), ==, 0);

	/* What arrives later is */
	mock_do ("/_test/new-draft");
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);
	wait_for_changed (&changed, 2);
	g_assert_true (g_hash_table_contains (changed.recent, "DRAFT1"));
	g_assert_cmpuint (g_hash_table_size (changed.recent), ==, 1);

	g_object_unref (folder);
	g_hash_table_destroy (changed.recent);
}

static gint
mock_calls (const gchar *action)
{
	gchar *path = g_strdup_printf ("/_test/calls?action=%s", action);
	gchar *text = mock_get (path);
	gint count = atoi (text);

	g_free (text);
	g_free (path);

	return count;
}

static void
test_quick_check (Fixture *fixture,
		  gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);	/* a full scan */
	CamelMessageInfo *info;
	GError *error = NULL;
	gint cursors, quick;

	/* Nothing changed: one getQuickMessages, no cursor over the folder */
	cursors = mock_calls ("createCursor");
	quick = mock_calls ("getQuickMessages");
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpint (mock_calls ("createCursor"), ==, cursors);
	g_assert_cmpint (mock_calls ("getQuickMessages"), ==, quick + 1);

	/* Changed elsewhere, new and deleted */
	mock_do ("/_test/set-read?id=%s&value=%d", MAIL1, 0);
	mock_do ("/_test/new-draft");
	mock_do ("/_test/hide?id=%s", MAIL2);

	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);
	/* one cursor for the summary of the new item, none over the whole folder */
	g_assert_cmpint (mock_calls ("createCursor"), ==, cursors + 1);

	info = camel_folder_get_message_info (folder, MAIL1);
	g_assert_false (camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN);
	g_object_unref (info);
	info = camel_folder_get_message_info (folder, "DRAFT1");
	g_assert_nonnull (info);
	g_object_unref (info);

	/* ... the deletion comes with the next full scan */
	info = camel_folder_get_message_info (folder, MAIL2);
	g_assert_nonnull (info);
	g_object_unref (info);
	g_setenv ("GROUPWISE_FULL_SCAN_SECONDS", "0", TRUE);
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_unsetenv ("GROUPWISE_FULL_SCAN_SECONDS");
	g_assert_no_error (error);
	g_assert_null (camel_folder_get_message_info (folder, MAIL2));

	g_object_unref (folder);
}

/* An invitation leaves the Mailbox once answered (the POA lists it no
 * more); the quick check sees it without waiting for a full scan */
static void
test_answered_invitation (Fixture *fixture,
			  gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	CamelMessageInfo *info;
	GError *error = NULL;

	mock_do ("/_test/new-invitation");
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);
	info = camel_folder_get_message_info (folder, "INV1@4:" INBOX);
	g_assert_nonnull (info);
	g_object_unref (info);

	/* Answered elsewhere (in the calendar, in the GroupWise client) */
	mock_do ("/_test/hide?id=INV1@4:" INBOX);
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);
	g_assert_null (camel_folder_get_message_info (folder, "INV1@4:" INBOX));

	/* Mail stays */
	info = camel_folder_get_message_info (folder, MAIL1);
	g_assert_nonnull (info);
	g_object_unref (info);

	g_object_unref (folder);
}

/* The user changed a subject in the GroupWise client */
static void
test_changed_subject (Fixture *fixture,
		      gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	CamelMimeMessage *message;
	CamelMessageInfo *info;
	GError *error = NULL;

	mock_do ("/_test/set-subject?id=%s&value=%s", MAIL2, "Neuer%20Betreff");
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);

	/* In the message list ... */
	info = camel_folder_get_message_info (folder, MAIL2);
	g_assert_cmpstr (camel_message_info_get_subject (info), ==, "Neuer Betreff");
	g_object_unref (info);

	/* ... and in the opened message */
	message = camel_folder_get_message_sync (folder, MAIL2, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (camel_mime_message_get_subject (message), ==, "Neuer Betreff");
	g_object_unref (message);

	g_object_unref (folder);
}

static gboolean
has_label (CamelFolder *folder,
	   const gchar *uid,
	   const gchar *tag)
{
	CamelMessageInfo *info = camel_folder_get_message_info (folder, uid);
	gboolean has = info && camel_message_info_get_user_flag (info, tag);

	g_clear_object (&info);

	return has;
}

/* Categories are labels: the built-in ones Evolution's, the others by name */
static void
test_labels (Fixture *fixture,
	     gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
		"org.gnome.evolution.mail", TRUE);
	GError *error = NULL;
	gchar *text;

	/* Set in GroupWise: Urgent and Kurs; the hidden category is no label */
	mock_do ("/_test/set-categories?id=%s&value=%s", MAIL2,
		"3.domain1.po1.100.0.1.0.1@12,K1.domain1.po1.100.0.1.0.1@12");
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (has_label (folder, MAIL2, "$Labelimportant"));
	g_assert_true (has_label (folder, MAIL2, "kurs"));
	g_assert_false (has_label (folder, MAIL1, "$Labelimportant"));

	/* Changed in Evolution: Kurs off, Personal on */
	camel_folder_set_message_user_flag (folder, MAIL2, "kurs", FALSE);
	camel_folder_set_message_user_flag (folder, MAIL2, "$Labelpersonal", TRUE);
	g_assert_true (camel_folder_synchronize_sync (folder, FALSE, NULL, &error));
	g_assert_no_error (error);
	text = mock_get_f ("/_test/categories?id=%s", MAIL2);
	g_assert_cmpstr (text, ==, "1.domain1.po1.100.0.1.0.1@12,3.domain1.po1.100.0.1.0.1@12");
	g_free (text);

	/* A refresh leaves them so */
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_true (has_label (folder, MAIL2, "$Labelpersonal"));
	g_assert_false (has_label (folder, MAIL2, "kurs"));

	/* With Evolution's settings: Kurs is a label now, and "Work" becomes a
	 * category when used */
	if (schema) {
		GSettings *settings = g_settings_new ("org.gnome.evolution.mail");
		gchar **labels = g_settings_get_strv (settings, "labels");
		gboolean found = FALSE;
		guint ii;

		for (ii = 0; labels[ii]; ii++)
			found = found || g_str_equal (labels[ii], "Kurs:#a6ffa6|kurs");
		g_assert_true (found);
		g_strfreev (labels);
		g_object_unref (settings);

		camel_folder_set_message_user_flag (folder, MAIL1, "$Labelwork", TRUE);
		g_assert_true (camel_folder_synchronize_sync (folder, FALSE, NULL, &error));
		g_assert_no_error (error);
		text = mock_get_f ("/_test/category-names");
		g_assert_nonnull (strstr (text, g_dgettext ("evolution", "Work")));
		g_free (text);
		text = mock_get_f ("/_test/categories?id=%s", MAIL1);
		g_assert_true (g_str_has_prefix (text, "NEWCAT"));
		g_free (text);
		g_settings_schema_unref (schema);
	}

	g_object_unref (folder);
}

static CamelFolder *
open_folder (Fixture *fixture,
	     const gchar *full_name)
{
	GError *error = NULL;
	CamelFolder *folder;

	folder = camel_store_get_folder_sync (CAMEL_STORE (fixture->store), full_name, 0, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_assert_no_error (error);

	return folder;
}

/* A full scan, as after 15 minutes: what the server really has */
static void
full_refresh (CamelFolder *folder)
{
	GError *error = NULL;

	g_setenv ("GROUPWISE_FULL_SCAN_SECONDS", "0", TRUE);
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_unsetenv ("GROUPWISE_FULL_SCAN_SECONDS");
	g_assert_no_error (error);
}

static gboolean
has_message (CamelFolder *folder,
	     const gchar *uid)
{
	CamelMessageInfo *info = camel_folder_get_message_info (folder, uid);
	gboolean found = info != NULL;

	g_clear_object (&info);

	return found;
}

static void
test_move (Fixture *fixture,
	   gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture), *archive = open_folder (fixture, "Archiv");
	GPtrArray *uids = g_ptr_array_new (), *transferred = NULL;
	CamelMimeMessage *message;
	GError *error = NULL;

	/* Fetched before: the move must not need a second download */
	message = camel_folder_get_message_sync (inbox, MAIL2, NULL, &error);
	g_assert_no_error (error);
	g_object_unref (message);

	g_ptr_array_add (uids, (gpointer) MAIL2);
	g_assert_true (camel_folder_transfer_messages_to_sync (inbox, uids, archive, TRUE, &transferred, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (transferred->len, ==, 1);
	g_assert_cmpstr (transferred->pdata[0], ==, IN (MAIL2_BASE, ARCHIV));

	g_assert_false (has_message (inbox, MAIL2));
	g_assert_true (has_message (archive, IN (MAIL2_BASE, ARCHIV)));
	g_assert_nonnull (camel_folder_get_message_cached (archive, IN (MAIL2_BASE, ARCHIV), NULL));

	/* ... and the server agrees */
	full_refresh (inbox);
	full_refresh (archive);
	g_assert_false (has_message (inbox, MAIL2));
	g_assert_true (has_message (archive, IN (MAIL2_BASE, ARCHIV)));
	g_assert_cmpint (camel_folder_get_message_count (inbox), ==, 1);

	g_ptr_array_unref (transferred);
	g_ptr_array_unref (uids);
	g_object_unref (archive);
	g_object_unref (inbox);
}

/* A search result folder is a view: its messages are in other folders */
static void
test_search_folder (Fixture *fixture,
		    gconstpointer data)
{
	CamelFolder *all = open_folder (fixture, "Alle Mails"), *inbox = open_inbox (fixture);
	CamelFolder *archive = open_folder (fixture, "Archiv");
	GPtrArray *uids = g_ptr_array_new ();
	GError *error = NULL;

	/* Under the IDs of the folder they are in */
	g_assert_true (has_message (all, MAIL1));
	g_assert_true (has_message (all, MAIL2));

	/* Nothing goes into it */
	g_ptr_array_add (uids, (gpointer) MAIL2);
	g_assert_false (camel_folder_transfer_messages_to_sync (inbox, uids, all, TRUE, NULL, NULL, &error));
	g_assert_error (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID);
	g_clear_error (&error);

	/* Moved out of the folder the message is in */
	g_assert_true (camel_folder_transfer_messages_to_sync (all, uids, archive, TRUE, NULL, NULL, &error));
	g_assert_no_error (error);
	full_refresh (inbox);
	full_refresh (archive);
	g_assert_false (has_message (inbox, MAIL2));
	g_assert_true (has_message (archive, IN (MAIL2_BASE, ARCHIV)));

	/* Deleted in the folder the message is in */
	camel_folder_set_message_flags (all, MAIL1, CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
	g_assert_true (camel_folder_synchronize_sync (all, FALSE, NULL, &error));
	g_assert_no_error (error);
	full_refresh (inbox);
	g_assert_false (has_message (inbox, MAIL1));

	g_ptr_array_unref (uids);
	g_object_unref (archive);
	g_object_unref (inbox);
	g_object_unref (all);
}

static void
test_copy (Fixture *fixture,
	   gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture), *archive = open_folder (fixture, "Archiv");
	GPtrArray *uids = g_ptr_array_new ();
	GError *error = NULL;

	g_ptr_array_add (uids, (gpointer) MAIL1);
	g_assert_true (camel_folder_transfer_messages_to_sync (inbox, uids, archive, FALSE, NULL, NULL, &error));
	g_assert_no_error (error);

	full_refresh (inbox);
	full_refresh (archive);
	g_assert_true (has_message (inbox, MAIL1));
	g_assert_true (has_message (archive, IN (MAIL1_BASE, ARCHIV)));

	g_ptr_array_unref (uids);
	g_object_unref (archive);
	g_object_unref (inbox);
}

static void
test_delete (Fixture *fixture,
	     gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture), *trash;
	GError *error = NULL;

	/* Deleted in Evolution: into the Trash of the server */
	camel_folder_set_message_flags (inbox, MAIL1, CAMEL_MESSAGE_DELETED | CAMEL_MESSAGE_SEEN, CAMEL_MESSAGE_DELETED | CAMEL_MESSAGE_SEEN);
	g_assert_true (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (has_message (inbox, MAIL1));
	full_refresh (inbox);
	g_assert_false (has_message (inbox, MAIL1));

	trash = camel_store_get_trash_folder_sync (CAMEL_STORE (fixture->store), NULL, &error);
	g_assert_no_error (error);
	g_assert_true (camel_folder_refresh_info_sync (trash, NULL, &error));
	g_assert_true (has_message (trash, IN (MAIL1_BASE, TRASH)));

	/* Marked in the Trash, it stays until the Trash is emptied */
	camel_folder_set_message_flags (trash, IN (MAIL1_BASE, TRASH), CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
	g_assert_true (camel_folder_synchronize_sync (trash, FALSE, NULL, &error));
	g_assert_true (has_message (trash, IN (MAIL1_BASE, TRASH)));

	g_assert_true (camel_folder_expunge_sync (trash, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (has_message (trash, IN (MAIL1_BASE, TRASH)));
	full_refresh (trash);
	g_assert_false (has_message (trash, IN (MAIL1_BASE, TRASH)));

	g_object_unref (trash);
	g_object_unref (inbox);
}

static void
restore_from_trash (Fixture *fixture,
		    const gchar *uid)
{
	CamelFolder *trash;
	GError *error = NULL;

	trash = camel_store_get_trash_folder_sync (CAMEL_STORE (fixture->store), NULL, &error);
	g_assert_no_error (error);
	g_assert_true (camel_folder_refresh_info_sync (trash, NULL, &error));
	g_assert_true (has_message (trash, uid));

	camel_folder_set_message_user_flag (trash, uid, "gw-restore", TRUE);
	g_assert_true (camel_folder_synchronize_sync (trash, FALSE, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (has_message (trash, uid));
	full_refresh (trash);
	g_assert_false (has_message (trash, uid));

	g_object_unref (trash);
}

static void
test_restore (Fixture *fixture,
	      gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture), *archive = open_folder (fixture, "Archiv");
	GPtrArray *uids = g_ptr_array_new ();
	GError *error = NULL;

	/* Deleted from the Mailbox, restored: back in the Mailbox */
	camel_folder_set_message_flags (inbox, MAIL1, CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
	g_assert_true (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (has_message (inbox, MAIL1));
	restore_from_trash (fixture, IN (MAIL1_BASE, TRASH));
	full_refresh (inbox);
	g_assert_true (has_message (inbox, MAIL1));

	/* A copy deleted from a folder goes back there, the Mailbox keeps its own */
	g_ptr_array_add (uids, (gpointer) MAIL2);
	g_assert_true (camel_folder_transfer_messages_to_sync (inbox, uids, archive, FALSE, NULL, NULL, &error));
	g_assert_no_error (error);
	camel_folder_set_message_flags (archive, IN (MAIL2_BASE, ARCHIV), CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
	g_assert_true (camel_folder_synchronize_sync (archive, FALSE, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (has_message (archive, IN (MAIL2_BASE, ARCHIV)));
	restore_from_trash (fixture, IN (MAIL2_BASE, TRASH));
	full_refresh (archive);
	g_assert_true (has_message (archive, IN (MAIL2_BASE, ARCHIV)));
	full_refresh (inbox);
	g_assert_true (has_message (inbox, MAIL2));

	g_ptr_array_unref (uids);
	g_object_unref (archive);
	g_object_unref (inbox);
}

static void
test_restore_from_gone_folder (Fixture *fixture,
			       gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture), *gone;
	CamelFolderInfo *fi;
	GPtrArray *uids = g_ptr_array_new ();
	GError *error = NULL;

	/* A copy of MAIL2 and MAIL1 itself go to the Trash with their folder */
	fi = camel_store_create_folder_sync (CAMEL_STORE (fixture->store), NULL, "Weg", NULL, &error);
	g_assert_no_error (error);
	camel_folder_info_free (fi);
	gone = open_folder (fixture, "Weg");
	g_ptr_array_add (uids, (gpointer) MAIL2);
	g_assert_true (camel_folder_transfer_messages_to_sync (inbox, uids, gone, FALSE, NULL, NULL, &error));
	g_assert_no_error (error);
	uids->pdata[0] = (gpointer) MAIL1;
	g_assert_true (camel_folder_transfer_messages_to_sync (inbox, uids, gone, TRUE, NULL, NULL, &error));
	g_assert_no_error (error);
	g_object_unref (gone);
	g_assert_true (camel_store_delete_folder_sync (CAMEL_STORE (fixture->store), "Weg", NULL, &error));
	g_assert_no_error (error);
	full_refresh (inbox);
	g_assert_true (has_message (inbox, MAIL2));
	g_assert_false (has_message (inbox, MAIL1));

	/* The copy leaves the Trash, the Mailbox keeps MAIL2; MAIL1 is in no
	 * folder any more and goes into the Mailbox */
	restore_from_trash (fixture, IN (MAIL2_BASE, TRASH));
	restore_from_trash (fixture, IN (MAIL1_BASE, TRASH));
	full_refresh (inbox);
	g_assert_true (has_message (inbox, MAIL2));
	g_assert_true (has_message (inbox, MAIL1));

	g_ptr_array_unref (uids);
	g_object_unref (inbox);
}

#define JUNK "J.domain1.po1.100.0.1.0.1@17"

/* Marked junk: into the Junk Mail folder, the Internet sender onto the junk
 * list (off the trust list); marked not junk: back, onto the trust list */
static void
test_junk (Fixture *fixture,
	   gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture), *junk;
	GError *error = NULL;
	gchar *text;

	junk = camel_store_get_junk_folder_sync (CAMEL_STORE (fixture->store), NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (junk);
	g_assert_true (camel_folder_get_flags (junk) & CAMEL_FOLDER_IS_JUNK);
	g_assert_true (camel_store_get_flags (CAMEL_STORE (fixture->store)) & CAMEL_STORE_REAL_JUNK_FOLDER);

	/* As Evolution marks it */
	camel_folder_set_message_flags (inbox, MAIL2, CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_NOTJUNK | CAMEL_MESSAGE_JUNK_LEARN,
		CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_JUNK_LEARN);
	g_assert_true (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (has_message (inbox, MAIL2));
	full_refresh (inbox);
	g_assert_false (has_message (inbox, MAIL2));
	g_assert_true (camel_folder_refresh_info_sync (junk, NULL, &error));
	g_assert_true (has_message (junk, IN (MAIL2_BASE, JUNK)));
	text = mock_get_f ("/_test/junk-entries");
	g_assert_cmpstr (text, ==, "junk:bob@phantom.com,junk:spam.example");
	g_free (text);

	/* Not junk after all */
	camel_folder_set_message_flags (junk, IN (MAIL2_BASE, JUNK),
		CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_NOTJUNK | CAMEL_MESSAGE_JUNK_LEARN,
		CAMEL_MESSAGE_NOTJUNK | CAMEL_MESSAGE_JUNK_LEARN);
	g_assert_true (camel_folder_synchronize_sync (junk, FALSE, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (has_message (junk, IN (MAIL2_BASE, JUNK)));
	full_refresh (inbox);
	g_assert_true (has_message (inbox, MAIL2));
	text = mock_get_f ("/_test/junk-entries");
	g_assert_cmpstr (text, ==, "junk:spam.example,trust:bob@phantom.com");
	g_free (text);

	/* The junk test of Evolution (no "learn" flag) moves, the lists stay */
	camel_folder_set_message_flags (inbox, MAIL2, CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_NOTJUNK, CAMEL_MESSAGE_JUNK);
	g_assert_true (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error));
	g_assert_false (has_message (inbox, MAIL2));
	text = mock_get_f ("/_test/junk-entries");
	g_assert_cmpstr (text, ==, "junk:spam.example,trust:bob@phantom.com");
	g_free (text);

	g_object_unref (junk);
	g_object_unref (inbox);
}

static void
test_prune (Fixture *fixture,
	    gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture), *trash;
	GError *error = NULL;

	/* Deleted and expunged at once: pruned, not into the Trash */
	camel_folder_set_message_flags (inbox, MAIL1, CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
	g_assert_true (camel_folder_expunge_sync (inbox, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (has_message (inbox, MAIL1));
	full_refresh (inbox);
	g_assert_false (has_message (inbox, MAIL1));

	trash = camel_store_get_trash_folder_sync (CAMEL_STORE (fixture->store), NULL, &error);
	g_assert_true (camel_folder_refresh_info_sync (trash, NULL, &error));
	g_assert_false (has_message (trash, IN (MAIL1_BASE, TRASH)));

	g_object_unref (trash);
	g_object_unref (inbox);
}

static void
test_empty_trash_keeps_linked (Fixture *fixture,
			       gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture), *archive = open_folder (fixture, "Archiv"), *trash;
	GPtrArray *uids = g_ptr_array_new ();
	GError *error = NULL;

	/* A copy deleted in one folder goes to the Trash while the mail stays
	 * in the Mailbox. Purging it there would delete it from the Mailbox too;
	 * emptying the Trash only takes it out of the Trash. */
	g_ptr_array_add (uids, (gpointer) MAIL2);
	g_assert_true (camel_folder_transfer_messages_to_sync (inbox, uids, archive, FALSE, NULL, NULL, &error));
	g_assert_no_error (error);
	camel_folder_set_message_flags (archive, IN (MAIL2_BASE, ARCHIV), CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
	g_assert_true (camel_folder_synchronize_sync (archive, FALSE, NULL, &error));
	g_assert_no_error (error);

	trash = camel_store_get_trash_folder_sync (CAMEL_STORE (fixture->store), NULL, &error);
	g_assert_true (camel_folder_refresh_info_sync (trash, NULL, &error));
	g_assert_true (has_message (trash, IN (MAIL2_BASE, TRASH)));

	camel_folder_set_message_flags (trash, IN (MAIL2_BASE, TRASH), CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
	g_assert_true (camel_folder_expunge_sync (trash, NULL, &error));
	g_assert_no_error (error);

	/* Out of the Trash, not purged: still in the Mailbox */
	g_assert_false (has_message (trash, IN (MAIL2_BASE, TRASH)));
	full_refresh (trash);
	g_assert_false (has_message (trash, IN (MAIL2_BASE, TRASH)));
	full_refresh (inbox);
	g_assert_true (has_message (inbox, MAIL2));
	full_refresh (archive);
	g_assert_false (has_message (archive, IN (MAIL2_BASE, ARCHIV)));

	g_ptr_array_unref (uids);
	g_object_unref (trash);
	g_object_unref (archive);
	g_object_unref (inbox);
}

static void
test_read_only_writes_nothing (Fixture *fixture,
			       gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture), *archive = open_folder (fixture, "Archiv");
	GPtrArray *uids = g_ptr_array_new ();
	CamelSettings *settings;
	GError *error = NULL;

	settings = camel_service_ref_settings (fixture->store);
	g_object_set (settings, "read-only", TRUE, NULL);
	g_object_unref (settings);

	g_ptr_array_add (uids, (gpointer) MAIL1);
	g_assert_false (camel_folder_transfer_messages_to_sync (inbox, uids, archive, TRUE, NULL, NULL, &error));
	g_assert_error (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID);
	g_clear_error (&error);

	camel_folder_set_message_flags (inbox, MAIL1, CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
	g_assert_true (camel_folder_synchronize_sync (inbox, TRUE, NULL, &error));
	g_assert_true (camel_folder_expunge_sync (inbox, NULL, &error));
	full_refresh (inbox);
	full_refresh (archive);
	g_assert_true (has_message (inbox, MAIL1));
	g_assert_false (has_message (archive, IN (MAIL1_BASE, ARCHIV)));

	g_ptr_array_unref (uids);
	g_object_unref (archive);
	g_object_unref (inbox);
}

static CamelFolderInfo *
server_folders (Fixture *fixture)
{
	GError *error = NULL;
	CamelFolderInfo *tree;

	/* REFRESH: the folder list from the server, not from the summary */
	tree = camel_store_get_folder_info_sync (CAMEL_STORE (fixture->store), NULL,
		CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_REFRESH, NULL, &error);
	g_assert_no_error (error);

	return tree;
}

static void
test_folders (Fixture *fixture,
	      gconstpointer data)
{
	CamelStore *store = CAMEL_STORE (fixture->store);
	CamelFolderInfo *fi, *tree;
	CamelFolder *folder;
	GError *error = NULL;

	/* Create below a folder and at the top */
	fi = camel_store_create_folder_sync (store, "Archiv", "Projekte", NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (fi->full_name, ==, "Archiv/Projekte");
	camel_folder_info_free (fi);
	fi = camel_store_create_folder_sync (store, NULL, "Oben", NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (fi->full_name, ==, "Oben");
	camel_folder_info_free (fi);

	g_assert_null (camel_store_create_folder_sync (store, NULL, "a/b", NULL, &error));
	g_assert_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID);
	g_clear_error (&error);

	tree = server_folders (fixture);
	g_assert_nonnull (find_info (tree, "Archiv/Projekte"));
	g_assert_nonnull (find_info (tree, "Oben"));
	camel_folder_info_free (tree);

	/* Rename, then move into another folder */
	g_assert_true (camel_store_rename_folder_sync (store, "Archiv/Projekte", "Archiv/Kunden", NULL, &error));
	g_assert_no_error (error);
	g_assert_true (camel_store_rename_folder_sync (store, "Archiv/Kunden", "Oben/Kunden", NULL, &error));
	g_assert_no_error (error);
	tree = server_folders (fixture);
	g_assert_null (find_info (tree, "Archiv/Projekte"));
	g_assert_null (find_info (tree, "Archiv/Kunden"));
	g_assert_nonnull (find_info (tree, "Oben/Kunden"));
	camel_folder_info_free (tree);

	/* A renamed folder keeps its messages */
	folder = open_folder (fixture, "Archiv/Amazon");
	g_assert_cmpint (camel_folder_get_message_count (folder), ==, 1);
	g_object_unref (folder);
	g_assert_true (camel_store_rename_folder_sync (store, "Archiv/Amazon", "Archiv/Bestellungen", NULL, &error));
	g_assert_no_error (error);
	g_assert_true (camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (store), FALSE, NULL, &error));
	folder = camel_store_get_folder_sync (store, "Archiv/Bestellungen", 0, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpint (camel_folder_get_message_count (folder), ==, 1);
	g_object_unref (folder);
	g_assert_true (camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (store), TRUE, NULL, &error));

	/* Delete with its subfolder */
	g_assert_true (camel_store_delete_folder_sync (store, "Oben", NULL, &error));
	g_assert_no_error (error);
	tree = server_folders (fixture);
	g_assert_null (find_info (tree, "Oben"));
	g_assert_null (find_info (tree, "Oben/Kunden"));
	camel_folder_info_free (tree);

	/* System folders stay */
	g_assert_false (camel_store_rename_folder_sync (store, "Mailbox", "Eingang", NULL, &error));
	g_assert_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID);
	g_clear_error (&error);
	g_assert_false (camel_store_delete_folder_sync (store, "Trash", NULL, &error));
	g_assert_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID);
	g_clear_error (&error);
}

static void
test_folders_read_only (Fixture *fixture,
			gconstpointer data)
{
	CamelSettings *settings = camel_service_ref_settings (fixture->store);
	GError *error = NULL;
	CamelFolderInfo *tree;

	g_object_set (settings, "read-only", TRUE, NULL);
	g_object_unref (settings);

	g_assert_null (camel_store_create_folder_sync (CAMEL_STORE (fixture->store), "Archiv", "Neu", NULL, &error));
	g_assert_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID);
	g_clear_error (&error);
	g_assert_false (camel_store_rename_folder_sync (CAMEL_STORE (fixture->store), "Archiv", "Archiv2", NULL, &error));
	g_assert_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID);
	g_clear_error (&error);
	g_assert_false (camel_store_delete_folder_sync (CAMEL_STORE (fixture->store), "Archiv", NULL, &error));
	g_assert_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID);
	g_clear_error (&error);

	tree = server_folders (fixture);
	g_assert_nonnull (find_info (tree, "Archiv"));
	g_assert_null (find_info (tree, "Archiv/Neu"));
	camel_folder_info_free (tree);
}

static CamelMimeMessage *
compose (const gchar *raw)
{
	CamelMimeMessage *message = camel_mime_message_new ();
	GInputStream *in = g_memory_input_stream_new_from_data (raw, strlen (raw), NULL);

	g_assert_true (camel_data_wrapper_construct_from_input_stream_sync (CAMEL_DATA_WRAPPER (message), in, NULL, NULL));
	g_object_unref (in);

	return message;
}

static CamelService *
new_transport (Fixture *fixture)
{
	GError *error = NULL;
	CamelService *transport;
	CamelSettings *settings;

	transport = camel_session_add_service (fixture->session, "gw-transport", "groupwise", CAMEL_PROVIDER_TRANSPORT, &error);
	g_assert_no_error (error);
	g_assert_true (CAMEL_IS_TRANSPORT (transport));

	settings = camel_service_ref_settings (transport);
	g_object_set (settings, "host", "127.0.0.1", "port", mock_port, "user", "u1",
		"security-method", CAMEL_NETWORK_SECURITY_METHOD_NONE, NULL);
	g_object_unref (settings);

	return transport;
}

static void
test_send (Fixture *fixture,
	   gconstpointer data)
{
	CamelService *transport = new_transport (fixture);
	CamelMimeMessage *message = compose (
		"From: User One <u1@phantom.com>\r\nTo: Bob <bob@phantom.com>\r\nSubject: Aus Evolution\r\n\r\nHallo Bob\r\n");
	CamelInternetAddress *from = camel_internet_address_new (), *recipients = camel_internet_address_new ();
	CamelFolder *sent;
	gboolean saved = FALSE;
	GError *error = NULL;
	gchar *xml;

	camel_internet_address_add (from, "User One", "u1@phantom.com");
	camel_internet_address_add (recipients, "Bob", "bob@phantom.com");
	camel_internet_address_add (recipients, NULL, "blind@phantom.com");

	g_assert_true (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), message, CAMEL_ADDRESS (from),
		CAMEL_ADDRESS (recipients), &saved, NULL, &error));
	g_assert_no_error (error);
	/* The POA keeps the copy: Evolution must not store a second one */
	g_assert_true (saved);

	xml = mock_get ("/_test/last-send");
	g_assert_nonnull (strstr (xml, "<subject>Aus Evolution</subject>"));
	g_assert_nonnull (strstr (xml, "<email>bob@phantom.com</email><distType>TO</distType>"));
	g_assert_nonnull (strstr (xml, "<email>blind@phantom.com</email><distType>BC</distType>"));
	g_assert_null (strstr (xml, "<source>draft</source>"));
	g_free (xml);

	sent = open_folder (fixture, "Sent Items");
	g_assert_true (has_message (sent, "SENT1@1:S.domain1.po1.100.0.1.0.1@30"));
	g_object_unref (sent);

	g_object_unref (recipients);
	g_object_unref (from);
	g_object_unref (message);
	g_object_unref (transport);
}

static void
test_send_transport_without_settings (Fixture *fixture,
				      gconstpointer data)
{
	/* As Evolution makes it when the transport source has no Authentication extension */
	CamelService *transport = camel_session_add_service (fixture->session, "gw-transport", "groupwise",
		CAMEL_PROVIDER_TRANSPORT, NULL);
	CamelMimeMessage *message = compose ("To: bob@phantom.com\r\nSubject: Ohne Einstellungen\r\n\r\nText\r\n");
	CamelInternetAddress *from = camel_internet_address_new (), *recipients = camel_internet_address_new ();
	gboolean saved = FALSE;
	GError *error = NULL;
	gchar *xml;

	camel_internet_address_add (from, NULL, "u1@phantom.com");
	camel_internet_address_add (recipients, NULL, "bob@phantom.com");
	g_assert_true (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), message, CAMEL_ADDRESS (from),
		CAMEL_ADDRESS (recipients), &saved, NULL, &error));
	g_assert_no_error (error);

	xml = mock_get ("/_test/last-send");
	g_assert_nonnull (strstr (xml, "<subject>Ohne Einstellungen</subject>"));
	g_free (xml);

	g_object_unref (recipients);
	g_object_unref (from);
	g_object_unref (message);
	g_object_unref (transport);
}

/* Two GroupWise accounts: a transport sends through its own one only */
static void
test_send_right_account (Fixture *fixture,
			 gconstpointer data)
{
	CamelService *other_store, *transport;
	CamelSettings *settings;
	CamelMimeMessage *message = compose ("To: bob@phantom.com\r\nSubject: Welches Konto\r\n\r\nText\r\n");
	CamelInternetAddress *from = camel_internet_address_new (), *recipients = camel_internet_address_new ();
	gboolean saved = FALSE;
	GError *error = NULL;
	gchar *xml;

	other_store = camel_session_add_service (fixture->session, "gw-other", "groupwise", CAMEL_PROVIDER_STORE, &error);
	g_assert_no_error (error);
	settings = camel_service_ref_settings (other_store);
	g_object_set (settings, "host", "127.0.0.1", "port", mock_port, "user", "anna",
		"security-method", CAMEL_NETWORK_SECURITY_METHOD_NONE, NULL);
	g_object_unref (settings);
	camel_service_set_password (other_store, "secret");

	camel_internet_address_add (from, NULL, "anna@phantom.com");
	camel_internet_address_add (recipients, NULL, "bob@phantom.com");

	/* The second account cannot log in (unknown to the mock): its message
	 * must fail, not go out from the first account */
	transport = new_transport (fixture);
	settings = camel_service_ref_settings (transport);
	g_object_set (settings, "user", "anna", NULL);
	g_object_unref (settings);
	g_assert_false (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), message, CAMEL_ADDRESS (from),
		CAMEL_ADDRESS (recipients), &saved, NULL, &error));
	g_assert_nonnull (error);
	g_clear_error (&error);
	xml = mock_get ("/_test/last-send");
	g_assert_true (!xml || !*xml);
	g_free (xml);

	/* Without host and user it cannot choose between two accounts either */
	settings = camel_service_ref_settings (transport);
	g_object_set (settings, "host", "", "user", "", NULL);
	g_object_unref (settings);
	g_assert_false (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), message, CAMEL_ADDRESS (from),
		CAMEL_ADDRESS (recipients), &saved, NULL, &error));
	g_assert_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE);
	g_assert_nonnull (strstr (error->message, "was not found"));
	g_clear_error (&error);
	xml = mock_get ("/_test/last-send");
	g_assert_true (!xml || !*xml);
	g_free (xml);

	/* The first account's transport sends through the first account */
	settings = camel_service_ref_settings (transport);
	g_object_set (settings, "host", "127.0.0.1", "user", "u1", NULL);
	g_object_unref (settings);
	g_assert_true (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), message, CAMEL_ADDRESS (from),
		CAMEL_ADDRESS (recipients), &saved, NULL, &error));
	g_assert_no_error (error);

	g_object_unref (transport);
	g_object_unref (other_store);
	g_object_unref (recipients);
	g_object_unref (from);
	g_object_unref (message);
}

static void
test_send_read_only (Fixture *fixture,
		     gconstpointer data)
{
	CamelService *transport = new_transport (fixture);
	CamelMimeMessage *message = compose ("To: bob@phantom.com\r\nSubject: Nein\r\n\r\nText\r\n");
	CamelInternetAddress *from = camel_internet_address_new (), *recipients = camel_internet_address_new ();
	CamelSettings *settings = camel_service_ref_settings (fixture->store);
	gboolean saved = FALSE;
	GError *error = NULL;
	gchar *xml;

	g_object_set (settings, "read-only", TRUE, NULL);
	g_object_unref (settings);
	camel_internet_address_add (from, NULL, "u1@phantom.com");
	camel_internet_address_add (recipients, NULL, "bob@phantom.com");

	g_assert_false (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), message, CAMEL_ADDRESS (from),
		CAMEL_ADDRESS (recipients), &saved, NULL, &error));
	g_assert_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_INVALID);
	g_clear_error (&error);

	/* Nothing sent (an empty answer of the mock) */
	xml = mock_get ("/_test/last-send");
	g_assert_true (!xml || !*xml);
	g_free (xml);

	g_object_unref (recipients);
	g_object_unref (from);
	g_object_unref (message);
	g_object_unref (transport);
}

static CamelService *
new_proxy_store (Fixture *fixture,
		 const gchar *uid,
		 const gchar *proxy)
{
	CamelService *store;
	CamelSettings *settings;
	GError *error = NULL;

	store = camel_session_add_service (fixture->session, uid, "groupwise", CAMEL_PROVIDER_STORE, &error);
	g_assert_no_error (error);
	settings = camel_service_ref_settings (store);
	g_object_set (settings, "host", "127.0.0.1", "port", mock_port, "user", "u1",
		"security-method", CAMEL_NETWORK_SECURITY_METHOD_NONE, "proxy", proxy, NULL);
	g_object_unref (settings);
	camel_service_set_password (store, "secret");

	return store;
}

/* A proxy account: the own login in the other user's mailbox, with the
 * rights granted there */
static void
test_proxy_store (Fixture *fixture,
		  gconstpointer data)
{
	CamelService *reader = new_proxy_store (fixture, "gw-proxy-anna", "anna.mueller@example.com");
	CamelService *none = new_proxy_store (fixture, "gw-proxy-max", "max.weber@example.com");
	CamelFolder *folder;
	GError *error = NULL;
	gchar *name;

	g_assert_true (camel_service_connect_sync (reader, NULL, &error));
	g_assert_no_error (error);
	name = camel_service_get_name (reader, FALSE);
	g_assert_nonnull (strstr (name, "anna.mueller@example.com"));
	g_free (name);

	/* Without the right to write mail, the folders are read-only */
	folder = camel_store_get_folder_sync (CAMEL_STORE (reader), "Mailbox", 0, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (folder);
	g_assert_false (camel_store_create_folder_sync (CAMEL_STORE (reader), "Mailbox", "Neu", NULL, &error) != NULL);
	g_assert_nonnull (error);
	g_clear_error (&error);
	g_object_unref (folder);

	/* Without the right to read mail, no account */
	g_assert_false (camel_service_connect_sync (none, NULL, &error));
	g_assert_nonnull (error);
	g_assert_nonnull (strstr (error->message, "max.weber@example.com"));
	g_clear_error (&error);

	camel_service_disconnect_sync (reader, TRUE, NULL, NULL);
	g_object_unref (none);
	g_object_unref (reader);
}

/* A proxy account sends through its proxy session; the own account's
 * transport keeps to the own session although there are two stores now */
static void
test_proxy_send (Fixture *fixture,
		 gconstpointer data)
{
	CamelService *proxy_store = new_proxy_store (fixture, "gw-proxy-chef", "chef@example.com");
	CamelService *transport = new_transport (fixture), *proxy_transport;
	CamelMimeMessage *message = compose ("To: bob@phantom.com\r\nSubject: Im Auftrag\r\n\r\nText\r\n");
	CamelInternetAddress *from = camel_internet_address_new (), *recipients = camel_internet_address_new ();
	CamelSettings *settings;
	gboolean saved = FALSE;
	GError *error = NULL;
	gchar *session;

	g_assert_true (camel_service_connect_sync (proxy_store, NULL, &error));
	g_assert_no_error (error);

	proxy_transport = camel_session_add_service (fixture->session, "gw-proxy-transport", "groupwise",
		CAMEL_PROVIDER_TRANSPORT, &error);
	g_assert_no_error (error);
	settings = camel_service_ref_settings (proxy_transport);
	g_object_set (settings, "host", "127.0.0.1", "port", mock_port, "user", "u1",
		"security-method", CAMEL_NETWORK_SECURITY_METHOD_NONE, "proxy", "chef@example.com", NULL);
	g_object_unref (settings);

	camel_internet_address_add (from, NULL, "chef@phantom.com");
	camel_internet_address_add (recipients, NULL, "bob@phantom.com");

	g_assert_true (camel_transport_send_to_sync (CAMEL_TRANSPORT (proxy_transport), message, CAMEL_ADDRESS (from),
		CAMEL_ADDRESS (recipients), &saved, NULL, &error));
	g_assert_no_error (error);
	session = mock_get ("/_test/last-send-session");
	g_assert_cmpstr (session, ==, "PROXY9");
	g_free (session);

	g_assert_true (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), message, CAMEL_ADDRESS (from),
		CAMEL_ADDRESS (recipients), &saved, NULL, &error));
	g_assert_no_error (error);
	session = mock_get ("/_test/last-send-session");
	g_assert_cmpstr (session, ==, "SESSION123");
	g_free (session);

	/* Without host and user, the own transport still finds the own account */
	settings = camel_service_ref_settings (transport);
	g_object_set (settings, "host", "", "user", "", NULL);
	g_object_unref (settings);
	g_assert_true (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), message, CAMEL_ADDRESS (from),
		CAMEL_ADDRESS (recipients), &saved, NULL, &error));
	g_assert_no_error (error);
	session = mock_get ("/_test/last-send-session");
	g_assert_cmpstr (session, ==, "SESSION123");
	g_free (session);

	camel_service_disconnect_sync (proxy_store, TRUE, NULL, NULL);
	g_object_unref (proxy_transport);
	g_object_unref (transport);
	g_object_unref (proxy_store);
	g_object_unref (recipients);
	g_object_unref (from);
	g_object_unref (message);
}

static void
test_save_draft (Fixture *fixture,
		 gconstpointer data)
{
	CamelFolder *drafts = open_folder (fixture, "Work In Progress"), *inbox;
	CamelMimeMessage *message = compose ("To: bob@phantom.com\r\nSubject: Halb fertig\r\n\r\nEntwurf\r\n");
	CamelMessageInfo *info;
	GError *error = NULL;
	gchar *uid = NULL, *xml;

	g_assert_true (camel_folder_append_message_sync (drafts, message, NULL, &uid, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpstr (uid, ==, "SENT1@1:" WIP);

	xml = mock_get ("/_test/last-send");
	g_assert_nonnull (strstr (xml, "<source>draft</source>"));
	g_free (xml);

	/* In the drafts folder, not in the Mailbox where the POA created it */
	full_refresh (drafts);
	info = camel_folder_get_message_info (drafts, uid);
	g_assert_nonnull (info);
	g_assert_cmpstr (camel_message_info_get_subject (info), ==, "Halb fertig");
	g_object_unref (info);
	inbox = open_inbox (fixture);
	g_assert_false (has_message (inbox, "SENT1@1:" INBOX));
	g_object_unref (inbox);

	/* Readable without a download */
	g_assert_nonnull (camel_folder_get_message_cached (drafts, uid, NULL));

	g_free (uid);
	g_object_unref (message);
	g_object_unref (drafts);
}

static void
test_append_elsewhere (Fixture *fixture,
		       gconstpointer data)
{
	CamelFolder *inbox = open_inbox (fixture);
	CamelMimeMessage *message = compose ("To: bob@phantom.com\r\nSubject: x\r\n\r\nx\r\n");
	GError *error = NULL;

	g_assert_false (camel_folder_append_message_sync (inbox, message, NULL, NULL, NULL, &error));
	g_assert_error (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID);
	g_clear_error (&error);

	g_object_unref (message);
	g_object_unref (inbox);
}

static void
test_changes (Fixture *fixture,
	      gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	GError *error = NULL;
	CamelMessageInfo *info;

	mock_do ("/_test/hide?id=%s", MAIL1);
	mock_do ("/_test/new-draft");

	/* Deletions show up in a full scan only */
	g_setenv ("GROUPWISE_FULL_SCAN_SECONDS", "0", TRUE);
	g_assert_true (camel_folder_refresh_info_sync (folder, NULL, &error));
	g_unsetenv ("GROUPWISE_FULL_SCAN_SECONDS");
	g_assert_no_error (error);

	g_assert_cmpint (camel_folder_get_message_count (folder), ==, 2);
	info = camel_folder_get_message_info (folder, MAIL1);
	g_assert_null (info);

	info = camel_folder_get_message_info (folder, "DRAFT1");
	g_assert_nonnull (info);
	g_assert_cmpstr (camel_message_info_get_subject (info), ==, "My draft");
	g_assert_true (camel_message_info_get_flags (info) & CAMEL_MESSAGE_DRAFT);
	g_object_unref (info);

	g_object_unref (folder);
}

static gpointer
cancel_thread (gpointer cancellable)
{
	g_usleep (200 * 1000);
	g_cancellable_cancel (cancellable);

	return NULL;
}

static void
test_cancelled (Fixture *fixture,
		gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	GCancellable *cancellable = g_cancellable_new ();
	GError *error = NULL;

	/* The user clicked on while the message was on its way: an error, not
	 * just a failure (Camel checks the cancellable only before the call) */
	mock_do ("/_test/delay?action=getItem&seconds=1");
	g_thread_unref (g_thread_new ("cancel", cancel_thread, cancellable));
	g_assert_null (camel_folder_get_message_sync (folder, MAIL1, cancellable, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error (&error);

	g_object_unref (cancellable);
	g_object_unref (folder);
}

static void
test_original_unavailable (Fixture *fixture,
			   gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	GError *error = NULL;
	CamelMimeMessage *message;

	/* Falls back to the MIME the POA builds */
	mock_do ("/_test/break-mime822");
	message = camel_folder_get_message_sync (folder, MAIL2, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (message);
	g_assert_cmpstr (camel_mime_message_get_subject (message), ==, "Test message");
	g_assert_null (camel_medium_get_header (CAMEL_MEDIUM (message), "X-Original-MIME"));
	g_object_unref (message);

	g_object_unref (folder);
}

static void
test_summary_persists (Fixture *fixture,
		       gconstpointer data)
{
	CamelFolder *folder = open_inbox (fixture);
	GError *error = NULL;

	g_object_unref (folder);

	/* Offline and reopened: folders and messages known from the last session */
	g_assert_true (camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (fixture->store), FALSE, NULL, &error));
	folder = camel_store_get_folder_sync (CAMEL_STORE (fixture->store), "Archiv/Amazon", 0, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (folder);
	g_object_unref (folder);

	folder = camel_store_get_folder_sync (CAMEL_STORE (fixture->store), "Mailbox", 0, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpint (camel_folder_get_message_count (folder), ==, 2);
	g_object_unref (folder);

	g_assert_null (camel_store_get_folder_sync (CAMEL_STORE (fixture->store), "Calendar", 0, NULL, &error));
	g_assert_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER);
	g_clear_error (&error);
}

int
main (int argc,
      char **argv)
{
	GError *error = NULL;
	const gchar *port;

	g_test_init (&argc, &argv, NULL);
	/* Evolution's labels only in memory: the user's settings stay untouched */
	g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

	port = g_getenv ("GW_MOCK_PORT");
	if (!port || argc < 2) {
		g_printerr ("Usage: run-with-mock.py test-camel MODULE-PATH\n");
		return 77;
	}
	mock_port = (guint16) atoi (port);
	tls_port = (guint16) atoi (g_getenv ("GW_MOCK_TLS_PORT"));
	module_path = argv[1];

	g_log_set_always_fatal (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);
	/* No CA of the user's own configuration in the tests */
	g_setenv ("GROUPWISE_CA_DIR", "/nonexistent", TRUE);

	camel_init (NULL, FALSE);
	camel_provider_init ();
	if (!camel_provider_load (module_path, &error))
		g_error ("Cannot load %s: %s", module_path, error->message);

	g_test_add_func ("/camel/wrong-password", test_wrong_password);
	g_test_add_func ("/camel/untrusted-certificate", test_untrusted_certificate);
	g_test_add_func ("/camel/certificate-kept", test_certificate_kept);
	g_test_add ("/camel/folder-info", Fixture, NULL, fixture_setup, test_folder_info, fixture_teardown);
	g_test_add ("/camel/refresh", Fixture, NULL, fixture_setup, test_refresh, fixture_teardown);
	g_test_add ("/camel/get-message", Fixture, NULL, fixture_setup, test_get_message, fixture_teardown);
	g_test_add ("/camel/summary-persists", Fixture, NULL, fixture_setup, test_summary_persists, fixture_teardown);
	/* These change the state of the mock; keep them last */
	g_test_add ("/camel/flags", Fixture, NULL, fixture_setup, test_flags, fixture_teardown);
	g_test_add ("/camel/flag-merge", Fixture, NULL, fixture_setup, test_flag_merge, fixture_teardown);
	g_test_add ("/camel/read-only", Fixture, NULL, fixture_setup, test_read_only, fixture_teardown);
	g_test_add ("/camel/recent", Fixture, NULL, fixture_setup, test_recent, fixture_teardown);
	g_test_add ("/camel/cancelled", Fixture, NULL, fixture_setup, test_cancelled, fixture_teardown);
	g_test_add ("/camel/changes", Fixture, NULL, fixture_setup, test_changes, fixture_teardown);
	g_test_add ("/camel/quick-check", Fixture, NULL, fixture_setup, test_quick_check, fixture_teardown);
	g_test_add ("/camel/answered-invitation", Fixture, NULL, fixture_setup, test_answered_invitation, fixture_teardown);
	g_test_add ("/camel/changed-subject", Fixture, NULL, fixture_setup, test_changed_subject, fixture_teardown);
	g_test_add ("/camel/labels", Fixture, NULL, fixture_setup, test_labels, fixture_teardown);
	g_test_add ("/camel/search-folder", Fixture, NULL, fixture_setup, test_search_folder, fixture_teardown);
	g_test_add ("/camel/move", Fixture, NULL, fixture_setup, test_move, fixture_teardown);
	g_test_add ("/camel/copy", Fixture, NULL, fixture_setup, test_copy, fixture_teardown);
	g_test_add ("/camel/delete", Fixture, NULL, fixture_setup, test_delete, fixture_teardown);
	g_test_add ("/camel/prune", Fixture, NULL, fixture_setup, test_prune, fixture_teardown);
	g_test_add ("/camel/empty-trash-keeps-linked", Fixture, NULL, fixture_setup, test_empty_trash_keeps_linked, fixture_teardown);
	g_test_add ("/camel/restore", Fixture, NULL, fixture_setup, test_restore, fixture_teardown);
	g_test_add ("/camel/junk", Fixture, NULL, fixture_setup, test_junk, fixture_teardown);
	g_test_add ("/camel/restore-from-gone-folder", Fixture, NULL, fixture_setup, test_restore_from_gone_folder, fixture_teardown);
	g_test_add ("/camel/read-only-writes-nothing", Fixture, NULL, fixture_setup, test_read_only_writes_nothing, fixture_teardown);
	g_test_add ("/camel/folders", Fixture, NULL, fixture_setup, test_folders, fixture_teardown);
	g_test_add ("/camel/folders-read-only", Fixture, NULL, fixture_setup, test_folders_read_only, fixture_teardown);
	g_test_add ("/camel/send", Fixture, NULL, fixture_setup, test_send, fixture_teardown);
	g_test_add ("/camel/proxy-store", Fixture, NULL, fixture_setup, test_proxy_store, fixture_teardown);
	g_test_add ("/camel/proxy-send", Fixture, NULL, fixture_setup, test_proxy_send, fixture_teardown);
	g_test_add ("/camel/send-read-only", Fixture, NULL, fixture_setup, test_send_read_only, fixture_teardown);
	g_test_add ("/camel/send-right-account", Fixture, NULL, fixture_setup, test_send_right_account, fixture_teardown);
	g_test_add ("/camel/send-transport-without-settings", Fixture, NULL, fixture_setup, test_send_transport_without_settings, fixture_teardown);
	g_test_add ("/camel/save-draft", Fixture, NULL, fixture_setup, test_save_draft, fixture_teardown);
	g_test_add ("/camel/append-elsewhere", Fixture, NULL, fixture_setup, test_append_elsewhere, fixture_teardown);
	g_test_add ("/camel/original-unavailable", Fixture, NULL, fixture_setup, test_original_unavailable, fixture_teardown);

	return g_test_run ();
}

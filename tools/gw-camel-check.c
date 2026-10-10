/*
 * gw-camel-check.c: runs the groupwise Camel provider against a real POA,
 * the way Evolution uses it, without changing the mailbox.
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
 * Usage: GW_HOST=.. GW_USER=.. GW_PASSWORD=.. gw-camel-check MODULE-PATH [--insecure] [--ssl] [--uid ID] [DATA-DIR]
 * With DATA-DIR the cache is kept, so a second run shows the incremental refresh.
 * --uid fetches that message of the Mailbox instead of the newest one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>
#include <locale.h>
#include <libintl.h>

#include <camel/camel.h>

typedef struct {
	CamelSession parent;
	gboolean insecure;
} CheckSession;

typedef struct {
	CamelSessionClass parent_class;
} CheckSessionClass;

static GType check_session_get_type (void);

G_DEFINE_TYPE (CheckSession, check_session, CAMEL_TYPE_SESSION)

static gboolean
check_session_authenticate_sync (CamelSession *session,
				 CamelService *service,
				 const gchar *mechanism,
				 GCancellable *cancellable,
				 GError **error)
{
	CamelAuthenticationResult result = camel_service_authenticate_sync (service, mechanism, cancellable, error);

	if (result == CAMEL_AUTHENTICATION_REJECTED)
		g_set_error_literal (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE, "Password rejected");

	return result == CAMEL_AUTHENTICATION_ACCEPTED;
}

static CamelCertTrust
check_session_trust_prompt (CamelSession *session,
			    CamelService *service,
			    GTlsCertificate *certificate,
			    GTlsCertificateFlags errors)
{
	gchar *subject = NULL, *issuer = NULL;

	g_object_get (certificate, "subject-name", &subject, "issuer-name", &issuer, NULL);
	printf ("trust prompt: %s (issuer %s), errors 0x%x -> %s\n", subject, issuer, errors,
		((CheckSession *) session)->insecure ? "accepted (--insecure)" : "refused");
	g_free (subject);
	g_free (issuer);

	return ((CheckSession *) session)->insecure ? CAMEL_CERT_TRUST_TEMPORARY : CAMEL_CERT_TRUST_NEVER;
}

static void
check_session_class_init (CheckSessionClass *class)
{
	CAMEL_SESSION_CLASS (class)->authenticate_sync = check_session_authenticate_sync;
	CAMEL_SESSION_CLASS (class)->trust_prompt = check_session_trust_prompt;
}

static void
check_session_init (CheckSession *session)
{
}

static void
print_tree (CamelFolderInfo *fi,
	    gint depth)
{
	for (; fi; fi = fi->next) {
		const gchar *type;

		switch (fi->flags & CAMEL_FOLDER_TYPE_MASK) {
		case CAMEL_FOLDER_TYPE_INBOX: type = "inbox"; break;
		case CAMEL_FOLDER_TYPE_TRASH: type = "trash"; break;
		case CAMEL_FOLDER_TYPE_JUNK: type = "junk"; break;
		case CAMEL_FOLDER_TYPE_SENT: type = "sent"; break;
		case CAMEL_FOLDER_TYPE_DRAFTS: type = "drafts"; break;
		default: type = ""; break;
		}
		if (fi->flags & CAMEL_FOLDER_VIRTUAL)
			type = "virtual";

		printf ("  %*s%-*s %-7s total %4d unread %3d\n", depth * 2, "", 34 - depth * 2, fi->display_name,
			type, fi->total, fi->unread);
		print_tree (fi->child, depth + 1);
	}
}

static gint
compare_date (gconstpointer a,
	      gconstpointer b)
{
	gint64 da = camel_message_info_get_date_received (*(CamelMessageInfo **) a);
	gint64 db = camel_message_info_get_date_received (*(CamelMessageInfo **) b);

	return da < db ? 1 : da > db ? -1 : 0;
}

static void
print_part (CamelDataWrapper *wrapper,
	    gint depth)
{
	CamelContentType *ct = camel_data_wrapper_get_mime_type_field (wrapper);
	gchar *type = camel_content_type_simple (ct);

	printf ("    %*s%s\n", depth * 2, "", type);
	g_free (type);

	if (CAMEL_IS_MULTIPART (wrapper)) {
		guint ii;

		for (ii = 0; ii < camel_multipart_get_number (CAMEL_MULTIPART (wrapper)); ii++)
			print_part (CAMEL_DATA_WRAPPER (camel_multipart_get_part (CAMEL_MULTIPART (wrapper), ii)), depth + 1);
	} else if (CAMEL_IS_MIME_PART (wrapper)) {
		print_part (camel_medium_get_content (CAMEL_MEDIUM (wrapper)), depth + 1);
	}
}

static void
events_changed_cb (CamelFolder *folder,
		   CamelFolderChangeInfo *changes,
		   gpointer user_data)
{
	GDateTime *now = g_date_time_new_now_local ();
	gchar *when = g_date_time_format (now, "%H:%M:%S");

	printf ("events: %s Mailbox changed: %u added, %u changed, %u removed; %u unread of %u\n", when,
		changes->uid_added->len, changes->uid_changed->len, changes->uid_removed->len,
		camel_folder_summary_get_unread_count (camel_folder_get_folder_summary (folder)),
		camel_folder_summary_count (camel_folder_get_folder_summary (folder)));
	fflush (stdout);
	g_free (when);
	g_date_time_unref (now);
}

static gboolean
events_stop_cb (gpointer loop)
{
	g_main_loop_quit (loop);

	return G_SOURCE_REMOVE;
}

int
main (int argc,
      char **argv)
{
	const gchar *module = NULL, *data_dir = NULL, *uid = NULL;
	gboolean insecure = FALSE, ssl = FALSE;
	GError *error = NULL;
	CamelSession *session;
	CamelService *store;
	CamelSettings *settings;
	CamelFolderInfo *tree;
	CamelFolder *inbox;
	GPtrArray *uids, *infos;
	gchar *tmp = NULL;
	gint64 start;
	gint ii;

	for (ii = 1; ii < argc; ii++) {
		if (g_str_equal (argv[ii], "--insecure"))
			insecure = TRUE;
		else if (g_str_equal (argv[ii], "--ssl"))
			ssl = TRUE;
		else if (g_str_equal (argv[ii], "--uid") && ii + 1 < argc)
			uid = argv[++ii];
		else if (!module)
			module = argv[ii];
		else
			data_dir = argv[ii];
	}
	if (!module || !g_getenv ("GW_HOST") || !g_getenv ("GW_USER") || !g_getenv ("GW_PASSWORD")) {
		fprintf (stderr, "Usage: GW_HOST=.. GW_USER=.. GW_PASSWORD=.. [GW_PROXY=..] [GW_READ_ONLY=1] %s MODULE-PATH [--insecure] [--ssl] [--uid ID] [DATA-DIR]\n", argv[0]);
		return 2;
	}
	if (!data_dir)
		data_dir = tmp = g_dir_make_tmp ("gw-check-XXXXXX", NULL);

	camel_init (NULL, FALSE);
	camel_provider_init ();
	if (!camel_provider_load (module, &error))
		g_error ("%s", error->message);
	/* The folder names in the user's language; GW_LOCALEDIR: translations
	 * not installed yet (a directory with de/LC_MESSAGES/evolution-groupwise.mo) */
	setlocale (LC_ALL, "");
	if (g_getenv ("GW_LOCALEDIR"))
		bindtextdomain ("evolution-groupwise", g_getenv ("GW_LOCALEDIR"));

	session = g_object_new (check_session_get_type (), "user-data-dir", data_dir, "user-cache-dir", data_dir, "online", TRUE, NULL);
	((CheckSession *) session)->insecure = insecure;

	store = camel_session_add_service (session, "check", "groupwise", CAMEL_PROVIDER_STORE, &error);
	settings = camel_service_ref_settings (store);
	g_object_set (settings, "host", g_getenv ("GW_HOST"), "port", 7191, "user", g_getenv ("GW_USER"),
		"security-method", ssl ? CAMEL_NETWORK_SECURITY_METHOD_SSL_ON_ALTERNATE_PORT : CAMEL_NETWORK_SECURITY_METHOD_NONE,
		NULL);
	/* GW_PROXY: the mailbox of that user, as proxy; GW_READ_ONLY: write nothing */
	g_object_set (settings, "proxy", g_getenv ("GW_PROXY"), "read-only", g_getenv ("GW_READ_ONLY") != NULL, NULL);
	/* GW_EVENTS=SECONDS: asks for the events of the mailbox (the shortest
	 * interval) and shows that long what the Mailbox learns from them; it
	 * leaves no configuration in the mailbox */
	if (g_getenv ("GW_EVENTS"))
		g_object_set (settings, "use-events-interval", TRUE, "events-interval", 15, NULL);
	g_object_unref (settings);
	camel_service_set_password (store, g_getenv ("GW_PASSWORD"));

	start = g_get_monotonic_time ();
	if (!camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (store), TRUE, NULL, &error) ||
	    camel_service_get_connection_status (store) != CAMEL_SERVICE_CONNECTED) {
		fprintf (stderr, "connect: %s\n", error ? error->message : "not connected");
		return 1;
	}
	printf ("connected in %.2f s\n", (g_get_monotonic_time () - start) / 1e6);

	tree = camel_store_get_folder_info_sync (CAMEL_STORE (store), NULL, CAMEL_STORE_FOLDER_INFO_RECURSIVE, NULL, &error);
	if (!tree) {
		fprintf (stderr, "folders: %s\n", error ? error->message : "none");
		return 1;
	}
	printf ("folders:\n");
	print_tree (tree, 0);
	camel_folder_info_free (tree);

	inbox = camel_store_get_inbox_folder_sync (CAMEL_STORE (store), NULL, &error);
	if (!inbox) {
		fprintf (stderr, "inbox: %s\n", error->message);
		return 1;
	}

	start = g_get_monotonic_time ();
	if (!camel_folder_refresh_info_sync (inbox, NULL, &error)) {
		fprintf (stderr, "refresh: %s\n", error->message);
		return 1;
	}
	printf ("\n%s: %d messages, %d unread, refreshed in %.2f s\n", camel_folder_get_full_name (inbox),
		camel_folder_get_message_count (inbox), camel_folder_get_unread_message_count (inbox),
		(g_get_monotonic_time () - start) / 1e6);

	/* The next refresh is a quick check (getQuickMessages) */
	start = g_get_monotonic_time ();
	if (!camel_folder_refresh_info_sync (inbox, NULL, &error)) {
		fprintf (stderr, "second refresh: %s\n", error->message);
		return 1;
	}
	printf ("second refresh in %.2f s\n", (g_get_monotonic_time () - start) / 1e6);

	uids = camel_folder_get_uids (inbox);
	infos = g_ptr_array_new_with_free_func (g_object_unref);
	for (ii = 0; ii < (gint) uids->len; ii++)
		g_ptr_array_add (infos, camel_folder_get_message_info (inbox, uids->pdata[ii]));
	camel_folder_free_uids (inbox, uids);
	g_ptr_array_sort (infos, compare_date);

	for (ii = 0; ii < (gint) infos->len && ii < 8; ii++) {
		CamelMessageInfo *info = infos->pdata[ii];
		GDateTime *dt = g_date_time_new_from_unix_local (camel_message_info_get_date_received (info));
		gchar *date = g_date_time_format (dt, "%Y-%m-%d %H:%M");
		guint32 flags = camel_message_info_get_flags (info);

		printf ("  %c%c %s  %-28.28s %s\n", flags & CAMEL_MESSAGE_SEEN ? ' ' : 'N',
			flags & CAMEL_MESSAGE_ATTACHMENTS ? '@' : ' ', date,
			camel_message_info_get_from (info), camel_message_info_get_subject (info));
		g_free (date);
		g_date_time_unref (dt);
	}

	if (!uid && infos->len > 0)
		uid = camel_message_info_get_uid (infos->pdata[0]);
	if (uid) {
		CamelMimeMessage *message;
		CamelMessageInfo *info;
		gchar *filename;
		GStatBuf st;

		start = g_get_monotonic_time ();
		message = camel_folder_get_message_sync (inbox, uid, NULL, &error);
		if (!message) {
			fprintf (stderr, "message: %s\n", error->message);
			return 1;
		}
		printf ("\nmessage in %.2f s: \"%s\" from %s\n", (g_get_monotonic_time () - start) / 1e6,
			camel_mime_message_get_subject (message),
			camel_address_format (CAMEL_ADDRESS (camel_mime_message_get_from (message))));
		printf ("  Message-ID %s, X-Mailer %s, Received %s, DKIM %s\n",
			camel_mime_message_get_message_id (message),
			camel_medium_get_header (CAMEL_MEDIUM (message), "X-Mailer"),
			camel_medium_get_header (CAMEL_MEDIUM (message), "Received") ? "yes" : "no",
			camel_medium_get_header (CAMEL_MEDIUM (message), "DKIM-Signature") ? "yes" : "no");
		filename = camel_folder_get_filename (inbox, uid, NULL);
		if (filename && g_stat (filename, &st) == 0)
			printf ("  cached: %ld bytes\n", (long) st.st_size);
		g_free (filename);
		info = camel_folder_get_message_info (inbox, uid);
		if (info) {
			printf ("  still unread on the server: %s\n", camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN ? "no (was read)" : "yes");
			g_object_unref (info);
		}
		print_part (CAMEL_DATA_WRAPPER (message), 0);
		g_object_unref (message);
	}

	g_ptr_array_unref (infos);
	if (g_getenv ("GW_EVENTS")) {
		GMainLoop *loop = g_main_loop_new (NULL, FALSE);
		guint seconds = MAX (atoi (g_getenv ("GW_EVENTS")), 20);

		printf ("events: watching the Mailbox for %u seconds\n", seconds);
		fflush (stdout);
		g_signal_connect (inbox, "changed", G_CALLBACK (events_changed_cb), NULL);
		g_timeout_add_seconds (seconds, events_stop_cb, loop);
		g_main_loop_run (loop);
		/* Switched off: the next question takes the configuration away */
		settings = camel_service_ref_settings (store);
		g_object_set (settings, "use-events-interval", FALSE, NULL);
		g_object_unref (settings);
		g_timeout_add_seconds (20, events_stop_cb, loop);
		g_main_loop_run (loop);
		g_main_loop_unref (loop);
	}
	g_object_unref (inbox);
	camel_service_disconnect_sync (store, TRUE, NULL, NULL);
	g_object_unref (store);
	g_object_unref (session);
	g_free (tmp);

	return 0;
}

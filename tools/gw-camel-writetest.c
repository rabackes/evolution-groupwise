/*
 * gw-camel-writetest.c: runs the writing paths of the groupwise Camel
 * provider against a real POA. It CHANGES the mailbox: only for test
 * accounts. Everything it does is undone at the end.
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
 * Usage: GW_HOST=.. GW_USER=.. GW_PASSWORD=.. GW_EMAIL=.. gw-camel-writetest MODULE-PATH --this-is-a-test-account
 *
 * GW_EMAIL is the address of the test account itself: the test mail goes
 * only there. The mail and the draft it makes stay in the mailbox.
 */

#include <stdio.h>
#include <string.h>

#include <camel/camel.h>

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
	return camel_service_authenticate_sync (service, mechanism, cancellable, error) == CAMEL_AUTHENTICATION_ACCEPTED;
}

/* A test account on an internal POA: its certificate is accepted */
static CamelCertTrust
test_session_trust_prompt (CamelSession *session,
			   CamelService *service,
			   GTlsCertificate *certificate,
			   GTlsCertificateFlags errors)
{
	return CAMEL_CERT_TRUST_TEMPORARY;
}

static void
test_session_class_init (TestSessionClass *class)
{
	CAMEL_SESSION_CLASS (class)->authenticate_sync = test_session_authenticate_sync;
	CAMEL_SESSION_CLASS (class)->trust_prompt = test_session_trust_prompt;
}

static void
test_session_init (TestSession *session)
{
}

static gint failures;

static void
check (gboolean ok,
       const gchar *what,
       GError **error)
{
	printf ("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, error && *error ? ": " : "", error && *error ? (*error)->message : "");
	if (!ok)
		failures++;
	if (error)
		g_clear_error (error);
}

static CamelFolder *
open_folder (CamelStore *store,
	     const gchar *name)
{
	GError *error = NULL;
	CamelFolder *folder = camel_store_get_folder_sync (store, name, 0, NULL, &error);

	if (folder)
		camel_folder_refresh_info_sync (folder, NULL, &error);
	check (folder != NULL && !error, name, &error);

	return folder;
}

/* A full scan: what the server really has */
static gboolean
server_has (CamelFolder *folder,
	    const gchar *uid)
{
	CamelMessageInfo *info;
	gboolean found;

	g_setenv ("GROUPWISE_FULL_SCAN_SECONDS", "0", TRUE);
	camel_folder_refresh_info_sync (folder, NULL, NULL);
	g_unsetenv ("GROUPWISE_FULL_SCAN_SECONDS");

	info = camel_folder_get_message_info (folder, uid);
	found = info != NULL;
	g_clear_object (&info);

	return found;
}

/* After a full scan the labels are what the server has */
static gboolean
label_on_server (CamelFolder *folder,
		 const gchar *uid,
		 const gchar *tag)
{
	CamelMessageInfo *info;
	gboolean found;

	server_has (folder, uid);
	info = camel_folder_get_message_info (folder, uid);
	found = info && camel_message_info_get_user_flag (info, tag);
	g_clear_object (&info);

	return found;
}

static gchar *
id_in (const gchar *uid,
       CamelFolder *folder)
{
	/* The provider's rule: the first part of the ID stays, the container changes */
	GPtrArray *uids = camel_folder_get_uids (folder);
	const gchar *colon = strchr (uid, ':');
	gchar *found = NULL;
	guint ii;

	for (ii = 0; ii < uids->len && !found; ii++) {
		if (strncmp (uids->pdata[ii], uid, colon - uid + 1) == 0)
			found = g_strdup (uids->pdata[ii]);
	}
	camel_folder_free_uids (folder, uids);

	return found;
}

int
main (int argc,
      char **argv)
{
	GError *error = NULL;
	CamelSession *session;
	CamelService *service;
	CamelStore *store;
	CamelSettings *settings;
	CamelFolder *inbox, *test, *trash;
	CamelFolderInfo *fi;
	GPtrArray *uids, *one;
	gchar *tmp, *name, *renamed, *uid, *copy_uid, *trash_uid;

	if (argc != 3 || !g_str_equal (argv[2], "--this-is-a-test-account") ||
	    !g_getenv ("GW_HOST") || !g_getenv ("GW_USER") || !g_getenv ("GW_PASSWORD") || !g_getenv ("GW_EMAIL")) {
		fprintf (stderr, "Usage: GW_HOST=.. GW_USER=.. GW_PASSWORD=.. GW_EMAIL=.. %s MODULE-PATH --this-is-a-test-account\n"
			"Moves, copies and deletes in the mailbox: only for test accounts.\n", argv[0]);
		return 2;
	}

	/* Evolution's labels only in memory: the user's settings stay untouched */
	g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
	tmp = g_dir_make_tmp ("gw-writetest-XXXXXX", NULL);
	camel_init (NULL, FALSE);
	camel_provider_init ();
	if (!camel_provider_load (argv[1], &error))
		g_error ("%s", error->message);

	session = g_object_new (test_session_get_type (), "user-data-dir", tmp, "user-cache-dir", tmp, "online", TRUE, NULL);
	service = camel_session_add_service (session, "writetest", "groupwise", CAMEL_PROVIDER_STORE, &error);
	store = CAMEL_STORE (service);
	settings = camel_service_ref_settings (service);
	g_object_set (settings, "host", g_getenv ("GW_HOST"), "port", 7191, "user", g_getenv ("GW_USER"),
		"security-method", CAMEL_NETWORK_SECURITY_METHOD_NONE, NULL);
	g_object_unref (settings);
	camel_service_set_password (service, g_getenv ("GW_PASSWORD"));
	check (camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (store), TRUE, NULL, &error), "connect", &error);

	name = g_strdup_printf ("Evolution-Test-%" G_GINT64_FORMAT, g_get_real_time () / G_USEC_PER_SEC);
	renamed = g_strconcat (name, "-umbenannt", NULL);

	fi = camel_store_create_folder_sync (store, NULL, name, NULL, &error);
	check (fi != NULL, "create folder", &error);
	if (fi)
		camel_folder_info_free (fi);

	inbox = open_folder (store, "Mailbox");
	test = open_folder (store, name);
	if (!inbox || !test)
		return 1;

	uids = camel_folder_get_uids (inbox);
	uid = g_strdup (uids->pdata[0]);
	camel_folder_free_uids (inbox, uids);
	printf ("      mail: %s\n", uid);
	one = g_ptr_array_new ();
	g_ptr_array_add (one, uid);

	/* Move there and back */
	check (camel_folder_transfer_messages_to_sync (inbox, one, test, TRUE, NULL, NULL, &error), "move to test folder", &error);
	copy_uid = id_in (uid, test);
	check (copy_uid && server_has (test, copy_uid) && !server_has (inbox, uid), "on the server in the test folder only", NULL);
	one->pdata[0] = copy_uid;
	check (camel_folder_transfer_messages_to_sync (test, one, inbox, TRUE, NULL, NULL, &error), "move back", &error);
	check (server_has (inbox, uid) && !server_has (test, copy_uid), "on the server back in the Mailbox", NULL);
	g_free (copy_uid);

	/* A label is a category: set, written, read back after a full scan, taken off */
	camel_folder_set_message_user_flag (inbox, uid, "$Labelimportant", TRUE);
	check (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error), "label Important", &error);
	check (server_has (inbox, uid) && label_on_server (inbox, uid, "$Labelimportant"), "category Urgent on the server", NULL);
	camel_folder_set_message_user_flag (inbox, uid, "$Labelimportant", FALSE);
	check (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error), "label off", &error);
	check (!label_on_server (inbox, uid, "$Labelimportant"), "category gone on the server", NULL);

	/* The follow-up flag is the Tasklist: on it with a due date, completed,
	 * off again (checked in the Tasklist folder, read from the server) */
	{
		CamelFolder *tasklist = open_folder (store, "Tasklist");
		gchar *due = camel_header_format_date (time (NULL) + 2 * 86400, 0);
		gchar *on_list;
		CamelMessageInfo *info;

		camel_folder_set_message_user_tag (inbox, uid, "follow-up", "Follow-Up");
		camel_folder_set_message_user_tag (inbox, uid, "due-by", due);
		check (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error), "flag for follow-up", &error);
		on_list = tasklist && server_has (tasklist, uid) ? g_strdup (uid) : NULL;
		if (!on_list && tasklist)
			on_list = id_in (uid, tasklist);
		check (on_list && tasklist && server_has (tasklist, on_list), "on the Tasklist", NULL);
		info = on_list && tasklist ? camel_folder_get_message_info (tasklist, on_list) : NULL;
		check (info && camel_message_info_get_user_tag (info, "due-by") &&
			camel_header_decode_date (camel_message_info_get_user_tag (info, "due-by"), NULL) ==
			camel_header_decode_date (due, NULL), "with the due date", NULL);
		g_clear_object (&info);

		camel_folder_set_message_user_tag (inbox, uid, "completed-on", camel_header_format_date (time (NULL), 0));
		check (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error), "mark completed", &error);
		if (tasklist && on_list)
			server_has (tasklist, on_list);
		info = on_list && tasklist ? camel_folder_get_message_info (tasklist, on_list) : NULL;
		check (info && camel_message_info_get_user_tag (info, "completed-on") &&
			*camel_message_info_get_user_tag (info, "completed-on"), "completed on the Tasklist", NULL);
		g_clear_object (&info);

		camel_folder_set_message_user_tag (inbox, uid, "follow-up", NULL);
		camel_folder_set_message_user_tag (inbox, uid, "due-by", NULL);
		camel_folder_set_message_user_tag (inbox, uid, "completed-on", NULL);
		check (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error), "flag off", &error);
		check (tasklist && on_list && !server_has (tasklist, on_list), "off the Tasklist", NULL);
		check (server_has (inbox, uid), "still in the Mailbox", NULL);

		g_free (on_list);
		g_free (due);
		g_clear_object (&tasklist);
	}

	/* Junk: an Internet mail into the Junk Mail folder and back, as Evolution
	 * marks it (the sender goes onto the junk list, then the trust list;
	 * see them with gw-cli raw getJunkEntries) */
	{
		GPtrArray *all = camel_folder_get_uids (inbox);
		gchar *external = NULL;
		guint ii;

		for (ii = 0; ii < all->len && !external; ii++) {
			CamelMessageInfo *info = camel_folder_get_message_info (inbox, all->pdata[ii]);
			const gchar *from = info ? camel_message_info_get_from (info) : NULL;

			if (from && g_getenv ("GW_JUNK_SENDER") && strstr (from, g_getenv ("GW_JUNK_SENDER")))
				external = g_strdup (all->pdata[ii]);
			g_clear_object (&info);
		}
		camel_folder_free_uids (inbox, all);

		if (external) {
			CamelFolder *junk = camel_store_get_junk_folder_sync (store, NULL, &error);
			gchar *in_junk;

			check (junk != NULL, "junk folder", &error);
			camel_folder_set_message_flags (inbox, external, CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_NOTJUNK | CAMEL_MESSAGE_JUNK_LEARN,
				CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_JUNK_LEARN);
			check (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error), "mark junk", &error);
			if (junk)
				camel_folder_refresh_info_sync (junk, NULL, NULL);
			in_junk = junk ? id_in (external, junk) : NULL;
			check (in_junk && server_has (junk, in_junk) && !server_has (inbox, external), "in the Junk Mail folder", NULL);
			if (in_junk) {
				camel_folder_set_message_flags (junk, in_junk, CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_NOTJUNK | CAMEL_MESSAGE_JUNK_LEARN,
					CAMEL_MESSAGE_NOTJUNK | CAMEL_MESSAGE_JUNK_LEARN);
				check (camel_folder_synchronize_sync (junk, FALSE, NULL, &error), "mark not junk", &error);
				check (server_has (inbox, external) && !server_has (junk, in_junk), "back in the Mailbox", NULL);
			}
			g_free (in_junk);
			g_clear_object (&junk);
			g_free (external);
		} else {
			printf ("      (no mail from $GW_JUNK_SENDER: junk not tested)\n");
		}
	}

	/* Copy and delete the copy: it goes to the Trash, the mail stays */
	one->pdata[0] = uid;
	check (camel_folder_transfer_messages_to_sync (inbox, one, test, FALSE, NULL, NULL, &error), "copy to test folder", &error);
	copy_uid = id_in (uid, test);
	check (copy_uid && server_has (test, copy_uid) && server_has (inbox, uid), "on the server in both folders", NULL);
	if (copy_uid) {
		camel_folder_set_message_flags (test, copy_uid, CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
		check (camel_folder_synchronize_sync (test, FALSE, NULL, &error), "delete the copy", &error);
		check (!server_has (test, copy_uid) && server_has (inbox, uid), "copy gone, original there", NULL);
	}

	trash = camel_store_get_trash_folder_sync (store, NULL, &error);
	check (trash != NULL, "trash", &error);
	camel_folder_refresh_info_sync (trash, NULL, NULL);
	trash_uid = id_in (uid, trash);
	check (trash_uid != NULL, "the copy is in the Trash", NULL);

	/* Restore: the copy goes back into the test folder, then the original,
	 * deleted from the Mailbox, back into the Mailbox */
	if (trash_uid) {
		camel_folder_set_message_user_flag (trash, trash_uid, "gw-restore", TRUE);
		check (camel_folder_synchronize_sync (trash, FALSE, NULL, &error), "restore the copy", &error);
		g_free (copy_uid);
		camel_folder_refresh_info_sync (test, NULL, NULL);
		copy_uid = id_in (uid, test);
		check (copy_uid && server_has (test, copy_uid), "copy back in the test folder", NULL);
		check (server_has (inbox, uid), "original still in the Mailbox", NULL);
		check (!server_has (trash, trash_uid), "copy out of the Trash", NULL);

		camel_folder_set_message_flags (inbox, uid, CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
		check (camel_folder_synchronize_sync (inbox, FALSE, NULL, &error), "delete the original", &error);
		camel_folder_refresh_info_sync (trash, NULL, NULL);
		g_free (trash_uid);
		trash_uid = id_in (uid, trash);
		check (trash_uid != NULL, "original in the Trash", NULL);
		check (!server_has (inbox, uid), "original out of the Mailbox", NULL);
		if (trash_uid) {
			camel_folder_set_message_user_flag (trash, trash_uid, "gw-restore", TRUE);
			check (camel_folder_synchronize_sync (trash, FALSE, NULL, &error), "restore the original", &error);
			check (server_has (inbox, uid), "original back in the Mailbox", NULL);
			check (!server_has (trash, trash_uid), "original out of the Trash", NULL);
			check (copy_uid && server_has (test, copy_uid), "copy still in the test folder", NULL);
		}
	}

	/* Delete the copy again and empty it from the Trash: out of the Trash,
	 * but not purged, the mail stays */
	if (copy_uid) {
		camel_folder_set_message_flags (test, copy_uid, CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
		check (camel_folder_synchronize_sync (test, FALSE, NULL, &error), "delete the copy again", &error);
	}
	camel_folder_refresh_info_sync (trash, NULL, NULL);
	g_free (trash_uid);
	trash_uid = id_in (uid, trash);
	check (trash_uid != NULL, "the copy is in the Trash again", NULL);
	if (trash_uid) {
		camel_folder_set_message_flags (trash, trash_uid, CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
		check (camel_folder_expunge_sync (trash, NULL, &error), "empty it from the Trash", &error);
		check (server_has (inbox, uid), "the original survives", NULL);
		check (!server_has (trash, trash_uid), "out of the Trash", NULL);
		check (!copy_uid || !server_has (test, copy_uid), "not back in the test folder", NULL);
	}

	/* A copy for the test folder to take into the Trash when it is deleted */
	check (camel_folder_transfer_messages_to_sync (inbox, one, test, FALSE, NULL, NULL, &error), "copy to test folder again", &error);

	/* Send an HTML mail with a picture and an attachment to the account itself */
	{
		const gchar *self = g_getenv ("GW_EMAIL");
		gchar *subject = g_strdup_printf ("Evolution-Sendetest %s", name);
		gchar *raw = g_strdup_printf (
			"From: <%s>\r\nTo: <%s>\r\nSubject: %s\r\nX-Priority: 1\r\nMIME-Version: 1.0\r\n"
			"Content-Type: multipart/mixed; boundary=\"MIX\"\r\n\r\n"
			"--MIX\r\nContent-Type: multipart/alternative; boundary=\"ALT\"\r\n\r\n"
			"--ALT\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\nGrüße aus Evolution (Text)\r\n"
			"--ALT\r\nContent-Type: multipart/related; boundary=\"REL\"\r\n\r\n"
			"--REL\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n"
			"<p><b>Grüße</b> aus Evolution <img src=\"cid:punkt@test\"></p>\r\n"
			"--REL\r\nContent-Type: image/png\r\nContent-ID: <punkt@test>\r\nContent-Transfer-Encoding: base64\r\n\r\n"
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==\r\n"
			"--REL--\r\n--ALT--\r\n"
			"--MIX\r\nContent-Type: text/plain; name=\"notiz.txt\"\r\nContent-Disposition: attachment; filename=\"notiz.txt\"\r\n\r\n"
			"Anhang aus Evolution\r\n--MIX--\r\n", self, self, subject);
		CamelMimeMessage *message = camel_mime_message_new ();
		GInputStream *in = g_memory_input_stream_new_from_data (raw, -1, NULL);
		CamelService *transport = camel_session_add_service (session, "writetest-transport", "groupwise",
			CAMEL_PROVIDER_TRANSPORT, &error);
		CamelInternetAddress *from = camel_internet_address_new (), *to = camel_internet_address_new ();
		CamelFolder *drafts;
		gboolean saved = FALSE, arrived = FALSE;
		gchar *draft_uid = NULL, *arrived_uid = NULL;
		gint tries;

		camel_data_wrapper_construct_from_input_stream_sync (CAMEL_DATA_WRAPPER (message), in, NULL, NULL);
		settings = camel_service_ref_settings (transport);
		g_object_set (settings, "host", g_getenv ("GW_HOST"), "port", 7191, "user", g_getenv ("GW_USER"), NULL);
		g_object_unref (settings);
		camel_internet_address_add (from, NULL, self);
		camel_internet_address_add (to, NULL, self);

		check (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), message, CAMEL_ADDRESS (from),
			CAMEL_ADDRESS (to), &saved, NULL, &error) && saved, "send to the account itself", &error);

		/* It arrives in the Mailbox within a few seconds */
		for (tries = 0; tries < 15 && !arrived; tries++) {
			GPtrArray *all;
			guint ii;

			g_usleep (G_USEC_PER_SEC);
			camel_folder_refresh_info_sync (inbox, NULL, NULL);
			all = camel_folder_get_uids (inbox);
			for (ii = 0; ii < all->len && !arrived; ii++) {
				CamelMessageInfo *info = camel_folder_get_message_info (inbox, all->pdata[ii]);

				if (info && g_strcmp0 (camel_message_info_get_subject (info), subject) == 0) {
					CamelMimeMessage *received = camel_folder_get_message_sync (inbox, all->pdata[ii], NULL, &error);

					arrived = TRUE;
					arrived_uid = g_strdup (all->pdata[ii]);
					check (received != NULL, "received and readable", &error);
					if (received) {
						printf ("      received: priority %s, %d top-level parts, flags %s%s\n",
							camel_medium_get_header (CAMEL_MEDIUM (received), "X-Priority"),
							CAMEL_IS_MULTIPART (camel_medium_get_content (CAMEL_MEDIUM (received))) ?
								camel_multipart_get_number (CAMEL_MULTIPART (camel_medium_get_content (CAMEL_MEDIUM (received)))) : 1,
							camel_message_info_get_flags (info) & CAMEL_MESSAGE_ATTACHMENTS ? "attachment " : "",
							camel_message_info_get_flags (info) & CAMEL_MESSAGE_FLAGGED ? "important" : "");
						g_object_unref (received);
					}
				}
				g_clear_object (&info);
			}
			camel_folder_free_uids (inbox, all);
		}
		check (arrived, "arrived in the Mailbox", NULL);

		/* Forward it as attachment, as Evolution does: the message as the
		 * provider hands it out, as a message/rfc822 part */
		if (arrived_uid) {
			CamelMimeMessage *original = camel_folder_get_message_sync (inbox, arrived_uid, NULL, &error);
			CamelMimeMessage *forward = camel_mime_message_new ();
			CamelMultipart *mixed = camel_multipart_new ();
			CamelMimePart *text = camel_mime_part_new (), *attached = camel_mime_part_new ();
			gchar *fwd_subject = g_strdup_printf ("Evolution-Weiterleitungstest %s", name);

			check (original && camel_medium_get_header (CAMEL_MEDIUM (original), "X-GroupWise-Item-Id"),
				"the message names its GroupWise item", &error);
			camel_mime_part_set_content (text, "Weitergeleitet als Anhang\n", -1, "text/plain; charset=UTF-8");
			camel_multipart_add_part (mixed, text);
			if (original) {
				camel_medium_set_content (CAMEL_MEDIUM (attached), CAMEL_DATA_WRAPPER (original));
				camel_mime_part_set_disposition (attached, "attachment");
				camel_multipart_add_part (mixed, attached);
			}
			camel_multipart_set_boundary (mixed, NULL);
			camel_medium_set_content (CAMEL_MEDIUM (forward), CAMEL_DATA_WRAPPER (mixed));
			camel_mime_message_set_subject (forward, fwd_subject);
			camel_mime_message_set_from (forward, from);
			camel_mime_message_set_recipients (forward, CAMEL_RECIPIENT_TYPE_TO, to);
			check (camel_transport_send_to_sync (CAMEL_TRANSPORT (transport), forward, CAMEL_ADDRESS (from),
				CAMEL_ADDRESS (to), &saved, NULL, &error), "forward it as attachment", &error);
			printf ("      forwarded: \"%s\" (check its attachment on the server)\n", fwd_subject);

			g_free (fwd_subject);
			g_object_unref (text);
			g_object_unref (attached);
			g_object_unref (mixed);
			g_object_unref (forward);
			g_clear_object (&original);
			g_free (arrived_uid);
		}

		/* Save it as a draft */
		drafts = camel_store_get_folder_sync (store, "Work In Progress", 0, NULL, &error);
		check (drafts != NULL, "drafts folder", &error);
		if (drafts) {
			camel_folder_refresh_info_sync (drafts, NULL, NULL);
			check (camel_folder_append_message_sync (drafts, message, NULL, &draft_uid, NULL, &error), "save draft", &error);
			check (draft_uid && server_has (drafts, draft_uid), "draft in Work In Progress on the server", NULL);
			g_object_unref (drafts);
		}

		g_free (draft_uid);
		g_object_unref (from);
		g_object_unref (to);
		g_object_unref (transport);
		g_object_unref (in);
		g_object_unref (message);
		g_free (raw);
		g_free (subject);
	}

	/* Rename and delete the test folder */
	g_object_unref (test);
	check (camel_store_rename_folder_sync (store, name, renamed, NULL, &error), "rename folder", &error);
	fi = camel_store_get_folder_info_sync (store, renamed, CAMEL_STORE_FOLDER_INFO_REFRESH, NULL, &error);
	check (fi != NULL, "renamed on the server", &error);
	if (fi)
		camel_folder_info_free (fi);
	check (camel_store_delete_folder_sync (store, renamed, NULL, &error), "delete folder", &error);
	fi = camel_store_get_folder_info_sync (store, renamed, CAMEL_STORE_FOLDER_INFO_REFRESH, NULL, NULL);
	check (fi == NULL, "deleted on the server", NULL);
	if (fi)
		camel_folder_info_free (fi);

	/* The copy went to the Trash with the folder; restored, it only leaves
	 * the Trash, the original stays in the Mailbox */
	camel_folder_refresh_info_sync (trash, NULL, NULL);
	g_free (trash_uid);
	trash_uid = id_in (uid, trash);
	check (trash_uid != NULL, "copy in the Trash with its folder", NULL);
	if (trash_uid) {
		camel_folder_set_message_user_flag (trash, trash_uid, "gw-restore", TRUE);
		check (camel_folder_synchronize_sync (trash, FALSE, NULL, &error), "restore it", &error);
		check (!server_has (trash, trash_uid), "out of the Trash", NULL);
		check (server_has (inbox, uid), "original still in the Mailbox", NULL);
	}

	camel_service_disconnect_sync (service, TRUE, NULL, NULL);
	printf ("%s\n", failures ? "FAILURES" : "all ok");

	return failures ? 1 : 0;
}

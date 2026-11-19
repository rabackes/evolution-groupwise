/*
 * e-groupwise-reply-fixup.c: restore recipients Reply-to-All strips as "own" aliases
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

/*
 * gw-senders.c lists the mailboxes of the GroupWise proxy list as aliases of
 * the account identity, so the composer offers them in "From". Evolution
 * core treats identity aliases as the user's own addresses everywhere,
 * including when it builds a Reply-to-All, so a message sent to one of
 * these mailboxes (even without a proxy account configured for it) loses
 * that recipient from the reply, as if it were the user replying to
 * themselves (GitHub issue #3).
 *
 * This restores such recipients right as a reply composer window appears,
 * before the user can edit anything: "window-added" on the shell catches
 * every new composer; its own "show" signal, which fires once the composer
 * is fully populated but still before the user can interact, is where the
 * fix-up runs. The original message is found via the X-Evolution-Source-*
 * headers em_utils_reply_to_message() leaves on the composer; which
 * addresses are "proxy, not really me" comes from the same senders.ini
 * gw-senders.c itself writes.
 */

#include <string.h>

#include <camel/camel.h>
#include <libedataserver/libedataserver.h>
#include <libemail-engine/libemail-engine.h>
#include <mail/e-mail-backend.h>
#include <mail/em-composer-utils.h>
#include <composer/e-msg-composer.h>
#include <shell/e-shell.h>
#include <shell/e-shell-backend.h>

#include "e-groupwise-reply-fixup.h"

/* As in gw-senders.c */
#define SENDERS_GROUP "Senders"
#define SENDERS_KEY "Addresses"

typedef struct {
	EExtension parent;
} EGroupwiseReplyFixup;

typedef struct {
	EExtensionClass parent_class;
} EGroupwiseReplyFixupClass;

GType e_groupwise_reply_fixup_get_type (void);

G_DEFINE_DYNAMIC_TYPE (EGroupwiseReplyFixup, e_groupwise_reply_fixup, E_TYPE_EXTENSION)

static gboolean
strv_contains_nocase (gchar **strv,
		      const gchar *str)
{
	guint ii;

	for (ii = 0; strv && strv[ii]; ii++) {
		if (g_ascii_strcasecmp (strv[ii], str) == 0)
			return TRUE;
	}

	return FALSE;
}

static gboolean
destinations_contain (EDestination **destinations,
		      const gchar *address)
{
	guint ii;

	for (ii = 0; destinations && destinations[ii]; ii++) {
		const gchar *addr = e_destination_get_email (destinations[ii]);

		if (addr && g_ascii_strcasecmp (addr, address) == 0)
			return TRUE;
	}

	return FALSE;
}

/* The addresses gw-senders.c entered as aliases of @collection_uid's
 * identity, straight from its own cache file: not "me", just mailboxes we
 * may send as */
static gchar **
dup_proxy_addresses (const gchar *collection_uid)
{
	gchar *filename;
	GKeyFile *key_file;
	gchar **addresses;

	filename = g_build_filename (e_get_user_cache_dir (), "sources", collection_uid, "senders.ini", NULL);

	key_file = g_key_file_new ();
	if (!g_key_file_load_from_file (key_file, filename, G_KEY_FILE_NONE, NULL)) {
		g_key_file_free (key_file);
		g_free (filename);
		return NULL;
	}
	g_free (filename);

	addresses = g_key_file_get_string_list (key_file, SENDERS_GROUP, SENDERS_KEY, NULL, NULL);
	g_key_file_free (key_file);

	return addresses;
}

/* The GroupWise collection owning @identity_uid, else NULL (also for any
 * other kind of account) */
static ESource *
ref_groupwise_collection (ESourceRegistry *registry,
			  const gchar *identity_uid)
{
	ESource *identity, *collection;

	identity = e_source_registry_ref_source (registry, identity_uid);
	if (!identity)
		return NULL;

	collection = e_source_registry_find_extension (registry, identity, E_SOURCE_EXTENSION_COLLECTION);
	g_object_unref (identity);

	if (collection && g_strcmp0 (e_source_backend_get_backend_name (
			E_SOURCE_BACKEND (e_source_get_extension (collection, E_SOURCE_EXTENSION_COLLECTION))), "groupwise") != 0)
		g_clear_object (&collection);

	return collection;
}

static void
restore_dropped_proxy_recipients (EMsgComposer *composer)
{
	EComposerHeaderTable *table;
	EShell *shell;
	EShellBackend *mail_backend;
	EMailSession *session;
	ESourceRegistry *registry;
	ESource *collection;
	const gchar *flags_header, *folder_uri, *message_uid;
	gchar *identity_uid;
	gchar **addresses;
	CamelFolder *folder;
	CamelMimeMessage *message;
	CamelInternetAddress *orig_to, *orig_cc, *missing_to, *missing_cc;
	EDestination **to, **cc, **add;
	gint ii, len;

	flags_header = e_msg_composer_get_header (composer, "X-Evolution-Source-Flags", 0);
	/* Only a Reply-to-All/-List: a plain Reply-to-Sender deliberately
	 * narrows the recipients on its own, and Forward starts empty */
	if (!flags_header || !strstr (flags_header, "ANSWERED_ALL"))
		return;

	folder_uri = e_msg_composer_get_header (composer, "X-Evolution-Source-Folder", 0);
	message_uid = e_msg_composer_get_header (composer, "X-Evolution-Source-Message", 0);
	if (!folder_uri || !message_uid)
		return;

	table = e_msg_composer_get_header_table (composer);
	identity_uid = e_composer_header_table_dup_identity_uid (table, NULL, NULL);
	if (!identity_uid)
		return;

	shell = e_msg_composer_get_shell (composer);
	mail_backend = e_shell_get_backend_by_name (shell, "mail");
	session = mail_backend ? e_mail_backend_get_session (E_MAIL_BACKEND (mail_backend)) : NULL;
	if (!session) {
		g_free (identity_uid);
		return;
	}
	registry = e_mail_session_get_registry (session);

	collection = ref_groupwise_collection (registry, identity_uid);
	g_free (identity_uid);
	if (!collection)
		return;

	addresses = dup_proxy_addresses (e_source_get_uid (collection));
	g_object_unref (collection);
	if (!addresses || !addresses[0]) {
		g_strfreev (addresses);
		return;
	}

	folder = e_mail_session_uri_to_folder_sync (session, folder_uri, 0, NULL, NULL);
	if (!folder) {
		g_strfreev (addresses);
		return;
	}

	message = camel_folder_get_message_sync (folder, message_uid, NULL, NULL);
	g_object_unref (folder);
	if (!message) {
		g_strfreev (addresses);
		return;
	}

	orig_to = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO);
	orig_cc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC);

	to = e_composer_header_table_get_destinations_to (table);
	cc = e_composer_header_table_get_destinations_cc (table);

	missing_to = camel_internet_address_new ();
	missing_cc = camel_internet_address_new ();

	len = orig_to ? camel_address_length (CAMEL_ADDRESS (orig_to)) : 0;
	for (ii = 0; ii < len; ii++) {
		const gchar *name = NULL, *addr = NULL;

		if (camel_internet_address_get (orig_to, ii, &name, &addr) && addr && *addr &&
		    strv_contains_nocase (addresses, addr) &&
		    !destinations_contain (to, addr) && !destinations_contain (cc, addr))
			camel_internet_address_add (missing_to, name, addr);
	}

	len = orig_cc ? camel_address_length (CAMEL_ADDRESS (orig_cc)) : 0;
	for (ii = 0; ii < len; ii++) {
		const gchar *name = NULL, *addr = NULL;

		if (camel_internet_address_get (orig_cc, ii, &name, &addr) && addr && *addr &&
		    strv_contains_nocase (addresses, addr) &&
		    !destinations_contain (to, addr) && !destinations_contain (cc, addr))
			camel_internet_address_add (missing_cc, name, addr);
	}

	if (camel_address_length (CAMEL_ADDRESS (missing_to))) {
		add = em_utils_camel_address_to_destination (missing_to);
		e_composer_header_table_add_destinations_to (table, add);
		e_destination_freev (add);
	}
	if (camel_address_length (CAMEL_ADDRESS (missing_cc))) {
		add = em_utils_camel_address_to_destination (missing_cc);
		e_composer_header_table_add_destinations_cc (table, add);
		e_destination_freev (add);
	}

	g_object_unref (missing_to);
	g_object_unref (missing_cc);
	e_destination_freev (to);
	e_destination_freev (cc);
	g_object_unref (message);
	g_strfreev (addresses);
}

static void
composer_show_cb (GtkWidget *widget,
		  gpointer user_data)
{
	g_signal_handlers_disconnect_by_func (widget, composer_show_cb, user_data);
	restore_dropped_proxy_recipients (E_MSG_COMPOSER (widget));
}

static void
window_added_cb (GtkApplication *application,
		 GtkWindow *window,
		 gpointer user_data)
{
	/* Headers are not filled in yet at this point (the reply action runs
	 * after the composer is constructed): wait for its own "show" */
	if (E_IS_MSG_COMPOSER (window))
		g_signal_connect (window, "show", G_CALLBACK (composer_show_cb), NULL);
}

static void
e_groupwise_reply_fixup_constructed (GObject *object)
{
	EShell *shell;

	G_OBJECT_CLASS (e_groupwise_reply_fixup_parent_class)->constructed (object);

	shell = E_SHELL (e_extension_get_extensible (E_EXTENSION (object)));
	g_signal_connect (shell, "window-added", G_CALLBACK (window_added_cb), NULL);
}

static void
e_groupwise_reply_fixup_class_init (EGroupwiseReplyFixupClass *class)
{
	G_OBJECT_CLASS (class)->constructed = e_groupwise_reply_fixup_constructed;
	E_EXTENSION_CLASS (class)->extensible_type = E_TYPE_SHELL;
}

static void
e_groupwise_reply_fixup_class_finalize (EGroupwiseReplyFixupClass *class)
{
}

static void
e_groupwise_reply_fixup_init (EGroupwiseReplyFixup *extension)
{
}

void
e_groupwise_reply_fixup_type_register (GTypeModule *type_module)
{
	e_groupwise_reply_fixup_register_type (type_module);
}

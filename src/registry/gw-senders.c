/*
 * gw-senders.c: the mailboxes the user may send from as senders of the account
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
 * The GroupWise client offers the mailboxes the user is proxy of (users and
 * resources) in "From" of a message. Here they become aliases of the
 * identity of the account, which the composer lists in its "From"; the
 * transport sends a message with such a sender through a proxy session.
 *
 * Aliases the user entered are kept: the ones written here are remembered
 * (senders.ini in the cache directory) and only those are taken away again.
 */

#include <string.h>

#include "e-gw-proxy.h"

#include "gw-senders.h"

#define GROUP "Senders"
#define KEY "Addresses"

static ESource *
ref_identity (ECollectionBackend *backend)
{
	GList *sources = e_collection_backend_list_mail_sources (backend), *link;
	ESource *identity = NULL;

	for (link = sources; link && !identity; link = g_list_next (link)) {
		if (e_source_has_extension (link->data, E_SOURCE_EXTENSION_MAIL_IDENTITY))
			identity = g_object_ref (link->data);
	}
	g_list_free_full (sources, g_object_unref);

	return identity;
}

static void
append_alias (GString *aliases,
	      const gchar *name,
	      const gchar *email)
{
	if (aliases->len)
		g_string_append (aliases, ", ");
	if (name && *name) {
		const gchar *p;

		g_string_append_c (aliases, '"');
		for (p = name; *p; p++) {
			if (*p == '"' || *p == '\\')
				g_string_append_c (aliases, '\\');
			g_string_append_c (aliases, *p);
		}
		g_string_append (aliases, "\" ");
	}
	g_string_append_printf (aliases, "<%s>", email);
}

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

void
gw_collection_sync_senders (ECollectionBackend *backend,
			    EGwConnection *cnc,
			    GCancellable *cancellable)
{
	GPtrArray *users, *ours;
	GError *error = NULL;
	ESource *identity;
	ESourceMailIdentity *extension;
	GHashTable *current;
	GHashTableIter iter;
	gpointer key, value;
	GKeyFile *key_file;
	GString *aliases;
	gchar *filename, *old, **last;
	const gchar *own;
	guint ii;

	users = e_gw_connection_get_proxy_users_sync (cnc, cancellable, &error);
	if (!users) {
		/* Not known now: what is there stays */
		if (error) {
			g_warning ("GroupWise proxy list of %s: %s", e_source_get_uid (e_backend_get_source (E_BACKEND (backend))), error->message);
			g_clear_error (&error);
			return;
		}
		users = g_ptr_array_new ();
	}

	identity = ref_identity (backend);
	if (!identity) {
		g_ptr_array_unref (users);
		return;
	}
	extension = e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY);

	filename = g_build_filename (e_collection_backend_get_cache_dir (backend), "senders.ini", NULL);
	key_file = g_key_file_new ();
	g_key_file_load_from_file (key_file, filename, G_KEY_FILE_NONE, NULL);
	last = g_key_file_get_string_list (key_file, GROUP, KEY, NULL, NULL);

	/* What the user entered: everything not written here the last time */
	aliases = g_string_new (NULL);
	current = e_source_mail_identity_get_aliases_as_hash_table (extension);
	if (current) {
		g_hash_table_iter_init (&iter, current);
		while (g_hash_table_iter_next (&iter, &key, &value)) {
			if (!strv_contains_nocase (last, key))
				append_alias (aliases, value, key);
		}
	}

	own = e_gw_connection_get_user_email (cnc);
	ours = g_ptr_array_new ();
	for (ii = 0; ii < users->len; ii++) {
		EGwProxyUser *user = g_ptr_array_index (users, ii);

		if (!user->email || !*user->email || (own && g_ascii_strcasecmp (user->email, own) == 0))
			continue;
		/* An address the user entered himself stays his */
		if (current && g_hash_table_contains (current, user->email) && !strv_contains_nocase (last, user->email))
			continue;
		append_alias (aliases, user->display_name, user->email);
		g_ptr_array_add (ours, user->email);
	}

	old = e_source_mail_identity_dup_aliases (extension);
	if (g_strcmp0 (old && *old ? old : NULL, aliases->len ? aliases->str : NULL) != 0) {
		e_source_mail_identity_set_aliases (extension, aliases->len ? aliases->str : NULL);
		if (!e_source_write_sync (identity, NULL, &error)) {
			g_warning ("GroupWise: cannot store %s: %s", e_source_get_uid (identity), error->message);
			g_clear_error (&error);
		}
		g_debug ("senders of %s: %u from the proxy list", e_source_get_uid (identity), ours->len);
	}
	g_free (old);

	g_key_file_set_string_list (key_file, GROUP, KEY, (const gchar * const *) ours->pdata, ours->len);
	g_mkdir_with_parents (e_collection_backend_get_cache_dir (backend), 0700);
	g_key_file_save_to_file (key_file, filename, NULL);

	g_clear_pointer (&current, g_hash_table_destroy);
	g_ptr_array_unref (ours);
	g_ptr_array_unref (users);
	g_string_free (aliases, TRUE);
	g_strfreev (last);
	g_key_file_free (key_file);
	g_free (filename);
	g_object_unref (identity);
}

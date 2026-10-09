/*
 * gw-signatures.c: the GroupWise signatures as signatures of Evolution
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
 * From GroupWise to Evolution, at each login of the collection: a signature
 * source per GroupWise signature (a child of the collection, its GroupWise
 * ID in the GroupWise Folder extension with the checksum of the content as
 * synchronized), updated when the server has changed it, removed when the
 * server has not got it any more. The default signature of GroupWise
 * becomes the signature of the identity whenever it changed in GroupWise.
 *
 * The way back (edits, new and removed signatures in Evolution) goes from
 * Evolution itself, which has the user's password at hand.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-signature.h"
#include "e-gw-signature-mime.h"
#include "e-source-groupwise-folder.h"

#include "gw-signatures.h"

#define SIGNATURE_PREFIX "signature:"

static gchar *
signature_id (ESource *source)
{
	if (!e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
		return NULL;

	return e_source_groupwise_folder_dup_id (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
}

/* The signatures below the collection: GroupWise ID -> source; those still
 * without an ID (made in Evolution, not yet sent) are left alone */
static GHashTable *
list_signatures (ECollectionBackend *backend,
		 ESourceRegistryServer *server)
{
	ESource *collection = e_backend_get_source (E_BACKEND (backend));
	GHashTable *by_id = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
	GList *sources, *link;

	sources = e_source_registry_server_list_sources (server, E_SOURCE_EXTENSION_MAIL_SIGNATURE);
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;
		gchar *id;

		if (g_strcmp0 (e_source_get_parent (source), e_source_get_uid (collection)) != 0)
			continue;
		id = signature_id (source);
		if (id && *id)
			g_hash_table_insert (by_id, id, g_object_ref (source));
		else
			g_free (id);
	}
	g_list_free_full (sources, g_object_unref);

	return by_id;
}

static gboolean
write_content (ESource *source,
	       const gchar *content)
{
	GFile *file = e_source_mail_signature_get_file (e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE));
	GFile *parent = g_file_get_parent (file);
	GError *error = NULL;
	gboolean success;

	g_file_make_directory_with_parents (parent, NULL, NULL);
	g_object_unref (parent);
	success = g_file_replace_contents (file, content, strlen (content), NULL, FALSE, G_FILE_CREATE_PRIVATE,
		NULL, NULL, &error);
	if (!success) {
		g_warning ("GroupWise: cannot write signature %s: %s", e_source_get_uid (source), error->message);
		g_clear_error (&error);
	}

	return success;
}

static gchar *
signature_name (ECollectionBackend *backend,
		const EGwSignature *signature,
		const gchar *proxy)
{
	const gchar *name = signature->name && *signature->name ? signature->name : _("Signature");

	/* The signatures of all accounts are in one list: another user's are
	 * named with the account */
	if (proxy)
		return g_strdup_printf ("%s – %s", name, e_source_get_display_name (e_backend_get_source (E_BACKEND (backend))));

	return g_strdup (name);
}

/* The identity below the collection */
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

/* The default of GroupWise as the signature of the identity, when it
 * changed in GroupWise (then a choice made in Evolution stays) */
static void
apply_default (ECollectionBackend *backend,
	       ESource *default_source,
	       const gchar *default_id)
{
	gchar *filename = g_build_filename (e_collection_backend_get_cache_dir (backend), "signatures.ini", NULL);
	GKeyFile *key_file = g_key_file_new ();
	gchar *last;
	ESource *identity;

	g_key_file_load_from_file (key_file, filename, G_KEY_FILE_NONE, NULL);
	last = g_key_file_get_string (key_file, "Signatures", "Default", NULL);
	if (g_strcmp0 (last, default_id ? default_id : "") == 0) {
		g_free (last);
		g_key_file_free (key_file);
		g_free (filename);
		return;
	}

	identity = ref_identity (backend);
	if (identity) {
		ESourceMailIdentity *extension = e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY);
		GError *error = NULL;

		e_source_mail_identity_set_signature_uid (extension, default_source ? e_source_get_uid (default_source) : "none");
		if (!e_source_write_sync (identity, NULL, &error)) {
			g_warning ("GroupWise: cannot store %s: %s", e_source_get_uid (identity), error->message);
			g_clear_error (&error);
		}
		g_debug ("signature of %s: %s", e_source_get_uid (identity), default_source ? e_source_get_uid (default_source) : "none");
		g_object_unref (identity);
	}

	g_key_file_set_string (key_file, "Signatures", "Default", default_id ? default_id : "");
	g_key_file_save_to_file (key_file, filename, NULL);
	g_free (last);
	g_key_file_free (key_file);
	g_free (filename);
}

void
gw_collection_sync_signatures (ECollectionBackend *backend,
			       EGwConnection *cnc,
			       const gchar *proxy,
			       GCancellable *cancellable)
{
	ESourceRegistryServer *server = e_collection_backend_ref_server (backend);
	EGwSignatures *signatures;
	GHashTable *by_id;
	GList *gone, *link;
	ESource *default_source = NULL;
	const gchar *default_id = NULL;
	GError *error = NULL;
	guint ii;

	signatures = e_gw_connection_get_signatures_sync (cnc, cancellable, &error);
	if (!signatures) {
		g_warning ("GroupWise signatures of %s: %s", e_source_get_uid (e_backend_get_source (E_BACKEND (backend))),
			error->message);
		g_clear_error (&error);
		g_object_unref (server);
		return;
	}

	by_id = list_signatures (backend, server);
	for (ii = 0; ii < signatures->signatures->len; ii++) {
		EGwSignature *signature = signatures->signatures->pdata[ii];
		gchar *mime_type = NULL, *content, *checksum, *name;
		ESource *source;

		content = signature->mime ? e_gw_signature_mime_to_evolution (signature->mime, &mime_type) : NULL;
		if (!content)
			content = g_strdup (""), mime_type = g_strdup ("text/plain");
		checksum = e_gw_signature_checksum (content, mime_type);
		name = signature_name (backend, signature, proxy);

		source = g_hash_table_lookup (by_id, signature->id);
		if (source) {
			ESourceGroupwiseFolder *extension = e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER);
			gchar *old_checksum = e_source_groupwise_folder_dup_checksum (extension);
			gboolean changed = FALSE;

			if (g_strcmp0 (old_checksum, checksum) != 0) {
				/* Changed in GroupWise: the checksum first, then Evolution
				 * does not send the new content back */
				e_source_groupwise_folder_set_checksum (extension, checksum);
				e_source_mail_signature_set_mime_type (
					e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE), mime_type);
				changed = TRUE;
			}
			if (g_strcmp0 (e_source_get_display_name (source), name) != 0) {
				e_source_set_display_name (source, name);
				changed = TRUE;
			}
			if (changed && !e_source_write_sync (source, NULL, &error)) {
				g_warning ("GroupWise: cannot store %s: %s", e_source_get_uid (source), error->message);
				g_clear_error (&error);
			}
			if (g_strcmp0 (old_checksum, checksum) != 0)
				write_content (source, content);
			g_free (old_checksum);
			if (signature->is_default)
				default_source = g_object_ref (source);
			g_hash_table_remove (by_id, signature->id);
		} else {
			gchar *resource_id = g_strconcat (SIGNATURE_PREFIX, signature->id, NULL);
			ESourceGroupwiseFolder *extension;

			source = e_collection_backend_new_child (backend, resource_id);
			e_source_set_display_name (source, name);
			e_source_mail_signature_set_mime_type (
				e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE), mime_type);
			extension = e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER);
			e_source_groupwise_folder_set_id (extension, signature->id);
			e_source_groupwise_folder_set_checksum (extension, checksum);
			write_content (source, content);
			e_source_registry_server_add_source (server, source);
			if (signature->is_default)
				default_source = g_object_ref (source);
			g_object_unref (source);
			g_free (resource_id);
		}
		if (signature->is_default)
			default_id = signature->id;

		g_free (name);
		g_free (checksum);
		g_free (content);
		g_free (mime_type);
	}

	/* What is left is gone from GroupWise */
	gone = g_hash_table_get_values (by_id);
	for (link = gone; link; link = g_list_next (link)) {
		GFile *file = e_source_mail_signature_get_file (e_source_get_extension (link->data, E_SOURCE_EXTENSION_MAIL_SIGNATURE));

		g_file_delete (file, NULL, NULL);
		e_source_registry_server_remove_source (server, link->data);
	}
	g_list_free (gone);
	g_hash_table_destroy (by_id);

	/* Signatures switched off in GroupWise: no default to add */
	if (signatures->enabled)
		apply_default (backend, default_source, default_id);

	g_clear_object (&default_source);
	e_gw_signatures_free (signatures);
	g_object_unref (server);
}

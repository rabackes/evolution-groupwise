/*
 * e-groupwise-signature-sync.c: signatures edited in Evolution to GroupWise
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
 * The registry brings the GroupWise signatures as children of the account's
 * collection (gw-signatures.c). Evolution edits them where it edits every
 * signature; this sends the changes to GroupWise: a new content (the file
 * of the signature changed and its checksum differs from the one last
 * synchronized), a new name, a new signature below the collection (no
 * GroupWise ID yet), a removed one. A signature removed because its account
 * went is not removed in GroupWise.
 */

#include <glib/gi18n-lib.h>

#include "e-gw-backend-utils.h"
#include "e-gw-signature.h"
#include "e-gw-signature-mime.h"
#include "e-source-groupwise-folder.h"

#include "e-groupwise-signature-sync.h"
#include "e-groupwise-ui-utils.h"

/* The editor writes the source first, the content then */
#define PUSH_DELAY_SECONDS 2

typedef struct {
	ESourceRegistry *registry;
	GHashTable *monitors;	/* UID -> GFileMonitor */
	GHashTable *names;	/* UID -> name last seen */
	GHashTable *pending;	/* UID -> timeout ID */
} Sync;

static Sync *sync_data;

/* A signature of a GroupWise account: a child of a GroupWise collection */
static gboolean
is_groupwise_signature (ESourceRegistry *registry,
			ESource *source,
			gchar **out_collection_uid)
{
	ESource *parent;
	gboolean is_gw = FALSE;

	if (!e_source_has_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE) || !e_source_get_parent (source))
		return FALSE;

	parent = e_source_registry_ref_source (registry, e_source_get_parent (source));
	if (parent && e_source_has_extension (parent, E_SOURCE_EXTENSION_COLLECTION))
		is_gw = g_strcmp0 (e_source_backend_get_backend_name (
			e_source_get_extension (parent, E_SOURCE_EXTENSION_COLLECTION)), "groupwise") == 0;
	if (is_gw && out_collection_uid)
		*out_collection_uid = g_strdup (e_source_get_uid (parent));
	g_clear_object (&parent);

	return is_gw;
}

static gchar *
dup_signature_id (ESource *source)
{
	gchar *id;

	if (!e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
		return NULL;

	id = e_source_groupwise_folder_dup_id (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
	if (id && !*id)
		g_clear_pointer (&id, g_free);

	return id;
}

/* ------------------------------------------------------------------ */
/* Sending */

static void
push_thread (GTask *task,
	     gpointer source_object,
	     gpointer task_data,
	     GCancellable *cancellable)
{
	ESource *source = task_data;
	ESourceGroupwiseFolder *extension = e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER);
	const gchar *mime_type = e_source_mail_signature_get_mime_type (
		e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE));
	gchar *content = NULL, *checksum, *old_checksum, *id;
	EGwConnection *cnc;
	GError *error = NULL;
	gboolean success = TRUE, changed = FALSE;

	if (!mime_type)
		mime_type = "text/html";
	if (!e_source_mail_signature_load_sync (source, &content, NULL, cancellable, &error)) {
		/* Not written yet */
		g_debug ("signature %s: %s", e_source_get_uid (source), error->message);
		g_clear_error (&error);
		g_task_return_boolean (task, FALSE);
		return;
	}

	checksum = e_gw_signature_checksum (content, mime_type);
	old_checksum = e_source_groupwise_folder_dup_checksum (extension);
	id = dup_signature_id (source);

	cnc = e_groupwise_ui_connect_sync (sync_data->registry, source, cancellable, &error);
	if (!cnc) {
		success = FALSE;
	} else if (!id) {
		GBytes *mime = e_gw_signature_mime_from_evolution (content, mime_type);

		id = e_gw_connection_create_signature_sync (cnc, e_source_get_display_name (source), mime, FALSE,
			cancellable, &error);
		g_bytes_unref (mime);
		success = id != NULL;
		if (success) {
			e_source_groupwise_folder_set_id (extension, id);
			changed = TRUE;
		}
		g_debug ("signature %s created in GroupWise: %s", e_source_get_uid (source), id ? id : error->message);
	} else {
		GBytes *mime = g_strcmp0 (checksum, old_checksum) != 0 ? e_gw_signature_mime_from_evolution (content, mime_type) : NULL;

		/* The name always, the content when it changed */
		success = e_gw_connection_modify_signature_sync (cnc, id, e_source_get_display_name (source), mime, -1,
			cancellable, &error);
		changed = mime != NULL;
		g_debug ("signature %s changed in GroupWise: %s", id, success ? (mime ? "name and content" : "name") : error->message);
		g_clear_pointer (&mime, g_bytes_unref);
	}
	if (cnc) {
		e_gw_connection_logout_sync (cnc, NULL);
		g_object_unref (cnc);
	}

	if (success && changed) {
		e_source_groupwise_folder_set_checksum (extension, checksum);
		if (!e_source_write_sync (source, cancellable, &error)) {
			g_warning ("GroupWise: cannot store %s: %s", e_source_get_uid (source), error->message);
			g_clear_error (&error);
		}
	}

	g_free (id);
	g_free (old_checksum);
	g_free (checksum);
	g_free (content);

	if (error)
		g_task_return_error (task, error);
	else
		g_task_return_boolean (task, success);
}

static void
push_done (GObject *source_object,
	   GAsyncResult *result,
	   gpointer user_data)
{
	GError *error = NULL;

	if (!g_task_propagate_boolean (G_TASK (result), &error) && error) {
		g_warning ("GroupWise: the signature cannot be saved in GroupWise: %s", error->message);
		g_clear_error (&error);
	}
}

static gboolean
push_timeout_cb (gpointer user_data)
{
	gchar *uid = user_data;
	ESource *source = e_source_registry_ref_source (sync_data->registry, uid);

	g_hash_table_remove (sync_data->pending, uid);
	if (source) {
		GTask *task = g_task_new (NULL, NULL, push_done, NULL);

		/* Sent with the name it has now */
		g_hash_table_insert (sync_data->names, g_strdup (uid), g_strdup (e_source_get_display_name (source)));
		g_task_set_task_data (task, source, g_object_unref);
		g_task_run_in_thread (task, push_thread);
		g_object_unref (task);
	}

	return G_SOURCE_REMOVE;
}

static void
schedule_push (ESource *source)
{
	const gchar *uid = e_source_get_uid (source);
	guint id;

	id = GPOINTER_TO_UINT (g_hash_table_lookup (sync_data->pending, uid));
	if (id)
		g_source_remove (id);
	id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, PUSH_DELAY_SECONDS, push_timeout_cb, g_strdup (uid), g_free);
	g_hash_table_insert (sync_data->pending, g_strdup (uid), GUINT_TO_POINTER (id));
}

/* The file changed: send it when it differs from what was synchronized */
static void
file_changed_cb (GFileMonitor *monitor,
		 GFile *file,
		 GFile *other_file,
		 GFileMonitorEvent event,
		 gpointer user_data)
{
	const gchar *uid = user_data;
	ESource *source;
	gchar *content = NULL, *checksum, *old_checksum;
	const gchar *mime_type;

	if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_CREATED)
		return;

	source = e_source_registry_ref_source (sync_data->registry, uid);
	if (!source)
		return;

	mime_type = e_source_mail_signature_get_mime_type (e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE));
	if (g_file_load_contents (file, NULL, &content, NULL, NULL, NULL)) {
		checksum = e_gw_signature_checksum (content, mime_type ? mime_type : "text/html");
		old_checksum = e_source_groupwise_folder_dup_checksum (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
		if (g_strcmp0 (checksum, old_checksum) != 0)
			schedule_push (source);
		g_free (old_checksum);
		g_free (checksum);
		g_free (content);
	}
	g_object_unref (source);
}

static void
free_uid (gpointer data,
	  GClosure *closure)
{
	g_free (data);
}

static void
watch (ESource *source)
{
	GFile *file = e_source_mail_signature_get_file (e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE));
	GFileMonitor *monitor;

	if (g_hash_table_contains (sync_data->monitors, e_source_get_uid (source)))
		return;

	monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, NULL);
	if (monitor) {
		g_signal_connect_data (monitor, "changed", G_CALLBACK (file_changed_cb),
			g_strdup (e_source_get_uid (source)), free_uid, 0);
		g_hash_table_insert (sync_data->monitors, g_strdup (e_source_get_uid (source)), monitor);
	}
	g_hash_table_insert (sync_data->names, g_strdup (e_source_get_uid (source)),
		g_strdup (e_source_get_display_name (source)));
}

/* ------------------------------------------------------------------ */
/* Removing */

typedef struct {
	gchar *id;
	gchar *collection_uid;
	ESource *source;
} Removal;

static void
removal_free (Removal *removal)
{
	g_free (removal->id);
	g_free (removal->collection_uid);
	g_object_unref (removal->source);
	g_free (removal);
}

static void
remove_thread (GTask *task,
	       gpointer source_object,
	       gpointer task_data,
	       GCancellable *cancellable)
{
	Removal *removal = task_data;
	ESource *collection = e_source_registry_ref_source (sync_data->registry, removal->collection_uid);
	EGwConnection *cnc;
	GError *error = NULL;
	gboolean success;

	/* The account went (or is going): its signatures stay in GroupWise */
	if (!collection) {
		g_debug ("signature %s: account removed, kept in GroupWise", removal->id);
		g_task_return_boolean (task, TRUE);
		return;
	}

	cnc = e_groupwise_ui_connect_sync (sync_data->registry, collection, cancellable, &error);
	success = cnc && e_gw_connection_remove_signature_sync (cnc, removal->id, cancellable, &error);
	g_debug ("signature %s removed in GroupWise: %s", removal->id, success ? "yes" : error->message);
	if (cnc) {
		e_gw_connection_logout_sync (cnc, NULL);
		g_object_unref (cnc);
	}
	g_object_unref (collection);

	/* Gone already (removed in GroupWise, then here) is fine */
	if (!success && g_error_matches (error, E_GW_ERROR, E_GW_ERROR_ITEM_NOT_FOUND))
		g_clear_error (&error), success = TRUE;
	if (error)
		g_task_return_error (task, error);
	else
		g_task_return_boolean (task, success);
}

static gboolean
remove_timeout_cb (gpointer user_data)
{
	GTask *task = user_data;

	g_task_run_in_thread (task, remove_thread);
	g_object_unref (task);

	return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */

static void
source_added_cb (ESourceRegistry *registry,
		 ESource *source,
		 gpointer user_data)
{
	gchar *id;

	if (!is_groupwise_signature (registry, source, NULL))
		return;

	watch (source);
	/* Made in Evolution: to GroupWise */
	id = dup_signature_id (source);
	if (!id)
		schedule_push (source);
	g_free (id);
}

static void
source_changed_cb (ESourceRegistry *registry,
		   ESource *source,
		   gpointer user_data)
{
	const gchar *name;

	if (!g_hash_table_contains (sync_data->names, e_source_get_uid (source)))
		return;

	/* Renamed in Evolution */
	name = g_hash_table_lookup (sync_data->names, e_source_get_uid (source));
	if (g_strcmp0 (name, e_source_get_display_name (source)) != 0)
		schedule_push (source);
}

static void
source_removed_cb (ESourceRegistry *registry,
		   ESource *source,
		   gpointer user_data)
{
	const gchar *uid = e_source_get_uid (source);
	gchar *id, *collection_uid = NULL;
	guint pending;

	if (!g_hash_table_contains (sync_data->names, uid))
		return;

	g_hash_table_remove (sync_data->monitors, uid);
	g_hash_table_remove (sync_data->names, uid);
	pending = GPOINTER_TO_UINT (g_hash_table_lookup (sync_data->pending, uid));
	if (pending) {
		g_source_remove (pending);
		g_hash_table_remove (sync_data->pending, uid);
	}

	id = dup_signature_id (source);
	if (id && e_source_get_parent (source)) {
		Removal *removal = g_new0 (Removal, 1);
		GTask *task = g_task_new (NULL, NULL, push_done, NULL);

		removal->id = g_steal_pointer (&id);
		removal->collection_uid = g_strdup (e_source_get_parent (source));
		removal->source = g_object_ref (source);
		g_task_set_task_data (task, removal, (GDestroyNotify) removal_free);
		/* Later: when the whole account goes, the collection is gone then */
		g_timeout_add_seconds (PUSH_DELAY_SECONDS, remove_timeout_cb, task);
	}
	g_free (id);
	g_free (collection_uid);
}

void
e_groupwise_signature_sync_start (ESourceRegistry *registry)
{
	GList *sources, *link;

	g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));

	if (sync_data)
		return;

	/* The GroupWise Folder extension of the signatures */
	e_gw_backend_ensure_types ();

	sync_data = g_new0 (Sync, 1);
	sync_data->registry = g_object_ref (registry);
	sync_data->monitors = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
	sync_data->names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	sync_data->pending = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	sources = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_MAIL_SIGNATURE);
	for (link = sources; link; link = g_list_next (link)) {
		gchar *id;

		if (!is_groupwise_signature (registry, link->data, NULL))
			continue;
		watch (link->data);
		/* Made while Evolution was not running? */
		id = dup_signature_id (link->data);
		if (!id)
			schedule_push (link->data);
		g_free (id);
	}
	g_list_free_full (sources, g_object_unref);

	g_signal_connect (registry, "source-added", G_CALLBACK (source_added_cb), NULL);
	g_signal_connect (registry, "source-changed", G_CALLBACK (source_changed_cb), NULL);
	g_signal_connect (registry, "source-removed", G_CALLBACK (source_removed_cb), NULL);
}

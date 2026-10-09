/*
 * camel-groupwise-store-summary.c: the mail folders of a GroupWise account
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

#include <string.h>

#include <glib/gi18n-lib.h>

#include "camel-groupwise-store-summary.h"

struct _CamelGroupwiseStoreSummary {
	GMutex lock;
	gchar *filename;
	GPtrArray *records;	/* CamelGroupwiseFolderRecord */
};

void
camel_groupwise_folder_record_free (CamelGroupwiseFolderRecord *record)
{
	if (record) {
		g_free (record->id);
		g_free (record->full_name);
		g_free (record);
	}
}

static gboolean
is_mail_type (EGwFolderType type)
{
	switch (type) {
	case E_GW_FOLDER_TYPE_NORMAL:
	case E_GW_FOLDER_TYPE_MAILBOX:
	case E_GW_FOLDER_TYPE_SENT_ITEMS:
	case E_GW_FOLDER_TYPE_DRAFT:
	case E_GW_FOLDER_TYPE_TRASH:
	case E_GW_FOLDER_TYPE_JUNK:
	case E_GW_FOLDER_TYPE_CABINET:
	case E_GW_FOLDER_TYPE_CHECKLIST:
	/* Search result folders ("all mail"): views onto items of the others */
	case E_GW_FOLDER_TYPE_QUERY:
		return TRUE;
	default:
		return FALSE;
	}
}

static EGwFolder *
find_folder (GHashTable *by_id,
	     const gchar *id)
{
	return id ? g_hash_table_lookup (by_id, id) : NULL;
}

/* NULL when the folder or one of its ancestors is no mail folder, or when
 * a parent is gone: shared folders whose owner removed the folder above them
 * stay in the list, the GroupWise client leaves them out */
static gchar *
build_full_name (GHashTable *by_id,
		 EGwFolder *folder,
		 guint depth)
{
	EGwFolder *parent;
	gchar *name, *parent_name, *full_name;

	/* A cycle in broken data must not hang the account */
	if (depth > 64 || folder->type == E_GW_FOLDER_TYPE_ROOT || !is_mail_type (folder->type))
		return NULL;

	/* '/' separates the levels of Camel full names */
	name = g_strdup (folder->name && *folder->name ? folder->name : folder->id);
	g_strdelimit (name, "/", '_');

	parent = find_folder (by_id, folder->parent_id);
	if (!parent && folder->parent_id && *folder->parent_id) {
		g_free (name);
		return NULL;
	}
	if (!parent || parent->type == E_GW_FOLDER_TYPE_ROOT)
		return name;

	parent_name = build_full_name (by_id, parent, depth + 1);
	if (!parent_name) {
		g_free (name);
		return NULL;
	}

	full_name = g_strconcat (parent_name, "/", name, NULL);
	g_free (parent_name);
	g_free (name);

	return full_name;
}

GPtrArray *
camel_groupwise_folder_records_from_folders (GPtrArray *folders)
{
	GHashTable *by_id, *full_names;
	GPtrArray *records;
	guint ii;

	by_id = g_hash_table_new (g_str_hash, g_str_equal);
	full_names = g_hash_table_new (g_str_hash, g_str_equal);
	records = g_ptr_array_new_with_free_func ((GDestroyNotify) camel_groupwise_folder_record_free);

	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];

		g_hash_table_insert (by_id, folder->id, folder);
	}

	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];
		CamelGroupwiseFolderRecord *record;
		gchar *full_name = build_full_name (by_id, folder, 0);

		/* The root is the parent of new top level folders; its full name is "" */
		if (!full_name && folder->type == E_GW_FOLDER_TYPE_ROOT)
			full_name = g_strdup ("");
		if (!full_name)
			continue;

		/* Two siblings with one name: keep both apart */
		if (g_hash_table_contains (full_names, full_name)) {
			gchar *unique = g_strdup_printf ("%s (%s)", full_name, folder->id);

			g_free (full_name);
			full_name = unique;
		}

		record = g_new0 (CamelGroupwiseFolderRecord, 1);
		record->id = g_strdup (folder->id);
		record->full_name = full_name;
		record->type = folder->type;
		record->total = folder->count;
		record->unread = folder->unread_count;
		g_ptr_array_add (records, record);
		g_hash_table_add (full_names, record->full_name);
	}

	g_hash_table_destroy (full_names);
	g_hash_table_destroy (by_id);

	return records;
}

static void
load (CamelGroupwiseStoreSummary *summary)
{
	GKeyFile *key_file = g_key_file_new ();
	gchar **groups;
	guint ii;

	if (!g_key_file_load_from_file (key_file, summary->filename, G_KEY_FILE_NONE, NULL)) {
		g_key_file_free (key_file);
		return;
	}

	groups = g_key_file_get_groups (key_file, NULL);
	for (ii = 0; groups[ii]; ii++) {
		CamelGroupwiseFolderRecord *record = g_new0 (CamelGroupwiseFolderRecord, 1);

		record->id = g_strdup (groups[ii]);
		record->full_name = g_key_file_get_string (key_file, groups[ii], "FullName", NULL);
		record->type = g_key_file_get_integer (key_file, groups[ii], "Type", NULL);
		record->total = g_key_file_get_int64 (key_file, groups[ii], "Total", NULL);
		record->unread = g_key_file_get_int64 (key_file, groups[ii], "Unread", NULL);

		if (record->full_name)
			g_ptr_array_add (summary->records, record);
		else
			camel_groupwise_folder_record_free (record);
	}

	g_strfreev (groups);
	g_key_file_free (key_file);
}

CamelGroupwiseStoreSummary *
camel_groupwise_store_summary_new (const gchar *filename)
{
	CamelGroupwiseStoreSummary *summary;

	g_return_val_if_fail (filename != NULL, NULL);

	summary = g_new0 (CamelGroupwiseStoreSummary, 1);
	g_mutex_init (&summary->lock);
	summary->filename = g_strdup (filename);
	summary->records = g_ptr_array_new_with_free_func ((GDestroyNotify) camel_groupwise_folder_record_free);
	load (summary);

	return summary;
}

void
camel_groupwise_store_summary_free (CamelGroupwiseStoreSummary *summary)
{
	if (!summary)
		return;

	g_ptr_array_unref (summary->records);
	g_free (summary->filename);
	g_mutex_clear (&summary->lock);
	g_free (summary);
}

gboolean
camel_groupwise_store_summary_replace (CamelGroupwiseStoreSummary *summary,
				       GPtrArray *records,
				       GError **error)
{
	GKeyFile *key_file = g_key_file_new ();
	gchar *dirname;
	gboolean success;
	guint ii;

	g_mutex_lock (&summary->lock);

	g_ptr_array_unref (summary->records);
	summary->records = records;

	for (ii = 0; ii < records->len; ii++) {
		CamelGroupwiseFolderRecord *record = records->pdata[ii];

		g_key_file_set_string (key_file, record->id, "FullName", record->full_name);
		g_key_file_set_integer (key_file, record->id, "Type", record->type);
		g_key_file_set_int64 (key_file, record->id, "Total", record->total);
		g_key_file_set_int64 (key_file, record->id, "Unread", record->unread);
	}

	dirname = g_path_get_dirname (summary->filename);
	g_mkdir_with_parents (dirname, 0700);
	g_free (dirname);

	success = g_key_file_save_to_file (key_file, summary->filename, error);

	g_mutex_unlock (&summary->lock);
	g_key_file_free (key_file);

	return success;
}

gboolean
camel_groupwise_store_summary_is_empty (CamelGroupwiseStoreSummary *summary)
{
	gboolean empty;

	g_mutex_lock (&summary->lock);
	empty = summary->records->len == 0;
	g_mutex_unlock (&summary->lock);

	return empty;
}

/* Call with the lock held */
static CamelGroupwiseFolderRecord *
find_record (CamelGroupwiseStoreSummary *summary,
	     const gchar *full_name,
	     const gchar *id,
	     gint type)
{
	guint ii;

	for (ii = 0; ii < summary->records->len; ii++) {
		CamelGroupwiseFolderRecord *record = summary->records->pdata[ii];

		if ((full_name && g_strcmp0 (record->full_name, full_name) == 0) ||
		    (id && g_strcmp0 (record->id, id) == 0) ||
		    (type >= 0 && record->type == (EGwFolderType) type))
			return record;
	}

	return NULL;
}

gchar *
camel_groupwise_store_summary_dup_id (CamelGroupwiseStoreSummary *summary,
				      const gchar *full_name)
{
	CamelGroupwiseFolderRecord *record;
	gchar *id;

	g_mutex_lock (&summary->lock);
	record = find_record (summary, full_name, NULL, -1);
	id = record ? g_strdup (record->id) : NULL;
	g_mutex_unlock (&summary->lock);

	return id;
}

gchar *
camel_groupwise_store_summary_dup_full_name (CamelGroupwiseStoreSummary *summary,
					     const gchar *id)
{
	CamelGroupwiseFolderRecord *record;
	gchar *full_name;

	g_mutex_lock (&summary->lock);
	record = find_record (summary, NULL, id, -1);
	full_name = record ? g_strdup (record->full_name) : NULL;
	g_mutex_unlock (&summary->lock);

	return full_name;
}

gchar *
camel_groupwise_store_summary_dup_full_name_by_type (CamelGroupwiseStoreSummary *summary,
						     EGwFolderType type)
{
	CamelGroupwiseFolderRecord *record;
	gchar *full_name;

	g_mutex_lock (&summary->lock);
	record = find_record (summary, NULL, NULL, type);
	full_name = record ? g_strdup (record->full_name) : NULL;
	g_mutex_unlock (&summary->lock);

	return full_name;
}

EGwFolderType
camel_groupwise_store_summary_get_type (CamelGroupwiseStoreSummary *summary,
					const gchar *full_name)
{
	CamelGroupwiseFolderRecord *record;
	EGwFolderType type;

	g_mutex_lock (&summary->lock);
	record = find_record (summary, full_name, NULL, -1);
	type = record ? record->type : E_GW_FOLDER_TYPE_OTHER;
	g_mutex_unlock (&summary->lock);

	return type;
}

/* The system folders under the names the GroupWise client shows in the
 * user's language; the full names stay those of the server. Only under the
 * English name the POA reports: a folder the user named stays as named. */
static gchar *
dup_display_name (const CamelGroupwiseFolderRecord *record)
{
	static const struct {
		EGwFolderType type;
		const gchar *name;
		const gchar *display_name;
	} system_folders[] = {
		{ E_GW_FOLDER_TYPE_MAILBOX, "Mailbox", N_("Mailbox") },
		{ E_GW_FOLDER_TYPE_JUNK, "Junk Mail", N_("Junk Mail") },
		{ E_GW_FOLDER_TYPE_TRASH, "Trash", N_("Trash") },
		{ E_GW_FOLDER_TYPE_SENT_ITEMS, "Sent Items", N_("Sent Items") },
		{ E_GW_FOLDER_TYPE_DRAFT, "Work In Progress", N_("Work In Progress") },
		{ E_GW_FOLDER_TYPE_CABINET, "Cabinet", N_("Cabinet") },
		{ E_GW_FOLDER_TYPE_CHECKLIST, "Tasklist", N_("Tasklist") },
		{ E_GW_FOLDER_TYPE_CHECKLIST, "Checklist", N_("Tasklist") },
		{ E_GW_FOLDER_TYPE_CALENDAR, "Calendar", N_("Calendar") },
		{ E_GW_FOLDER_TYPE_CONTACTS, "Contacts", N_("Contacts") },
		{ E_GW_FOLDER_TYPE_DOCUMENTS, "Documents", N_("Documents") }
	};
	const gchar *leaf = strrchr (record->full_name, '/') ? strrchr (record->full_name, '/') + 1 : record->full_name;
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (system_folders); ii++) {
		if (record->type == system_folders[ii].type && g_ascii_strcasecmp (leaf, system_folders[ii].name) == 0)
			return g_strdup (_(system_folders[ii].display_name));
	}

	return g_strdup (leaf);
}

static guint32
folder_info_flags (EGwFolderType type)
{
	switch (type) {
	case E_GW_FOLDER_TYPE_MAILBOX:
		return CAMEL_FOLDER_TYPE_INBOX;
	case E_GW_FOLDER_TYPE_SENT_ITEMS:
		return CAMEL_FOLDER_TYPE_SENT;
	case E_GW_FOLDER_TYPE_DRAFT:
		return CAMEL_FOLDER_TYPE_DRAFTS;
	case E_GW_FOLDER_TYPE_TRASH:
		return CAMEL_FOLDER_TYPE_TRASH;
	case E_GW_FOLDER_TYPE_JUNK:
		return CAMEL_FOLDER_TYPE_JUNK;
	default:
		return CAMEL_FOLDER_TYPE_NORMAL;
	}
}

CamelFolderInfo *
camel_groupwise_store_summary_build_folder_info (CamelGroupwiseStoreSummary *summary,
						 const gchar *top,
						 gboolean recursive)
{
	GPtrArray *infos = g_ptr_array_new ();
	CamelFolderInfo *result;
	gsize top_len = top && *top ? strlen (top) : 0;
	guint ii;

	g_mutex_lock (&summary->lock);

	for (ii = 0; ii < summary->records->len; ii++) {
		CamelGroupwiseFolderRecord *record = summary->records->pdata[ii];
		const gchar *below = record->full_name;
		CamelFolderInfo *fi;

		if (record->type == E_GW_FOLDER_TYPE_ROOT)
			continue;

		if (top_len) {
			if (g_strcmp0 (record->full_name, top) != 0 &&
			    (strncmp (record->full_name, top, top_len) != 0 || record->full_name[top_len] != '/'))
				continue;
			below = record->full_name + top_len;
		}
		if (!recursive && strchr (*below == '/' ? below + 1 : below, '/'))
			continue;

		fi = camel_folder_info_new ();
		fi->full_name = g_strdup (record->full_name);
		fi->display_name = dup_display_name (record);
		fi->flags = folder_info_flags (record->type);
		fi->total = record->total >= 0 ? (gint) record->total : -1;
		fi->unread = record->unread >= 0 ? (gint) record->unread : -1;
		g_ptr_array_add (infos, fi);
	}

	g_mutex_unlock (&summary->lock);

	/* Builds the tree from the full names and sets the child flags */
	result = camel_folder_info_build (infos, top ? top : "", '/', TRUE);
	g_ptr_array_free (infos, TRUE);

	return result;
}

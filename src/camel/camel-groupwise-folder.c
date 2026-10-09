/*
 * camel-groupwise-folder.c: a GroupWise mail folder
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
 * The message list comes from SOAP (a cursor over the folder), the messages
 * themselves as RFC 822 from the streaming interface of the POA.
 */

#include <errno.h>
#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-category.h"
#include "e-gw-junk.h"
#include "e-gw-xml.h"

#include "camel-groupwise-folder.h"
#include "camel-groupwise-message-info.h"
#include "camel-groupwise-mime.h"
#include "camel-groupwise-settings.h"
#include "camel-groupwise-store.h"
#include "camel-groupwise-utils.h"

#define CACHE_PATH "cur"
#define PAGE_SIZE 250		/* full summaries */
#define STATE_PAGE_SIZE 1000	/* id, modified, status only */
#define FULL_SCAN_SECONDS (15 * 60)	/* between them only getQuickMessages */
#define MARK_BATCH 100

struct _CamelGroupwiseFolder {
	CamelOfflineFolder parent;

	gchar *id;
	CamelDataCache *cache;

	GMutex search_lock;
	CamelFolderSearch *search;

	/* One refresh at a time; Evolution may ask from several threads */
	GMutex refresh_lock;

	/* Between full scans, changes come from getQuickMessages since this
	 * server time (not in the trash: the POA does not support it there) */
	gboolean quick_supported;
	gboolean is_trash;
	gboolean is_drafts;
	gboolean is_junk;
	/* Messages the user marked junk (TRUE) or not junk (FALSE): Camel
	 * clears the "learn" flag soon, the next synchronization tells the
	 * junk lists of GroupWise */
	GMutex learn_lock;
	GHashTable *learn;	/* UID -> GINT_TO_POINTER (junk) */
	/* A search result folder: a view onto items of other folders, whose IDs
	 * name the folder they are in */
	gboolean is_query;
	gchar *quick_since;
	gint64 last_full_scan;	/* monotonic, microseconds */

	/* IDs of appointments, tasks and notes in the folder: the POA takes an
	 * invitation out of the Mailbox once it is answered, which the quick
	 * check does not report */
	GHashTable *invitations;
};

G_DEFINE_TYPE (CamelGroupwiseFolder, camel_groupwise_folder, CAMEL_TYPE_OFFLINE_FOLDER)

const gchar *
camel_groupwise_folder_get_id (CamelGroupwiseFolder *folder)
{
	g_return_val_if_fail (CAMEL_IS_GROUPWISE_FOLDER (folder), NULL);

	return folder->id;
}

static CamelGroupwiseStore *
folder_ref_store (CamelFolder *folder)
{
	CamelStore *store = camel_folder_get_parent_store (folder);

	return store ? g_object_ref (CAMEL_GROUPWISE_STORE (store)) : NULL;
}

static gboolean
folder_is_online (CamelFolder *folder)
{
	CamelStore *store = camel_folder_get_parent_store (folder);

	return store && camel_offline_store_get_online (CAMEL_OFFLINE_STORE (store));
}

static gboolean
store_is_proxy (CamelStore *store)
{
	CamelSettings *settings = camel_service_ref_settings (CAMEL_SERVICE (store));
	gchar *proxy = camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (settings));
	gboolean is_proxy = proxy != NULL;

	g_free (proxy);
	g_object_unref (settings);

	return is_proxy;
}

static gboolean
folder_is_read_only (CamelFolder *folder)
{
	CamelStore *store = camel_folder_get_parent_store (folder);

	return !store || camel_groupwise_store_get_read_only (CAMEL_GROUPWISE_STORE (store));
}

static EGwConnection *
folder_ref_connection_sync (CamelFolder *folder,
			    GCancellable *cancellable,
			    GError **error)
{
	CamelGroupwiseStore *store = folder_ref_store (folder);
	EGwConnection *cnc;

	if (!store) {
		g_set_error_literal (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID, _("The folder has no store"));
		return NULL;
	}

	cnc = camel_groupwise_store_ref_connection_sync (store, cancellable, error);
	g_object_unref (store);

	return cnc;
}

/* ------------------------------------------------------------------ */
/* Refresh */

static void	apply_tasklist			(CamelFolder *folder,
						 EGwConnection *cnc,
						 CamelFolderChangeInfo *changes,
						 GCancellable *cancellable);

typedef struct {
	guint32 flags;
	gchar *modified;
	gchar *subject;		/* NULL: not in the view */
	gchar **categories;
	gboolean calendar;
} ServerState;

static void
server_state_free (ServerState *state)
{
	g_free (state->modified);
	g_free (state->subject);
	g_strfreev (state->categories);
	g_free (state);
}

static gboolean
collect_state_cb (xmlNode *item,
		  gpointer user_data)
{
	GHashTable *server = user_data;
	ServerState *state;
	gchar *id = camel_groupwise_item_dup_id (item);

	if (!id)
		return TRUE;

	state = g_new0 (ServerState, 1);
	state->flags = camel_groupwise_item_get_flags (item);
	state->modified = e_gw_xml_dup_text (item, "modified");
	state->subject = e_gw_xml_find (item, "subject") ? e_gw_xml_dup_text (item, "subject") : NULL;
	state->categories = e_gw_item_dup_categories (item);
	state->calendar = camel_groupwise_item_is_calendar (item);
	g_hash_table_replace (server, id, state);

	return TRUE;
}

typedef struct {
	CamelFolderSummary *summary;
	CamelFolderChangeInfo *changes;
	GHashTable *wanted;		/* IDs to add; NULL adds every unknown item */
	gboolean mark_recent;
	GHashTable *invitations;	/* gets the calendar items */
	CamelGroupwiseLabels *labels;
} AddData;

static gboolean
add_item_cb (xmlNode *item,
	     gpointer user_data)
{
	AddData *data = user_data;
	CamelMessageInfo *info;
	gchar *id = camel_groupwise_item_dup_id (item);

	if (!id || (data->wanted && !g_hash_table_contains (data->wanted, id)) ||
	    camel_folder_summary_check_uid (data->summary, id)) {
		g_free (id);
		return TRUE;
	}

	info = camel_groupwise_message_info_new_from_item (data->summary, item);
	if (info) {
		gchar **categories = e_gw_item_dup_categories (item);

		camel_groupwise_labels_apply (data->labels, info, (const gchar * const *) categories);
		camel_groupwise_message_info_set_server_categories (CAMEL_GROUPWISE_MESSAGE_INFO (info),
			(const gchar * const *) categories);
		g_strfreev (categories);
	}
	if (info && data->invitations && camel_groupwise_item_is_calendar (item))
		g_hash_table_add (data->invitations, g_strdup (id));
	if (info) {
		camel_folder_summary_add (data->summary, info, TRUE);
		camel_folder_change_info_add_uid (data->changes, id);
		if (data->mark_recent && !(camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN))
			camel_folder_change_info_recent_uid (data->changes, id);
		g_object_unref (info);
	}
	g_free (id);

	return TRUE;
}

/* Whether the labels of @info are the categories @ids */
static gboolean
categories_equal (CamelGroupwiseLabels *labels,
		  CamelMessageInfo *info,
		  const gchar * const *ids)
{
	gchar **tags = camel_groupwise_labels_dup_info_tags (labels, info);
	guint ii, n_known = 0;
	gboolean equal = TRUE;

	for (ii = 0; ids && ids[ii]; ii++) {
		gchar *tag = camel_groupwise_labels_dup_tag (labels, ids[ii]);

		if (tag) {
			n_known++;
			equal = equal && g_strv_contains ((const gchar * const *) tags, tag);
		}
		g_free (tag);
	}
	equal = equal && g_strv_length (tags) == n_known;
	g_strfreev (tags);

	return equal;
}

/* Merges the server flags into @info: bits the user changed and did not
 * write back yet stay as they are, all others follow the server.
 * Returns whether the visible flags changed. */
static gboolean
take_server_flags (CamelGroupwiseMessageInfo *info,
		   guint32 server)
{
	CamelMessageInfo *mi = CAMEL_MESSAGE_INFO (info);
	guint32 known = camel_groupwise_message_info_get_server_flags (info) & CAMEL_GROUPWISE_SERVER_FLAGS;
	guint32 local = camel_message_info_get_flags (mi) & CAMEL_GROUPWISE_SERVER_FLAGS;
	guint32 user_changed = camel_message_info_get_folder_flagged (mi) ? (local ^ known) : 0;
	guint32 merged = (server & ~user_changed) | (local & user_changed);
	gboolean was_flagged = camel_message_info_get_folder_flagged (mi);

	camel_groupwise_message_info_set_server_flags (info, server);

	if (merged == local)
		return FALSE;

	camel_message_info_set_flags (mi, CAMEL_GROUPWISE_SERVER_FLAGS, merged);
	/* The server's own change is nothing to write back */
	if (!was_flagged)
		camel_message_info_set_folder_flagged (mi, FALSE);

	return TRUE;
}

/* The labels after the categories of the server, unless the user changed
 * them and did not write them back. Returns whether they changed. */
static gboolean
take_server_categories (CamelGroupwiseLabels *labels,
			CamelGroupwiseMessageInfo *info,
			const gchar * const *server)
{
	CamelMessageInfo *mi = CAMEL_MESSAGE_INFO (info);
	gboolean was_flagged = camel_message_info_get_folder_flagged (mi);
	gchar **known = camel_groupwise_message_info_dup_server_categories (info);
	gboolean changed = FALSE;

	if (!was_flagged || categories_equal (labels, mi, (const gchar * const *) known)) {
		changed = camel_groupwise_labels_apply (labels, mi, server);
		/* The server's own change is nothing to write back */
		if (changed && !was_flagged)
			camel_message_info_set_folder_flagged (mi, FALSE);
	}
	camel_groupwise_message_info_set_server_categories (info, server);
	g_strfreev (known);

	return changed;
}

static gchar *
modified_filter (const gchar *since)
{
	GString *filter = g_string_new ("<filter><element type=\"FilterEntry\">");

	e_gw_xml_add_leaf (filter, "op", "gte");
	e_gw_xml_add_leaf (filter, "field", "modified");
	e_gw_xml_add_leaf (filter, "value", since);
	g_string_append (filter, "</element></filter>");

	return g_string_free (filter, FALSE);
}

/* Takes over the server state of @server (ID -> ServerState) into the summary:
 * flags of known items (except bits the user changed and did not write back),
 * new items read with the full view (those modified since the oldest new one).
 * With @complete, @server is the whole folder: what is missing was deleted. */
static gboolean
apply_server_state (CamelFolder *folder,
		    EGwConnection *cnc,
		    GHashTable *server,
		    gboolean complete,
		    CamelFolderChangeInfo *changes,
		    GCancellable *cancellable,
		    GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	GHashTable *wanted = g_hash_table_new (g_str_hash, g_str_equal);
	CamelGroupwiseLabels *labels = camel_groupwise_store_get_labels (CAMEL_GROUPWISE_STORE (camel_folder_get_parent_store (folder)));
	AddData add_data = { summary, changes, wanted, (camel_folder_get_flags (folder) & CAMEL_FOLDER_FILTER_RECENT) != 0,
		gw_folder->invitations, labels };
	GHashTableIter iter;
	gpointer key, value;
	const gchar *oldest_new = NULL;
	gboolean success = TRUE;

	if (complete) {
		GPtrArray *known = camel_folder_summary_get_array (summary);
		guint ii;

		for (ii = 0; ii < known->len; ii++) {
			const gchar *uid = known->pdata[ii];

			if (!g_hash_table_contains (server, uid)) {
				camel_folder_summary_remove_uid (summary, uid);
				camel_folder_change_info_remove_uid (changes, uid);
				camel_data_cache_remove (gw_folder->cache, CACHE_PATH, uid, NULL);
			}
		}
		camel_folder_summary_free_array (known);
	}

	g_hash_table_iter_init (&iter, server);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		ServerState *state = value;
		CamelMessageInfo *info = camel_folder_summary_get (summary, key);

		if (state->calendar)
			g_hash_table_add (gw_folder->invitations, g_strdup (key));

		if (info) {
			gboolean changed = take_server_flags (CAMEL_GROUPWISE_MESSAGE_INFO (info), state->flags);

			changed = take_server_categories (labels, CAMEL_GROUPWISE_MESSAGE_INFO (info),
				(const gchar * const *) state->categories) || changed;

			/* A subject the user changed in GroupWise */
			if (state->subject && *state->subject &&
			    g_strcmp0 (camel_message_info_get_subject (info), state->subject) != 0) {
				camel_message_info_set_subject (info, state->subject);
				changed = TRUE;
			}
			if (changed)
				camel_folder_change_info_change_uid (changes, key);
			g_object_unref (info);
			continue;
		}

		g_hash_table_add (wanted, key);
		if (!state->modified || !*state->modified)
			oldest_new = "";	/* no date: read everything */
		else if (!oldest_new || (*oldest_new && strcmp (state->modified, oldest_new) < 0))
			oldest_new = state->modified;
	}

	if (g_hash_table_size (wanted) > 0) {
		gchar *filter = oldest_new && *oldest_new ? modified_filter (oldest_new) : NULL;

		success = e_gw_connection_foreach_item_sync (cnc, gw_folder->id, CAMEL_GROUPWISE_SUMMARY_VIEW, filter,
			PAGE_SIZE, add_item_cb, &add_data, cancellable, error);
		g_free (filter);
	}

	g_hash_table_destroy (wanted);

	return success;
}

static GHashTable *
new_server_state (void)
{
	return g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) server_state_free);
}

/* The current time of the POA, the start of the next quick check */
static gchar *
server_time (CamelGroupwiseFolder *gw_folder,
	     EGwConnection *cnc,
	     GCancellable *cancellable)
{
	GDateTime *now = g_date_time_new_now_utc ();
	gchar *since = g_date_time_format (now, "%Y-%m-%dT%H:%M:%SZ");
	EGwResponse *response;
	gchar *result = NULL;

	response = e_gw_connection_get_quick_messages_sync (cnc, "Modified", since, gw_folder->id, "id",
		&result, cancellable, NULL);
	e_gw_response_free (response);

	g_free (since);
	g_date_time_unref (now);

	return result;
}

/* The state of every item (cheap view); an empty summary is filled in one
 * pass with the full view instead. */
static gboolean
groupwise_folder_full_scan (CamelFolder *folder,
			    EGwConnection *cnc,
			    CamelFolderChangeInfo *changes,
			    GCancellable *cancellable,
			    GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	gchar *started = gw_folder->quick_supported ? server_time (gw_folder, cnc, cancellable) : NULL;
	gboolean success;

	if (camel_folder_summary_count (summary) == 0) {
		/* The first fill is no new mail. Recent messages go through the
		 * filters and the junk test, which fetch each of them: for a
		 * mailbox with hundreds of unread messages that means hundreds
		 * of downloads before anything shows. */
		AddData add_data = { summary, changes, NULL, FALSE, gw_folder->invitations,
			camel_groupwise_store_get_labels (CAMEL_GROUPWISE_STORE (camel_folder_get_parent_store (folder))) };

		success = e_gw_connection_foreach_item_sync (cnc, gw_folder->id, CAMEL_GROUPWISE_SUMMARY_VIEW, NULL,
			PAGE_SIZE, add_item_cb, &add_data, cancellable, error);
	} else {
		GHashTable *server = new_server_state ();

		success = e_gw_connection_foreach_item_sync (cnc, gw_folder->id, CAMEL_GROUPWISE_FLAGS_VIEW, NULL,
			STATE_PAGE_SIZE, collect_state_cb, server, cancellable, error) &&
			apply_server_state (folder, cnc, server, TRUE, changes, cancellable, error);
		g_hash_table_destroy (server);
	}

	if (success) {
		g_free (gw_folder->quick_since);
		gw_folder->quick_since = started;
		gw_folder->last_full_scan = g_get_monotonic_time ();
	} else {
		g_free (started);
	}

	return success;
}

/* The appointments, tasks and notes still in the folder: an answered
 * invitation leaves the Mailbox */
static gboolean
check_invitations (CamelFolder *folder,
		   EGwConnection *cnc,
		   CamelFolderChangeInfo *changes,
		   GCancellable *cancellable,
		   GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	static const gchar *filter =
		"<filter><element type=\"FilterGroup\"><op>or</op>"
		"<element type=\"FilterEntry\"><op>eq</op><field>@type</field><value>Appointment</value></element>"
		"<element type=\"FilterEntry\"><op>eq</op><field>@type</field><value>Task</value></element>"
		"<element type=\"FilterEntry\"><op>eq</op><field>@type</field><value>Note</value></element>"
		"</element></filter>";
	GHashTable *present;
	GHashTableIter iter;
	gpointer key;
	gboolean success;

	if (!g_hash_table_size (gw_folder->invitations))
		return TRUE;

	present = new_server_state ();
	success = e_gw_connection_foreach_item_sync (cnc, gw_folder->id, "id peek", filter, STATE_PAGE_SIZE,
		collect_state_cb, present, cancellable, error);
	if (success) {
		g_hash_table_iter_init (&iter, gw_folder->invitations);
		while (g_hash_table_iter_next (&iter, &key, NULL)) {
			if (g_hash_table_contains (present, key))
				continue;
			if (camel_folder_summary_check_uid (summary, key)) {
				camel_folder_summary_remove_uid (summary, key);
				camel_folder_change_info_remove_uid (changes, key);
				camel_data_cache_remove (gw_folder->cache, CACHE_PATH, key, NULL);
			}
			g_hash_table_iter_remove (&iter);
		}
	}
	g_hash_table_destroy (present);

	return success;
}

/* Only what changed since the last check */
static gboolean
groupwise_folder_quick_check (CamelFolder *folder,
			      EGwConnection *cnc,
			      CamelFolderChangeInfo *changes,
			      GCancellable *cancellable,
			      GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	GHashTable *server;
	EGwResponse *response;
	gchar *next = NULL;
	xmlNode *item;
	gboolean success;

	response = e_gw_connection_get_quick_messages_sync (cnc, "Modified", gw_folder->quick_since, gw_folder->id,
		CAMEL_GROUPWISE_FLAGS_VIEW, &next, cancellable, error);
	if (!response)
		return FALSE;

	server = new_server_state ();
	item = e_gw_xml_find (e_gw_response_get_node (response), "items");
	for (item = e_gw_xml_first_child (item, "item"); item; item = e_gw_xml_next_sibling (item, "item"))
		collect_state_cb (item, server);
	e_gw_response_free (response);

	success = apply_server_state (folder, cnc, server, FALSE, changes, cancellable, error) &&
		check_invitations (folder, cnc, changes, cancellable, error);
	g_hash_table_destroy (server);

	if (success) {
		g_free (gw_folder->quick_since);
		gw_folder->quick_since = next;
	} else {
		g_free (next);
	}

	return success;
}

/* Deleted items show only in a full scan; tests can shorten the interval */
static gint64
full_scan_interval (void)
{
	const gchar *env = g_getenv ("GROUPWISE_FULL_SCAN_SECONDS");

	return (env ? g_ascii_strtoll (env, NULL, 10) : FULL_SCAN_SECONDS) * G_USEC_PER_SEC;
}

static gboolean
groupwise_folder_refresh_locked (CamelFolder *folder,
				 EGwConnection *cnc,
				 GCancellable *cancellable,
				 GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	CamelFolderChangeInfo *changes = camel_folder_change_info_new ();
	gboolean success = FALSE, quick;

	/* A search result folder has no quick check: a full scan now and then */
	if (gw_folder->is_query && camel_folder_summary_count (summary) > 0 &&
	    g_get_monotonic_time () - gw_folder->last_full_scan < full_scan_interval ()) {
		camel_folder_change_info_free (changes);
		return TRUE;
	}

	quick = gw_folder->quick_supported && gw_folder->quick_since &&
		camel_folder_summary_count (summary) > 0 &&
		g_get_monotonic_time () - gw_folder->last_full_scan < full_scan_interval ();

	if (quick) {
		GError *local_error = NULL;

		success = groupwise_folder_quick_check (folder, cnc, changes, cancellable, &local_error);
		if (!success && !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_debug ("quick check of %s failed, full scan: %s", camel_folder_get_full_name (folder),
				local_error ? local_error->message : "?");
			g_clear_error (&local_error);
			quick = FALSE;
		} else if (local_error) {
			g_propagate_error (error, local_error);
		}
	}

	if (!quick)
		success = groupwise_folder_full_scan (folder, cnc, changes, cancellable, error);
	if (success)
		apply_tasklist (folder, cnc, changes, cancellable);

	g_debug ("refresh %s: %s %s, %u messages", camel_folder_get_full_name (folder), quick ? "quick" : "full",
		success ? "ok" : "failed", camel_folder_summary_count (summary));

	camel_folder_summary_save (summary, NULL);

	if (camel_folder_change_info_changed (changes))
		camel_folder_changed (folder, changes);
	camel_folder_change_info_free (changes);

	return success;
}

static gboolean
groupwise_folder_refresh_info_sync (CamelFolder *folder,
				    GCancellable *cancellable,
				    GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	EGwConnection *cnc;
	gboolean success;

	if (!folder_is_online (folder))
		return TRUE;

	cnc = folder_ref_connection_sync (folder, cancellable, error);
	if (!cnc)
		return FALSE;

	g_mutex_lock (&gw_folder->refresh_lock);
	success = groupwise_folder_refresh_locked (folder, cnc, cancellable, error);
	g_mutex_unlock (&gw_folder->refresh_lock);

	g_object_unref (cnc);

	return success;
}

/* ------------------------------------------------------------------ */
/* Writing flags back */

static gboolean
mark_read_batched (EGwConnection *cnc,
		   GPtrArray *ids,
		   gboolean read,
		   GCancellable *cancellable,
		   GError **error)
{
	guint start;

	for (start = 0; start < ids->len; start += MARK_BATCH) {
		guint count = MIN (MARK_BATCH, ids->len - start);
		gchar **batch = g_new0 (gchar *, count + 1);
		gboolean success;

		memcpy (batch, ids->pdata + start, count * sizeof (gchar *));
		success = e_gw_connection_mark_read_sync (cnc, (const gchar * const *) batch, read, cancellable, error);
		g_free (batch);

		if (!success)
			return FALSE;
	}

	return TRUE;
}

/* ------------------------------------------------------------------ */
/* The Tasklist: Evolution's follow-up flag of a mail stands for it being on
 * the Tasklist of GroupWise, with its due date and completion. The Tasklist
 * is a view: an item is on it while it has a checklist entry (modifyItem
 * <update><checklist>), and leaves it when that is deleted; it stays in its
 * folder. Found on the POA: a checklist sequence of 0 does not take an item
 * off, and after the delete a checklist with sequence 0 remains — only the
 * listing of the Tasklist tells what is on it. */

#define FOLLOWUP_TAG "follow-up"
#define DUE_TAG "due-by"
#define COMPLETED_TAG "completed-on"
#define TASKLIST_SECONDS 60	/* the folders of a refresh share a listing */

typedef struct {
	GMutex lock;
	GHashTable *states;	/* item base -> "due|completed" */
	gint64 read_at;
} TasklistCache;

static void
tasklist_cache_free (TasklistCache *cache)
{
	g_clear_pointer (&cache->states, g_hash_table_unref);
	g_mutex_clear (&cache->lock);
	g_free (cache);
}

static gchar *
item_base_of (const gchar *id)
{
	return id ? g_strndup (id, strcspn (id, ":")) : NULL;
}

static gint64
iso_to_time (const gchar *iso)
{
	GDateTime *dt = iso && *iso ? g_date_time_new_from_iso8601 (iso, NULL) : NULL;
	gint64 when = dt ? g_date_time_to_unix (dt) : 0;

	g_clear_pointer (&dt, g_date_time_unref);

	return when;
}

static gboolean
collect_tasklist_cb (xmlNode *item,
		     gpointer user_data)
{
	GHashTable *states = user_data;
	gchar *id = camel_groupwise_item_dup_id (item);
	gchar *due = e_gw_xml_dup_text (item, "checklist/dueDate");
	gchar *completed = e_gw_xml_dup_text (item, "checklist/completed");

	if (id)
		g_hash_table_replace (states, item_base_of (id), g_strdup_printf ("%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT,
			iso_to_time (due), iso_to_time (completed)));
	g_free (id);
	g_free (due);
	g_free (completed);

	return TRUE;
}

/* What is on the Tasklist of the mailbox (item base -> state); NULL when it
 * cannot be read or the mailbox has none */
static GHashTable *
tasklist_states_sync (CamelFolder *folder,
		      EGwConnection *cnc,
		      gboolean fresh,
		      GCancellable *cancellable)
{
	CamelStore *store = camel_folder_get_parent_store (folder);
	CamelGroupwiseStoreSummary *store_summary = camel_groupwise_store_get_summary (CAMEL_GROUPWISE_STORE (store));
	TasklistCache *cache;
	GHashTable *states = NULL;
	gchar *full_name, *id;

	cache = g_object_get_data (G_OBJECT (store), "groupwise-tasklist");
	if (!cache) {
		cache = g_new0 (TasklistCache, 1);
		g_mutex_init (&cache->lock);
		g_object_set_data_full (G_OBJECT (store), "groupwise-tasklist", cache, (GDestroyNotify) tasklist_cache_free);
	}

	g_mutex_lock (&cache->lock);
	if (!fresh && cache->states && g_get_monotonic_time () - cache->read_at < TASKLIST_SECONDS * G_USEC_PER_SEC) {
		states = g_hash_table_ref (cache->states);
		g_mutex_unlock (&cache->lock);
		return states;
	}

	full_name = camel_groupwise_store_summary_dup_full_name_by_type (store_summary, E_GW_FOLDER_TYPE_CHECKLIST);
	id = full_name ? camel_groupwise_store_summary_dup_id (store_summary, full_name) : NULL;
	if (id) {
		states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
		if (!e_gw_connection_foreach_item_sync (cnc, id, "id checklist", NULL, PAGE_SIZE,
			collect_tasklist_cb, states, cancellable, NULL))
			g_clear_pointer (&states, g_hash_table_unref);
	}
	if (states) {
		g_clear_pointer (&cache->states, g_hash_table_unref);
		cache->states = g_hash_table_ref (states);
		cache->read_at = g_get_monotonic_time ();
	}
	g_mutex_unlock (&cache->lock);
	g_free (full_name);
	g_free (id);

	return states;
}

/* The listing is old once the Tasklist of an item changed */
static void
tasklist_states_forget (CamelFolder *folder)
{
	TasklistCache *cache = g_object_get_data (G_OBJECT (camel_folder_get_parent_store (folder)), "groupwise-tasklist");

	if (cache) {
		g_mutex_lock (&cache->lock);
		cache->read_at = 0;
		g_mutex_unlock (&cache->lock);
	}
}

static gint64
tag_time (CamelMessageInfo *info,
	  const gchar *tag)
{
	const gchar *value = camel_message_info_get_user_tag (info, tag);

	return value && *value ? (gint64) camel_header_decode_date (value, NULL) : 0;
}

/* The follow-up flag of @info as a Tasklist state ("" without) */
static gchar *
local_followup (CamelMessageInfo *info)
{
	const gchar *flag = camel_message_info_get_user_tag (info, FOLLOWUP_TAG);

	if (!flag || !*flag)
		return g_strdup ("");

	return g_strdup_printf ("%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT, tag_time (info, DUE_TAG), tag_time (info, COMPLETED_TAG));
}

static void
set_tag_time (CamelMessageInfo *info,
	      const gchar *tag,
	      gint64 when)
{
	gchar *text = when > 0 ? camel_header_format_date ((time_t) when, 0) : NULL;

	camel_message_info_set_user_tag (info, tag, text);
	g_free (text);
}

/* The follow-up flag after the Tasklist of the server, unless the user
 * changed it and did not write it back. Returns whether it changed. */
static gboolean
take_server_followup (CamelGroupwiseMessageInfo *info,
		      const gchar *server)
{
	CamelMessageInfo *mi = CAMEL_MESSAGE_INFO (info);
	gboolean was_flagged = camel_message_info_get_folder_flagged (mi);
	gchar *known = camel_groupwise_message_info_dup_server_followup (info);
	gchar *local = local_followup (mi);
	gboolean changed = FALSE;

	/* A follow-up flag from before the Tasklist was known goes onto it */
	if (!camel_groupwise_message_info_knows_server_followup (info) && *local && !*server) {
		camel_groupwise_message_info_set_server_followup (info, "");
		camel_message_info_set_folder_flagged (mi, TRUE);
		g_free (known);
		g_free (local);
		return FALSE;
	}

	if (g_strcmp0 (local, server) != 0 && (!was_flagged || g_strcmp0 (local, known) == 0)) {
		if (!*server) {
			camel_message_info_set_user_tag (mi, FOLLOWUP_TAG, NULL);
			camel_message_info_set_user_tag (mi, DUE_TAG, NULL);
			camel_message_info_set_user_tag (mi, COMPLETED_TAG, NULL);
		} else {
			gint64 due = g_ascii_strtoll (server, NULL, 10);
			const gchar *bar = strchr (server, '|');
			gint64 completed = bar ? g_ascii_strtoll (bar + 1, NULL, 10) : 0;
			const gchar *flag = camel_message_info_get_user_tag (mi, FOLLOWUP_TAG);

			/* Evolution's own name of the flag */
			if (!flag || !*flag)
				camel_message_info_set_user_tag (mi, FOLLOWUP_TAG, g_dgettext ("evolution", "Follow-Up"));
			set_tag_time (mi, DUE_TAG, due);
			set_tag_time (mi, COMPLETED_TAG, completed);
		}
		changed = TRUE;
		/* The server's own change is nothing to write back */
		if (!was_flagged)
			camel_message_info_set_folder_flagged (mi, FALSE);
	}
	camel_groupwise_message_info_set_server_followup (info, server);
	g_free (known);
	g_free (local);

	return changed;
}

/* Every message of the folder after the Tasklist */
static void
apply_tasklist (CamelFolder *folder,
		EGwConnection *cnc,
		CamelFolderChangeInfo *changes,
		GCancellable *cancellable)
{
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	GHashTable *states = tasklist_states_sync (folder, cnc, FALSE, cancellable);
	GPtrArray *known;
	guint ii;

	if (!states)
		return;

	known = camel_folder_summary_get_array (summary);
	for (ii = 0; known && ii < known->len; ii++) {
		const gchar *uid = known->pdata[ii];
		CamelMessageInfo *info = camel_folder_summary_get (summary, uid);
		gchar *base = item_base_of (uid);
		const gchar *state = base ? g_hash_table_lookup (states, base) : NULL;

		if (info && CAMEL_IS_GROUPWISE_MESSAGE_INFO (info) &&
		    take_server_followup (CAMEL_GROUPWISE_MESSAGE_INFO (info), state ? state : ""))
			camel_folder_change_info_change_uid (changes, uid);
		g_clear_object (&info);
		g_free (base);
	}
	camel_folder_summary_free_array (known);
	g_hash_table_unref (states);
}

/* The Tasklist after the follow-up flag of @info, where it differs from
 * the server's */
static gboolean
sync_followup (CamelFolder *folder,
	       EGwConnection *cnc,
	       CamelGroupwiseMessageInfo *info,
	       GCancellable *cancellable,
	       GError **error)
{
	const gchar *uid = camel_message_info_get_uid (CAMEL_MESSAGE_INFO (info));
	gchar *server = camel_groupwise_message_info_dup_server_followup (info);
	gchar *local = local_followup (CAMEL_MESSAGE_INFO (info));
	gboolean success = TRUE;

	if (g_strcmp0 (server, local) != 0) {
		gint64 due = *local ? g_ascii_strtoll (local, NULL, 10) : 0;
		gint64 completed = *local && strchr (local, '|') ? g_ascii_strtoll (strchr (local, '|') + 1, NULL, 10) : 0;
		gint64 was_due = *server ? g_ascii_strtoll (server, NULL, 10) : 0;
		gboolean was_completed = *server && strchr (server, '|') && g_ascii_strtoll (strchr (server, '|') + 1, NULL, 10) > 0;

		if (!*local) {
			g_debug ("tasklist: %s off", uid);
			success = e_gw_connection_modify_item_sync (cnc, uid, "<delete><checklist/></delete>", cancellable, error);
		} else {
			GString *updates = g_string_new (NULL);

			/* On the Tasklist (its sequence), with the due date */
			if (!*server || (due > 0 && due != was_due)) {
				g_string_append (updates, "<update><checklist>");
				if (!*server)
					e_gw_xml_add_leaf (updates, "sequence", "1");
				if (due > 0) {
					GDateTime *dt = g_date_time_new_from_unix_utc (due);
					gchar *iso = g_date_time_format (dt, "%Y-%m-%dT%H:%M:%SZ");

					e_gw_xml_add_leaf (updates, "dueDate", iso);
					g_free (iso);
					g_date_time_unref (dt);
				}
				g_string_append (updates, "</checklist></update>");
			}
			/* The due date taken away: it stays on the Tasklist */
			if (*server && due <= 0 && was_due > 0)
				g_string_append (updates, "<delete><checklist><dueDate/></checklist></delete>");
			if (updates->len) {
				g_debug ("tasklist: %s %s", uid, updates->str);
				success = e_gw_connection_modify_item_sync (cnc, uid, updates->str, cancellable, error);
			}
			g_string_free (updates, TRUE);
			if (success && (completed > 0) != was_completed)
				success = e_gw_connection_complete_sync (cnc, uid, completed > 0, cancellable, error);
		}
		if (success) {
			camel_groupwise_message_info_set_server_followup (info, local);
			tasklist_states_forget (folder);
		}
	}

	g_free (server);
	g_free (local);

	return success;
}

/* The categories of the item after the labels of @info, where they differ
 * from the server's; a label GroupWise does not know becomes a category */
static gboolean
sync_categories (CamelFolder *folder,
		 EGwConnection *cnc,
		 CamelGroupwiseMessageInfo *info,
		 GCancellable *cancellable,
		 GError **error)
{
	CamelGroupwiseLabels *labels = camel_groupwise_store_get_labels (CAMEL_GROUPWISE_STORE (camel_folder_get_parent_store (folder)));
	gchar **server = camel_groupwise_message_info_dup_server_categories (info);
	gchar **tags, *updates;
	GPtrArray *wanted;
	gboolean success = TRUE;
	guint ii;

	if (categories_equal (labels, CAMEL_MESSAGE_INFO (info), (const gchar * const *) server)) {
		g_strfreev (server);
		return TRUE;
	}

	/* Categories the labels do not stand for stay as they are */
	wanted = g_ptr_array_new_with_free_func (g_free);
	for (ii = 0; server[ii]; ii++) {
		gchar *tag = camel_groupwise_labels_dup_tag (labels, server[ii]);

		if (!tag)
			g_ptr_array_add (wanted, g_strdup (server[ii]));
		g_free (tag);
	}
	tags = camel_groupwise_labels_dup_info_tags (labels, CAMEL_MESSAGE_INFO (info));
	for (ii = 0; success && tags[ii]; ii++) {
		gchar *id = camel_groupwise_labels_dup_category_sync (labels, tags[ii], cnc, cancellable, error);

		if (id)
			g_ptr_array_add (wanted, id);
		else
			success = FALSE;
	}
	g_ptr_array_add (wanted, NULL);
	g_strfreev (tags);

	if (success) {
		updates = e_gw_categories_updates_xml ((const gchar * const *) server, (const gchar * const *) wanted->pdata);
		if (*updates) {
			g_debug ("categories of %s: %s", camel_message_info_get_uid (CAMEL_MESSAGE_INFO (info)), updates);
			success = e_gw_connection_modify_item_sync (cnc, camel_message_info_get_uid (CAMEL_MESSAGE_INFO (info)),
				updates, cancellable, error);
		}
		if (success)
			camel_groupwise_message_info_set_server_categories (info, (const gchar * const *) wanted->pdata);
		g_free (updates);
	}

	g_ptr_array_unref (wanted);
	g_strfreev (server);

	return success;
}

static gboolean
groupwise_folder_sync_flags (CamelFolder *folder,
			     GCancellable *cancellable,
			     GError **error)
{
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	GPtrArray *changed, *read, *unread, *infos;
	EGwConnection *cnc;
	gboolean success = TRUE;
	guint ii;

	changed = camel_folder_summary_get_changed (summary);
	if (!changed || changed->len == 0) {
		if (changed)
			g_ptr_array_unref (changed);
		return camel_folder_summary_save (summary, error);
	}

	cnc = folder_ref_connection_sync (folder, cancellable, error);
	if (!cnc) {
		g_ptr_array_unref (changed);
		return FALSE;
	}

	read = g_ptr_array_new ();
	unread = g_ptr_array_new ();
	infos = g_ptr_array_new_with_free_func (g_object_unref);

	/* GroupWise knows read/unread and the categories (labels); other flags
	 * stay in Evolution. Only a read state that differs from the server's
	 * is sent. */
	for (ii = 0; ii < changed->len; ii++) {
		CamelMessageInfo *info = camel_folder_summary_get (summary, changed->pdata[ii]);
		guint32 local, server;

		if (!info)
			continue;

		g_ptr_array_add (infos, info);
		local = camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN;
		server = camel_groupwise_message_info_get_server_flags (CAMEL_GROUPWISE_MESSAGE_INFO (info)) & CAMEL_MESSAGE_SEEN;
		if (local == server)
			continue;

		if (local)
			g_ptr_array_add (read, (gpointer) camel_message_info_get_uid (info));
		else
			g_ptr_array_add (unread, (gpointer) camel_message_info_get_uid (info));
	}

	if (read->len)
		success = mark_read_batched (cnc, read, TRUE, cancellable, error);
	if (success && unread->len)
		success = mark_read_batched (cnc, unread, FALSE, cancellable, error);

	/* Labels are categories: what the user changed goes to the server */
	for (ii = 0; success && ii < infos->len; ii++)
		success = sync_categories (folder, cnc, infos->pdata[ii], cancellable, error);
	/* The follow-up flag is the Tasklist */
	for (ii = 0; success && ii < infos->len; ii++)
		success = sync_followup (folder, cnc, infos->pdata[ii], cancellable, error);

	if (success) {
		for (ii = 0; ii < infos->len; ii++) {
			CamelGroupwiseMessageInfo *info = infos->pdata[ii];
			guint32 server = camel_groupwise_message_info_get_server_flags (info);

			server = (server & ~CAMEL_MESSAGE_SEEN) |
				(camel_message_info_get_flags (CAMEL_MESSAGE_INFO (info)) & CAMEL_MESSAGE_SEEN);
			camel_groupwise_message_info_set_server_flags (info, server);
			camel_message_info_set_folder_flagged (CAMEL_MESSAGE_INFO (info), FALSE);
		}
		success = camel_folder_summary_save (summary, error);
	}

	g_ptr_array_unref (infos);
	g_ptr_array_unref (unread);
	g_ptr_array_unref (read);
	g_ptr_array_unref (changed);
	g_object_unref (cnc);

	return success;
}

typedef gboolean (*BatchFunc) (EGwConnection *cnc,
			       const gchar * const *ids,
			       gpointer user_data,
			       GCancellable *cancellable,
			       GError **error);

/* Calls @func with at most MARK_BATCH IDs at a time */
static gboolean
in_batches (EGwConnection *cnc,
	    GPtrArray *ids,
	    BatchFunc func,
	    gpointer user_data,
	    GCancellable *cancellable,
	    GError **error)
{
	guint start;

	for (start = 0; start < ids->len; start += MARK_BATCH) {
		guint count = MIN (MARK_BATCH, ids->len - start);
		gchar **batch = g_new0 (gchar *, count + 1);
		gboolean success;

		memcpy (batch, ids->pdata + start, count * sizeof (gchar *));
		success = func (cnc, (const gchar * const *) batch, user_data, cancellable, error);
		g_free (batch);

		if (!success)
			return FALSE;
	}

	return TRUE;
}

static gboolean
remove_batch (EGwConnection *cnc,
	      const gchar * const *ids,
	      gpointer container,
	      GCancellable *cancellable,
	      GError **error)
{
	return e_gw_connection_remove_items_sync (cnc, ids, container, cancellable, error);
}

static gboolean
purge_batch (EGwConnection *cnc,
	     const gchar * const *ids,
	     gpointer user_data,
	     GCancellable *cancellable,
	     GError **error)
{
	return e_gw_connection_purge_sync (cnc, ids, cancellable, error);
}

typedef struct {
	const gchar *container;
	const gchar *from;	/* NULL: copy */
} MoveData;

static gboolean
move_batch (EGwConnection *cnc,
	    const gchar * const *ids,
	    gpointer user_data,
	    GCancellable *cancellable,
	    GError **error)
{
	MoveData *data = user_data;

	return e_gw_connection_move_items_sync (cnc, ids, data->container, data->from, cancellable, error);
}

/* The folder an item is really in: a search result folder lists the items
 * of other folders under their IDs there */
static const gchar *
real_container (const gchar *uid)
{
	const gchar *colon = uid ? strchr (uid, ':') : NULL;

	return colon ? colon + 1 : NULL;
}

/* @uids by the folder they are in; the arrays do not own their IDs */
static GHashTable *
group_by_container (GPtrArray *uids)
{
	GHashTable *groups = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) g_ptr_array_unref);
	guint ii;

	for (ii = 0; ii < uids->len; ii++) {
		const gchar *container = real_container (uids->pdata[ii]);
		GPtrArray *group;

		if (!container)
			continue;
		group = g_hash_table_lookup (groups, container);
		if (!group) {
			group = g_ptr_array_new ();
			g_hash_table_insert (groups, (gpointer) container, group);
		}
		g_ptr_array_add (group, uids->pdata[ii]);
	}

	return groups;
}

static void
forget_message (CamelGroupwiseFolder *gw_folder,
		const gchar *uid,
		CamelFolderChangeInfo *changes)
{
	CamelFolderSummary *summary = camel_folder_get_folder_summary (CAMEL_FOLDER (gw_folder));

	camel_folder_summary_remove_uid (summary, uid);
	camel_folder_change_info_remove_uid (changes, uid);
	camel_data_cache_remove (gw_folder->cache, CACHE_PATH, uid, NULL);
}

/* Trash items that are nowhere else any more: every <container> of the item
 * carries a "deleted" date. Purging one that is still linked into another
 * folder would delete it there as well. */
static gboolean
collect_purgeable_cb (xmlNode *item,
		      gpointer user_data)
{
	GHashTable *purgeable = user_data;
	xmlNode *container;
	gboolean elsewhere = FALSE, any = FALSE;

	for (container = e_gw_xml_first_child (item, "container"); container; container = e_gw_xml_next_sibling (container, "container")) {
		gchar *deleted = e_gw_xml_dup_attr (container, "deleted");

		any = TRUE;
		if (!deleted)
			elsewhere = TRUE;
		g_free (deleted);
	}

	if (any && !elsewhere) {
		gchar *id = camel_groupwise_item_dup_id (item);

		if (id)
			g_hash_table_add (purgeable, id);
	}

	return TRUE;
}

/* Messages marked deleted leave the folder: into the Trash, which the POA
 * does itself. On expunge they are pruned instead, as with "prune" in the
 * GroupWise client: gone from every folder, without the Trash. In the Trash
 * expunge is "Empty Trash": purged, except those still in other folders,
 * which are only removed from the Trash (it drops their "deleted" containers
 * and leaves the others alone). */
static gboolean
groupwise_folder_sync_deleted (CamelFolder *folder,
			       gboolean expunge,
			       GCancellable *cancellable,
			       GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	CamelFolderChangeInfo *changes;
	GPtrArray *uids, *deleted;
	EGwConnection *cnc;
	gboolean success = TRUE;
	guint ii;

	if (!gw_folder->is_trash || expunge) {
		uids = camel_folder_summary_get_array (summary);
		deleted = g_ptr_array_new ();
		for (ii = 0; ii < uids->len; ii++) {
			if (camel_folder_summary_get_info_flags (summary, uids->pdata[ii]) & CAMEL_MESSAGE_DELETED)
				g_ptr_array_add (deleted, uids->pdata[ii]);
		}
	} else {
		return TRUE;
	}

	if (deleted->len == 0) {
		g_ptr_array_unref (deleted);
		camel_folder_summary_free_array (uids);
		return TRUE;
	}

	cnc = folder_ref_connection_sync (folder, cancellable, error);
	if (!cnc) {
		g_ptr_array_unref (deleted);
		camel_folder_summary_free_array (uids);
		return FALSE;
	}

	changes = camel_folder_change_info_new ();

	if (!gw_folder->is_trash && gw_folder->is_query && !expunge) {
		/* Deleted in the folder each message is in */
		GHashTable *groups = group_by_container (deleted);
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init (&iter, groups);
		while (success && g_hash_table_iter_next (&iter, &key, &value))
			success = in_batches (cnc, value, remove_batch, key, cancellable, error);
		g_hash_table_destroy (groups);
		for (ii = 0; success && ii < deleted->len; ii++)
			forget_message (gw_folder, deleted->pdata[ii], changes);
	} else if (!gw_folder->is_trash) {
		success = expunge ? in_batches (cnc, deleted, purge_batch, NULL, cancellable, error) :
			in_batches (cnc, deleted, remove_batch, gw_folder->id, cancellable, error);
		for (ii = 0; success && ii < deleted->len; ii++)
			forget_message (gw_folder, deleted->pdata[ii], changes);
	} else {
		GHashTable *purgeable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		GPtrArray *purge = g_ptr_array_new (), *drop = g_ptr_array_new ();

		success = e_gw_connection_foreach_item_sync (cnc, gw_folder->id, "id container peek", NULL,
			STATE_PAGE_SIZE, collect_purgeable_cb, purgeable, cancellable, error);

		for (ii = 0; success && ii < deleted->len; ii++) {
			if (g_hash_table_contains (purgeable, deleted->pdata[ii])) {
				g_ptr_array_add (purge, deleted->pdata[ii]);
			} else {
				/* Still in another folder: only out of the Trash */
				g_debug ("not purging %s: still in another folder", (const gchar *) deleted->pdata[ii]);
				g_ptr_array_add (drop, deleted->pdata[ii]);
			}
		}

		if (success)
			success = in_batches (cnc, purge, purge_batch, NULL, cancellable, error);
		for (ii = 0; success && ii < purge->len; ii++)
			forget_message (gw_folder, purge->pdata[ii], changes);
		if (success)
			success = in_batches (cnc, drop, remove_batch, gw_folder->id, cancellable, error);
		for (ii = 0; success && ii < drop->len; ii++)
			forget_message (gw_folder, drop->pdata[ii], changes);

		g_ptr_array_unref (drop);
		g_ptr_array_unref (purge);
		g_hash_table_destroy (purgeable);
	}

	camel_folder_summary_save (summary, NULL);
	if (camel_folder_change_info_changed (changes))
		camel_folder_changed (folder, changes);
	camel_folder_change_info_free (changes);

	g_object_unref (cnc);
	g_ptr_array_unref (deleted);
	camel_folder_summary_free_array (uids);

	return success;
}

/* Where a Trash item was deleted from (its containers with a "deleted"
 * date), and whether it is still in a folder. A folder that is gone shows
 * up as the Trash itself. */
typedef struct {
	GPtrArray *deleted_from;
	gboolean from_gone_folder;
	gboolean elsewhere;
} Origins;

static void
origins_free (Origins *origins)
{
	g_ptr_array_unref (origins->deleted_from);
	g_free (origins);
}

typedef struct {
	const gchar *trash_id;
	GHashTable *origins;	/* item ID -> Origins */
} OriginsData;

static gboolean
collect_origins_cb (xmlNode *item,
		    gpointer user_data)
{
	OriginsData *data = user_data;
	Origins *origins = g_new0 (Origins, 1);
	xmlNode *container;
	gchar *id;

	origins->deleted_from = g_ptr_array_new_with_free_func (g_free);
	for (container = e_gw_xml_first_child (item, "container"); container; container = e_gw_xml_next_sibling (container, "container")) {
		gchar *deleted = e_gw_xml_dup_attr (container, "deleted");
		gchar *raw = e_gw_xml_dup_text (container, NULL);
		gchar *value = raw && *raw ? e_gw_clean_id (raw) : NULL;

		g_free (raw);
		if (value && g_strcmp0 (value, data->trash_id) == 0) {
			if (deleted)
				origins->from_gone_folder = TRUE;
		} else if (value) {
			if (deleted)
				g_ptr_array_add (origins->deleted_from, g_steal_pointer (&value));
			else
				origins->elsewhere = TRUE;
		}
		g_free (deleted);
		g_free (value);
	}

	id = camel_groupwise_item_dup_id (item);
	if (id)
		g_hash_table_insert (data->origins, id, origins);
	else
		origins_free (origins);

	return TRUE;
}

static void
add_to_group (GHashTable *groups,
	      const gchar *container,
	      const gchar *uid)
{
	GPtrArray *group = g_hash_table_lookup (groups, container);

	if (!group) {
		group = g_ptr_array_new ();
		g_hash_table_insert (groups, g_strdup (container), group);
	}
	g_ptr_array_add (group, (gpointer) uid);
}

/* Refreshes the folders the messages went back to, where they are open */
static void
refresh_folders (CamelStore *store,
		 GHashTable *container_ids)
{
	GPtrArray *folders = camel_store_dup_opened_folders (store);
	guint ii;

	for (ii = 0; folders && ii < folders->len; ii++) {
		CamelFolder *folder = folders->pdata[ii];

		if (CAMEL_IS_GROUPWISE_FOLDER (folder) &&
		    g_hash_table_contains (container_ids, CAMEL_GROUPWISE_FOLDER (folder)->id))
			camel_folder_refresh_info (folder, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
	}
	if (folders)
		g_ptr_array_unref (folders);
}

static gboolean
move_groups (EGwConnection *cnc,
	     GHashTable *groups,
	     const gchar *from,
	     GCancellable *cancellable,
	     GError **error)
{
	GHashTableIter iter;
	gpointer key, value;
	gboolean success = TRUE;
	MoveData move;

	move.from = from;
	g_hash_table_iter_init (&iter, groups);
	while (success && g_hash_table_iter_next (&iter, &key, &value)) {
		move.container = key;
		g_debug ("restoring %u item(s) into %s%s", ((GPtrArray *) value)->len, (const gchar *) key,
			from ? " out of the Trash" : "");
		success = in_batches (cnc, value, move_batch, &move, cancellable, error);
	}

	return success;
}

/* Messages in the Trash flagged CAMEL_GROUPWISE_RESTORE_FLAG go back, like
 * "Undelete" in GroupWise. The POA has no request for it and the Trash is a
 * view: an item is in it while one of its containers carries a "deleted"
 * date. Linking it into such a container again (moveItems without a source)
 * clears the date, and without one left it leaves the Trash. Of a folder
 * that is gone only the Trash knows: removing the item from the Trash drops
 * that, while the item stays in its folders. Moving it out of the Trash
 * would take it out of those as well; that is only right for items in no
 * folder any more, which go into the Mailbox. */
static gboolean
groupwise_folder_sync_restore (CamelFolder *folder,
			       GCancellable *cancellable,
			       GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	CamelStore *store = camel_folder_get_parent_store (folder);
	CamelFolderChangeInfo *changes;
	GHashTable *links, *moves, *targets;
	GPtrArray *unlink;
	GHashTableIter iter;
	gpointer key;
	OriginsData data;
	GPtrArray *uids, *restore;
	EGwConnection *cnc;
	gchar *mailbox_id = NULL;
	gboolean success;
	guint ii;

	uids = camel_folder_summary_get_array (summary);
	restore = g_ptr_array_new ();
	for (ii = 0; ii < uids->len; ii++) {
		CamelMessageInfo *info = camel_folder_summary_get (summary, uids->pdata[ii]);

		if (info && camel_message_info_get_user_flag (info, CAMEL_GROUPWISE_RESTORE_FLAG))
			g_ptr_array_add (restore, uids->pdata[ii]);
		g_clear_object (&info);
	}

	if (restore->len == 0) {
		g_ptr_array_unref (restore);
		camel_folder_summary_free_array (uids);
		return TRUE;
	}

	cnc = folder_ref_connection_sync (folder, cancellable, error);
	if (!cnc) {
		g_ptr_array_unref (restore);
		camel_folder_summary_free_array (uids);
		return FALSE;
	}

	data.trash_id = gw_folder->id;
	data.origins = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) origins_free);
	success = e_gw_connection_foreach_item_sync (cnc, gw_folder->id, "id container peek", NULL,
		STATE_PAGE_SIZE, collect_origins_cb, &data, cancellable, error);

	links = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
	moves = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
	unlink = g_ptr_array_new ();

	for (ii = 0; success && ii < restore->len; ii++) {
		const gchar *uid = restore->pdata[ii];
		Origins *origins = g_hash_table_lookup (data.origins, uid);
		guint jj;

		/* No longer in the Trash on the server */
		if (!origins)
			continue;

		for (jj = 0; jj < origins->deleted_from->len; jj++)
			add_to_group (links, origins->deleted_from->pdata[jj], uid);
		if (origins->deleted_from->len > 0 || origins->elsewhere) {
			if (origins->from_gone_folder)
				g_ptr_array_add (unlink, (gpointer) uid);
			continue;
		}

		if (!mailbox_id) {
			CamelGroupwiseStoreSummary *store_summary =
				camel_groupwise_store_get_summary (CAMEL_GROUPWISE_STORE (store));
			gchar *full_name = camel_groupwise_store_summary_dup_full_name_by_type (store_summary, E_GW_FOLDER_TYPE_MAILBOX);

			mailbox_id = full_name ? camel_groupwise_store_summary_dup_id (store_summary, full_name) : NULL;
			g_free (full_name);
		}
		if (!mailbox_id) {
			g_set_error_literal (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID,
				_("The Mailbox folder is not known"));
			success = FALSE;
			break;
		}
		add_to_group (moves, mailbox_id, uid);
	}

	if (success)
		success = move_groups (cnc, links, NULL, cancellable, error);
	if (success && unlink->len > 0) {
		g_debug ("restoring %u item(s) deleted with their folder", unlink->len);
		success = in_batches (cnc, unlink, remove_batch, gw_folder->id, cancellable, error);
	}
	if (success)
		success = move_groups (cnc, moves, gw_folder->id, cancellable, error);

	/* Out of the Trash; on failure only the flag goes, so the next
	 * synchronization does not try again and again */
	changes = camel_folder_change_info_new ();
	for (ii = 0; ii < restore->len; ii++) {
		if (success)
			forget_message (gw_folder, restore->pdata[ii], changes);
		else
			camel_folder_set_message_user_flag (folder, restore->pdata[ii], CAMEL_GROUPWISE_RESTORE_FLAG, FALSE);
	}
	camel_folder_summary_save (summary, NULL);
	if (camel_folder_change_info_changed (changes))
		camel_folder_changed (folder, changes);
	camel_folder_change_info_free (changes);

	if (success) {
		targets = g_hash_table_new (g_str_hash, g_str_equal);
		g_hash_table_iter_init (&iter, links);
		while (g_hash_table_iter_next (&iter, &key, NULL))
			g_hash_table_add (targets, key);
		g_hash_table_iter_init (&iter, moves);
		while (g_hash_table_iter_next (&iter, &key, NULL))
			g_hash_table_add (targets, key);
		refresh_folders (store, targets);
		g_hash_table_destroy (targets);
	}

	g_hash_table_destroy (links);
	g_hash_table_destroy (moves);
	g_ptr_array_unref (unlink);
	g_hash_table_destroy (data.origins);
	g_free (mailbox_id);
	g_object_unref (cnc);
	g_ptr_array_unref (restore);
	camel_folder_summary_free_array (uids);

	return success;
}

/* ------------------------------------------------------------------ */
/* Junk */

/* The user marked messages junk or not junk (Evolution sets the "learn"
 * flag then; the junk test of Evolution does not) */
static void
note_junk_learn_cb (CamelFolder *folder,
		    CamelFolderChangeInfo *info,
		    gpointer user_data)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	GPtrArray *changed = info ? camel_folder_change_info_get_changed_uids (info) : NULL;
	guint ii;

	for (ii = 0; changed && ii < changed->len; ii++) {
		guint32 flags = camel_folder_summary_get_info_flags (summary, changed->pdata[ii]);

		if (flags != (guint32) ~0 && (flags & CAMEL_MESSAGE_JUNK_LEARN) != 0) {
			g_mutex_lock (&gw_folder->learn_lock);
			g_hash_table_replace (gw_folder->learn, g_strdup (changed->pdata[ii]),
				GINT_TO_POINTER ((flags & CAMEL_MESSAGE_JUNK) != 0));
			g_mutex_unlock (&gw_folder->learn_lock);
		}
	}
}

/* The sender of an item from the Internet (internal senders have a UUID;
 * the junk lists of GroupWise are for Internet mail only), else NULL */
static gchar *
dup_internet_sender (EGwConnection *cnc,
		     const gchar *id,
		     GCancellable *cancellable)
{
	EGwResponse *response = e_gw_connection_get_item_sync (cnc, id, "id distribution peek", cancellable, NULL);
	xmlNode *from = response ? e_gw_xml_find (e_gw_response_get_node (response), "item/distribution/from") : NULL;
	gchar *address = NULL;

	if (from && !e_gw_xml_find (from, "uuid")) {
		address = e_gw_xml_dup_text (from, "email");
		if (address && !strchr (address, '@'))
			g_clear_pointer (&address, g_free);
	}
	e_gw_response_free (response);

	return address;
}

static gboolean
move_to (CamelFolder *folder,
	 GPtrArray *uids,
	 EGwFolderType type,
	 GCancellable *cancellable,
	 GError **error)
{
	CamelStore *store = camel_folder_get_parent_store (folder);
	CamelGroupwiseStoreSummary *store_summary = camel_groupwise_store_get_summary (CAMEL_GROUPWISE_STORE (store));
	gchar *full_name = camel_groupwise_store_summary_dup_full_name_by_type (store_summary, type);
	CamelFolder *target;
	gboolean success;

	/* GroupWise makes its Junk Mail folder when junk handling is on */
	if (!full_name) {
		g_debug ("junk: no folder of type %d, the messages stay", type);
		return TRUE;
	}

	target = camel_store_get_folder_sync (store, full_name, 0, cancellable, error);
	g_free (full_name);
	if (!target)
		return FALSE;

	g_debug ("junk: moving %u message(s) into %s", uids->len, camel_folder_get_full_name (target));
	success = camel_folder_transfer_messages_to_sync (folder, uids, target, TRUE, NULL, cancellable, error);
	g_object_unref (target);

	return success;
}

/* Messages marked junk go into the Junk Mail folder, ones marked not junk
 * from there into the Mailbox; what the user marked tells GroupWise's junk
 * lists about the (Internet) sender */
static gboolean
groupwise_folder_sync_junk (CamelFolder *folder,
			    GCancellable *cancellable,
			    GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	GPtrArray *uids, *to_junk, *to_mailbox;
	GHashTable *learn;
	EGwConnection *cnc = NULL;
	gboolean success = TRUE;
	guint ii;

	/* A search result folder shows items of other folders; the Trash keeps them */
	if (gw_folder->is_query || gw_folder->is_trash)
		return TRUE;

	g_mutex_lock (&gw_folder->learn_lock);
	learn = gw_folder->learn;
	gw_folder->learn = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	g_mutex_unlock (&gw_folder->learn_lock);

	to_junk = g_ptr_array_new_with_free_func ((GDestroyNotify) camel_pstring_free);
	to_mailbox = g_ptr_array_new_with_free_func ((GDestroyNotify) camel_pstring_free);
	uids = camel_folder_summary_get_array (summary);
	for (ii = 0; ii < uids->len; ii++) {
		guint32 flags = camel_folder_summary_get_info_flags (summary, uids->pdata[ii]);

		if (flags == (guint32) ~0 || (flags & CAMEL_MESSAGE_DELETED) != 0)
			continue;
		/* The "learn" flag still there (without a junk filter nobody
		 * clears it): the user marked it */
		if ((flags & CAMEL_MESSAGE_JUNK_LEARN) != 0 && !g_hash_table_contains (learn, uids->pdata[ii])) {
			CamelMessageInfo *info = camel_folder_summary_get (summary, uids->pdata[ii]);

			g_hash_table_insert (learn, g_strdup (uids->pdata[ii]), GINT_TO_POINTER ((flags & CAMEL_MESSAGE_JUNK) != 0));
			if (info)
				camel_message_info_set_flags (info, CAMEL_MESSAGE_JUNK_LEARN, 0);
			g_clear_object (&info);
		}
		if (!gw_folder->is_junk && (flags & CAMEL_MESSAGE_JUNK) != 0)
			g_ptr_array_add (to_junk, (gpointer) camel_pstring_strdup (uids->pdata[ii]));
		else if (gw_folder->is_junk && (flags & CAMEL_MESSAGE_NOTJUNK) != 0)
			g_ptr_array_add (to_mailbox, (gpointer) camel_pstring_strdup (uids->pdata[ii]));
	}
	camel_folder_summary_free_array (uids);

	if (to_junk->len == 0 && to_mailbox->len == 0 && g_hash_table_size (learn) == 0)
		goto out;

	cnc = folder_ref_connection_sync (folder, cancellable, error);
	if (!cnc) {
		success = FALSE;
		goto out;
	}

	/* The lists first: the IDs change with the move */
	if (g_hash_table_size (learn) > 0) {
		GPtrArray *entries = e_gw_connection_get_junk_entries_sync (cnc, cancellable, NULL);
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init (&iter, learn);
		while (entries && g_hash_table_iter_next (&iter, &key, &value)) {
			gchar *address = dup_internet_sender (cnc, key, cancellable);
			GError *local_error = NULL;

			/* A failure here does not keep the messages where they are */
			if (address && !e_gw_connection_learn_junk_sender_sync (cnc, address, GPOINTER_TO_INT (value),
					entries, cancellable, &local_error)) {
				g_debug ("junk: %s: %s", address, local_error ? local_error->message : "?");
				g_clear_error (&local_error);
			}
			g_free (address);
		}
		if (entries)
			g_ptr_array_unref (entries);
	}

	if (to_junk->len > 0)
		success = move_to (folder, to_junk, E_GW_FOLDER_TYPE_JUNK, cancellable, error);
	if (success && to_mailbox->len > 0)
		success = move_to (folder, to_mailbox, E_GW_FOLDER_TYPE_MAILBOX, cancellable, error);

 out:
	g_hash_table_destroy (learn);
	g_ptr_array_unref (to_junk);
	g_ptr_array_unref (to_mailbox);
	g_clear_object (&cnc);

	return success;
}

static gboolean
groupwise_folder_synchronize_sync (CamelFolder *folder,
				   gboolean expunge,
				   GCancellable *cancellable,
				   GError **error)
{
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);

	if (!folder_is_online (folder))
		return TRUE;

	/* Local changes stay local; they are kept apart from server changes
	 * in the next refresh as before */
	if (folder_is_read_only (folder))
		return camel_folder_summary_save (summary, error);

	/* Restored and deleted first: their flags need not be written */
	if (CAMEL_GROUPWISE_FOLDER (folder)->is_trash &&
	    !groupwise_folder_sync_restore (folder, cancellable, error))
		return FALSE;
	if (!groupwise_folder_sync_junk (folder, cancellable, error))
		return FALSE;

	return groupwise_folder_sync_deleted (folder, expunge, cancellable, error) &&
	       groupwise_folder_sync_flags (folder, cancellable, error);
}

static gboolean
groupwise_folder_expunge_sync (CamelFolder *folder,
			       GCancellable *cancellable,
			       GError **error)
{
	if (!folder_is_online (folder) || folder_is_read_only (folder))
		return TRUE;

	return groupwise_folder_sync_deleted (folder, TRUE, cancellable, error);
}

/* Moves or copies within the account on the server; the summary and the
 * cached message go along under the ID in the destination. */
static gboolean
groupwise_folder_transfer_messages_to_sync (CamelFolder *source,
					    GPtrArray *uids,
					    CamelFolder *destination,
					    gboolean delete_originals,
					    GPtrArray **transferred_uids,
					    GCancellable *cancellable,
					    GError **error)
{
	CamelGroupwiseFolder *src = CAMEL_GROUPWISE_FOLDER (source), *dst;
	CamelFolderSummary *src_summary = camel_folder_get_folder_summary (source), *dst_summary;
	CamelFolderChangeInfo *src_changes, *dst_changes;
	EGwConnection *cnc;
	MoveData data;
	gboolean success;
	guint ii;

	/* Another account: the generic way (fetch and append) */
	if (!CAMEL_IS_GROUPWISE_FOLDER (destination) ||
	    camel_folder_get_parent_store (destination) != camel_folder_get_parent_store (source))
		return CAMEL_FOLDER_CLASS (camel_groupwise_folder_parent_class)->transfer_messages_to_sync (
			source, uids, destination, delete_originals, transferred_uids, cancellable, error);

	if (folder_is_read_only (source)) {
		g_set_error_literal (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID,
			_("The account is read-only"));
		return FALSE;
	}

	dst = CAMEL_GROUPWISE_FOLDER (destination);
	dst_summary = camel_folder_get_folder_summary (destination);

	if (dst->is_query) {
		g_set_error_literal (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID,
			_("A search result folder shows messages of other folders; nothing can be moved or copied into it"));
		return FALSE;
	}

	cnc = folder_ref_connection_sync (source, cancellable, error);
	if (!cnc)
		return FALSE;

	/* moveItems with a source takes the items out of it, without one it links them */
	data.container = dst->id;
	if (src->is_query && delete_originals) {
		/* Out of the folder each message is in */
		GHashTable *groups = group_by_container (uids);
		GHashTableIter iter;
		gpointer key, value;

		success = TRUE;
		g_hash_table_iter_init (&iter, groups);
		while (success && g_hash_table_iter_next (&iter, &key, &value)) {
			data.from = key;
			success = in_batches (cnc, value, move_batch, &data, cancellable, error);
		}
		g_hash_table_destroy (groups);
	} else {
		data.from = delete_originals ? src->id : NULL;
		success = in_batches (cnc, uids, move_batch, &data, cancellable, error);
	}
	g_object_unref (cnc);

	if (!success)
		return FALSE;

	src_changes = camel_folder_change_info_new ();
	dst_changes = camel_folder_change_info_new ();
	if (transferred_uids)
		*transferred_uids = g_ptr_array_new ();

	for (ii = 0; ii < uids->len; ii++) {
		const gchar *uid = uids->pdata[ii];
		gchar *new_uid = e_gw_item_id_in_container (uid, dst->id);
		CamelMessageInfo *info = camel_folder_summary_get (src_summary, uid);
		GIOStream *cached;

		if (info && !camel_folder_summary_check_uid (dst_summary, new_uid)) {
			CamelMessageInfo *clone = camel_message_info_clone (info, dst_summary);

			camel_message_info_set_uid (clone, new_uid);
			camel_message_info_set_flags (clone, CAMEL_MESSAGE_DELETED, 0);
			camel_folder_summary_add (dst_summary, clone, TRUE);
			camel_folder_change_info_add_uid (dst_changes, new_uid);
			g_object_unref (clone);
		}
		g_clear_object (&info);

		/* No second download for a message already here */
		cached = camel_data_cache_get (src->cache, CACHE_PATH, uid, NULL);
		if (cached) {
			GIOStream *copy = camel_data_cache_add (dst->cache, CACHE_PATH, new_uid, NULL);

			if (copy) {
				if (g_output_stream_splice (g_io_stream_get_output_stream (copy),
					g_io_stream_get_input_stream (cached), 0, cancellable, NULL) < 0)
					camel_data_cache_remove (dst->cache, CACHE_PATH, new_uid, NULL);
				g_object_unref (copy);
			}
			g_object_unref (cached);
		}

		if (delete_originals)
			forget_message (src, uid, src_changes);

		if (transferred_uids)
			g_ptr_array_add (*transferred_uids, (gpointer) camel_pstring_strdup (new_uid));
		g_free (new_uid);
	}

	camel_folder_summary_save (src_summary, NULL);
	camel_folder_summary_save (dst_summary, NULL);

	if (camel_folder_change_info_changed (src_changes))
		camel_folder_changed (source, src_changes);
	if (camel_folder_change_info_changed (dst_changes))
		camel_folder_changed (destination, dst_changes);
	camel_folder_change_info_free (src_changes);
	camel_folder_change_info_free (dst_changes);

	return TRUE;
}

/* Saving a draft. The POA creates drafts in the Mailbox; like the GroupWise
 * client keeps them, the draft is moved into this folder ("Work In
 * Progress"). Other messages cannot be stored: GroupWise has no way to make
 * a received message from MIME. */
static gboolean
groupwise_folder_append_message_sync (CamelFolder *folder,
				      CamelMimeMessage *message,
				      CamelMessageInfo *info,
				      gchar **appended_uid,
				      GCancellable *cancellable,
				      GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	CamelFolderChangeInfo *changes;
	CamelMessageInfo *mi;
	EGwConnection *cnc;
	EGwResponse *response;
	GIOStream *stream;
	gchar *item, *raw, *id, *uid;
	const gchar *colon;
	gboolean success = TRUE;

	if (folder_is_read_only (folder)) {
		g_set_error_literal (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID, _("The account is read-only"));
		return FALSE;
	}

	if (!gw_folder->is_drafts) {
		g_set_error_literal (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID,
			_("GroupWise can only store drafts this way. Messages can be moved or copied between GroupWise folders."));
		return FALSE;
	}

	item = camel_groupwise_item_from_message (message, NULL, TRUE, NULL, NULL, cancellable, error);
	if (!item)
		return FALSE;

	cnc = folder_ref_connection_sync (folder, cancellable, error);
	if (!cnc) {
		g_free (item);
		return FALSE;
	}

	response = e_gw_connection_call_sync (cnc, "sendItem", item, cancellable, error);
	g_free (item);
	if (!response) {
		g_object_unref (cnc);
		return FALSE;
	}

	raw = e_gw_xml_dup_text (e_gw_response_get_node (response), "id");
	id = raw && *raw ? e_gw_clean_id (raw) : NULL;
	g_free (raw);
	e_gw_response_free (response);

	if (!id) {
		g_set_error_literal (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID, _("The server returned no ID for the draft"));
		g_object_unref (cnc);
		return FALSE;
	}

	uid = e_gw_item_id_in_container (id, gw_folder->id);
	colon = strchr (id, ':');
	if (colon && g_strcmp0 (colon + 1, gw_folder->id) != 0) {
		const gchar *ids[] = { id, NULL };

		success = e_gw_connection_move_items_sync (cnc, ids, gw_folder->id, colon + 1, cancellable, error);
	}
	g_object_unref (cnc);

	if (!success) {
		g_free (uid);
		g_free (id);
		return FALSE;
	}

	/* Known and readable at once, offline too */
	mi = camel_folder_summary_info_new_from_message (summary, message);
	camel_message_info_set_uid (mi, uid);
	camel_message_info_set_flags (mi, ~0, CAMEL_MESSAGE_DRAFT | CAMEL_MESSAGE_SEEN |
		(info ? camel_message_info_get_flags (info) & CAMEL_MESSAGE_USER : 0));
	camel_groupwise_message_info_set_server_flags (CAMEL_GROUPWISE_MESSAGE_INFO (mi), CAMEL_MESSAGE_SEEN);
	camel_message_info_set_folder_flagged (mi, FALSE);
	camel_folder_summary_add (summary, mi, TRUE);
	g_object_unref (mi);

	stream = camel_data_cache_add (gw_folder->cache, CACHE_PATH, uid, NULL);
	if (stream) {
		if (camel_data_wrapper_write_to_output_stream_sync (CAMEL_DATA_WRAPPER (message),
			g_io_stream_get_output_stream (stream), cancellable, NULL) < 0)
			camel_data_cache_remove (gw_folder->cache, CACHE_PATH, uid, NULL);
		g_object_unref (stream);
	}

	camel_folder_summary_save (summary, NULL);
	changes = camel_folder_change_info_new ();
	camel_folder_change_info_add_uid (changes, uid);
	camel_folder_changed (folder, changes);
	camel_folder_change_info_free (changes);

	if (appended_uid)
		*appended_uid = uid;
	else
		g_free (uid);
	g_free (id);

	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Messages */

static CamelMimeMessage *
groupwise_folder_get_message_cached (CamelFolder *folder,
				     const gchar *message_uid,
				     GCancellable *cancellable)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	CamelMimeMessage *message;
	GIOStream *stream;

	stream = camel_data_cache_get (gw_folder->cache, CACHE_PATH, message_uid, NULL);
	if (!stream)
		return NULL;

	message = camel_mime_message_new ();
	if (!camel_data_wrapper_construct_from_input_stream_sync (CAMEL_DATA_WRAPPER (message),
			g_io_stream_get_input_stream (stream), cancellable, NULL))
		g_clear_object (&message);

	g_object_unref (stream);

	/* The subject as GroupWise shows it: the user may have changed it; the
	 * cached original stays as it came */
	if (message) {
		CamelMessageInfo *info = camel_folder_summary_get (camel_folder_get_folder_summary (folder), message_uid);
		const gchar *subject = info ? camel_message_info_get_subject (info) : NULL;

		if (subject && *subject && g_strcmp0 (subject, camel_mime_message_get_subject (message)) != 0)
			camel_mime_message_set_subject (message, subject);
		g_clear_object (&info);

		/* Forwarded as attachment, it goes as the GroupWise item itself */
		camel_medium_set_header (CAMEL_MEDIUM (message), CAMEL_GROUPWISE_ITEM_ID_HEADER, message_uid);
	}

	return message;
}

/* One download into the cache entry of @message_uid */
static gboolean
download_to_cache (CamelGroupwiseFolder *gw_folder,
		   EGwConnection *cnc,
		   const gchar *message_uid,
		   const gchar *id,
		   gboolean as_mime,
		   GCancellable *cancellable,
		   GError **error)
{
	GIOStream *stream;
	gboolean success;

	stream = camel_data_cache_add (gw_folder->cache, CACHE_PATH, message_uid, error);
	if (!stream)
		return FALSE;

	success = e_gw_connection_download_sync (cnc, id, as_mime, g_io_stream_get_output_stream (stream),
			cancellable, error) &&
		g_output_stream_flush (g_io_stream_get_output_stream (stream), cancellable, error);

	g_object_unref (stream);

	if (!success)
		camel_data_cache_remove (gw_folder->cache, CACHE_PATH, message_uid, NULL);

	return success;
}

/* The hidden "Mime.822" attachment of a message from the Internet */
static gchar *
find_original_mime (EGwConnection *cnc,
		    const gchar *message_uid,
		    GCancellable *cancellable)
{
	EGwResponse *response;
	xmlNode *node;
	gchar *id = NULL;

	response = e_gw_connection_get_item_sync (cnc, message_uid, "attachments peek", cancellable, NULL);
	if (!response)
		return NULL;

	node = e_gw_xml_find (e_gw_response_get_node (response), "item/attachments");
	for (node = e_gw_xml_first_child (node, "attachment"); node && !id; node = e_gw_xml_next_sibling (node, "attachment")) {
		gchar *name = e_gw_xml_dup_text (node, "name");

		if (name && g_ascii_strcasecmp (name, "Mime.822") == 0) {
			gchar *raw = e_gw_xml_dup_text (node, "id");

			id = e_gw_clean_id (raw);
			g_free (raw);
		}
		g_free (name);
	}

	e_gw_response_free (response);

	return id;
}

/* Brings the message into the cache. For mail from the Internet the POA
 * keeps the original as the hidden attachment "Mime.822": exact headers,
 * Message-ID and signatures, which mime=1 would rebuild or lose. Like the
 * gac archive connector, the attachment is tried first, then with the ID
 * suffix of this message (after a move it may name another copy), and
 * finally the item as RFC 822. */
static gboolean
groupwise_folder_download (CamelFolder *folder,
			   const gchar *message_uid,
			   GCancellable *cancellable,
			   GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	EGwConnection *cnc;
	GError *local_error = NULL;
	gchar *original;
	gboolean success = FALSE;

	cnc = folder_ref_connection_sync (folder, cancellable, error);
	if (!cnc)
		return FALSE;

	original = find_original_mime (cnc, message_uid, cancellable);
	if (original) {
		success = download_to_cache (gw_folder, cnc, message_uid, original, FALSE, cancellable, &local_error);

		if (!success && !g_cancellable_is_cancelled (cancellable)) {
			const gchar *colon = strchr (original, ':');
			gchar *alternative = g_strdup_printf ("%.*s:%s",
				(gint) (colon ? colon - original : (gssize) strlen (original)), original, message_uid);

			if (g_strcmp0 (alternative, original) != 0) {
				g_clear_error (&local_error);
				success = download_to_cache (gw_folder, cnc, message_uid, alternative, FALSE, cancellable, &local_error);
			}
			g_free (alternative);
		}
		g_free (original);
	}

	if (!success && !g_cancellable_is_cancelled (cancellable)) {
		g_clear_error (&local_error);
		success = download_to_cache (gw_folder, cnc, message_uid, message_uid, TRUE, cancellable, &local_error);
	}

	g_object_unref (cnc);

	/* Cancelled before any attempt: no attempt left an error */
	if (!success && !local_error && !g_cancellable_set_error_if_cancelled (cancellable, &local_error))
		g_set_error (&local_error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID,
			_("Cannot get the message %s"), message_uid);

	if (local_error)
		g_propagate_error (error, local_error);

	return success;
}

/* SOAP has no References and no Message-ID for mail from the Internet; the
 * message itself has both, so the thread view fills in as messages arrive. */
static void
update_threading (CamelFolder *folder,
		  const gchar *message_uid,
		  CamelMimeMessage *message)
{
	CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
	CamelMessageInfo *info, *parsed;

	info = camel_folder_summary_get (summary, message_uid);
	if (!info)
		return;

	parsed = camel_message_info_new_from_headers (summary, camel_medium_get_headers (CAMEL_MEDIUM (message)));
	if (camel_message_info_get_message_id (parsed))
		camel_message_info_set_message_id (info, camel_message_info_get_message_id (parsed));
	if (camel_message_info_get_references (parsed))
		camel_message_info_take_references (info, camel_message_info_dup_references (parsed));

	g_object_unref (parsed);
	g_object_unref (info);
}

static CamelMimeMessage *
groupwise_folder_get_message_sync (CamelFolder *folder,
				   const gchar *message_uid,
				   GCancellable *cancellable,
				   GError **error)
{
	CamelMimeMessage *message;

	message = groupwise_folder_get_message_cached (folder, message_uid, cancellable);
	if (message)
		return message;

	if (!folder_is_online (folder)) {
		g_set_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
			_("This message is not available in offline mode."));
		return NULL;
	}

	if (!groupwise_folder_download (folder, message_uid, cancellable, error))
		return NULL;

	message = groupwise_folder_get_message_cached (folder, message_uid, cancellable);
	if (!message)
		g_set_error (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID,
			_("Cannot read the message %s"), message_uid);
	else
		update_threading (folder, message_uid, message);

	return message;
}

static gboolean
groupwise_folder_synchronize_message_sync (CamelFolder *folder,
					   const gchar *message_uid,
					   GCancellable *cancellable,
					   GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	GIOStream *stream = camel_data_cache_get (gw_folder->cache, CACHE_PATH, message_uid, NULL);

	if (stream) {
		g_object_unref (stream);
		return TRUE;
	}

	return groupwise_folder_download (folder, message_uid, cancellable, error);
}

static GPtrArray *
groupwise_folder_get_uncached_uids (CamelFolder *folder,
				    GPtrArray *uids,
				    GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	GPtrArray *uncached = g_ptr_array_new ();
	guint ii;

	for (ii = 0; ii < uids->len; ii++) {
		GIOStream *stream = camel_data_cache_get (gw_folder->cache, CACHE_PATH, uids->pdata[ii], NULL);

		if (stream)
			g_object_unref (stream);
		else
			g_ptr_array_add (uncached, (gpointer) camel_pstring_strdup (uids->pdata[ii]));
	}

	return uncached;
}

static gchar *
groupwise_folder_get_filename (CamelFolder *folder,
			       const gchar *uid,
			       GError **error)
{
	return camel_data_cache_get_filename (CAMEL_GROUPWISE_FOLDER (folder)->cache, CACHE_PATH, uid);
}

static guint32
groupwise_folder_get_permanent_flags (CamelFolder *folder)
{
	/* Labels, "important" and the like are kept by Evolution itself */
	return CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_ANSWERED | CAMEL_MESSAGE_FLAGGED |
	       CAMEL_MESSAGE_DRAFT | CAMEL_MESSAGE_DELETED | CAMEL_MESSAGE_USER;
}

/* ------------------------------------------------------------------ */
/* Searching the local summary */

static GPtrArray *
groupwise_folder_search_by_expression (CamelFolder *folder,
				       const gchar *expression,
				       GCancellable *cancellable,
				       GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	GPtrArray *matches;

	g_mutex_lock (&gw_folder->search_lock);
	camel_folder_search_set_folder (gw_folder->search, folder);
	matches = camel_folder_search_search (gw_folder->search, expression, NULL, cancellable, error);
	g_mutex_unlock (&gw_folder->search_lock);

	return matches;
}

static GPtrArray *
groupwise_folder_search_by_uids (CamelFolder *folder,
				 const gchar *expression,
				 GPtrArray *uids,
				 GCancellable *cancellable,
				 GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	GPtrArray *matches;

	if (uids->len == 0)
		return g_ptr_array_new ();

	g_mutex_lock (&gw_folder->search_lock);
	camel_folder_search_set_folder (gw_folder->search, folder);
	matches = camel_folder_search_search (gw_folder->search, expression, uids, cancellable, error);
	g_mutex_unlock (&gw_folder->search_lock);

	return matches;
}

static guint32
groupwise_folder_count_by_expression (CamelFolder *folder,
				      const gchar *expression,
				      GCancellable *cancellable,
				      GError **error)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	guint32 count;

	g_mutex_lock (&gw_folder->search_lock);
	camel_folder_search_set_folder (gw_folder->search, folder);
	count = camel_folder_search_count (gw_folder->search, expression, cancellable, error);
	g_mutex_unlock (&gw_folder->search_lock);

	return count;
}

static void
groupwise_folder_search_free (CamelFolder *folder,
			      GPtrArray *uids)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (folder);

	g_return_if_fail (gw_folder->search != NULL);

	g_mutex_lock (&gw_folder->search_lock);
	camel_folder_search_free_result (gw_folder->search, uids);
	g_mutex_unlock (&gw_folder->search_lock);
}

/* ------------------------------------------------------------------ */

static void
groupwise_folder_finalize (GObject *object)
{
	CamelGroupwiseFolder *gw_folder = CAMEL_GROUPWISE_FOLDER (object);

	g_clear_object (&gw_folder->cache);
	g_clear_object (&gw_folder->search);
	g_mutex_clear (&gw_folder->search_lock);
	g_mutex_clear (&gw_folder->refresh_lock);
	g_free (gw_folder->id);
	g_free (gw_folder->quick_since);
	g_hash_table_destroy (gw_folder->invitations);
	g_mutex_clear (&gw_folder->learn_lock);
	g_hash_table_destroy (gw_folder->learn);

	G_OBJECT_CLASS (camel_groupwise_folder_parent_class)->finalize (object);
}

static void
camel_groupwise_folder_class_init (CamelGroupwiseFolderClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	CamelFolderClass *folder_class = CAMEL_FOLDER_CLASS (class);

	object_class->finalize = groupwise_folder_finalize;

	folder_class->get_permanent_flags = groupwise_folder_get_permanent_flags;
	folder_class->search_by_expression = groupwise_folder_search_by_expression;
	folder_class->search_by_uids = groupwise_folder_search_by_uids;
	folder_class->count_by_expression = groupwise_folder_count_by_expression;
	folder_class->search_free = groupwise_folder_search_free;
	folder_class->get_uncached_uids = groupwise_folder_get_uncached_uids;
	folder_class->get_filename = groupwise_folder_get_filename;
	folder_class->get_message_cached = groupwise_folder_get_message_cached;
	folder_class->get_message_sync = groupwise_folder_get_message_sync;
	folder_class->refresh_info_sync = groupwise_folder_refresh_info_sync;
	folder_class->synchronize_sync = groupwise_folder_synchronize_sync;
	folder_class->synchronize_message_sync = groupwise_folder_synchronize_message_sync;
	folder_class->expunge_sync = groupwise_folder_expunge_sync;
	folder_class->append_message_sync = groupwise_folder_append_message_sync;
	folder_class->transfer_messages_to_sync = groupwise_folder_transfer_messages_to_sync;
}

static void
camel_groupwise_folder_init (CamelGroupwiseFolder *gw_folder)
{
	g_mutex_init (&gw_folder->search_lock);
	g_mutex_init (&gw_folder->refresh_lock);
	gw_folder->search = camel_folder_search_new ();
	gw_folder->invitations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	g_mutex_init (&gw_folder->learn_lock);
	gw_folder->learn = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

/* Follows the setting while the folder is open */
static void
filter_inbox_changed_cb (CamelSettings *settings,
			 GParamSpec *param,
			 CamelFolder *folder)
{
	guint32 flags = camel_folder_get_flags (folder);

	if (camel_groupwise_settings_get_filter_inbox (CAMEL_GROUPWISE_SETTINGS (settings)))
		flags |= CAMEL_FOLDER_FILTER_RECENT;
	else
		flags &= ~CAMEL_FOLDER_FILTER_RECENT;

	camel_folder_set_flags (folder, flags);
}

CamelFolder *
camel_groupwise_folder_new (CamelStore *store,
			    const gchar *full_name,
			    const gchar *id,
			    const gchar *folder_dir,
			    GCancellable *cancellable,
			    GError **error)
{
	CamelGroupwiseStoreSummary *store_summary;
	CamelGroupwiseFolder *gw_folder;
	CamelFolder *folder;
	CamelFolderSummary *summary;
	const gchar *short_name;
	gchar *path;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_STORE (store), NULL);
	g_return_val_if_fail (full_name != NULL, NULL);
	g_return_val_if_fail (id != NULL, NULL);

	short_name = strrchr (full_name, '/');
	short_name = short_name ? short_name + 1 : full_name;

	if (g_mkdir_with_parents (folder_dir, 0700) != 0) {
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
			_("Cannot create folder cache %s: %s"), folder_dir, g_strerror (errno));
		return NULL;
	}

	folder = g_object_new (CAMEL_TYPE_GROUPWISE_FOLDER,
		"display-name", short_name,
		"full-name", full_name,
		"parent-store", store,
		NULL);
	gw_folder = CAMEL_GROUPWISE_FOLDER (folder);
	gw_folder->id = g_strdup (id);

	summary = camel_groupwise_summary_new (folder);
	camel_folder_take_folder_summary (folder, summary);

	/* Keeps folder properties such as "offline-sync" */
	path = g_build_filename (folder_dir, "cmeta", NULL);
	camel_object_set_state_filename (CAMEL_OBJECT (folder), path);
	camel_object_state_read (CAMEL_OBJECT (folder));
	g_free (path);

	path = g_build_filename (folder_dir, "cache", NULL);
	gw_folder->cache = camel_data_cache_new (path, error);
	g_free (path);
	if (!gw_folder->cache) {
		g_object_unref (folder);
		return NULL;
	}

	/* New mail in the Mailbox runs through Evolution's filters if wanted */
	store_summary = camel_groupwise_store_get_summary (CAMEL_GROUPWISE_STORE (store));
	gw_folder->is_trash = camel_groupwise_store_summary_get_type (store_summary, full_name) == E_GW_FOLDER_TYPE_TRASH;
	gw_folder->is_query = camel_groupwise_store_summary_get_type (store_summary, full_name) == E_GW_FOLDER_TYPE_QUERY;
	/* The POA answers no quick check in the Trash and in search result
	 * folders, nor in a proxy session (error 59414): there every refresh
	 * reads the state of the items */
	gw_folder->quick_supported = !gw_folder->is_trash && !gw_folder->is_query && !store_is_proxy (store);
	/* Their messages are in other folders already (and "all mail" is big) */
	if (gw_folder->is_query)
		camel_offline_folder_set_offline_sync (CAMEL_OFFLINE_FOLDER (folder), CAMEL_THREE_STATE_OFF);
	gw_folder->is_drafts = camel_groupwise_store_summary_get_type (store_summary, full_name) == E_GW_FOLDER_TYPE_DRAFT;
	gw_folder->is_junk = camel_groupwise_store_summary_get_type (store_summary, full_name) == E_GW_FOLDER_TYPE_JUNK;
	/* Evolution shows the junk there, and hides it everywhere else */
	if (gw_folder->is_junk)
		camel_folder_set_flags (folder, camel_folder_get_flags (folder) | CAMEL_FOLDER_IS_JUNK);
	g_signal_connect (folder, "changed", G_CALLBACK (note_junk_learn_cb), NULL);
	if (camel_groupwise_store_summary_get_type (store_summary, full_name) == E_GW_FOLDER_TYPE_MAILBOX) {
		CamelSettings *settings = camel_service_ref_settings (CAMEL_SERVICE (store));

		g_signal_connect_object (settings, "notify::filter-inbox", G_CALLBACK (filter_inbox_changed_cb), folder, 0);
		filter_inbox_changed_cb (settings, NULL, folder);
		g_object_unref (settings);
	}

	return folder;
}

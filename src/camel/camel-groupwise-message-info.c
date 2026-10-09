/*
 * camel-groupwise-message-info.c: message info with the last known server flags
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

#include "camel-groupwise-message-info.h"

struct _CamelGroupwiseMessageInfo {
	CamelMessageInfoBase parent;

	guint32 server_flags;
	gchar *server_categories;	/* category IDs, space separated */
	gchar *server_followup;		/* the Tasklist state, see the header */
};

G_DEFINE_TYPE (CamelGroupwiseMessageInfo, camel_groupwise_message_info, CAMEL_TYPE_MESSAGE_INFO_BASE)

static CamelMessageInfo *
groupwise_message_info_clone (const CamelMessageInfo *mi,
			      CamelFolderSummary *assign_summary)
{
	CamelMessageInfo *result;

	result = CAMEL_MESSAGE_INFO_CLASS (camel_groupwise_message_info_parent_class)->clone (mi, assign_summary);
	if (CAMEL_IS_GROUPWISE_MESSAGE_INFO (result)) {
		CAMEL_GROUPWISE_MESSAGE_INFO (result)->server_flags = ((const CamelGroupwiseMessageInfo *) mi)->server_flags;
		CAMEL_GROUPWISE_MESSAGE_INFO (result)->server_categories =
			g_strdup (((const CamelGroupwiseMessageInfo *) mi)->server_categories);
		CAMEL_GROUPWISE_MESSAGE_INFO (result)->server_followup =
			g_strdup (((const CamelGroupwiseMessageInfo *) mi)->server_followup);
	}

	return result;
}

static gboolean
groupwise_message_info_load (CamelMessageInfo *mi,
			     const CamelMIRecord *record,
			     gchar **bdata_ptr)
{
	if (!CAMEL_MESSAGE_INFO_CLASS (camel_groupwise_message_info_parent_class)->load (mi, record, bdata_ptr))
		return FALSE;

	/* Summaries from before this field read as 0 and are corrected by the next refresh */
	CAMEL_GROUPWISE_MESSAGE_INFO (mi)->server_flags = (guint32) camel_util_bdata_get_number (bdata_ptr, 0);
	/* ... and from before the categories as without any */
	CAMEL_GROUPWISE_MESSAGE_INFO (mi)->server_categories = camel_util_bdata_get_string (bdata_ptr, NULL);
	/* ... and from before the Tasklist as not known; "-" is "not on it"
	 * (an empty string reads back as none) */
	CAMEL_GROUPWISE_MESSAGE_INFO (mi)->server_followup = camel_util_bdata_get_string (bdata_ptr, NULL);
	if (g_strcmp0 (CAMEL_GROUPWISE_MESSAGE_INFO (mi)->server_followup, "-") == 0)
		CAMEL_GROUPWISE_MESSAGE_INFO (mi)->server_followup[0] = '\0';

	return TRUE;
}

static gboolean
groupwise_message_info_save (const CamelMessageInfo *mi,
			     CamelMIRecord *record,
			     GString *bdata_str)
{
	if (!CAMEL_MESSAGE_INFO_CLASS (camel_groupwise_message_info_parent_class)->save (mi, record, bdata_str))
		return FALSE;

	camel_util_bdata_put_number (bdata_str, ((const CamelGroupwiseMessageInfo *) mi)->server_flags);
	camel_util_bdata_put_string (bdata_str, ((const CamelGroupwiseMessageInfo *) mi)->server_categories ?
		((const CamelGroupwiseMessageInfo *) mi)->server_categories : "");
	{
		const gchar *followup = ((const CamelGroupwiseMessageInfo *) mi)->server_followup;

		camel_util_bdata_put_string (bdata_str, !followup ? "" : *followup ? followup : "-");
	}

	return TRUE;
}

static void
groupwise_message_info_finalize (GObject *object)
{
	g_free (CAMEL_GROUPWISE_MESSAGE_INFO (object)->server_categories);
	g_free (CAMEL_GROUPWISE_MESSAGE_INFO (object)->server_followup);

	G_OBJECT_CLASS (camel_groupwise_message_info_parent_class)->finalize (object);
}

static void
camel_groupwise_message_info_class_init (CamelGroupwiseMessageInfoClass *class)
{
	CamelMessageInfoClass *mi_class = CAMEL_MESSAGE_INFO_CLASS (class);

	G_OBJECT_CLASS (class)->finalize = groupwise_message_info_finalize;

	mi_class->clone = groupwise_message_info_clone;
	mi_class->load = groupwise_message_info_load;
	mi_class->save = groupwise_message_info_save;
}

static void
camel_groupwise_message_info_init (CamelGroupwiseMessageInfo *info)
{
}

guint32
camel_groupwise_message_info_get_server_flags (CamelGroupwiseMessageInfo *info)
{
	guint32 flags;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_MESSAGE_INFO (info), 0);

	camel_message_info_property_lock (CAMEL_MESSAGE_INFO (info));
	flags = info->server_flags;
	camel_message_info_property_unlock (CAMEL_MESSAGE_INFO (info));

	return flags;
}

void
camel_groupwise_message_info_set_server_flags (CamelGroupwiseMessageInfo *info,
					       guint32 server_flags)
{
	gboolean changed;

	g_return_if_fail (CAMEL_IS_GROUPWISE_MESSAGE_INFO (info));

	camel_message_info_property_lock (CAMEL_MESSAGE_INFO (info));
	changed = info->server_flags != server_flags;
	info->server_flags = server_flags;
	camel_message_info_property_unlock (CAMEL_MESSAGE_INFO (info));

	/* Saved with the summary, but no change Evolution must see */
	if (changed && !camel_message_info_get_abort_notifications (CAMEL_MESSAGE_INFO (info)))
		camel_message_info_set_dirty (CAMEL_MESSAGE_INFO (info), TRUE);
}

gchar *
camel_groupwise_message_info_dup_server_followup (CamelGroupwiseMessageInfo *info)
{
	gchar *state;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_MESSAGE_INFO (info), g_strdup (""));

	camel_message_info_property_lock (CAMEL_MESSAGE_INFO (info));
	state = g_strdup (info->server_followup ? info->server_followup : "");
	camel_message_info_property_unlock (CAMEL_MESSAGE_INFO (info));

	return state;
}

gboolean
camel_groupwise_message_info_knows_server_followup (CamelGroupwiseMessageInfo *info)
{
	gboolean known;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_MESSAGE_INFO (info), FALSE);

	camel_message_info_property_lock (CAMEL_MESSAGE_INFO (info));
	known = info->server_followup != NULL;
	camel_message_info_property_unlock (CAMEL_MESSAGE_INFO (info));

	return known;
}

void
camel_groupwise_message_info_set_server_followup (CamelGroupwiseMessageInfo *info,
						  const gchar *state)
{
	gboolean changed;

	g_return_if_fail (CAMEL_IS_GROUPWISE_MESSAGE_INFO (info));

	camel_message_info_property_lock (CAMEL_MESSAGE_INFO (info));
	changed = g_strcmp0 (info->server_followup ? info->server_followup : "", state ? state : "") != 0;
	if (changed) {
		g_free (info->server_followup);
		info->server_followup = g_strdup (state ? state : "");
	}
	camel_message_info_property_unlock (CAMEL_MESSAGE_INFO (info));

	if (changed && !camel_message_info_get_abort_notifications (CAMEL_MESSAGE_INFO (info)))
		camel_message_info_set_dirty (CAMEL_MESSAGE_INFO (info), TRUE);
}

gchar **
camel_groupwise_message_info_dup_server_categories (CamelGroupwiseMessageInfo *info)
{
	gchar **ids;

	g_return_val_if_fail (CAMEL_IS_GROUPWISE_MESSAGE_INFO (info), NULL);

	camel_message_info_property_lock (CAMEL_MESSAGE_INFO (info));
	ids = g_strsplit (info->server_categories ? info->server_categories : "", " ", -1);
	camel_message_info_property_unlock (CAMEL_MESSAGE_INFO (info));

	/* "" splits into one empty string */
	if (ids[0] && !*ids[0])
		g_clear_pointer (&ids[0], g_free);

	return ids;
}

void
camel_groupwise_message_info_set_server_categories (CamelGroupwiseMessageInfo *info,
						    const gchar * const *ids)
{
	gchar *joined;
	gboolean changed;

	g_return_if_fail (CAMEL_IS_GROUPWISE_MESSAGE_INFO (info));

	joined = ids && ids[0] ? g_strjoinv (" ", (gchar **) ids) : NULL;
	camel_message_info_property_lock (CAMEL_MESSAGE_INFO (info));
	changed = g_strcmp0 (info->server_categories ? info->server_categories : "", joined ? joined : "") != 0;
	if (changed) {
		g_free (info->server_categories);
		info->server_categories = g_steal_pointer (&joined);
	}
	camel_message_info_property_unlock (CAMEL_MESSAGE_INFO (info));
	g_free (joined);

	if (changed && !camel_message_info_get_abort_notifications (CAMEL_MESSAGE_INFO (info)))
		camel_message_info_set_dirty (CAMEL_MESSAGE_INFO (info), TRUE);
}

/* ------------------------------------------------------------------ */

struct _CamelGroupwiseSummary {
	CamelFolderSummary parent;
};

G_DEFINE_TYPE (CamelGroupwiseSummary, camel_groupwise_summary, CAMEL_TYPE_FOLDER_SUMMARY)

static void
camel_groupwise_summary_class_init (CamelGroupwiseSummaryClass *class)
{
	CAMEL_FOLDER_SUMMARY_CLASS (class)->message_info_type = CAMEL_TYPE_GROUPWISE_MESSAGE_INFO;
}

static void
camel_groupwise_summary_init (CamelGroupwiseSummary *summary)
{
}

CamelFolderSummary *
camel_groupwise_summary_new (CamelFolder *folder)
{
	CamelFolderSummary *summary;

	summary = g_object_new (CAMEL_TYPE_GROUPWISE_SUMMARY, "folder", folder, NULL);
	camel_folder_summary_load (summary, NULL);

	return summary;
}

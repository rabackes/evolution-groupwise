/*
 * camel-groupwise-store-summary.h: the mail folders of a GroupWise account
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

#ifndef CAMEL_GROUPWISE_STORE_SUMMARY_H
#define CAMEL_GROUPWISE_STORE_SUMMARY_H

#include <camel/camel.h>

#include "e-gw-folder.h"

G_BEGIN_DECLS

/* One mail folder: its Camel full name ("Cabinet/Projekte") and GroupWise ID */
typedef struct {
	gchar *id;
	gchar *full_name;
	EGwFolderType type;
	gint64 total;
	gint64 unread;
} CamelGroupwiseFolderRecord;

void		camel_groupwise_folder_record_free
						(CamelGroupwiseFolderRecord *record);

/* The mail folders among the folders of a mailbox, parents first. The root
 * folder has the full name "" (it is not shown), its children are top level
 * folders. Calendars, contacts, documents, query folders and everything
 * below them are skipped.
 * Returns: (element-type CamelGroupwiseFolderRecord) (transfer full) */
GPtrArray *	camel_groupwise_folder_records_from_folders
						(GPtrArray *folders);

/* Thread-safe, persisted in a key file so folders are known offline. */
typedef struct _CamelGroupwiseStoreSummary CamelGroupwiseStoreSummary;

CamelGroupwiseStoreSummary *
		camel_groupwise_store_summary_new
						(const gchar *filename);
void		camel_groupwise_store_summary_free
						(CamelGroupwiseStoreSummary *summary);

/* Replaces all records (takes @records) and saves them */
gboolean	camel_groupwise_store_summary_replace
						(CamelGroupwiseStoreSummary *summary,
						 GPtrArray *records,
						 GError **error);
gboolean	camel_groupwise_store_summary_is_empty
						(CamelGroupwiseStoreSummary *summary);
gchar *		camel_groupwise_store_summary_dup_id
						(CamelGroupwiseStoreSummary *summary,
						 const gchar *full_name);
gchar *		camel_groupwise_store_summary_dup_full_name
						(CamelGroupwiseStoreSummary *summary,
						 const gchar *id);
/* The first folder of a type, e.g. the Mailbox */
gchar *		camel_groupwise_store_summary_dup_full_name_by_type
						(CamelGroupwiseStoreSummary *summary,
						 EGwFolderType type);
EGwFolderType	camel_groupwise_store_summary_get_type
						(CamelGroupwiseStoreSummary *summary,
						 const gchar *full_name);
CamelFolderInfo *
		camel_groupwise_store_summary_build_folder_info
						(CamelGroupwiseStoreSummary *summary,
						 const gchar *top,
						 gboolean recursive);

G_END_DECLS

#endif /* CAMEL_GROUPWISE_STORE_SUMMARY_H */

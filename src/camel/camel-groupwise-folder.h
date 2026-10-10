/*
 * camel-groupwise-folder.h: a GroupWise mail folder
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

#ifndef CAMEL_GROUPWISE_FOLDER_H
#define CAMEL_GROUPWISE_FOLDER_H

#include <camel/camel.h>

G_BEGIN_DECLS

#define CAMEL_TYPE_GROUPWISE_FOLDER (camel_groupwise_folder_get_type ())
G_DECLARE_FINAL_TYPE (CamelGroupwiseFolder, camel_groupwise_folder, CAMEL, GROUPWISE_FOLDER, CamelOfflineFolder)

/* Message UIDs are the GroupWise item IDs. */
CamelFolder *	camel_groupwise_folder_new	(CamelStore *store,
						 const gchar *full_name,
						 const gchar *id,
						 const gchar *folder_dir,
						 GCancellable *cancellable,
						 GError **error);
const gchar *	camel_groupwise_folder_get_id	(CamelGroupwiseFolder *folder);

/* What an event of the POA says about an item (its ID without type and
 * container), put into the folder at once: read or unread, or gone from
 * it. Returns whether the folder has the item. */
typedef enum {
	CAMEL_GROUPWISE_EVENT_READ,
	CAMEL_GROUPWISE_EVENT_UNREAD,
	CAMEL_GROUPWISE_EVENT_GONE
} CamelGroupwiseEvent;

gboolean	camel_groupwise_folder_apply_event
						(CamelGroupwiseFolder *folder,
						 const gchar *item,
						 CamelGroupwiseEvent event);

/* A user flag on messages in the Trash: the next synchronization puts them
 * back where they were deleted from (Evolution's "Restore" of this package) */
#define CAMEL_GROUPWISE_RESTORE_FLAG "gw-restore"

G_END_DECLS

#endif /* CAMEL_GROUPWISE_FOLDER_H */

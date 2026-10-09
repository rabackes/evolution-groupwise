/*
 * camel-groupwise-message-info.h: message info with the last known server flags
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

#ifndef CAMEL_GROUPWISE_MESSAGE_INFO_H
#define CAMEL_GROUPWISE_MESSAGE_INFO_H

#include <camel/camel.h>

G_BEGIN_DECLS

#define CAMEL_TYPE_GROUPWISE_MESSAGE_INFO (camel_groupwise_message_info_get_type ())
G_DECLARE_FINAL_TYPE (CamelGroupwiseMessageInfo, camel_groupwise_message_info, CAMEL, GROUPWISE_MESSAGE_INFO, CamelMessageInfoBase)

/* The flags as the server had them at the last refresh or write. Only the
 * difference to them is written back, so a flag change of Evolution's own
 * (junk test, labels) never resends a stale read state. */
guint32		camel_groupwise_message_info_get_server_flags
						(CamelGroupwiseMessageInfo *info);
void		camel_groupwise_message_info_set_server_flags
						(CamelGroupwiseMessageInfo *info,
						 guint32 server_flags);

/* The categories (IDs as items name them) the server had at the last
 * refresh or write; NULL-terminated, never NULL */
gchar **	camel_groupwise_message_info_dup_server_categories
						(CamelGroupwiseMessageInfo *info);
void		camel_groupwise_message_info_set_server_categories
						(CamelGroupwiseMessageInfo *info,
						 const gchar * const *ids);

/* Whether and how the item is on the Tasklist, as the server had it at the
 * last refresh or write: "" not on it, else "due|completed" with the times
 * as numbers (0: none); the follow-up flag of Evolution stands for it */
gchar *		camel_groupwise_message_info_dup_server_followup
						(CamelGroupwiseMessageInfo *info);
/* Whether a Tasklist state was ever read for it (summaries from before) */
gboolean	camel_groupwise_message_info_knows_server_followup
						(CamelGroupwiseMessageInfo *info);
void		camel_groupwise_message_info_set_server_followup
						(CamelGroupwiseMessageInfo *info,
						 const gchar *state);

#define CAMEL_TYPE_GROUPWISE_SUMMARY (camel_groupwise_summary_get_type ())
G_DECLARE_FINAL_TYPE (CamelGroupwiseSummary, camel_groupwise_summary, CAMEL, GROUPWISE_SUMMARY, CamelFolderSummary)

CamelFolderSummary *
		camel_groupwise_summary_new	(CamelFolder *folder);

G_END_DECLS

#endif /* CAMEL_GROUPWISE_MESSAGE_INFO_H */

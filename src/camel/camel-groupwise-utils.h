/*
 * camel-groupwise-utils.h: GroupWise items to Camel message infos
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

#ifndef CAMEL_GROUPWISE_UTILS_H
#define CAMEL_GROUPWISE_UTILS_H

#include <camel/camel.h>
#include <libxml/tree.h>

G_BEGIN_DECLS

/* Just enough to compare the state of known items (the user may change the
 * subject of an item in GroupWise; with a view naming its fields, the POA
 * gives that subject only when originalSubject is asked for as well, else
 * the original one). "category" gives <categories>. "peek" in every view:
 * looking at an item must not mark it opened or read. */
#define CAMEL_GROUPWISE_FLAGS_VIEW "id modified status subject originalSubject category peek"

/* Everything the message list shows. "messageID" is spelled like this in
 * views, the element in the response is <messageId>. */
#define CAMEL_GROUPWISE_SUMMARY_VIEW \
	"id modified status source subject from to cc delivered created size hasAttachment messageID priority startDate container originalSubject category peek"

/* The message flags GroupWise keeps; everything else is local to Evolution */
#define CAMEL_GROUPWISE_SERVER_FLAGS (CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_ANSWERED)

gchar *		camel_groupwise_item_dup_id	(xmlNode *item);
guint32		camel_groupwise_item_get_flags	(xmlNode *item);
gboolean	camel_groupwise_item_is_calendar
						(xmlNode *item);
gint64		camel_groupwise_parse_time	(const gchar *iso8601);
CamelMessageInfo *
		camel_groupwise_message_info_new_from_item
						(CamelFolderSummary *summary,
						 xmlNode *item);

G_END_DECLS

#endif /* CAMEL_GROUPWISE_UTILS_H */

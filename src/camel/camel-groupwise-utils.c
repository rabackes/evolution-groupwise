/*
 * camel-groupwise-utils.c: GroupWise items to Camel message infos
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

#include "e-gw-xml.h"

#include "camel-groupwise-message-info.h"
#include "camel-groupwise-utils.h"

gchar *
camel_groupwise_item_dup_id (xmlNode *item)
{
	gchar *raw = e_gw_xml_dup_text (item, "id");
	gchar *id = raw && *raw ? e_gw_clean_id (raw) : NULL;

	g_free (raw);

	return id;
}

/* An appointment, task or note (an invitation while it is in the Mailbox) */
gboolean
camel_groupwise_item_is_calendar (xmlNode *item)
{
	gchar *type = e_gw_xml_dup_attr (item, "type");
	gboolean calendar = type && (g_str_has_suffix (type, "Appointment") || g_str_has_suffix (type, "Task") ||
		g_str_has_suffix (type, "Note"));

	g_free (type);

	return calendar;
}

/* An unread item comes without <status> at all */
guint32
camel_groupwise_item_get_flags (xmlNode *item)
{
	guint32 flags = 0;

	if (e_gw_xml_get_bool (item, "status/read"))
		flags |= CAMEL_MESSAGE_SEEN;
	if (e_gw_xml_get_bool (item, "status/replied"))
		flags |= CAMEL_MESSAGE_ANSWERED;

	return flags;
}

gint64
camel_groupwise_parse_time (const gchar *iso8601)
{
	GDateTime *dt;
	gint64 result;

	if (!iso8601 || !*iso8601)
		return 0;

	dt = g_date_time_new_from_iso8601 (iso8601, NULL);
	if (!dt)
		return 0;

	result = g_date_time_to_unix (dt);
	g_date_time_unref (dt);

	return result;
}

static gchar *
format_from (xmlNode *item)
{
	gchar *name = e_gw_xml_dup_text (item, "distribution/from/displayName");
	gchar *email = e_gw_xml_dup_text (item, "distribution/from/email");
	CamelInternetAddress *address;
	gchar *text;

	/* For Internet senders the POA puts "Name <address>" into displayName */
	if (name && strchr (name, '<')) {
		CamelInternetAddress *parsed = camel_internet_address_new ();
		const gchar *parsed_name = NULL, *parsed_email = NULL;

		if (camel_address_unformat (CAMEL_ADDRESS (parsed), name) == 1 &&
		    camel_internet_address_get (parsed, 0, &parsed_name, &parsed_email)) {
			if (!email || !*email) {
				g_free (email);
				email = g_strdup (parsed_email);
			}
			g_free (name);
			name = g_strdup (parsed_name);
		}
		g_object_unref (parsed);
	}

	if ((!name || !*name) && (!email || !*email)) {
		g_free (name);
		g_free (email);
		return NULL;
	}

	address = camel_internet_address_new ();
	camel_internet_address_add (address, name, email ? email : "");
	text = camel_address_format (CAMEL_ADDRESS (address));
	g_object_unref (address);

	g_free (name);
	g_free (email);

	return text;
}

CamelMessageInfo *
camel_groupwise_message_info_new_from_item (CamelFolderSummary *summary,
					    xmlNode *item)
{
	CamelMessageInfo *info;
	gchar *id, *text;
	gint64 date;
	guint32 flags;

	id = camel_groupwise_item_dup_id (item);
	if (!id)
		return NULL;

	info = camel_message_info_new (summary);
	camel_message_info_set_abort_notifications (info, TRUE);

	camel_message_info_set_uid (info, id);

	text = e_gw_xml_dup_text (item, "subject");
	camel_message_info_set_subject (info, text);
	g_free (text);

	text = format_from (item);
	camel_message_info_set_from (info, text);
	g_free (text);

	text = e_gw_xml_dup_text (item, "distribution/to");
	camel_message_info_set_to (info, text);
	g_free (text);

	text = e_gw_xml_dup_text (item, "distribution/cc");
	camel_message_info_set_cc (info, text);
	g_free (text);

	/* Drafts and personal items are not delivered */
	text = e_gw_xml_dup_text (item, "delivered");
	date = camel_groupwise_parse_time (text);
	g_free (text);
	if (!date) {
		text = e_gw_xml_dup_text (item, "created");
		date = camel_groupwise_parse_time (text);
		g_free (text);
	}
	camel_message_info_set_date_received (info, date);

	/* In the Trash: when it was deleted (the latest of the folders it left),
	 * as the GroupWise client sorts the Trash */
	{
		xmlNode *container;
		gint64 deleted = 0;

		for (container = e_gw_xml_first_child (item, "container"); container;
		     container = e_gw_xml_next_sibling (container, "container")) {
			gchar *when = e_gw_xml_dup_attr (container, "deleted");
			gint64 value = camel_groupwise_parse_time (when);

			if (value > deleted)
				deleted = value;
			g_free (when);
		}
		if (deleted)
			camel_message_info_set_date_received (info, deleted);
	}

	/* The GroupWise client lists an appointment, task or note by its day */
	if (camel_groupwise_item_is_calendar (item)) {
		gchar *start = e_gw_xml_dup_text (item, "startDate");
		gint64 start_date = 0;

		/* Tasks and notes have plain days (2026-10-09) */
		if (start && strlen (start) == 10) {
			gchar *midnight = g_strconcat (start, "T00:00:00Z", NULL);

			start_date = camel_groupwise_parse_time (midnight);
			g_free (midnight);
		} else if (start) {
			start_date = camel_groupwise_parse_time (start);
		}
		if (start_date)
			date = start_date;
		g_free (start);
	}
	camel_message_info_set_date_sent (info, date);

	camel_message_info_set_size (info, (guint32) e_gw_xml_get_int (item, "size", 0));

	/* The RFC 822 Message-ID: lets the message list build threads */
	text = e_gw_xml_dup_text (item, "messageId");
	if (text && *text)
		camel_message_info_set_message_id (info, camel_folder_search_util_hash_message_id (text, TRUE));
	g_free (text);

	flags = camel_groupwise_item_get_flags (item);
	if (CAMEL_IS_GROUPWISE_MESSAGE_INFO (info))
		camel_groupwise_message_info_set_server_flags (CAMEL_GROUPWISE_MESSAGE_INFO (info), flags);
	if (e_gw_xml_get_bool (item, "hasAttachment"))
		flags |= CAMEL_MESSAGE_ATTACHMENTS;
	text = e_gw_xml_dup_text (item, "source");
	if (g_strcmp0 (text, "draft") == 0)
		flags |= CAMEL_MESSAGE_DRAFT;
	g_free (text);
	camel_message_info_set_flags (info, ~0, flags);

	/* High priority shows up as "Important" */
	text = e_gw_xml_dup_text (item, "options/priority");
	if (g_strcmp0 (text, "High") == 0)
		camel_message_info_set_flags (info, CAMEL_MESSAGE_FLAGGED, CAMEL_MESSAGE_FLAGGED);
	g_free (text);

	camel_message_info_set_abort_notifications (info, FALSE);
	g_free (id);

	return info;
}

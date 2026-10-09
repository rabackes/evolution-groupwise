/*
 * e-gw-vacation.c: the out of office rule of GroupWise
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

#include <libxml/tree.h>

#include "e-gw-vacation.h"
#include "e-gw-xml.h"

EGwVacation *
e_gw_vacation_new (void)
{
	return g_new0 (EGwVacation, 1);
}

void
e_gw_vacation_free (EGwVacation *vacation)
{
	if (!vacation)
		return;

	g_free (vacation->subject);
	g_free (vacation->message);
	g_free (vacation->external_subject);
	g_free (vacation->external_message);
	g_free (vacation->start_day);
	g_free (vacation->end_day);
	g_clear_pointer (&vacation->start, g_date_time_unref);
	g_clear_pointer (&vacation->end, g_date_time_unref);
	g_free (vacation);
}

/* <message><part>base64</part></message> as text */
static gchar *
dup_message (xmlNode *parent,
	     const gchar *name)
{
	xmlNode *part = e_gw_xml_first_child (e_gw_xml_find (parent, name), "part");
	gchar *encoded = part ? e_gw_xml_dup_text (part, NULL) : NULL;
	guchar *decoded;
	gsize len = 0;
	gchar *text;

	if (!encoded || !*encoded) {
		g_free (encoded);
		return NULL;
	}

	decoded = g_base64_decode (encoded, &len);
	text = g_strndup ((const gchar *) decoded, len);
	g_free (decoded);
	g_free (encoded);

	if (!g_utf8_validate (text, -1, NULL)) {
		gchar *valid = g_utf8_make_valid (text, -1);

		g_free (text);
		text = valid;
	}

	return text;
}

static GDateTime *
dup_time (xmlNode *parent,
	  const gchar *name)
{
	gchar *text = e_gw_xml_dup_text (parent, name);
	GDateTime *dt = text && *text ? g_date_time_new_from_iso8601 (text, NULL) : NULL;

	g_free (text);

	return dt;
}

/* Whether a range is whole days: the appointment of the rule tells (as
 * GroupWise Web asks it), else the times at local midnight */
static gboolean
range_is_all_day (EGwConnection *cnc,
		  xmlNode *rule,
		  EGwVacation *vacation,
		  GCancellable *cancellable)
{
	gchar *appointment = e_gw_xml_dup_text (rule, "appointmentId");
	gboolean all_day;

	if (appointment && *appointment) {
		EGwResponse *response = e_gw_connection_get_item_sync (cnc, appointment, "id allDayEvent", cancellable, NULL);
		xmlNode *item = response ? e_gw_xml_find (e_gw_response_get_node (response), "item") : NULL;

		if (item) {
			all_day = e_gw_xml_get_bool (item, "allDayEvent");
			e_gw_response_free (response);
			g_free (appointment);
			return all_day;
		}
		e_gw_response_free (response);
	}
	g_free (appointment);

	if (!vacation->start)
		return vacation->start_day != NULL;

	{
		GDateTime *local = g_date_time_to_local (vacation->start);

		all_day = g_date_time_get_hour (local) == 0 && g_date_time_get_minute (local) == 0;
		g_date_time_unref (local);
	}

	return all_day;
}

EGwVacation *
e_gw_connection_get_vacation_sync (EGwConnection *cnc,
				   GCancellable *cancellable,
				   GError **error)
{
	EGwVacation *vacation;
	EGwResponse *response;
	xmlNode *rule;

	response = e_gw_connection_call_sync (cnc, "getRuleList", NULL, cancellable, error);
	if (!response)
		return NULL;

	vacation = e_gw_vacation_new ();
	for (rule = e_gw_xml_first_child (e_gw_xml_find (e_gw_response_get_node (response), "rules"), "rule");
	     rule; rule = e_gw_xml_next_sibling (rule, "rule")) {
		gchar *type = e_gw_xml_dup_attr (rule, "type");
		gboolean is_vacation = type && strstr (type, "VacationRule");

		g_free (type);
		if (!is_vacation)
			continue;

		vacation->enabled = e_gw_xml_get_bool (rule, "enabled");
		vacation->subject = e_gw_xml_dup_text (rule, "subject");
		vacation->message = dup_message (rule, "message");
		vacation->include_sender_message = e_gw_xml_get_bool (rule, "includeSenderMessage");
		vacation->reply_to_external = e_gw_xml_get_bool (rule, "replyToExternalUsers");
		vacation->my_contacts_only = e_gw_xml_get_bool (rule, "myContactsOnly");
		vacation->external_subject = e_gw_xml_dup_text (rule, "externalSubject");
		vacation->external_message = dup_message (rule, "externalMessage");
		vacation->start_day = e_gw_xml_dup_text (rule, "startDay");
		vacation->end_day = e_gw_xml_dup_text (rule, "endDay");
		vacation->start = dup_time (rule, "startDate");
		vacation->end = dup_time (rule, "endDate");
		vacation->has_range = (vacation->start && vacation->end) ||
			(vacation->start_day && *vacation->start_day && vacation->end_day && *vacation->end_day);
		if (vacation->has_range)
			vacation->all_day = range_is_all_day (cnc, rule, vacation, cancellable);
		break;
	}
	e_gw_response_free (response);

	return vacation;
}

static void
add_message (GString *xml,
	     const gchar *name,
	     const gchar *text)
{
	gchar *encoded;

	if (!text || !*text)
		return;

	encoded = g_base64_encode ((const guchar *) text, strlen (text));
	g_string_append_printf (xml, "<%s><part>%s</part></%s>", name, encoded, name);
	g_free (encoded);
}

static void
add_time (GString *xml,
	  const gchar *name,
	  GDateTime *dt)
{
	GDateTime *utc = g_date_time_to_utc (dt);
	gchar *text = g_date_time_format (utc, "%Y-%m-%dT%H:%M:%SZ");

	e_gw_xml_add_leaf (xml, name, text);
	g_free (text);
	g_date_time_unref (utc);
}

gboolean
e_gw_connection_set_vacation_sync (EGwConnection *cnc,
				   const EGwVacation *vacation,
				   GCancellable *cancellable,
				   GError **error)
{
	GString *xml = g_string_new ("<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"VacationRule\">");
	gchar *timezone = NULL, *id;
	gboolean range;

	g_return_val_if_fail (vacation != NULL, FALSE);

	range = vacation->has_range && (vacation->all_day ? vacation->start_day && vacation->end_day :
		vacation->start && vacation->end);
	/* The dates of the range are in the user's time zone */
	if (range) {
		timezone = e_gw_connection_dup_local_timezone_xml_sync (cnc, cancellable, error);
		if (!timezone) {
			g_string_free (xml, TRUE);
			return FALSE;
		}
	}

	/* In the order of the schema, as GroupWise Web sends it */
	e_gw_xml_add_bool (xml, "enabled", vacation->enabled);
	if (vacation->subject && *vacation->subject)
		e_gw_xml_add_leaf (xml, "subject", vacation->subject);
	add_message (xml, "message", vacation->message);
	e_gw_xml_add_bool (xml, "replyToExternalUsers", vacation->reply_to_external);
	if (vacation->reply_to_external) {
		e_gw_xml_add_bool (xml, "myContactsOnly", vacation->my_contacts_only);
		if (vacation->external_subject && *vacation->external_subject)
			e_gw_xml_add_leaf (xml, "externalSubject", vacation->external_subject);
		add_message (xml, "externalMessage", vacation->external_message);
	}
	e_gw_xml_add_bool (xml, "includeSenderMessage", vacation->include_sender_message);
	if (range && vacation->all_day) {
		e_gw_xml_add_leaf (xml, "startDay", vacation->start_day);
		e_gw_xml_add_leaf (xml, "endDay", vacation->end_day);
	} else if (range) {
		add_time (xml, "startDate", vacation->start);
		add_time (xml, "endDate", vacation->end);
	}
	if (timezone)
		g_string_append (xml, timezone);
	g_string_append (xml, "</item>");

	id = e_gw_connection_create_item_sync (cnc, xml->str, cancellable, error);
	g_string_free (xml, TRUE);
	g_free (timezone);
	g_free (id);

	return id != NULL;
}

/* The months (1-12) in which the local offset changes to @to_offset this year, 0: none */
static gint
change_month (GTimeZone *zone,
	      gint year,
	      gint32 to_offset)
{
	GDateTime *day = g_date_time_new (zone, year, 1, 1, 12, 0, 0);
	gint32 previous = g_date_time_get_utc_offset (day) / G_USEC_PER_SEC;
	gint month = 0, ii;

	for (ii = 1; ii < 366 && !month; ii++) {
		GDateTime *next = g_date_time_add_days (day, 1);
		gint32 offset = g_date_time_get_utc_offset (next) / G_USEC_PER_SEC;

		if (offset != previous && offset == to_offset)
			month = g_date_time_get_month (next);
		previous = offset;
		g_date_time_unref (day);
		day = next;
	}
	g_date_time_unref (day);

	return month;
}

gchar *
e_gw_connection_dup_local_timezone_xml_sync (EGwConnection *cnc,
					     GCancellable *cancellable,
					     GError **error)
{
	GTimeZone *zone = g_time_zone_new_local ();
	GDateTime *now = g_date_time_new_now (zone);
	gint year = g_date_time_get_year (now);
	GDateTime *jan = g_date_time_new (zone, year, 1, 15, 12, 0, 0);
	GDateTime *jul = g_date_time_new (zone, year, 7, 15, 12, 0, 0);
	gint32 off_jan = g_date_time_get_utc_offset (jan) / G_USEC_PER_SEC;
	gint32 off_jul = g_date_time_get_utc_offset (jul) / G_USEC_PER_SEC;
	gint32 standard = MIN (off_jan, off_jul), daylight = MAX (off_jan, off_jul);
	gint daylight_month = standard != daylight ? change_month (zone, year, daylight) : 0;
	gint standard_month = standard != daylight ? change_month (zone, year, standard) : 0;
	EGwResponse *response;
	xmlNode *node, *best = NULL;
	gchar *xml = NULL;

	g_date_time_unref (jan);
	g_date_time_unref (jul);
	g_date_time_unref (now);
	g_time_zone_unref (zone);

	response = e_gw_connection_call_sync (cnc, "getTimezoneList", NULL, cancellable, error);
	if (!response)
		return NULL;

	for (node = e_gw_xml_first_child (e_gw_xml_find (e_gw_response_get_node (response), "timezones"), "timezone");
	     node; node = e_gw_xml_next_sibling (node, "timezone")) {
		gboolean has_daylight = e_gw_xml_find (node, "daylight") != NULL;

		if (e_gw_xml_get_int (node, "standard/offset", G_MININT32) != standard)
			continue;
		if (standard == daylight) {
			if (!has_daylight) {
				best = node;
				break;
			}
			continue;
		}
		if (!has_daylight || e_gw_xml_get_int (node, "daylight/offset", G_MININT32) != daylight)
			continue;
		/* The offsets fit; the months of change too? */
		if (e_gw_xml_get_int (node, "daylight/month", 0) == daylight_month &&
		    e_gw_xml_get_int (node, "standard/month", 0) == standard_month) {
			best = node;
			break;
		}
		if (!best)
			best = node;
	}

	if (best) {
		xmlBuffer *buffer = xmlBufferCreate ();

		xmlNodeDump (buffer, best->doc, best, 0, 0);
		xml = g_strndup ((const gchar *) xmlBufferContent (buffer), xmlBufferLength (buffer));
		xmlBufferFree (buffer);
	} else {
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, "The server knows no time zone like the local one");
	}
	e_gw_response_free (response);

	return xml;
}

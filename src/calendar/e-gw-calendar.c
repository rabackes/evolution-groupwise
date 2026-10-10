/*
 * e-gw-calendar.c: GroupWise calendar items and iCalendar components
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
 * What the POA gives (GroupWise 26.2, checked on a real system):
 * - times in UTC; an all-day event runs from local midnight of its first
 *   day to local midnight after its last day, startDay/endDay carry the
 *   days themselves (like DTSTART/DTEND with VALUE=DATE);
 * - tasks and notes have plain days (startDate, dueDate);
 * - <alarm> counts seconds before the start;
 * - a series is one item per instance, sharing a recurrenceKey;
 * - the text is a base64 <message><part>; the POA cannot delete it, a blank
 *   replaces it.
 * The element order when writing follows gwmcp, which works on a real POA.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-xml.h"

#include "e-gw-calendar.h"

static gchar *
item_type (xmlNode *item)
{
	gchar *type = e_gw_xml_dup_attr (item, "type");

	/* "xsi:type" arrives as the attribute "type" in the xsi namespace */
	if (type && strchr (type, ':')) {
		gchar *plain = g_strdup (strchr (type, ':') + 1);

		g_free (type);
		type = plain;
	}

	return type;
}

static gchar *
non_empty (gchar *text)
{
	if (text && !*text) {
		g_free (text);
		return NULL;
	}

	return text;
}

ICalComponentKind
e_gw_calendar_item_kind (xmlNode *item)
{
	gchar *type = item_type (item);
	ICalComponentKind kind = I_CAL_NO_COMPONENT;

	if (g_strcmp0 (type, "Appointment") == 0)
		kind = I_CAL_VEVENT_COMPONENT;
	else if (g_strcmp0 (type, "Task") == 0)
		kind = I_CAL_VTODO_COMPONENT;
	else if (g_strcmp0 (type, "Note") == 0)
		kind = I_CAL_VJOURNAL_COMPONENT;
	g_free (type);

	return kind;
}

/* ---------------------------------------------------------------------- */
/* Times */

/* "2026-09-28T07:30:00Z" or "2026-09-28" */
static ICalTime *
parse_time (const gchar *text)
{
	GString *compact;
	ICalTime *tt;
	const gchar *pp;

	if (!text || !*text)
		return NULL;

	compact = g_string_new (NULL);
	for (pp = text; *pp; pp++) {
		if (*pp != '-' && *pp != ':')
			g_string_append_c (compact, *pp);
	}
	tt = i_cal_time_new_from_string (compact->str);
	g_string_free (compact, TRUE);

	if (tt && (i_cal_time_is_null_time (tt) || !i_cal_time_is_valid_time (tt)))
		g_clear_object (&tt);

	return tt;
}

static ICalTime *
new_date (gint year,
	  gint month,
	  gint day)
{
	ICalTime *tt = i_cal_time_new_null_time ();

	i_cal_time_set_date (tt, year, month, day);
	i_cal_time_set_is_date (tt, TRUE);

	return tt;
}

/* The day of @tt in @zone (a date stays as it is) */
static ICalTime *
day_of (ICalTime *tt,
	ICalTimezone *zone)
{
	ICalTime *local;
	ICalTime *day;

	if (i_cal_time_is_date (tt))
		return i_cal_time_clone (tt);

	local = i_cal_time_clone (tt);
	if (zone)
		i_cal_time_convert_to_zone_inplace (local, zone);
	day = new_date (i_cal_time_get_year (local), i_cal_time_get_month (local), i_cal_time_get_day (local));
	g_object_unref (local);

	return day;
}

/* A day of the item: the plain day element if it has one, else the day of
 * the time element in the user's zone */
static ICalTime *
item_day (xmlNode *item,
	  const gchar *day_path,
	  const gchar *time_path,
	  ICalTimezone *zone)
{
	gchar *text = day_path ? non_empty (e_gw_xml_dup_text (item, day_path)) : NULL;
	ICalTime *tt, *day;

	if (!text)
		text = non_empty (e_gw_xml_dup_text (item, time_path));
	tt = parse_time (text);
	g_free (text);
	if (!tt)
		return NULL;

	day = day_of (tt, zone);
	g_object_unref (tt);

	return day;
}

static gchar *
format_day (ICalTime *tt)
{
	return g_strdup_printf ("%04d-%02d-%02d", i_cal_time_get_year (tt), i_cal_time_get_month (tt), i_cal_time_get_day (tt));
}

/* @tt as UTC, in the POA's form */
static gchar *
format_utc (ICalTime *tt)
{
	ICalTime *utc = i_cal_time_clone (tt);
	gchar *text;

	i_cal_time_convert_to_zone_inplace (utc, i_cal_timezone_get_utc_timezone ());
	text = g_strdup_printf ("%04d-%02d-%02dT%02d:%02d:%02dZ",
		i_cal_time_get_year (utc), i_cal_time_get_month (utc), i_cal_time_get_day (utc),
		i_cal_time_get_hour (utc), i_cal_time_get_minute (utc), i_cal_time_get_second (utc));
	g_object_unref (utc);

	return text;
}

/* Midnight of the day @date in @zone, as UTC */
static gchar *
local_midnight (ICalTime *date,
		ICalTimezone *zone)
{
	ICalTime *tt = i_cal_time_new_null_time ();
	gchar *text;

	i_cal_time_set_date (tt, i_cal_time_get_year (date), i_cal_time_get_month (date), i_cal_time_get_day (date));
	i_cal_time_set_is_date (tt, FALSE);
	i_cal_time_set_time (tt, 0, 0, 0);
	i_cal_time_set_timezone (tt, zone ? zone : i_cal_timezone_get_utc_timezone ());
	text = format_utc (tt);
	g_object_unref (tt);

	return text;
}

/* The time of a property with its zone: the TZID from the cache or the
 * built-in zones, a floating time in the user's zone */
static ICalTime *
component_time (ICalComponent *comp,
		ICalPropertyKind kind,
		ETimezoneCache *tz_cache,
		ICalTimezone *zone)
{
	ICalProperty *prop = i_cal_component_get_first_property (comp, kind);
	ICalTime *tt;

	if (!prop)
		return NULL;

	tt = i_cal_property_get_datetime_with_component (prop, comp);
	if (tt && (i_cal_time_is_null_time (tt) || !i_cal_time_is_valid_time (tt)))
		g_clear_object (&tt);

	if (tt && !i_cal_time_is_date (tt) && !i_cal_time_is_utc (tt) && !i_cal_time_get_timezone (tt)) {
		gchar *tzid = i_cal_property_get_parameter_as_string (prop, "TZID");
		ICalTimezone *tz = NULL;

		if (tzid && tz_cache)
			tz = e_timezone_cache_get_timezone (tz_cache, tzid);
		if (tzid && !tz)
			tz = i_cal_timezone_get_builtin_timezone_from_tzid (tzid);
		if (tzid && !tz)
			tz = i_cal_timezone_get_builtin_timezone (tzid);
		i_cal_time_set_timezone (tt, tz ? tz : zone);
		g_free (tzid);
	}
	g_object_unref (prop);

	return tt;
}

/* ---------------------------------------------------------------------- */
/* Reading */

gchar *
e_gw_calendar_uid (xmlNode *item)
{
	gchar *ical_id = non_empty (e_gw_xml_dup_text (item, "iCalId"));
	gchar *uid;

	if (!ical_id) {
		gchar *id = e_gw_xml_dup_text (item, "id");

		uid = e_gw_clean_id (id);
		g_free (id);
		return uid;
	}

	/* 2026-09-25T15:06:24Z_... -> 20260925T150624Z_... */
	if (strlen (ical_id) > 20 && ical_id[4] == '-' && ical_id[7] == '-' && ical_id[10] == 'T' &&
	    ical_id[13] == ':' && ical_id[16] == ':' && ical_id[19] == 'Z') {
		GString *compact = g_string_new (NULL);
		gint ii;

		for (ii = 0; ii < 20; ii++) {
			if (ical_id[ii] != '-' && ical_id[ii] != ':')
				g_string_append_c (compact, ical_id[ii]);
		}
		g_string_append (compact, ical_id + 20);
		uid = g_string_free (compact, FALSE);
		g_free (ical_id);
	} else {
		uid = ical_id;
	}

	/* The instances of a series share the iCalId */
	if (e_gw_xml_find (item, "recurrenceKey")) {
		gchar *start = non_empty (e_gw_xml_dup_text (item, "startDate"));
		GString *with_start = g_string_new (uid);
		const gchar *pp;

		g_string_append_c (with_start, '-');
		for (pp = start; pp && *pp; pp++) {
			if (*pp != '-' && *pp != ':')
				g_string_append_c (with_start, *pp);
		}
		g_free (start);
		g_free (uid);
		uid = g_string_free (with_start, FALSE);
	}

	return uid;
}

gchar *
e_gw_calendar_revision (xmlNode *item)
{
	xmlBuffer *buffer;
	gchar *revision;

	/* The whole item: the answers of the attendees and the status flags
	 * change without a new modification time */
	buffer = xmlBufferCreate ();
	xmlNodeDump (buffer, item->doc, item, 0, 0);
	revision = g_compute_checksum_for_data (G_CHECKSUM_SHA1, xmlBufferContent (buffer), xmlBufferLength (buffer));
	xmlBufferFree (buffer);

	return revision;
}

/* Tags out, for an item that only has an HTML text */
static gchar *
strip_html (const gchar *html)
{
	GString *text = g_string_new (NULL);
	gboolean in_tag = FALSE;
	const gchar *pp;

	for (pp = html; *pp; pp++) {
		if (*pp == '<')
			in_tag = TRUE;
		else if (*pp == '>')
			in_tag = FALSE;
		else if (!in_tag)
			g_string_append_c (text, *pp);
	}

	return g_string_free (text, FALSE);
}

static gchar *
item_description (xmlNode *item)
{
	xmlNode *message = e_gw_xml_find (item, "message"), *part, *chosen = NULL;
	gboolean html = FALSE;
	gchar *encoded, *text;
	guchar *data;
	gsize len = 0;

	for (part = e_gw_xml_first_child (message, "part"); part; part = e_gw_xml_next_sibling (part, "part")) {
		gchar *type = e_gw_xml_dup_attr (part, "contentType");

		if (!type || g_ascii_strcasecmp (type, "text/plain") == 0) {
			chosen = part;
			html = FALSE;
			g_free (type);
			break;
		}
		if (!chosen) {
			chosen = part;
			html = g_ascii_strcasecmp (type, "text/html") == 0;
		}
		g_free (type);
	}
	if (!chosen)
		return NULL;

	encoded = e_gw_xml_dup_text (chosen, NULL);
	data = g_base64_decode (encoded, &len);
	g_free (encoded);
	text = g_utf8_make_valid ((const gchar *) data, len);
	g_free (data);

	if (html) {
		gchar *plain = strip_html (text);

		g_free (text);
		text = plain;
	}

	/* A blank is how an emptied text looks */
	g_strstrip (text);

	return non_empty (text);
}

static void
add_text_property (ICalComponent *comp,
		   ICalProperty *prop)
{
	i_cal_component_take_property (comp, prop);
}

static void
set_x (ICalComponent *comp,
       const gchar *name,
       gchar *value)
{
	if (value && *value)
		e_cal_util_component_set_x_property (comp, name, value);
	g_free (value);
}

static gboolean
same_email (const gchar *a,
	    const gchar *b)
{
	return a && b && g_ascii_strcasecmp (a, b) == 0;
}

static ICalProperty *
person_property (gboolean organizer,
		 const gchar *email,
		 const gchar *name)
{
	gchar *address = g_strconcat ("mailto:", email, NULL);
	ICalProperty *prop = organizer ? i_cal_property_new_organizer (address) : i_cal_property_new_attendee (address);

	if (name && *name)
		i_cal_property_take_parameter (prop, i_cal_parameter_new_cn (name));
	g_free (address);

	return prop;
}

/* The organizer and the attendees of an item sent to others. Personal
 * items have none. */
static void
add_people (ICalComponent *comp,
	    xmlNode *item,
	    const gchar *user_email)
{
	xmlNode *recipients = e_gw_xml_find (item, "distribution/recipients"), *recipient;
	gchar *source = e_gw_xml_dup_text (item, "source");
	gchar *organizer = e_gw_xml_dup_text (item, "distribution/from/email");
	gboolean sent = g_strcmp0 (source, "sent") == 0;

	if (!e_gw_xml_first_child (recipients, "recipient")) {
		g_free (source);
		g_free (organizer);
		return;
	}

	if (organizer && *organizer) {
		gchar *name = e_gw_xml_dup_text (item, "distribution/from/displayName");

		i_cal_component_take_property (comp, person_property (TRUE, organizer, name));
		g_free (name);
	} else if (sent && user_email) {
		g_free (organizer);
		organizer = g_strdup (user_email);
		i_cal_component_take_property (comp, person_property (TRUE, organizer, NULL));
	}

	for (recipient = e_gw_xml_first_child (recipients, "recipient"); recipient;
	     recipient = e_gw_xml_next_sibling (recipient, "recipient")) {
		gchar *email = non_empty (e_gw_xml_dup_text (recipient, "email"));
		gchar *name = e_gw_xml_dup_text (recipient, "displayName");
		gchar *dist = e_gw_xml_dup_text (recipient, "distType");
		gchar *recip_type = e_gw_xml_dup_text (recipient, "recipType");
		ICalParameterPartstat partstat = I_CAL_PARTSTAT_NEEDSACTION;
		ICalParameterRole role = I_CAL_ROLE_REQPARTICIPANT;
		ICalProperty *prop;

		/* The organizer who deleted the meeting for themselves only (the
		 * attendees keep it) takes no part any more: "deleted" when it
		 * was done in the GroupWise client, "declined" over SOAP */
		if (!email || (sent && same_email (email, organizer) &&
			       (e_gw_xml_find (recipient, "recipientStatus/deleted") ||
				e_gw_xml_find (recipient, "recipientStatus/declined")))) {
			g_free (email);
			g_free (name);
			g_free (dist);
			g_free (recip_type);
			continue;
		}

		if (g_strcmp0 (dist, "CC") == 0)
			role = I_CAL_ROLE_OPTPARTICIPANT;
		else if (g_strcmp0 (dist, "BC") == 0)
			role = I_CAL_ROLE_NONPARTICIPANT;

		/* The answers the sender sees; on a received copy the user's own
		 * (also when the user invited themselves); the organizer takes part */
		if (e_gw_xml_find (recipient, "recipientStatus/declined"))
			partstat = I_CAL_PARTSTAT_DECLINED;
		else if (e_gw_xml_find (recipient, "recipientStatus/accepted")) {
			gchar *level = e_gw_xml_dup_text (recipient, "acceptLevel");

			/* Accepted as tentative: the attendee's accept level */
			partstat = g_strcmp0 (level, "Tentative") == 0 ? I_CAL_PARTSTAT_TENTATIVE : I_CAL_PARTSTAT_ACCEPTED;
			g_free (level);
		}
		else if (!sent && same_email (email, user_email)) {
			gchar *level = e_gw_xml_dup_text (item, "acceptLevel");

			/* Accepted as tentative: the user's accept level */
			partstat = !e_gw_xml_get_bool (item, "status/accepted") ? I_CAL_PARTSTAT_NEEDSACTION :
				g_strcmp0 (level, "Tentative") == 0 ? I_CAL_PARTSTAT_TENTATIVE : I_CAL_PARTSTAT_ACCEPTED;
			g_free (level);
		}
		else if (same_email (email, organizer))
			partstat = I_CAL_PARTSTAT_ACCEPTED;

		/* A resource has no first name: the POA makes "Beamer Beamer" of a
		 * resource invited without its name */
		if (g_strcmp0 (recip_type, "Resource") == 0 && name) {
			gsize len = strlen (name);

			if (len > 2 && len % 2 == 1 && name[len / 2] == ' ' && strncmp (name, name + len / 2 + 1, len / 2) == 0)
				name[len / 2] = '\0';
		}

		prop = person_property (FALSE, email, name);
		i_cal_property_take_parameter (prop, i_cal_parameter_new_role (role));
		i_cal_property_take_parameter (prop, i_cal_parameter_new_partstat (partstat));
		if (g_strcmp0 (recip_type, "Resource") == 0)
			i_cal_property_take_parameter (prop, i_cal_parameter_new_cutype (I_CAL_CUTYPE_RESOURCE));
		else if (g_strcmp0 (recip_type, "Group") == 0)
			i_cal_property_take_parameter (prop, i_cal_parameter_new_cutype (I_CAL_CUTYPE_GROUP));
		if (partstat == I_CAL_PARTSTAT_NEEDSACTION)
			i_cal_property_take_parameter (prop, i_cal_parameter_new_rsvp (I_CAL_RSVP_TRUE));
		i_cal_component_take_property (comp, prop);

		g_free (email);
		g_free (name);
		g_free (dist);
		g_free (recip_type);
	}

	g_free (source);
	g_free (organizer);
}

static void
add_alarm (ICalComponent *comp,
	   xmlNode *item,
	   const gchar *summary)
{
	xmlNode *node = e_gw_xml_find (item, "alarm");
	gchar *enabled = e_gw_xml_dup_attr (node, "enabled");
	gint64 seconds = e_gw_xml_get_int (item, "alarm", -1);
	ICalComponent *alarm;
	ICalTrigger *trigger;

	if (!node || seconds < 0 || g_strcmp0 (enabled, "0") == 0 || g_strcmp0 (enabled, "false") == 0) {
		g_free (enabled);
		return;
	}
	g_free (enabled);

	alarm = i_cal_component_new_valarm ();
	i_cal_component_take_property (alarm, i_cal_property_new_action (I_CAL_ACTION_DISPLAY));
	trigger = i_cal_trigger_new_from_int ((gint) -seconds);
	i_cal_component_take_property (alarm, i_cal_property_new_trigger (trigger));
	g_object_unref (trigger);
	i_cal_component_take_property (alarm, i_cal_property_new_description (summary ? summary : ""));
	i_cal_component_take_component (comp, alarm);
}

static const struct {
	const gchar *level;
	const gchar *busy_status;
	ICalPropertyTransp transp;
} accept_levels[] = {
	{ "Free", "FREE", I_CAL_TRANSP_TRANSPARENT },
	{ "Tentative", "TENTATIVE", I_CAL_TRANSP_OPAQUE },
	{ "Busy", "BUSY", I_CAL_TRANSP_OPAQUE },
	{ "OutOfOffice", "OOF", I_CAL_TRANSP_OPAQUE }
};

static void
add_event_times (ICalComponent *comp,
		 xmlNode *item,
		 ICalTimezone *zone)
{
	if (e_gw_xml_get_bool (item, "allDayEvent")) {
		ICalTime *first = item_day (item, "startDay", "startDate", zone);
		ICalTime *end = item_day (item, "endDay", "endDate", zone);

		if (!first)
			return;
		/* The end is the day after the last one, as in iCalendar */
		if (!end || i_cal_time_compare_date_only (end, first) <= 0) {
			g_clear_object (&end);
			end = i_cal_time_clone (first);
			i_cal_time_adjust (end, 1, 0, 0, 0);
		}
		i_cal_component_set_dtstart (comp, first);
		i_cal_component_set_dtend (comp, end);
		g_object_unref (first);
		g_object_unref (end);
	} else {
		gchar *text = e_gw_xml_dup_text (item, "startDate");
		ICalTime *start = parse_time (text), *end;

		g_free (text);
		text = e_gw_xml_dup_text (item, "endDate");
		end = parse_time (text);
		g_free (text);

		/* In the user's zone (with its TZID), as Evolution edits it */
		if (zone && zone != i_cal_timezone_get_utc_timezone ()) {
			if (start)
				i_cal_time_convert_to_zone_inplace (start, zone);
			if (end)
				i_cal_time_convert_to_zone_inplace (end, zone);
		}
		if (start)
			i_cal_component_set_dtstart (comp, start);
		if (end)
			i_cal_component_set_dtend (comp, end);
		g_clear_object (&start);
		g_clear_object (&end);
	}
}

static void
add_task_details (ICalComponent *comp,
		  xmlNode *item,
		  ICalTimezone *zone)
{
	ICalTime *due = item_day (item, NULL, "dueDate", zone);
	gchar *priority = non_empty (e_gw_xml_dup_text (item, "taskPriority"));

	if (due) {
		i_cal_component_set_due (comp, due);
		g_object_unref (due);
	}

	if (priority) {
		gchar letter = g_ascii_toupper (priority[0]);

		/* A high, B normal, C and later low */
		if (letter >= 'A' && letter <= 'Z')
			i_cal_component_take_property (comp, i_cal_property_new_priority (letter == 'A' ? 1 : letter == 'B' ? 5 : 9));
		set_x (comp, E_GW_CALENDAR_X_TASK_PRIORITY, priority);
	}

	if (e_gw_xml_get_bool (item, "status/completed") || e_gw_xml_get_bool (item, "completed")) {
		gchar *text = e_gw_xml_dup_text (item, "modified");
		ICalTime *done = parse_time (text);

		i_cal_component_set_status (comp, I_CAL_STATUS_COMPLETED);
		i_cal_component_take_property (comp, i_cal_property_new_percentcomplete (100));
		if (done) {
			i_cal_component_take_property (comp, i_cal_property_new_completed (done));
			g_object_unref (done);
		}
		g_free (text);
	}
}

ICalComponent *
e_gw_calendar_component_from_item (xmlNode *item,
				   ICalTimezone *zone,
				   const gchar *user_email)
{
	ICalComponentKind kind;
	ICalComponent *comp;
	ICalTime *tt;
	gchar *text, *summary, *id;

	g_return_val_if_fail (item != NULL, NULL);

	kind = e_gw_calendar_item_kind (item);
	if (kind == I_CAL_NO_COMPONENT)
		return NULL;

	comp = i_cal_component_new (kind);

	text = e_gw_calendar_uid (item);
	i_cal_component_set_uid (comp, text);
	g_free (text);

	text = e_gw_xml_dup_text (item, "id");
	id = e_gw_clean_id (text);
	g_free (text);
	set_x (comp, E_GW_CALENDAR_X_ITEM_ID, id);
	set_x (comp, E_GW_CALENDAR_X_SOURCE, e_gw_xml_dup_text (item, "source"));
	set_x (comp, E_GW_CALENDAR_X_ICAL_ID, e_gw_xml_dup_text (item, "iCalId"));
	set_x (comp, E_GW_CALENDAR_X_RECURRENCE_KEY, e_gw_xml_dup_text (item, "recurrenceKey"));

	text = e_gw_xml_dup_text (item, "created");
	tt = parse_time (text);
	g_free (text);
	if (tt) {
		i_cal_component_take_property (comp, i_cal_property_new_created (tt));
		g_object_unref (tt);
	}
	text = e_gw_xml_dup_text (item, "modified");
	tt = parse_time (text);
	g_free (text);
	if (tt) {
		i_cal_component_take_property (comp, i_cal_property_new_lastmodified (tt));
		i_cal_component_set_dtstamp (comp, tt);
		g_object_unref (tt);
	} else {
		tt = i_cal_time_new_current_with_zone (i_cal_timezone_get_utc_timezone ());
		i_cal_component_set_dtstamp (comp, tt);
		g_object_unref (tt);
	}

	summary = e_gw_xml_dup_text (item, "subject");
	if (summary && *summary)
		i_cal_component_set_summary (comp, summary);

	text = item_description (item);
	if (text)
		add_text_property (comp, i_cal_property_new_description (text));
	g_free (text);

	text = e_gw_xml_dup_text (item, "class");
	if (g_strcmp0 (text, "Private") == 0 || e_gw_xml_get_bool (item, "status/private"))
		i_cal_component_take_property (comp, i_cal_property_new_class (I_CAL_CLASS_PRIVATE));
	g_free (text);

	switch (kind) {
	case I_CAL_VEVENT_COMPONENT: {
		gchar *level = e_gw_xml_dup_text (item, "acceptLevel");
		guint ii;

		add_event_times (comp, item, zone);

		text = non_empty (e_gw_xml_dup_text (item, "place"));
		if (text)
			i_cal_component_set_location (comp, text);
		g_free (text);

		for (ii = 0; ii < G_N_ELEMENTS (accept_levels); ii++) {
			if (g_strcmp0 (level, accept_levels[ii].level) == 0) {
				i_cal_component_take_property (comp, i_cal_property_new_transp (accept_levels[ii].transp));
				e_cal_util_component_set_x_property (comp, E_GW_CALENDAR_X_BUSY_STATUS, accept_levels[ii].busy_status);
				break;
			}
		}
		g_free (level);

		add_alarm (comp, item, summary);
		add_people (comp, item, user_email);

		/* Preparation and travel time: only while the POA keeps an
		 * appointment for it (a travel time taken away stays as a number) */
		if (e_gw_xml_find (item, "travelAppointmentBefore") && e_gw_xml_get_int (item, "travelTimeBefore", 0) > 0)
			set_x (comp, E_GW_CALENDAR_X_TRAVEL_BEFORE, e_gw_xml_dup_text (item, "travelTimeBefore"));
		if (e_gw_xml_find (item, "travelAppointmentAfter") && e_gw_xml_get_int (item, "travelTimeAfter", 0) > 0)
			set_x (comp, E_GW_CALENDAR_X_TRAVEL_AFTER, e_gw_xml_dup_text (item, "travelTimeAfter"));
		set_x (comp, E_GW_CALENDAR_X_TRAVEL_OF, e_gw_xml_dup_text (item, "travelAppointmentBacklink"));
		break;
	}
	case I_CAL_VTODO_COMPONENT:
		add_task_details (comp, item, zone);
		/* The POA moves the start of an overdue open task to each new day
		 * (the GroupWise client lists it today); a completed one keeps the
		 * last of those days, often after its due date: the day it was
		 * given (assignedDate) is its start then */
		tt = e_gw_calendar_is_completed (comp) ? item_day (item, NULL, "assignedDate", zone) : NULL;
		if (!tt)
			tt = item_day (item, NULL, "startDate", zone);
		if (tt) {
			i_cal_component_set_dtstart (comp, tt);
			g_object_unref (tt);
		}
		add_people (comp, item, user_email);
		break;
	case I_CAL_VJOURNAL_COMPONENT:
		tt = item_day (item, NULL, "startDate", zone);
		if (tt) {
			i_cal_component_set_dtstart (comp, tt);
			g_object_unref (tt);
		}
		break;
	default:
		break;
	}

	g_free (summary);

	return comp;
}

ICalComponent *
e_gw_calendar_wrap (ICalComponent *comp,
		    ICalTimezone *zone)
{
	ICalComponent *vcalendar = i_cal_component_new_vcalendar ();

	if (zone && zone != i_cal_timezone_get_utc_timezone ()) {
		ICalComponent *vtimezone = i_cal_timezone_get_component (zone);

		if (vtimezone) {
			i_cal_component_take_component (vcalendar, i_cal_component_clone (vtimezone));
			g_object_unref (vtimezone);
		}
	}
	i_cal_component_take_component (vcalendar, i_cal_component_clone (comp));

	return vcalendar;
}

ICalParameterPartstat
e_gw_calendar_user_partstat (ICalComponent *comp,
			     const gchar *user_email)
{
	ICalParameterPartstat partstat = I_CAL_PARTSTAT_NONE;
	ICalProperty *prop;

	g_return_val_if_fail (comp != NULL, I_CAL_PARTSTAT_NONE);

	if (!user_email)
		return partstat;

	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		const gchar *address = i_cal_property_get_attendee (prop);
		ICalParameter *param;

		if (address && !g_ascii_strncasecmp (address, "mailto:", 7))
			address += 7;
		if (!address || g_ascii_strcasecmp (address, user_email) != 0)
			continue;

		partstat = I_CAL_PARTSTAT_NEEDSACTION;
		for (param = i_cal_property_get_first_parameter (prop, I_CAL_PARTSTAT_PARAMETER); param;
		     g_object_unref (param), param = i_cal_property_get_next_parameter (prop, I_CAL_PARTSTAT_PARAMETER))
			partstat = i_cal_parameter_get_partstat (param);
		g_object_unref (prop);
		break;
	}

	return partstat;
}

GPtrArray *
e_gw_calendar_dup_categories (ICalComponent *comp)
{
	GPtrArray *categories = g_ptr_array_new_with_free_func (g_free);
	ICalProperty *prop;

	g_return_val_if_fail (comp != NULL, categories);

	for (prop = i_cal_component_get_first_property (comp, I_CAL_CATEGORIES_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_CATEGORIES_PROPERTY)) {
		gchar **names = g_strsplit (i_cal_property_get_categories (prop) ? i_cal_property_get_categories (prop) : "", ",", -1);
		guint ii, jj;

		for (ii = 0; names[ii]; ii++) {
			gboolean known = FALSE;

			g_strstrip (names[ii]);
			for (jj = 0; jj < categories->len && !known; jj++)
				known = g_strcmp0 (categories->pdata[jj], names[ii]) == 0;
			if (*names[ii] && !known)
				g_ptr_array_add (categories, g_strdup (names[ii]));
		}
		g_strfreev (names);
	}

	return categories;
}

void
e_gw_calendar_set_categories (ICalComponent *comp,
			      const gchar * const *categories)
{
	ICalProperty *prop;
	guint ii;

	g_return_if_fail (comp != NULL);

	while ((prop = i_cal_component_get_first_property (comp, I_CAL_CATEGORIES_PROPERTY))) {
		i_cal_component_remove_property (comp, prop);
		g_object_unref (prop);
	}
	for (ii = 0; categories && categories[ii]; ii++)
		i_cal_component_take_property (comp, i_cal_property_new_categories (categories[ii]));
}

gboolean
e_gw_calendar_is_completed (ICalComponent *comp)
{
	ICalProperty *prop;
	gboolean completed = FALSE;

	g_return_val_if_fail (comp != NULL, FALSE);

	if (i_cal_component_get_status (comp) == I_CAL_STATUS_COMPLETED)
		return TRUE;

	prop = i_cal_component_get_first_property (comp, I_CAL_PERCENTCOMPLETE_PROPERTY);
	if (prop) {
		completed = i_cal_property_get_percentcomplete (prop) >= 100;
		g_object_unref (prop);
	}
	if (!completed) {
		prop = i_cal_component_get_first_property (comp, I_CAL_COMPLETED_PROPERTY);
		completed = prop != NULL;
		g_clear_object (&prop);
	}

	return completed;
}

/* ---------------------------------------------------------------------- */
/* Writing */

/* The fields of an item, element name -> value (with attributes to add) */
typedef struct {
	gchar *value;
	const gchar *attr;
} Field;

static void
field_free (gpointer ptr)
{
	Field *field = ptr;

	g_free (field->value);
	g_free (field);
}

static void
put (GHashTable *fields,
     const gchar *name,
     gchar *value,
     const gchar *attr)
{
	Field *field;

	if (!value)
		return;

	field = g_new0 (Field, 1);
	field->value = value;
	field->attr = attr;
	g_hash_table_insert (fields, (gpointer) name, field);
}

/* What each item type has, in the order the POA wants them */
static const gchar *appointment_fields[] = {
	"class", "acceptLevel", "subject", "startDate", "endDate", "alarm", "allDayEvent", "place",
	"travelTimeBefore", "travelTimeAfter"
};
static const gchar *task_fields[] = {
	"class", "subject", "startDate", "dueDate", "taskPriority"
};
static const gchar *note_fields[] = {
	"class", "subject", "startDate"
};

static const gchar **
fields_of_kind (ICalComponentKind kind,
		guint *n_fields)
{
	switch (kind) {
	case I_CAL_VEVENT_COMPONENT:
		*n_fields = G_N_ELEMENTS (appointment_fields);
		return appointment_fields;
	case I_CAL_VTODO_COMPONENT:
		*n_fields = G_N_ELEMENTS (task_fields);
		return task_fields;
	case I_CAL_VJOURNAL_COMPONENT:
		*n_fields = G_N_ELEMENTS (note_fields);
		return note_fields;
	default:
		*n_fields = 0;
		return NULL;
	}
}

/* Seconds before the start of the first alarm relative to the start, -1 for none */
static gint
alarm_seconds (ICalComponent *comp)
{
	ICalComponent *alarm;
	gint seconds = -1;

	for (alarm = i_cal_component_get_first_component (comp, I_CAL_VALARM_COMPONENT); alarm && seconds < 0;
	     g_object_unref (alarm), alarm = i_cal_component_get_next_component (comp, I_CAL_VALARM_COMPONENT)) {
		ICalProperty *prop = i_cal_component_get_first_property (alarm, I_CAL_TRIGGER_PROPERTY);
		ICalTrigger *trigger;
		ICalDuration *duration;
		gchar *related;

		if (!prop)
			continue;
		related = i_cal_property_get_parameter_as_string (prop, "RELATED");
		trigger = i_cal_property_get_trigger (prop);
		duration = trigger ? i_cal_trigger_get_duration (trigger) : NULL;
		if (duration && !i_cal_duration_is_null_duration (duration) && g_strcmp0 (related, "END") != 0) {
			gint value = i_cal_duration_as_int (duration);

			if (value <= 0)
				seconds = -value;
		} else if (duration && i_cal_duration_is_null_duration (duration) && g_strcmp0 (related, "END") != 0) {
			ICalTime *time = i_cal_trigger_get_time (trigger);

			/* A trigger at the start itself */
			if (!time || i_cal_time_is_null_time (time))
				seconds = 0;
			g_clear_object (&time);
		}
		g_clear_object (&duration);
		g_clear_object (&trigger);
		g_free (related);
		g_object_unref (prop);
	}
	g_clear_object (&alarm);

	return seconds;
}

static gchar *
accept_level (ICalComponent *comp)
{
	ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_TRANSP_PROPERTY);
	gchar *busy_status = e_cal_util_component_dup_x_property (comp, E_GW_CALENDAR_X_BUSY_STATUS);
	ICalPropertyTransp transp = I_CAL_TRANSP_OPAQUE;
	const gchar *level = NULL;
	guint ii;

	if (prop) {
		transp = i_cal_property_get_transp (prop);
		g_object_unref (prop);
	}

	for (ii = 0; ii < G_N_ELEMENTS (accept_levels); ii++) {
		if (busy_status && g_ascii_strcasecmp (busy_status, accept_levels[ii].busy_status) == 0)
			level = accept_levels[ii].level;
	}
	g_free (busy_status);

	/* Evolution only offers free or busy: that wins when they disagree */
	if (transp == I_CAL_TRANSP_TRANSPARENT || transp == I_CAL_TRANSP_TRANSPARENTNOCONFLICT)
		level = "Free";
	else if (!level || g_str_equal (level, "Free"))
		level = "Busy";

	return g_strdup (level);
}

static gchar *
task_priority (ICalComponent *comp)
{
	ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_PRIORITY_PROPERTY);
	gchar *stored = e_cal_util_component_dup_x_property (comp, E_GW_CALENDAR_X_TASK_PRIORITY);
	gint priority = 0;
	gchar letter, *result;

	if (prop) {
		priority = i_cal_property_get_priority (prop);
		g_object_unref (prop);
	}
	if (priority <= 0 || priority > 9) {
		g_free (stored);
		return NULL;
	}

	letter = priority <= 4 ? 'A' : priority == 5 ? 'B' : 'C';
	/* The GroupWise priority as it was while the level stays (A2 stays A2) */
	if (stored && *stored && g_ascii_toupper (stored[0]) == letter)
		return stored;

	result = g_strdup_printf ("%c%s", letter, stored && *stored && g_ascii_isalpha (stored[0]) ? stored + 1 : "");
	g_free (stored);

	return result;
}

static gchar *
component_day (ICalComponent *comp,
	       ICalPropertyKind kind,
	       ETimezoneCache *tz_cache,
	       ICalTimezone *zone)
{
	ICalTime *tt = component_time (comp, kind, tz_cache, zone), *day;
	gchar *text;

	if (!tt)
		return NULL;
	day = day_of (tt, zone);
	text = format_day (day);
	g_object_unref (day);
	g_object_unref (tt);

	return text;
}

/* A travel time of the component, NULL for none */
static gchar *
travel_seconds (ICalComponent *comp,
		const gchar *name)
{
	gchar *value = e_cal_util_component_dup_x_property (comp, name);
	gint64 seconds = value ? g_ascii_strtoll (value, NULL, 10) : 0;

	g_free (value);

	return seconds > 0 ? g_strdup_printf ("%" G_GINT64_FORMAT, seconds) : NULL;
}

static gboolean
event_fields (GHashTable *fields,
	      ICalComponent *comp,
	      ETimezoneCache *tz_cache,
	      ICalTimezone *zone,
	      GError **error)
{
	ICalTime *start = component_time (comp, I_CAL_DTSTART_PROPERTY, tz_cache, zone);
	ICalTime *end = component_time (comp, I_CAL_DTEND_PROPERTY, tz_cache, zone);
	gint seconds;
	gchar *text;

	if (!start) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_INVALID_ARG, _("An appointment needs a start"));
		g_clear_object (&end);
		return FALSE;
	}

	if (!end) {
		ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_DURATION_PROPERTY);

		end = i_cal_time_clone (start);
		if (prop) {
			ICalDuration *duration = i_cal_property_get_duration (prop);

			i_cal_time_adjust (end, 0, 0, 0, i_cal_duration_as_int (duration));
			g_object_unref (duration);
			g_object_unref (prop);
		} else if (i_cal_time_is_date (start)) {
			i_cal_time_adjust (end, 1, 0, 0, 0);
		}
	}

	if (i_cal_time_is_date (start)) {
		ICalTime *end_day = day_of (end, zone);

		/* GroupWise wants at least one day */
		if (i_cal_time_compare_date_only (end_day, start) <= 0) {
			g_object_unref (end_day);
			end_day = i_cal_time_clone (start);
			i_cal_time_adjust (end_day, 1, 0, 0, 0);
		}
		put (fields, "startDate", local_midnight (start, zone), NULL);
		put (fields, "endDate", local_midnight (end_day, zone), NULL);
		put (fields, "allDayEvent", g_strdup ("1"), NULL);
		g_object_unref (end_day);
	} else {
		if (i_cal_time_is_date (end)) {
			/* A date as end of a timed event: its midnight */
			gchar *text = local_midnight (end, zone);

			g_object_unref (end);
			end = parse_time (text);
			g_free (text);
		}
		put (fields, "startDate", format_utc (start), NULL);
		put (fields, "endDate", format_utc (i_cal_time_compare (end, start) < 0 ? start : end), NULL);
		put (fields, "allDayEvent", g_strdup ("0"), NULL);
	}
	g_object_unref (start);
	g_object_unref (end);

	put (fields, "acceptLevel", accept_level (comp), NULL);

	seconds = alarm_seconds (comp);
	if (seconds >= 0)
		put (fields, "alarm", g_strdup_printf ("%d", seconds), " enabled=\"1\"");

	text = g_strdup (i_cal_component_get_location (comp));
	if (text)
		g_strstrip (text);
	put (fields, "place", non_empty (text), NULL);

	/* The POA makes and moves the appointments of the travel time itself */
	put (fields, "travelTimeBefore", travel_seconds (comp, E_GW_CALENDAR_X_TRAVEL_BEFORE), NULL);
	put (fields, "travelTimeAfter", travel_seconds (comp, E_GW_CALENDAR_X_TRAVEL_AFTER), NULL);

	return TRUE;
}

/* The fields of @comp; NULL with @error set when it cannot be an item */
static GHashTable *
component_fields (ICalComponent *comp,
		  ETimezoneCache *tz_cache,
		  ICalTimezone *zone,
		  GError **error)
{
	GHashTable *fields = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, field_free);
	ICalProperty *prop;
	gchar *text;

	text = g_strdup (i_cal_component_get_summary (comp));
	put (fields, "subject", g_strstrip (text ? text : g_strdup ("")), NULL);

	prop = i_cal_component_get_first_property (comp, I_CAL_CLASS_PROPERTY);
	if (prop) {
		ICalProperty_Class klass = i_cal_property_get_class (prop);

		put (fields, "class", g_strdup (klass == I_CAL_CLASS_PRIVATE || klass == I_CAL_CLASS_CONFIDENTIAL ? "Private" : "Public"), NULL);
		g_object_unref (prop);
	} else {
		put (fields, "class", g_strdup ("Public"), NULL);
	}

	switch (i_cal_component_isa (comp)) {
	case I_CAL_VEVENT_COMPONENT:
		if (!event_fields (fields, comp, tz_cache, zone, error)) {
			g_hash_table_destroy (fields);
			return NULL;
		}
		break;
	case I_CAL_VTODO_COMPONENT:
		text = component_day (comp, I_CAL_DTSTART_PROPERTY, tz_cache, zone);
		if (!text) {
			/* GroupWise lists a task on its start day: today by default */
			ICalTime *today = i_cal_time_new_today ();

			text = format_day (today);
			g_object_unref (today);
		}
		put (fields, "startDate", text, NULL);
		put (fields, "dueDate", component_day (comp, I_CAL_DUE_PROPERTY, tz_cache, zone), NULL);
		put (fields, "taskPriority", task_priority (comp), NULL);
		break;
	case I_CAL_VJOURNAL_COMPONENT:
		text = component_day (comp, I_CAL_DTSTART_PROPERTY, tz_cache, zone);
		if (!text) {
			ICalTime *today = i_cal_time_new_today ();

			text = format_day (today);
			g_object_unref (today);
		}
		put (fields, "startDate", text, NULL);
		break;
	default:
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED,
			_("Only appointments, tasks and memos can be stored in GroupWise"));
		g_hash_table_destroy (fields);
		return NULL;
	}

	return fields;
}

static void
append_field (GString *xml,
	      const gchar *name,
	      Field *field)
{
	gchar *escaped = g_markup_escape_text (field->value, -1);

	g_string_append_printf (xml, "<%s%s>%s</%s>", name, field->attr ? field->attr : "", escaped, name);
	g_free (escaped);
}

static void
append_description (GString *xml,
		    const gchar *text)
{
	gchar *encoded = g_base64_encode ((const guchar *) text, strlen (text));

	g_string_append_printf (xml, "<message><part length=\"%" G_GSIZE_FORMAT "\" contentType=\"text/plain\">%s</part></message>",
		strlen (text), encoded);
	g_free (encoded);
}

static gchar *
component_description (ICalComponent *comp)
{
	gchar *text = g_strdup (i_cal_component_get_description (comp));

	if (text)
		g_strstrip (text);

	return non_empty (text);
}

/* The address of a mailto: value */
static const gchar *
strip_mailto (const gchar *value)
{
	if (value && g_ascii_strncasecmp (value, "mailto:", 7) == 0)
		return value + 7;

	return value;
}

/* <recipient>s for the attendees; NULL when there are none but the user or
 * someone else organizes it. The user among them goes along, as the
 * GroupWise client sends it: the POA keeps the user's own copy then
 * (accepted), the home of the user's alarm, categories and travel time */
static gchar *
recipients_xml (ICalComponent *comp,
		const gchar *user_email)
{
	ICalProperty *prop;
	GString *xml = g_string_new (NULL);
	gboolean any = FALSE;

	prop = i_cal_component_get_first_property (comp, I_CAL_ORGANIZER_PROPERTY);
	if (prop) {
		const gchar *organizer = strip_mailto (i_cal_property_get_organizer (prop));
		gboolean foreign = organizer && *organizer && user_email && !same_email (organizer, user_email);

		g_object_unref (prop);
		if (foreign) {
			g_string_free (xml, TRUE);
			return NULL;
		}
	}

	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		const gchar *email = strip_mailto (i_cal_property_get_attendee (prop));
		ICalParameter *param;
		const gchar *dist = "TO";
		gchar *name;

		if (!email || !*email)
			continue;

		param = i_cal_property_get_first_parameter (prop, I_CAL_ROLE_PARAMETER);
		if (param) {
			ICalParameterRole role = i_cal_parameter_get_role (param);

			if (role == I_CAL_ROLE_OPTPARTICIPANT)
				dist = "CC";
			else if (role == I_CAL_ROLE_NONPARTICIPANT)
				dist = "BC";
			g_object_unref (param);
		}
		/* A resource is invited like an attendee, as the GroupWise client does
		 * (Evolution puts resources in the role of non-participants) */
		param = i_cal_property_get_first_parameter (prop, I_CAL_CUTYPE_PARAMETER);
		if (param) {
			ICalParameterCutype cutype = i_cal_parameter_get_cutype (param);

			if (cutype == I_CAL_CUTYPE_RESOURCE || cutype == I_CAL_CUTYPE_ROOM)
				dist = "TO";
			g_object_unref (param);
		}

		g_string_append (xml, "<recipient>");
		name = i_cal_property_get_parameter_as_string (prop, "CN");
		if (name && *name)
			e_gw_xml_add_leaf (xml, "displayName", name);
		g_free (name);
		e_gw_xml_add_leaf (xml, "email", email);
		e_gw_xml_add_leaf (xml, "distType", dist);
		g_string_append (xml, "</recipient>");
		if (!same_email (email, user_email))
			any = TRUE;
	}

	if (!any) {
		g_string_free (xml, TRUE);
		return NULL;
	}

	return g_string_free (xml, FALSE);
}

static const gchar *weekdays[] = { NULL, "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const gchar *occurrences[] = { NULL, "First", "Second", "Third", "Fourth", "Fifth" };

/* The <rrule> of a new series */
static gboolean
append_rrule (GString *xml,
	      ICalComponent *comp,
	      ICalTimezone *zone,
	      GError **error)
{
	ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_RRULE_PROPERTY);
	ICalRecurrence *rule;
	ICalRecurrenceFrequency freq;
	const gchar *frequency;
	GArray *array;
	ICalTime *until;
	gboolean ok = TRUE;
	guint ii;

	if (!prop)
		return TRUE;

	rule = i_cal_property_get_rrule (prop);
	g_object_unref (prop);

	prop = i_cal_component_get_first_property (comp, I_CAL_RRULE_PROPERTY);
	if (prop && i_cal_component_get_next_property (comp, I_CAL_RRULE_PROPERTY))
		ok = FALSE;
	g_clear_object (&prop);

	freq = i_cal_recurrence_get_freq (rule);
	frequency = freq == I_CAL_DAILY_RECURRENCE ? "Daily" : freq == I_CAL_WEEKLY_RECURRENCE ? "Weekly" :
		freq == I_CAL_MONTHLY_RECURRENCE ? "Monthly" : freq == I_CAL_YEARLY_RECURRENCE ? "Yearly" : NULL;
	array = i_cal_recurrence_get_by_set_pos_array (rule);
	if (!frequency || !ok || (array && array->len && g_array_index (array, gshort, 0) != I_CAL_RECURRENCE_ARRAY_MAX)) {
		if (array)
			g_array_unref (array);
		g_object_unref (rule);
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED,
			_("GroupWise does not support this kind of recurrence"));
		return FALSE;
	}
	if (array)
		g_array_unref (array);

	g_string_append (xml, "<rrule>");
	e_gw_xml_add_leaf (xml, "frequency", frequency);
	/* The POA of GroupWise 26.2 makes one appointment less than <count>
	 * (and a year of them for 1): it gets one more */
	if (i_cal_recurrence_get_count (rule) > 0)
		e_gw_xml_add_int (xml, "count", i_cal_recurrence_get_count (rule) + 1);

	until = i_cal_recurrence_get_until (rule);
	if (until && !i_cal_time_is_null_time (until)) {
		gchar *text;

		if (i_cal_time_is_date (until)) {
			/* Through the end of that day */
			ICalTime *next = i_cal_time_clone (until);

			i_cal_time_adjust (next, 1, 0, 0, 0);
			text = local_midnight (next, zone);
			g_object_unref (next);
		} else {
			if (!i_cal_time_is_utc (until) && !i_cal_time_get_timezone (until))
				i_cal_time_set_timezone (until, zone);
			text = format_utc (until);
		}
		e_gw_xml_add_leaf (xml, "until", text);
		g_free (text);
	}
	g_clear_object (&until);

	if (i_cal_recurrence_get_interval (rule) > 1)
		e_gw_xml_add_int (xml, "interval", i_cal_recurrence_get_interval (rule));

	array = i_cal_recurrence_get_by_day_array (rule);
	if (array && array->len && g_array_index (array, gshort, 0) != I_CAL_RECURRENCE_ARRAY_MAX) {
		g_string_append (xml, "<byDay>");
		for (ii = 0; ii < array->len && g_array_index (array, gshort, ii) != I_CAL_RECURRENCE_ARRAY_MAX; ii++) {
			gshort value = g_array_index (array, gshort, ii);
			gint weekday = i_cal_recurrence_day_day_of_week (value);
			gint position = i_cal_recurrence_day_position (value);

			if (weekday < 1 || weekday > 7)
				continue;
			if (position == -1)
				g_string_append (xml, "<day occurrence=\"Last\">");
			else if (position >= 1 && position <= 5)
				g_string_append_printf (xml, "<day occurrence=\"%s\">", occurrences[position]);
			else
				g_string_append (xml, "<day>");
			g_string_append_printf (xml, "%s</day>", weekdays[weekday]);
		}
		g_string_append (xml, "</byDay>");
	}
	if (array)
		g_array_unref (array);

	array = i_cal_recurrence_get_by_month_day_array (rule);
	if (array && array->len && g_array_index (array, gshort, 0) != I_CAL_RECURRENCE_ARRAY_MAX) {
		g_string_append (xml, "<byMonthDay>");
		for (ii = 0; ii < array->len && g_array_index (array, gshort, ii) != I_CAL_RECURRENCE_ARRAY_MAX; ii++)
			e_gw_xml_add_int (xml, "day", g_array_index (array, gshort, ii));
		g_string_append (xml, "</byMonthDay>");
	}
	if (array)
		g_array_unref (array);

	array = i_cal_recurrence_get_by_year_day_array (rule);
	if (array && array->len && g_array_index (array, gshort, 0) != I_CAL_RECURRENCE_ARRAY_MAX) {
		g_string_append (xml, "<byYearDay>");
		for (ii = 0; ii < array->len && g_array_index (array, gshort, ii) != I_CAL_RECURRENCE_ARRAY_MAX; ii++)
			e_gw_xml_add_int (xml, "day", g_array_index (array, gshort, ii));
		g_string_append (xml, "</byYearDay>");
	}
	if (array)
		g_array_unref (array);

	array = i_cal_recurrence_get_by_month_array (rule);
	if (array && array->len && g_array_index (array, gshort, 0) != I_CAL_RECURRENCE_ARRAY_MAX) {
		g_string_append (xml, "<byMonth>");
		for (ii = 0; ii < array->len && g_array_index (array, gshort, ii) != I_CAL_RECURRENCE_ARRAY_MAX; ii++)
			e_gw_xml_add_int (xml, "month", i_cal_recurrence_month_month (g_array_index (array, gshort, ii)));
		g_string_append (xml, "</byMonth>");
	}
	if (array)
		g_array_unref (array);

	g_string_append (xml, "</rrule>");
	g_object_unref (rule);

	return TRUE;
}

static const gchar *
item_type_of_kind (ICalComponentKind kind)
{
	switch (kind) {
	case I_CAL_VEVENT_COMPONENT:
		return "Appointment";
	case I_CAL_VTODO_COMPONENT:
		return "Task";
	case I_CAL_VJOURNAL_COMPONENT:
		return "Note";
	default:
		return NULL;
	}
}

gboolean
e_gw_calendar_is_meeting (ICalComponent *comp,
			  const gchar *user_email)
{
	gchar *recipients = recipients_xml (comp, user_email);

	g_free (recipients);

	return recipients != NULL;
}

gchar *
e_gw_calendar_item_xml (ICalComponent *comp,
			ETimezoneCache *tz_cache,
			ICalTimezone *zone,
			const gchar *user_email,
			GError **error)
{
	ICalComponentKind kind;
	GHashTable *fields;
	GString *xml;
	gchar *recipients, *description;
	const gchar **names;
	guint n_names, ii;
	Field *field;

	g_return_val_if_fail (comp != NULL, NULL);

	kind = i_cal_component_isa (comp);
	fields = component_fields (comp, tz_cache, zone, error);
	if (!fields)
		return NULL;

	/* Notes are not sent to others here */
	recipients = kind == I_CAL_VJOURNAL_COMPONENT ? NULL : recipients_xml (comp, user_email);

	xml = g_string_new (NULL);
	g_string_append_printf (xml, "<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"%s\">",
		item_type_of_kind (kind));
	if (recipients)
		/* without it the POA returns no ID of a sent item */
		g_string_append (xml, "<returnSentItemsId>true</returnSentItemsId>");
	e_gw_xml_add_leaf (xml, "source", recipients ? "sent" : "personal");

	names = fields_of_kind (kind, &n_names);
	for (ii = 0; ii < n_names; ii++) {
		const gchar *name = names[ii];

		/* The distribution, the text and the recurrence go after the subject */
		if (g_str_equal (name, "startDate")) {
			if (recipients) {
				g_string_append_printf (xml, "<distribution><recipients>%s</recipients><sendoptions><requestReply/>"
					"<statusTracking>All</statusTracking><updateFrequentContacts>0</updateFrequentContacts>"
					"</sendoptions></distribution>", recipients);
			}
			description = component_description (comp);
			if (description)
				append_description (xml, description);
			g_free (description);
			if (kind == I_CAL_VEVENT_COMPONENT) {
				g_string_append (xml, "<options><priority>Standard</priority></options>");
				if (!append_rrule (xml, comp, zone, error)) {
					g_string_free (xml, TRUE);
					g_free (recipients);
					g_hash_table_destroy (fields);
					return NULL;
				}
			}
		}

		field = g_hash_table_lookup (fields, name);
		if (field)
			append_field (xml, name, field);
	}
	g_string_append (xml, "</item>");

	g_free (recipients);
	g_hash_table_destroy (fields);

	return g_string_free (xml, FALSE);
}

gchar *
e_gw_calendar_updates_xml (xmlNode *current,
			   ICalComponent *comp,
			   ETimezoneCache *tz_cache,
			   ICalTimezone *zone,
			   GError **error)
{
	ICalComponent *before;
	GHashTable *old_fields, *new_fields;
	GString *deletes, *adds, *updates, *xml;
	gchar *old_text, *new_text;
	const gchar **names;
	guint n_names, ii;

	g_return_val_if_fail (current != NULL, NULL);
	g_return_val_if_fail (comp != NULL, NULL);

	if (e_gw_calendar_item_kind (current) != i_cal_component_isa (comp)) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_INVALID_ARG,
			_("The item on the server is of another kind"));
		return NULL;
	}

	/* The current item seen as a component: the same rules on both sides */
	before = e_gw_calendar_component_from_item (current, zone, NULL);
	old_fields = component_fields (before, NULL, zone, NULL);
	new_fields = component_fields (comp, tz_cache, zone, error);
	if (!old_fields || !new_fields) {
		if (old_fields)
			g_hash_table_destroy (old_fields);
		if (new_fields)
			g_hash_table_destroy (new_fields);
		g_object_unref (before);
		if (error && !*error)
			g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_INVALID_ARG, _("Cannot read the item on the server"));
		return NULL;
	}

	deletes = g_string_new (NULL);
	adds = g_string_new (NULL);
	updates = g_string_new (NULL);

	names = fields_of_kind (i_cal_component_isa (comp), &n_names);
	for (ii = 0; ii < n_names; ii++) {
		const gchar *name = names[ii];
		Field *old_field = g_hash_table_lookup (old_fields, name);
		Field *new_field = g_hash_table_lookup (new_fields, name);
		gchar *server_text = non_empty (e_gw_xml_dup_text (current, name));
		/* An empty element (<place></place>) counts as none */
		gboolean on_server = server_text != NULL;

		g_free (server_text);

		/* The travel time goes its own way (e-cal-backend-groupwise.c) */
		if (g_str_has_prefix (name, "travelTime"))
			continue;

		/* Unchanged, also where the POA leaves out a default (class,
		 * allDayEvent of received appointments) */
		if (old_field && new_field && g_strcmp0 (old_field->value, new_field->value) == 0)
			continue;

		if (!new_field) {
			if (on_server) {
				/* What the POA has, as it has it */
				Field raw = { e_gw_xml_dup_text (current, name), NULL };

				append_field (deletes, name, &raw);
				g_free (raw.value);
			}
		} else if (!on_server) {
			append_field (adds, name, new_field);
		} else if (!old_field || g_strcmp0 (old_field->value, new_field->value) != 0) {
			append_field (updates, name, new_field);
		}
	}

	old_text = component_description (before);
	new_text = component_description (comp);
	if (g_strcmp0 (old_text, new_text) != 0) {
		gboolean on_server = e_gw_xml_find (current, "message/part") != NULL;

		if (new_text)
			append_description (on_server ? updates : adds, new_text);
		else if (on_server)
			/* the POA cannot delete the text, a blank replaces it */
			g_string_append (updates, "<message><part length=\"1\" contentType=\"text/plain\">IA==</part></message>");
	}
	g_free (old_text);
	g_free (new_text);

	/* Deletions are processed first */
	xml = g_string_new (NULL);
	if (deletes->len)
		g_string_append_printf (xml, "<delete>%s</delete>", deletes->str);
	if (adds->len)
		g_string_append_printf (xml, "<add>%s</add>", adds->str);
	if (updates->len)
		g_string_append_printf (xml, "<update>%s</update>", updates->str);

	g_string_free (deletes, TRUE);
	g_string_free (adds, TRUE);
	g_string_free (updates, TRUE);
	g_hash_table_destroy (old_fields);
	g_hash_table_destroy (new_fields);
	g_object_unref (before);

	return g_string_free (xml, FALSE);
}

gboolean
e_gw_calendar_event_range (ICalComponent *comp,
			   ETimezoneCache *tz_cache,
			   ICalTimezone *zone,
			   gchar **out_start,
			   gchar **out_end)
{
	GHashTable *fields;
	Field *start, *end;

	g_return_val_if_fail (comp != NULL, FALSE);

	fields = component_fields (comp, tz_cache, zone, NULL);
	if (!fields)
		return FALSE;

	start = g_hash_table_lookup (fields, "startDate");
	end = g_hash_table_lookup (fields, "endDate");
	*out_start = start ? g_strdup (start->value) : NULL;
	*out_end = end ? g_strdup (end->value) : NULL;
	g_hash_table_destroy (fields);

	return *out_start && *out_end;
}

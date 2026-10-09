/*
 * test-calendar.c: GroupWise calendar items and iCalendar components, with
 * XML as a GroupWise 26.2 POA returns it
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

#include <libxml/parser.h>

#include "e-gw-calendar.h"

#define XSI "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""

/* The time zone block every item carries */
#define TIMEZONE \
	"<timezone><id>W. Europe Standard Time</id>" \
	"<daylight><name>Mitteleurop\xc3\xa4ische Sommerzeit</name><month>3</month><dayOfWeek occurrence=\"Last\">Sunday</dayOfWeek>" \
	"<hour>2</hour><minute>0</minute><offset>7200</offset></daylight>" \
	"<standard><name>Mitteleurop\xc3\xa4ische Zeit</name><month>10</month><dayOfWeek occurrence=\"Last\">Sunday</dayOfWeek>" \
	"<hour>3</hour><minute>0</minute><offset>3600</offset></standard></timezone>"

/* Karl's all-day absence, made in the GroupWise client */
static const gchar *absence =
	"<item " XSI " xsi:type=\"Appointment\">"
	"<id>6762C047.dom.po1.100.1777A36.1.3BC.1@4:A.dom.po1.100.0.1.0.1@19</id>"
	"<version>1</version><modified>2024-12-18T11:29:59Z</modified>"
	"<container>A.dom.po1.100.0.1.0.1@19</container><created>2024-12-18T11:29:59Z</created>"
	"<status><accepted>1</accepted><opened>1</opened><read>1</read></status>"
	"<msgId>6762C047.dom.po1.100.1777A36.1.3BC.1</msgId><source>personal</source>"
	"<delivered>2024-12-18T11:30:00Z</delivered><security>Normal</security><subject>Bin nicht da</subject>"
	"<distribution><sendoptions><statusTracking>All</statusTracking></sendoptions></distribution>"
	"<options><priority>Standard</priority></options><size>277</size>"
	"<iCalId>2024-12-18T11:29:59Z_6536008FA5B@example.com</iCalId>"
	"<startDate>2024-12-15T23:00:00Z</startDate><endDate>2025-01-22T23:00:00Z</endDate>"
	"<startDay>2024-12-16</startDay><endDay>2025-01-23</endDay>"
	"<acceptLevel>OutOfOffice</acceptLevel><allDayEvent>1</allDayEvent>" TIMEZONE "</item>";

/* A personal appointment with a place and an alarm */
static const gchar *monday =
	"<item " XSI " xsi:type=\"Appointment\">"
	"<id>6AB6A9DB.dom.po1.100.1777A36.1.3C6.1@4:A.dom.po1.100.0.1.0.1@19</id>"
	"<version>1</version><modified>2026-09-25T15:05:31Z</modified>"
	"<container>A.dom.po1.100.0.1.0.1@19</container><created>2026-09-25T15:05:31Z</created>"
	"<status><accepted>1</accepted><opened>1</opened><read>1</read></status>"
	"<msgId>6AB6A9DB.dom.po1.100.1777A36.1.3C6.1</msgId><source>personal</source>"
	"<delivered>2026-09-25T15:05:32Z</delivered><security>Normal</security><subject>Test Montag</subject>"
	"<distribution><from><displayName>Karl Napp</displayName><email>KNapp@example.com</email>"
	"<uuid>22222222-145A-0000-992D-000000000002</uuid></from>"
	"<sendoptions><statusTracking>All</statusTracking></sendoptions></distribution>"
	"<options><priority>Standard</priority></options><size>256</size>"
	"<iCalId>2026-09-25T15:05:31Z_7536008F5D5@example.com</iCalId>"
	"<startDate>2026-09-28T07:30:00Z</startDate><endDate>2026-09-28T08:30:00Z</endDate>"
	"<acceptLevel>Busy</acceptLevel><alarm enabled=\"1\">300</alarm><allDayEvent>0</allDayEvent>"
	"<place>Irgendwo</place>" TIMEZONE "</item>";

/* A meeting Karl received (he invited himself and Rainer) */
static const gchar *tuesday =
	"<item " XSI " xsi:type=\"Appointment\">"
	"<id>6AB6AA10.dom.po1.100.1777A36.1.3C9.1@4:A.dom.po1.100.0.1.0.1@19</id>"
	"<version>3</version><modified>2026-09-25T15:06:24Z</modified>"
	"<container>A.dom.po1.100.0.1.0.1@19</container><created>2026-09-25T15:06:24Z</created>"
	"<status><accepted>1</accepted><read>1</read></status>"
	"<msgId>6AB68DF0.dom.po1.200.200008F.1.15985.1</msgId><source>received</source>"
	"<delivered>2026-09-25T15:06:25Z</delivered><security>Normal</security><subject>Test Dienstag mit User</subject>"
	"<distribution><from><displayName>Karl Napp</displayName><email>KNapp@example.com</email>"
	"<uuid>22222222-145A-0000-992D-000000000002</uuid></from>"
	"<to>Karl Napp;  Rainer Backes</to><recipients>"
	"<recipient><displayName>Karl Napp</displayName><email>KNapp@example.com</email>"
	"<uuid>22222222-145A-0000-992D-000000000002</uuid><distType>TO</distType><recipType>User</recipType></recipient>"
	"<recipient><displayName>Rainer Backes</displayName><email>RBackes@example.com</email>"
	"<uuid>11111111-0302-0000-B829-000000000001</uuid><distType>TO</distType><recipType>User</recipType></recipient>"
	"</recipients><sendoptions><statusTracking>All</statusTracking></sendoptions></distribution>"
	"<options><priority>Standard</priority></options><size>312669</size>"
	"<iCalId>2026-09-25T15:06:24Z_7536008F2ED@example.com</iCalId>"
	"<startDate>2026-09-29T07:30:00Z</startDate><endDate>2026-09-29T08:30:00Z</endDate>"
	"<acceptLevel>Busy</acceptLevel><alarm enabled=\"1\">300</alarm><allDayEvent>0</allDayEvent>"
	"<place>zuhause</place>" TIMEZONE "</item>";

/* A meeting sent to three people, with their answers */
static const gchar *sent =
	"<item " XSI " xsi:type=\"Appointment\">"
	"<id>SENT1@4:19.dom.po1.100.0.1.0.1@30</id><modified>2026-09-26T09:00:00Z</modified>"
	"<source>sent</source><subject>Besprechung</subject><class>Private</class>"
	"<distribution><from><displayName>Karl Napp</displayName><email>KNapp@example.com</email></from><recipients>"
	"<recipient><displayName>Rainer Backes</displayName><email>RBackes@example.com</email><distType>TO</distType>"
	"<recipType>User</recipType><recipientStatus><delivered>2026-09-26T09:00:01Z</delivered>"
	"<accepted>2026-09-26T09:10:00Z</accepted></recipientStatus></recipient>"
	"<recipient><displayName>Erika Muster</displayName><email>EMuster@example.com</email><distType>CC</distType>"
	"<recipType>User</recipType><recipientStatus><declined><comment>Urlaub</comment></declined></recipientStatus></recipient>"
	"<recipient><displayName>Beamer Beamer</displayName><email>Beamer@example.com</email><distType>TO</distType>"
	"<recipType>Resource</recipType><recipientStatus><delivered>2026-09-26T09:00:01Z</delivered></recipientStatus></recipient>"
	"</recipients></distribution>"
	"<iCalId>2026-09-26T09:00:00Z_1234@example.com</iCalId>"
	"<startDate>2026-10-01T12:00:00Z</startDate><endDate>2026-10-01T13:00:00Z</endDate>"
	"<acceptLevel>Tentative</acceptLevel><allDayEvent>0</allDayEvent></item>";

/* A personal task and a note, made through SOAP in Karl's mailbox */
static const gchar *task =
	"<item " XSI " xsi:type=\"Task\">"
	"<id>6AB93D0C.dom.po1.100.1777A36.1.431.1@3:7.dom.po1.100.0.1.0.1@16</id>"
	"<version>2</version><modified>2026-09-27T13:58:04Z</modified>"
	"<container>A.dom.po1.100.0.1.0.1@19</container><created>2026-09-27T13:58:04Z</created>"
	"<status><read>1</read></status><msgId>6AB93D0C.dom.po1.100.1777A36.1.431.1</msgId>"
	"<source>personal</source><delivered>2026-09-27T13:58:05Z</delivered><security>Normal</security>"
	"<subject>Testaufgabe P5</subject>"
	"<distribution><from><displayName>Karl Napp</displayName><email>KNapp@example.com</email></from>"
	"<sendoptions><statusTracking>All</statusTracking></sendoptions></distribution>"
	"<message><part contentType=\"text/plain\" length=\"52\">QmVzY2hyZWlidW5nIGRlciBBdWZnYWJlCnp3ZWl0ZSBaZWlsZQ==</part></message>"
	"<options><priority>Standard</priority></options><size>294</size>"
	"<iCalId>2026-09-27T13:58:04Z_7536008F1DA@example.com</iCalId>"
	"<startDate>2026-09-28</startDate><dueDate>2026-10-02</dueDate><assignedDate>2026-09-28</assignedDate>"
	"<taskPriority>A2</taskPriority></item>";

static const gchar *note =
	"<item " XSI " xsi:type=\"Note\">"
	"<id>6AB93D0D.dom.po1.100.1777A36.1.433.1@2:7.dom.po1.100.0.1.0.1@16</id>"
	"<version>1</version><modified>2026-09-27T13:58:05Z</modified>"
	"<container>A.dom.po1.100.0.1.0.1@19</container><created>2026-09-27T13:58:05Z</created>"
	"<status><read>1</read></status><source>personal</source><subject>Testnotiz P5</subject>"
	"<distribution><from><displayName>Karl Napp</displayName><email>KNapp@example.com</email></from></distribution>"
	"<message><part contentType=\"text/plain\" length=\"20\">VGV4dCBkZXIgTm90aXo=</part></message>"
	"<iCalId>2026-09-27T13:58:05Z_7536008F65E@example.com</iCalId><startDate>2026-09-29</startDate></item>";

static ICalTimezone *
berlin (void)
{
	ICalTimezone *zone = i_cal_timezone_get_builtin_timezone ("Europe/Berlin");

	g_assert_nonnull (zone);
	return zone;
}

static xmlDoc *
parse (const gchar *xml)
{
	xmlDoc *doc = xmlReadMemory (xml, strlen (xml), NULL, "UTF-8", 0);

	g_assert_nonnull (doc);
	return doc;
}

static ICalComponent *
component_of (const gchar *xml,
	      const gchar *user_email)
{
	xmlDoc *doc = parse (xml);
	ICalComponent *comp = e_gw_calendar_component_from_item (xmlDocGetRootElement (doc), berlin (), user_email);

	xmlFreeDoc (doc);
	g_assert_nonnull (comp);
	return comp;
}

static gchar *
time_string (ICalComponent *comp,
	     ICalPropertyKind kind)
{
	ICalProperty *prop = i_cal_component_get_first_property (comp, kind);
	gchar *text;

	g_assert_nonnull (prop);
	text = i_cal_property_get_value_as_string (prop);
	g_object_unref (prop);

	return text;
}

static void
assert_time (ICalComponent *comp,
	     ICalPropertyKind kind,
	     const gchar *expected)
{
	gchar *text = time_string (comp, kind);

	g_assert_cmpstr (text, ==, expected);
	g_free (text);
}

static gchar *
x_property (ICalComponent *comp,
	    const gchar *name)
{
	return e_cal_util_component_dup_x_property (comp, name);
}

static void
assert_x (ICalComponent *comp,
	  const gchar *name,
	  const gchar *expected)
{
	gchar *value = x_property (comp, name);

	g_assert_cmpstr (value, ==, expected);
	g_free (value);
}

/* The attendee with that address: its parameters as "PARTSTAT ROLE" */
static gchar *
attendee (ICalComponent *comp,
	  const gchar *email)
{
	ICalProperty *prop;
	gchar *found = NULL;

	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop && !found;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		if (g_ascii_strcasecmp (i_cal_property_get_attendee (prop), email) == 0) {
			gchar *partstat = i_cal_property_get_parameter_as_string (prop, "PARTSTAT");
			gchar *role = i_cal_property_get_parameter_as_string (prop, "ROLE");
			gchar *cutype = i_cal_property_get_parameter_as_string (prop, "CUTYPE");

			found = g_strdup_printf ("%s %s%s%s", partstat, role, cutype ? " " : "", cutype ? cutype : "");
			g_free (partstat);
			g_free (role);
			g_free (cutype);
		}
	}
	g_clear_object (&prop);

	return found;
}

static void
assert_attendee (ICalComponent *comp,
		 const gchar *email,
		 const gchar *expected)
{
	gchar *found = attendee (comp, email);

	g_assert_cmpstr (found, ==, expected);
	g_free (found);
}

static gint
count_properties (ICalComponent *comp,
		  ICalPropertyKind kind)
{
	return i_cal_component_count_properties (comp, kind);
}

static void
test_read_all_day (void)
{
	ICalComponent *comp = component_of (absence, "knapp@example.com");
	ICalProperty *prop;

	g_assert_cmpint (i_cal_component_isa (comp), ==, I_CAL_VEVENT_COMPONENT);
	g_assert_cmpstr (i_cal_component_get_uid (comp), ==, "20241218T112959Z_6536008FA5B@example.com");
	g_assert_cmpstr (i_cal_component_get_summary (comp), ==, "Bin nicht da");

	/* The first day and the day after the last one, as dates */
	assert_time (comp, I_CAL_DTSTART_PROPERTY, "20241216");
	assert_time (comp, I_CAL_DTEND_PROPERTY, "20250123");

	assert_x (comp, E_GW_CALENDAR_X_ITEM_ID, "6762C047.dom.po1.100.1777A36.1.3BC.1@4:A.dom.po1.100.0.1.0.1@19");
	assert_x (comp, E_GW_CALENDAR_X_SOURCE, "personal");
	assert_x (comp, E_GW_CALENDAR_X_BUSY_STATUS, "OOF");
	prop = i_cal_component_get_first_property (comp, I_CAL_TRANSP_PROPERTY);
	g_assert_cmpint (i_cal_property_get_transp (prop), ==, I_CAL_TRANSP_OPAQUE);
	g_object_unref (prop);

	/* Personal: nobody is invited, no alarm set */
	g_assert_cmpint (count_properties (comp, I_CAL_ORGANIZER_PROPERTY), ==, 0);
	g_assert_cmpint (count_properties (comp, I_CAL_ATTENDEE_PROPERTY), ==, 0);
	g_assert_cmpint (i_cal_component_count_components (comp, I_CAL_VALARM_COMPONENT), ==, 0);
	g_assert_null (i_cal_component_get_description (comp));

	g_object_unref (comp);
}

static void
test_read_all_day_without_days (void)
{
	/* Only the UTC times of local midnight: the days in the user's zone */
	const gchar *xml =
		"<item " XSI " xsi:type=\"Appointment\"><id>AD1@4:C@19</id><subject>Feiertag</subject>"
		"<startDate>2026-09-29T22:00:00Z</startDate><endDate>2026-09-30T22:00:00Z</endDate>"
		"<allDayEvent>1</allDayEvent></item>";
	const gchar *zero_length =
		"<item " XSI " xsi:type=\"Appointment\"><id>AD2@4:C@19</id><subject>Feiertag</subject>"
		"<startDate>2026-09-29T22:00:00Z</startDate><endDate>2026-09-29T22:00:00Z</endDate>"
		"<allDayEvent>1</allDayEvent></item>";
	ICalComponent *comp = component_of (xml, NULL);

	assert_time (comp, I_CAL_DTSTART_PROPERTY, "20260930");
	assert_time (comp, I_CAL_DTEND_PROPERTY, "20261001");
	/* No iCalId: the item ID */
	g_assert_cmpstr (i_cal_component_get_uid (comp), ==, "AD1@4:C@19");
	g_object_unref (comp);

	/* As gwmcp wrote them: at least the one day */
	comp = component_of (zero_length, NULL);
	assert_time (comp, I_CAL_DTSTART_PROPERTY, "20260930");
	assert_time (comp, I_CAL_DTEND_PROPERTY, "20261001");
	g_object_unref (comp);
}

static void
test_read_personal (void)
{
	ICalComponent *comp = component_of (monday, "KNapp@example.com"), *alarm;
	ICalProperty *prop;
	ICalTrigger *trigger;
	ICalDuration *duration;

	/* In the user's zone, as Evolution shows it in the editor: 07:30 UTC is 09:30 CEST */
	assert_time (comp, I_CAL_DTSTART_PROPERTY, "20260928T093000");
	assert_time (comp, I_CAL_DTEND_PROPERTY, "20260928T103000");
	{
		ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_DTSTART_PROPERTY);
		gchar *tzid = i_cal_property_get_parameter_as_string (prop, "TZID");

		g_assert_cmpstr (tzid, ==, i_cal_timezone_get_tzid (berlin ()));
		g_free (tzid);
		g_object_unref (prop);
	}
	assert_time (comp, I_CAL_LASTMODIFIED_PROPERTY, "20260925T150531Z");
	g_assert_cmpstr (i_cal_component_get_location (comp), ==, "Irgendwo");
	assert_x (comp, E_GW_CALENDAR_X_BUSY_STATUS, "BUSY");

	/* A sender but no recipients: still personal */
	g_assert_cmpint (count_properties (comp, I_CAL_ORGANIZER_PROPERTY), ==, 0);

	/* 300 seconds before */
	alarm = i_cal_component_get_first_component (comp, I_CAL_VALARM_COMPONENT);
	g_assert_nonnull (alarm);
	prop = i_cal_component_get_first_property (alarm, I_CAL_TRIGGER_PROPERTY);
	trigger = i_cal_property_get_trigger (prop);
	duration = i_cal_trigger_get_duration (trigger);
	g_assert_cmpint (i_cal_duration_as_int (duration), ==, -300);
	g_object_unref (duration);
	g_object_unref (trigger);
	g_object_unref (prop);
	g_object_unref (alarm);

	g_object_unref (comp);
}

static void
test_wrap (void)
{
	ICalComponent *comp = component_of (monday, NULL);
	ICalComponent *vcalendar = e_gw_calendar_wrap (comp, berlin ());
	ICalComponent *event;
	ICalTime *tt;

	g_assert_cmpint (i_cal_component_count_components (vcalendar, I_CAL_VTIMEZONE_COMPONENT), ==, 1);
	event = i_cal_component_get_first_component (vcalendar, I_CAL_VEVENT_COMPONENT);
	g_assert_nonnull (event);

	/* Read back as a client does: the time with its zone, the same instant */
	tt = i_cal_component_get_dtstart (event);
	g_assert_cmpint (i_cal_time_as_timet_with_zone (tt, i_cal_time_get_timezone (tt)), ==,
		i_cal_time_as_timet (i_cal_time_new_from_string ("20260928T073000Z")));
	g_object_unref (tt);

	g_object_unref (event);
	g_object_unref (vcalendar);
	g_object_unref (comp);
}

static void
test_read_received (void)
{
	ICalComponent *comp = component_of (tuesday, "rbackes@example.com");
	ICalProperty *prop;
	gchar *cn;

	g_assert_cmpstr (i_cal_component_get_uid (comp), ==, "20260925T150624Z_7536008F2ED@example.com");
	assert_x (comp, E_GW_CALENDAR_X_SOURCE, "received");

	prop = i_cal_component_get_first_property (comp, I_CAL_ORGANIZER_PROPERTY);
	g_assert_nonnull (prop);
	g_assert_cmpstr (i_cal_property_get_organizer (prop), ==, "mailto:KNapp@example.com");
	cn = i_cal_property_get_parameter_as_string (prop, "CN");
	g_assert_cmpstr (cn, ==, "Karl Napp");
	g_free (cn);
	g_object_unref (prop);

	/* The organizer takes part; the user accepted (status/accepted) */
	g_assert_cmpint (count_properties (comp, I_CAL_ATTENDEE_PROPERTY), ==, 2);
	assert_attendee (comp, "mailto:knapp@example.com", "ACCEPTED REQ-PARTICIPANT");
	assert_attendee (comp, "mailto:rbackes@example.com", "ACCEPTED REQ-PARTICIPANT");
	g_object_unref (comp);

	/* Invited themselves, not answered yet */
	{
		GString *unanswered = g_string_new (tuesday);

		g_string_replace (unanswered, "<status><accepted>1</accepted><read>1</read></status>", "<status><read>1</read></status>", 1);
		comp = component_of (unanswered->str, "knapp@example.com");
		assert_attendee (comp, "mailto:knapp@example.com", "NEEDS-ACTION REQ-PARTICIPANT");
		g_object_unref (comp);
		g_string_free (unanswered, TRUE);
	}

	/* Accepted as tentative */
	{
		GString *tentative = g_string_new (tuesday);

		g_string_replace (tentative, "<acceptLevel>Busy</acceptLevel>", "<acceptLevel>Tentative</acceptLevel>", 1);
		comp = component_of (tentative->str, "rbackes@example.com");
		assert_attendee (comp, "mailto:rbackes@example.com", "TENTATIVE REQ-PARTICIPANT");
		g_object_unref (comp);
		g_string_free (tentative, TRUE);
	}

	/* Seen by someone else, the user has not answered */
	comp = component_of (tuesday, "emuster@example.com");
	assert_attendee (comp, "mailto:rbackes@example.com", "NEEDS-ACTION REQ-PARTICIPANT");
	g_object_unref (comp);
}

static void
test_read_sent (void)
{
	ICalComponent *comp = component_of (sent, "KNapp@example.com");
	ICalProperty *prop;

	assert_attendee (comp, "mailto:rbackes@example.com", "ACCEPTED REQ-PARTICIPANT");
	assert_attendee (comp, "mailto:emuster@example.com", "DECLINED OPT-PARTICIPANT");
	assert_attendee (comp, "mailto:beamer@example.com", "NEEDS-ACTION REQ-PARTICIPANT RESOURCE");
	/* A resource has no first name: "Beamer Beamer" of the POA is "Beamer" */
	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		if (g_ascii_strcasecmp (i_cal_property_get_attendee (prop), "mailto:beamer@example.com") == 0) {
			gchar *cn = i_cal_property_get_parameter_as_string (prop, "CN");

			g_assert_cmpstr (cn, ==, "Beamer");
			g_free (cn);
		}
	}
	g_object_unref (comp);

	/* An answer "tentative": accepted with that accept level (GroupWise 26.2) */
	{
		GString *tentative = g_string_new (sent);

		g_string_replace (tentative, "<accepted>2026-09-26T09:10:00Z</accepted></recipientStatus>",
			"<accepted>2026-09-26T09:10:00Z</accepted></recipientStatus><acceptLevel>Tentative</acceptLevel>", 1);
		comp = component_of (tentative->str, "KNapp@example.com");
		assert_attendee (comp, "mailto:rbackes@example.com", "TENTATIVE REQ-PARTICIPANT");
		g_string_free (tentative, TRUE);
	}
	assert_x (comp, E_GW_CALENDAR_X_BUSY_STATUS, "TENTATIVE");

	prop = i_cal_component_get_first_property (comp, I_CAL_CLASS_PROPERTY);
	g_assert_nonnull (prop);
	g_assert_cmpint (i_cal_property_get_class (prop), ==, I_CAL_CLASS_PRIVATE);
	g_object_unref (prop);

	g_object_unref (comp);
}

static void
test_read_task_and_note (void)
{
	ICalComponent *comp = component_of (task, "KNapp@example.com");
	ICalProperty *prop;
	gchar *xml;

	g_assert_cmpint (i_cal_component_isa (comp), ==, I_CAL_VTODO_COMPONENT);
	g_assert_cmpstr (i_cal_component_get_summary (comp), ==, "Testaufgabe P5");
	g_assert_cmpstr (i_cal_component_get_description (comp), ==, "Beschreibung der Aufgabe\nzweite Zeile");
	assert_time (comp, I_CAL_DTSTART_PROPERTY, "20260928");
	assert_time (comp, I_CAL_DUE_PROPERTY, "20261002");
	prop = i_cal_component_get_first_property (comp, I_CAL_PRIORITY_PROPERTY);
	g_assert_cmpint (i_cal_property_get_priority (prop), ==, 1);
	g_object_unref (prop);
	assert_x (comp, E_GW_CALENDAR_X_TASK_PRIORITY, "A2");
	g_assert_false (e_gw_calendar_is_completed (comp));
	g_assert_cmpint (count_properties (comp, I_CAL_ORGANIZER_PROPERTY), ==, 0);
	g_object_unref (comp);

	/* Completed */
	xml = g_strjoin ("", "<item " XSI " xsi:type=\"Task\"><id>T2@3:C@19</id><modified>2026-09-27T14:00:00Z</modified>"
		"<status><completed>1</completed></status><subject>Erledigt</subject>"
		"<startDate>2026-09-28T22:00:00Z</startDate></item>", NULL);
	comp = component_of (xml, NULL);
	g_assert_true (e_gw_calendar_is_completed (comp));
	assert_time (comp, I_CAL_COMPLETED_PROPERTY, "20260927T140000Z");
	/* A start as UTC time: the day in the user's zone */
	assert_time (comp, I_CAL_DTSTART_PROPERTY, "20260929");
	g_object_unref (comp);
	g_free (xml);

	/* Completed after the POA moved its start past the due date: the day
	 * it was given is its start */
	xml = g_strjoin ("", "<item " XSI " xsi:type=\"Task\"><id>T3@3:C@19</id><modified>2018-01-06T10:00:00Z</modified>"
		"<status><completed>1</completed></status><subject>Elster füttern</subject>"
		"<startDate>2018-01-06</startDate><dueDate>2017-01-06</dueDate><assignedDate>2017-01-06</assignedDate></item>", NULL);
	comp = component_of (xml, NULL);
	assert_time (comp, I_CAL_DTSTART_PROPERTY, "20170106");
	assert_time (comp, I_CAL_DUE_PROPERTY, "20170106");
	g_object_unref (comp);
	g_free (xml);

	comp = component_of (note, NULL);
	g_assert_cmpint (i_cal_component_isa (comp), ==, I_CAL_VJOURNAL_COMPONENT);
	g_assert_cmpstr (i_cal_component_get_summary (comp), ==, "Testnotiz P5");
	g_assert_cmpstr (i_cal_component_get_description (comp), ==, "Text der Notiz");
	assert_time (comp, I_CAL_DTSTART_PROPERTY, "20260929");
	g_object_unref (comp);
}

static void
test_read_series_and_others (void)
{
	const gchar *instance =
		"<item " XSI " xsi:type=\"Appointment\"><id>S1@4:C@19</id><subject>Jour fixe</subject>"
		"<recurrenceKey>1234</recurrenceKey><iCalId>2026-09-01T10:00:00Z_AB@example.com</iCalId>"
		"<startDate>2026-10-05T08:00:00Z</startDate><endDate>2026-10-05T09:00:00Z</endDate>"
		"<allDayEvent>0</allDayEvent></item>";
	const gchar *mail = "<item " XSI " xsi:type=\"Mail\"><id>M1@1:C@16</id><subject>Hallo</subject></item>";
	const gchar *html =
		"<item " XSI " xsi:type=\"Note\"><id>N2@2:C@19</id><subject>HTML</subject>"
		"<message><part contentType=\"text/html\">PHA+RWluIDxiPlRleHQ8L2I+PC9wPg==</part></message>"
		"<startDate>2026-09-29</startDate></item>";
	const gchar *blank =
		"<item " XSI " xsi:type=\"Note\"><id>N3@2:C@19</id><subject>Leer</subject>"
		"<message><part length=\"1\" contentType=\"text/plain\">IA==</part></message>"
		"<startDate>2026-09-29</startDate></item>";
	xmlDoc *doc;
	ICalComponent *comp;

	/* The instances of a series share the iCalId */
	comp = component_of (instance, NULL);
	g_assert_cmpstr (i_cal_component_get_uid (comp), ==, "20260901T100000Z_AB@example.com-20261005T080000Z");
	assert_x (comp, E_GW_CALENDAR_X_RECURRENCE_KEY, "1234");
	g_object_unref (comp);

	doc = parse (mail);
	g_assert_null (e_gw_calendar_component_from_item (xmlDocGetRootElement (doc), berlin (), NULL));
	g_assert_cmpint (e_gw_calendar_item_kind (xmlDocGetRootElement (doc)), ==, I_CAL_NO_COMPONENT);
	xmlFreeDoc (doc);

	/* <p>Ein <b>Text</b></p> */
	comp = component_of (html, NULL);
	g_assert_cmpstr (i_cal_component_get_description (comp), ==, "Ein Text");
	g_object_unref (comp);

	/* The blank that replaces a deleted text */
	comp = component_of (blank, NULL);
	g_assert_null (i_cal_component_get_description (comp));
	g_object_unref (comp);
}

static ICalComponent *ical (const gchar *text);

static void
test_user_partstat (void)
{
	/* The attendee line of a GroupWise 26.2 invitation, after Evolution's
	 * "Accept" replaced the first PARTSTAT and appended its own */
	ICalComponent *comp = ical (
		"BEGIN:VEVENT\r\nUID:x\r\nORGANIZER;CN=\"Karl Napp\";ROLE=CHAIR:MAILTO:KNapp@example.com\r\n"
		"ATTENDEE;CN=\"Rainer Backes\";PARTSTAT=NEEDS-ACTION;RSVP=TRUE;\r\n"
		" ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION:MAILTO:RBackes@example.com\r\nEND:VEVENT\r\n");
	ICalProperty *prop;

	g_assert_cmpint (e_gw_calendar_user_partstat (comp, "rbackes@example.com"), ==, I_CAL_PARTSTAT_NEEDSACTION);
	g_assert_cmpint (e_gw_calendar_user_partstat (comp, "someone@example.com"), ==, I_CAL_PARTSTAT_NONE);

	prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY);
	i_cal_property_remove_parameter_by_kind (prop, I_CAL_PARTSTAT_PARAMETER);
	i_cal_property_take_parameter (prop, i_cal_parameter_new_partstat (I_CAL_PARTSTAT_ACCEPTED));
	g_object_unref (prop);
	g_assert_cmpint (e_gw_calendar_user_partstat (comp, "RBackes@example.com"), ==, I_CAL_PARTSTAT_ACCEPTED);

	g_object_unref (comp);
}

static void
test_categories (void)
{
	ICalComponent *comp = ical ("BEGIN:VEVENT\r\nUID:c\r\nCATEGORIES:Privat,Schulungen\r\n"
		"CATEGORIES:bond-außer Haus\r\nCATEGORIES:Privat\r\nEND:VEVENT\r\n");
	const gchar *set[] = { "webinar", NULL };
	GPtrArray *names = e_gw_calendar_dup_categories (comp);

	g_assert_cmpuint (names->len, ==, 3);
	g_assert_cmpstr (names->pdata[0], ==, "Privat");
	g_assert_cmpstr (names->pdata[1], ==, "Schulungen");
	g_assert_cmpstr (names->pdata[2], ==, "bond-außer Haus");
	g_ptr_array_unref (names);

	e_gw_calendar_set_categories (comp, set);
	names = e_gw_calendar_dup_categories (comp);
	g_assert_cmpuint (names->len, ==, 1);
	g_assert_cmpstr (names->pdata[0], ==, "webinar");
	g_ptr_array_unref (names);

	g_object_unref (comp);
}

static void
test_revision (void)
{
	xmlDoc *doc = parse (sent), *answered;
	gchar *before, *after, *xml;

	before = e_gw_calendar_revision (xmlDocGetRootElement (doc));

	/* An answer changes it, with the same modification time */
	xml = g_strdup (sent);
	g_assert_nonnull (strstr (xml, "<delivered>2026-09-26T09:00:01Z</delivered></recipientStatus></recipient></recipients>"));
	{
		GString *changed = g_string_new (xml);

		g_string_replace (changed, "<delivered>2026-09-26T09:00:01Z</delivered></recipientStatus></recipient></recipients>",
			"<delivered>2026-09-26T09:00:01Z</delivered><accepted>2026-09-26T10:00:00Z</accepted></recipientStatus></recipient></recipients>", 1);
		g_free (xml);
		xml = g_string_free (changed, FALSE);
	}
	answered = parse (xml);
	after = e_gw_calendar_revision (xmlDocGetRootElement (answered));
	g_assert_cmpstr (before, !=, after);

	g_free (before);
	g_free (after);
	g_free (xml);
	xmlFreeDoc (doc);
	xmlFreeDoc (answered);
}

static ICalComponent *
ical (const gchar *text)
{
	ICalComponent *comp = i_cal_component_new_from_string (text);

	g_assert_nonnull (comp);
	return comp;
}

static void
assert_contains (const gchar *xml,
		 const gchar *part)
{
	if (!strstr (xml, part))
		g_error ("\"%s\" not in:\n%s", part, xml);
}

static void
assert_lacks (const gchar *xml,
	      const gchar *part)
{
	if (strstr (xml, part))
		g_error ("\"%s\" in:\n%s", part, xml);
}

static void
test_write_personal (void)
{
	ICalComponent *comp = ical (
		"BEGIN:VEVENT\r\nUID:evo-1\r\nSUMMARY:Zahnarzt & Co\r\nLOCATION:Hauptstr. 1\r\n"
		"DESCRIPTION:Bitte\\npünktlich\r\nCLASS:PRIVATE\r\n"
		"DTSTART;TZID=Europe/Berlin:20261005T090000\r\nDTEND;TZID=Europe/Berlin:20261005T100000\r\n"
		"BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER;RELATED=START:-PT15M\r\nEND:VALARM\r\nEND:VEVENT\r\n");
	gchar *xml, *encoded;
	GError *error = NULL;

	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), "knapp@example.com", &error);
	g_assert_no_error (error);
	g_assert_true (g_str_has_prefix (xml, "<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Appointment\">"
		"<source>personal</source><class>Private</class><acceptLevel>Busy</acceptLevel><subject>Zahnarzt &amp; Co</subject>"));
	encoded = g_base64_encode ((const guchar *) "Bitte\npünktlich", strlen ("Bitte\npünktlich"));
	assert_contains (xml, encoded);
	g_free (encoded);
	/* CEST is two hours ahead of UTC */
	assert_contains (xml, "<options><priority>Standard</priority></options><startDate>2026-10-05T07:00:00Z</startDate>"
		"<endDate>2026-10-05T08:00:00Z</endDate><alarm enabled=\"1\">900</alarm><allDayEvent>0</allDayEvent>"
		"<place>Hauptstr. 1</place></item>");
	assert_lacks (xml, "distribution");
	assert_lacks (xml, "returnSentItemsId");
	g_free (xml);
	g_object_unref (comp);

	/* All day, in winter; shown as free */
	comp = ical ("BEGIN:VEVENT\r\nUID:evo-2\r\nSUMMARY:Urlaub\r\nTRANSP:TRANSPARENT\r\n"
		"DTSTART;VALUE=DATE:20261228\r\nDTEND;VALUE=DATE:20270102\r\nEND:VEVENT\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	g_assert_no_error (error);
	assert_contains (xml, "<acceptLevel>Free</acceptLevel>");
	assert_contains (xml, "<startDate>2026-12-27T23:00:00Z</startDate><endDate>2027-01-01T23:00:00Z</endDate>"
		"<allDayEvent>1</allDayEvent></item>");
	g_free (xml);
	g_object_unref (comp);

	/* One day without an end; a floating time is the user's */
	comp = ical ("BEGIN:VEVENT\r\nUID:evo-3\r\nSUMMARY:Feiertag\r\nDTSTART;VALUE=DATE:20261003\r\nEND:VEVENT\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	assert_contains (xml, "<startDate>2026-10-02T22:00:00Z</startDate><endDate>2026-10-03T22:00:00Z</endDate>");
	g_free (xml);
	g_object_unref (comp);

	comp = ical ("BEGIN:VEVENT\r\nUID:evo-4\r\nSUMMARY:Kurz\r\nDTSTART:20261003T100000\r\nDURATION:PT30M\r\n"
		"X-MICROSOFT-CDO-BUSYSTATUS:OOF\r\nEND:VEVENT\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	assert_contains (xml, "<acceptLevel>OutOfOffice</acceptLevel>");
	assert_contains (xml, "<startDate>2026-10-03T08:00:00Z</startDate><endDate>2026-10-03T08:30:00Z</endDate>");
	g_free (xml);
	g_object_unref (comp);
}

static void
test_write_meeting (void)
{
	ICalComponent *comp = ical (
		"BEGIN:VEVENT\r\nUID:evo-5\r\nSUMMARY:Planung\r\n"
		"DTSTART:20261006T120000Z\r\nDTEND:20261006T130000Z\r\n"
		"ORGANIZER;CN=Karl Napp:mailto:KNapp@example.com\r\n"
		"ATTENDEE;CN=Karl Napp;PARTSTAT=ACCEPTED:mailto:knapp@example.com\r\n"
		"ATTENDEE;CN=Rainer Backes;ROLE=REQ-PARTICIPANT;RSVP=TRUE:mailto:RBackes@example.com\r\n"
		"ATTENDEE;ROLE=OPT-PARTICIPANT:mailto:gast@example.com\r\nEND:VEVENT\r\n");
	GError *error = NULL;
	gchar *xml;

	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), "KNapp@example.com", &error);
	g_assert_no_error (error);
	assert_contains (xml, "<returnSentItemsId>true</returnSentItemsId><source>sent</source>");
	/* The user among the attendees goes along: the POA keeps the own copy */
	assert_contains (xml, "<distribution><recipients>"
		"<recipient><displayName>Karl Napp</displayName><email>knapp@example.com</email><distType>TO</distType></recipient>"
		"<recipient><displayName>Rainer Backes</displayName><email>RBackes@example.com</email><distType>TO</distType></recipient>"
		"<recipient><email>gast@example.com</email><distType>CC</distType></recipient>"
		"</recipients><sendoptions><requestReply/>");
	g_free (xml);

	g_assert_true (e_gw_calendar_is_meeting (comp, "KNapp@example.com"));
	g_assert_false (e_gw_calendar_is_meeting (comp, "rbackes@example.com"));

	/* Someone else's meeting (an imported file) stays personal */
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), "rbackes@example.com", &error);
	g_assert_no_error (error);
	assert_contains (xml, "<source>personal</source>");
	assert_lacks (xml, "distribution");
	g_free (xml);
	g_object_unref (comp);

	/* A resource from Evolution's "Resources" (non-participant) is invited
	 * like an attendee */
	comp = ical (
		"BEGIN:VEVENT\r\nUID:evo-5b\r\nSUMMARY:Mit Raum\r\n"
		"DTSTART:20261006T120000Z\r\nDTEND:20261006T130000Z\r\n"
		"ORGANIZER;CN=Karl Napp:mailto:KNapp@example.com\r\n"
		"ATTENDEE;CN=Karl Napp;PARTSTAT=ACCEPTED:mailto:knapp@example.com\r\n"
		"ATTENDEE;CUTYPE=RESOURCE;ROLE=NON-PARTICIPANT:mailto:Besprechung@example.com\r\nEND:VEVENT\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), "KNapp@example.com", &error);
	g_assert_no_error (error);
	assert_contains (xml, "<recipient><email>Besprechung@example.com</email><distType>TO</distType></recipient>");
	g_free (xml);
	g_object_unref (comp);

	/* The user alone is no meeting */
	comp = ical (
		"BEGIN:VEVENT\r\nUID:evo-5a\r\nSUMMARY:Allein\r\n"
		"DTSTART:20261006T120000Z\r\nDTEND:20261006T130000Z\r\n"
		"ORGANIZER;CN=Karl Napp:mailto:KNapp@example.com\r\n"
		"ATTENDEE;CN=Karl Napp;PARTSTAT=ACCEPTED:mailto:knapp@example.com\r\nEND:VEVENT\r\n");
	g_assert_false (e_gw_calendar_is_meeting (comp, "KNapp@example.com"));
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), "KNapp@example.com", &error);
	g_assert_no_error (error);
	assert_contains (xml, "<source>personal</source>");
	assert_lacks (xml, "distribution");
	g_free (xml);
	g_object_unref (comp);
}

static void
test_write_recurrence (void)
{
	ICalComponent *comp;
	GError *error = NULL;
	gchar *xml;

	comp = ical ("BEGIN:VEVENT\r\nUID:evo-6\r\nSUMMARY:Jour fixe\r\n"
		"DTSTART;TZID=Europe/Berlin:20261005T090000\r\nDTEND;TZID=Europe/Berlin:20261005T093000\r\n"
		"RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE;UNTIL=20261231\r\nEND:VEVENT\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	g_assert_no_error (error);
	/* Through the whole last day */
	assert_contains (xml, "<options><priority>Standard</priority></options><rrule><frequency>Weekly</frequency>"
		"<until>2026-12-31T23:00:00Z</until><interval>2</interval>"
		"<byDay><day>Monday</day><day>Wednesday</day></byDay></rrule><startDate>");
	g_free (xml);
	g_object_unref (comp);

	comp = ical ("BEGIN:VEVENT\r\nUID:evo-7\r\nSUMMARY:Monatlich\r\nDTSTART:20261013T080000Z\r\n"
		"RRULE:FREQ=MONTHLY;COUNT=6;BYDAY=2TU\r\nEND:VEVENT\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	g_assert_no_error (error);
	/* COUNT=6: GroupWise makes one less than it is told */
	assert_contains (xml, "<rrule><frequency>Monthly</frequency><count>7</count>"
		"<byDay><day occurrence=\"Second\">Tuesday</day></byDay></rrule>");
	g_free (xml);
	g_object_unref (comp);

	comp = ical ("BEGIN:VEVENT\r\nUID:evo-8\r\nSUMMARY:Letzter\r\nDTSTART:20261030T080000Z\r\n"
		"RRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1FR\r\nEND:VEVENT\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	g_assert_no_error (error);
	assert_contains (xml, "<rrule><frequency>Yearly</frequency><byDay><day occurrence=\"Last\">Friday</day></byDay>"
		"<byMonth><month>10</month></byMonth></rrule>");
	g_free (xml);
	g_object_unref (comp);

	/* What GroupWise cannot do */
	comp = ical ("BEGIN:VEVENT\r\nUID:evo-9\r\nSUMMARY:Stündlich\r\nDTSTART:20261030T080000Z\r\n"
		"RRULE:FREQ=HOURLY;COUNT=3\r\nEND:VEVENT\r\n");
	g_assert_null (e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error));
	g_assert_error (error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED);
	g_clear_error (&error);
	g_object_unref (comp);

	comp = ical ("BEGIN:VEVENT\r\nUID:evo-10\r\nSUMMARY:Werktag\r\nDTSTART:20261030T080000Z\r\n"
		"RRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1\r\nEND:VEVENT\r\n");
	g_assert_null (e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error));
	g_assert_error (error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED);
	g_clear_error (&error);
	g_object_unref (comp);
}

static void
test_write_task_and_note (void)
{
	ICalComponent *comp;
	GError *error = NULL;
	gchar *xml;

	comp = ical ("BEGIN:VTODO\r\nUID:evo-11\r\nSUMMARY:Bericht\r\nDESCRIPTION:Entwurf\r\n"
		"DTSTART;VALUE=DATE:20261001\r\nDUE;VALUE=DATE:20261009\r\nPRIORITY:5\r\nEND:VTODO\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	g_assert_no_error (error);
	assert_contains (xml, "xsi:type=\"Task\"><source>personal</source><class>Public</class><subject>Bericht</subject>"
		"<message><part length=\"7\" contentType=\"text/plain\">RW50d3VyZg==</part></message>"
		"<startDate>2026-10-01</startDate><dueDate>2026-10-09</dueDate><taskPriority>B</taskPriority></item>");
	g_free (xml);
	g_object_unref (comp);

	/* The GroupWise priority is kept while the level stays */
	comp = component_of (task, NULL);
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	assert_contains (xml, "<taskPriority>A2</taskPriority>");
	g_free (xml);
	i_cal_component_take_property (comp, i_cal_property_new_priority (9));
	{
		ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_PRIORITY_PROPERTY);

		i_cal_component_remove_property (comp, prop);
		g_object_unref (prop);
	}
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	assert_contains (xml, "<taskPriority>C2</taskPriority>");
	g_free (xml);
	g_object_unref (comp);

	/* A due time: its day in the user's zone */
	comp = ical ("BEGIN:VTODO\r\nUID:evo-12\r\nSUMMARY:Spät\r\nDTSTART;VALUE=DATE:20261001\r\n"
		"DUE:20261008T230000Z\r\nEND:VTODO\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	assert_contains (xml, "<dueDate>2026-10-09</dueDate>");
	assert_lacks (xml, "taskPriority");
	g_free (xml);
	g_object_unref (comp);

	comp = ical ("BEGIN:VJOURNAL\r\nUID:evo-13\r\nSUMMARY:Merkzettel\r\nDTSTART;VALUE=DATE:20261002\r\nEND:VJOURNAL\r\n");
	xml = e_gw_calendar_item_xml (comp, NULL, berlin (), NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (xml, ==, "<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Note\">"
		"<source>personal</source><class>Public</class><subject>Merkzettel</subject><startDate>2026-10-02</startDate></item>");
	g_free (xml);
	g_object_unref (comp);
}

static gchar *
updates_for (const gchar *item_xml,
	     ICalComponent *comp)
{
	xmlDoc *doc = parse (item_xml);
	GError *error = NULL;
	gchar *updates;

	updates = e_gw_calendar_updates_xml (xmlDocGetRootElement (doc), comp, NULL, berlin (), &error);
	g_assert_no_error (error);
	g_assert_nonnull (updates);
	xmlFreeDoc (doc);

	return updates;
}

/* As many received appointments in a 20 year old mailbox: no allDayEvent, an empty place */
static const gchar *sparse =
	"<item " XSI " xsi:type=\"Appointment\"><id>R1@4:C@19</id><source>received</source><subject>Friseur</subject>"
	"<iCalId>bef927f8791411e8af64005056880558</iCalId><place></place>"
	"<startDate>2018-06-01T08:00:00Z</startDate><endDate>2018-06-01T09:00:00Z</endDate></item>";

/* An appointment with preparation and travel time (as the GroupWise client
 * makes it), and the appointment the POA keeps for the time before */
static const gchar *with_travel =
	"<item " XSI " xsi:type=\"Appointment\"><id>T1@4:C@19</id><source>personal</source><subject>Ambulanz</subject>"
	"<iCalId>2025-10-09T12:43:25Z_5AB500AF190@example.com</iCalId><place>St.Wendel</place><acceptLevel>Busy</acceptLevel>"
	"<startDate>2026-10-12T10:20:00Z</startDate><endDate>2026-10-12T11:20:00Z</endDate><allDayEvent>0</allDayEvent>"
	"<travelTimeBefore>2400</travelTimeBefore><travelTimeAfter>2400</travelTimeAfter>"
	"<travelAppointmentBefore>T2</travelAppointmentBefore><travelAppointmentAfter>T3</travelAppointmentAfter></item>";
static const gchar *travel_before =
	"<item " XSI " xsi:type=\"Appointment\"><id>T2@4:C@19</id><source>personal</source>"
	"<subject>Before: Ambulanz</subject><iCalId>2025-10-09T12:43:25Z_5AB500AF5C4@example.com</iCalId>"
	"<startDate>2026-10-12T09:40:00Z</startDate><endDate>2026-10-12T10:20:00Z</endDate><acceptLevel>Busy</acceptLevel>"
	"<allDayEvent>0</allDayEvent><travelAppointmentBacklink>T1</travelAppointmentBacklink></item>";

static void
test_travel_time (void)
{
	ICalComponent *comp = component_of (with_travel, NULL);
	gchar *text, *updates, *start = NULL, *end = NULL;

	text = x_property (comp, E_GW_CALENDAR_X_TRAVEL_BEFORE);
	g_assert_cmpstr (text, ==, "2400");
	g_free (text);
	text = x_property (comp, E_GW_CALENDAR_X_TRAVEL_AFTER);
	g_assert_cmpstr (text, ==, "2400");
	g_free (text);
	g_assert_null (x_property (comp, E_GW_CALENDAR_X_TRAVEL_OF));

	/* Moved: the times change, the travel time goes with the POA */
	assert_time (comp, I_CAL_DTSTART_PROPERTY, "20261012T122000");
	g_assert_true (e_gw_calendar_event_range (comp, NULL, berlin (), &start, &end));
	g_assert_cmpstr (start, ==, "2026-10-12T10:20:00Z");
	g_assert_cmpstr (end, ==, "2026-10-12T11:20:00Z");
	g_free (start);
	g_free (end);

	/* The travel time goes its own way (the backend sets and removes it
	 * on the user's own item): not part of the updates */
	e_cal_util_component_set_x_property (comp, E_GW_CALENDAR_X_TRAVEL_BEFORE, "1800");
	updates = updates_for (with_travel, comp);
	g_assert_cmpstr (updates, ==, "");
	g_free (updates);
	g_object_unref (comp);

	comp = component_of (travel_before, NULL);
	text = x_property (comp, E_GW_CALENDAR_X_TRAVEL_OF);
	g_assert_cmpstr (text, ==, "T1");
	g_free (text);
	g_assert_null (x_property (comp, E_GW_CALENDAR_X_TRAVEL_BEFORE));
	g_object_unref (comp);
}

static void
test_updates_unchanged (void)
{
	const gchar *items[] = { absence, monday, tuesday, sent, task, note, sparse, with_travel, travel_before };
	guint ii;

	/* What Evolution gets back unchanged changes nothing */
	for (ii = 0; ii < G_N_ELEMENTS (items); ii++) {
		ICalComponent *comp = component_of (items[ii], "knapp@example.com");
		gchar *updates = updates_for (items[ii], comp);

		if (*updates)
			g_error ("item %u: %s", ii, updates);
		g_free (updates);
		g_object_unref (comp);
	}
}

static void
test_updates (void)
{
	ICalComponent *comp = component_of (monday, NULL), *alarm;
	ICalProperty *prop;
	ICalTime *tt;
	gchar *updates;

	i_cal_component_set_summary (comp, "Test Montag <verschoben>");
	tt = i_cal_time_new_from_string ("20260928T090000Z");
	i_cal_component_set_dtstart (comp, tt);
	g_object_unref (tt);
	tt = i_cal_time_new_from_string ("20260928T100000Z");
	i_cal_component_set_dtend (comp, tt);
	g_object_unref (tt);
	prop = i_cal_component_get_first_property (comp, I_CAL_LOCATION_PROPERTY);
	i_cal_component_remove_property (comp, prop);
	g_object_unref (prop);
	alarm = i_cal_component_get_first_component (comp, I_CAL_VALARM_COMPONENT);
	i_cal_component_remove_component (comp, alarm);
	g_object_unref (alarm);
	i_cal_component_take_property (comp, i_cal_property_new_description ("Neu"));

	updates = updates_for (monday, comp);
	g_assert_cmpstr (updates, ==,
		"<delete><alarm>300</alarm><place>Irgendwo</place></delete>"
		"<add><message><part length=\"3\" contentType=\"text/plain\">TmV1</part></message></add>"
		"<update><subject>Test Montag &lt;verschoben&gt;</subject><startDate>2026-09-28T09:00:00Z</startDate>"
		"<endDate>2026-09-28T10:00:00Z</endDate></update>");
	g_free (updates);
	g_object_unref (comp);

	/* An emptied text becomes a blank; private is a class */
	comp = component_of (task, NULL);
	prop = i_cal_component_get_first_property (comp, I_CAL_DESCRIPTION_PROPERTY);
	i_cal_component_remove_property (comp, prop);
	g_object_unref (prop);
	i_cal_component_take_property (comp, i_cal_property_new_class (I_CAL_CLASS_PRIVATE));
	prop = i_cal_component_get_first_property (comp, I_CAL_DUE_PROPERTY);
	i_cal_component_remove_property (comp, prop);
	g_object_unref (prop);

	updates = updates_for (task, comp);
	g_assert_cmpstr (updates, ==,
		"<delete><dueDate>2026-10-02</dueDate></delete>"
		"<add><class>Private</class></add>"
		"<update><message><part length=\"1\" contentType=\"text/plain\">IA==</part></message></update>");
	g_free (updates);
	g_object_unref (comp);

	/* All day moved by a day; an alarm switched on */
	comp = component_of (absence, NULL);
	tt = i_cal_time_new_from_string ("20241217");
	i_cal_component_set_dtstart (comp, tt);
	g_object_unref (tt);
	alarm = i_cal_component_new_valarm ();
	{
		ICalTrigger *trigger = i_cal_trigger_new_from_int (-3600);

		i_cal_component_take_property (alarm, i_cal_property_new_trigger (trigger));
		g_object_unref (trigger);
	}
	i_cal_component_take_component (comp, alarm);
	updates = updates_for (absence, comp);
	g_assert_cmpstr (updates, ==,
		"<add><alarm enabled=\"1\">3600</alarm></add>"
		"<update><startDate>2024-12-16T23:00:00Z</startDate></update>");
	g_free (updates);
	g_object_unref (comp);
}

static void
test_updates_refused (void)
{
	ICalComponent *comp = component_of (task, NULL);
	xmlDoc *doc = parse (monday);
	GError *error = NULL;

	g_assert_null (e_gw_calendar_updates_xml (xmlDocGetRootElement (doc), comp, NULL, berlin (), &error));
	g_assert_error (error, E_CLIENT_ERROR, E_CLIENT_ERROR_INVALID_ARG);
	g_clear_error (&error);

	xmlFreeDoc (doc);
	g_object_unref (comp);
}

int
main (int argc,
      char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/calendar/read-all-day", test_read_all_day);
	g_test_add_func ("/calendar/read-all-day-without-days", test_read_all_day_without_days);
	g_test_add_func ("/calendar/read-personal", test_read_personal);
	g_test_add_func ("/calendar/wrap", test_wrap);
	g_test_add_func ("/calendar/read-received", test_read_received);
	g_test_add_func ("/calendar/read-sent", test_read_sent);
	g_test_add_func ("/calendar/read-task-and-note", test_read_task_and_note);
	g_test_add_func ("/calendar/read-series-and-others", test_read_series_and_others);
	g_test_add_func ("/calendar/user-partstat", test_user_partstat);
	g_test_add_func ("/calendar/categories", test_categories);
	g_test_add_func ("/calendar/revision", test_revision);
	g_test_add_func ("/calendar/write-personal", test_write_personal);
	g_test_add_func ("/calendar/write-meeting", test_write_meeting);
	g_test_add_func ("/calendar/write-recurrence", test_write_recurrence);
	g_test_add_func ("/calendar/write-task-and-note", test_write_task_and_note);
	g_test_add_func ("/calendar/updates-unchanged", test_updates_unchanged);
	g_test_add_func ("/calendar/updates", test_updates);
	g_test_add_func ("/calendar/travel-time", test_travel_time);
	g_test_add_func ("/calendar/updates-refused", test_updates_refused);

	return g_test_run ();
}

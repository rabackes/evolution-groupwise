/*
 * gw-cal-check.c: the calendars of a GroupWise collection through the real
 * evolution-data-server, as Evolution sees them
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
 * Usage: gw-cal-check COLLECTION-UID [--write-test] [--answer UID PARTSTAT]
 *                     [--set-alarm UID MINUTES] [--delete UID] [--free-busy EMAIL,EMAIL]
 *                     [--invite EMAIL] (a meeting on 2026-10-09 10:00, karl only)
 *                     [--receive UID PARTSTAT] (as Evolution's invitation view answers)
 *                     [--set-categories UID "A,B"] (karl only; "" for none)
 * Lists the calendar, the task list and the memo list with their content.
 * --write-test creates, changes and deletes an appointment, a task and a
 * note, a series in each subcalendar, and moves an appointment between the
 * Calendar and two subcalendars: only for the test account karl.
 * Calendars of other users are only read.
 * --answer sets the PARTSTAT (ACCEPTED, TENTATIVE, DECLINED) of the user in
 * the appointment UID, as Evolution does when the user answers it.
 * The registry and the calendar factory must see the development prefix
 * (tools/dev-account.sh sets it up); the password comes from the keyring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libecal/libecal.h>

static gint failures;

/* Lets the notices of the backend in (property changes arrive through the main loop) */
static void
spin (guint milliseconds)
{
	gint64 end = g_get_monotonic_time () + milliseconds * 1000;

	while (g_get_monotonic_time () < end) {
		if (!g_main_context_iteration (NULL, FALSE))
			g_usleep (20000);
	}
}

static void
check (gboolean ok,
       const gchar *what,
       GError **error)
{
	printf ("    %s  %s%s%s\n", ok ? "ok  " : "FAIL", what, error && *error ? ": " : "", error && *error ? (*error)->message : "");
	if (!ok)
		failures++;
	if (error)
		g_clear_error (error);
}

static gchar *
describe_time (ICalComponent *comp,
	       ICalPropertyKind kind)
{
	ICalProperty *prop = i_cal_component_get_first_property (comp, kind);
	gchar *text;

	if (!prop)
		return g_strdup ("-");
	text = i_cal_property_get_value_as_string (prop);
	g_object_unref (prop);

	return text;
}

static void
list_objects (ECalClient *client)
{
	GSList *comps = NULL, *link;
	GError *error = NULL;

	if (!e_cal_client_get_object_list_sync (client, "#t", &comps, NULL, &error)) {
		printf ("    list: %s\n", error->message);
		g_clear_error (&error);
		return;
	}

	printf ("    %u objects\n", g_slist_length (comps));
	for (link = comps; link; link = g_slist_next (link)) {
		ICalComponent *comp = link->data;
		gchar *start = describe_time (comp, I_CAL_DTSTART_PROPERTY);
		gchar *end = describe_time (comp, i_cal_component_isa (comp) == I_CAL_VTODO_COMPONENT ? I_CAL_DUE_PROPERTY : I_CAL_DTEND_PROPERTY);
		gchar *source = e_cal_util_component_dup_x_property (comp, "X-GW-SOURCE");

		{
			ICalProperty *cat = i_cal_component_get_first_property (comp, I_CAL_CATEGORIES_PROPERTY);

			if (cat) {
				printf ("      [");
				for (; cat; g_object_unref (cat), cat = i_cal_component_get_next_property (comp, I_CAL_CATEGORIES_PROPERTY))
					printf ("%s;", i_cal_property_get_categories (cat));
				printf ("]\n");
			}
		}
		printf ("    %-28s %-18s %-18s %-9s attendees %d alarms %d  %s\n",
			i_cal_component_get_summary (comp) ? i_cal_component_get_summary (comp) : "",
			start, end, source ? source : "",
			i_cal_component_count_properties (comp, I_CAL_ATTENDEE_PROPERTY),
			i_cal_component_count_components (comp, I_CAL_VALARM_COMPONENT),
			i_cal_component_get_uid (comp));
		g_free (start);
		g_free (end);
		g_free (source);
	}

	g_slist_free_full (comps, g_object_unref);
}

static ICalComponent *
server_object (ECalClient *client,
	       const gchar *uid)
{
	ICalComponent *comp = NULL;
	GError *error = NULL;

	/* From the server, not the cache */
	e_client_refresh_sync (E_CLIENT (client), NULL, NULL);
	if (!e_cal_client_get_object_sync (client, uid, NULL, &comp, NULL, &error))
		g_clear_error (&error);

	return comp;
}

/* The start in a zone (TZID), not in UTC: what Evolution's editor shows */
static gboolean
has_tzid (ICalComponent *comp)
{
	ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_DTSTART_PROPERTY);
	gchar *tzid = prop ? i_cal_property_get_parameter_as_string (prop, "TZID") : NULL;
	gboolean has = tzid && *tzid;

	if (has)
		printf ("      TZID %s\n", tzid);
	g_free (tzid);
	g_clear_object (&prop);

	return has;
}

static gchar *
create (ECalClient *client,
	const gchar *ical)
{
	ICalComponent *comp = i_cal_component_new_from_string (ical);
	GError *error = NULL;
	gchar *uid = NULL;

	check (e_cal_client_create_object_sync (client, comp, E_CAL_OPERATION_FLAG_NONE, &uid, NULL, &error) && uid,
		"create", &error);
	if (uid)
		printf ("      UID %s\n", uid);
	g_object_unref (comp);

	return uid;
}

static void
modify (ECalClient *client,
	ICalComponent *comp,
	const gchar *what)
{
	GError *error = NULL;

	check (e_cal_client_modify_object_sync (client, comp, E_CAL_OBJ_MOD_THIS, E_CAL_OPERATION_FLAG_NONE, NULL, &error),
		what, &error);
}

static void
remove_object (ECalClient *client,
	       const gchar *uid)
{
	GError *error = NULL;

	check (e_cal_client_remove_object_sync (client, uid, NULL, E_CAL_OBJ_MOD_THIS, E_CAL_OPERATION_FLAG_NONE, NULL, &error),
		"delete", &error);
	check (server_object (client, uid) == NULL, "gone from the server", NULL);
}

/* The Calendar of the account, for the checks of the other calendars */
static ECalClient *main_events;

/* The role of a GroupWise calendar (own, proxy, shared; NULL: the Calendar),
 * from the key file: this tool does not know the extension type */
static gchar *
source_role (ESource *source)
{
	gchar *data = e_source_to_string (source, NULL);
	GKeyFile *key_file = g_key_file_new ();
	gchar *role = NULL;

	if (data && g_key_file_load_from_data (key_file, data, -1, G_KEY_FILE_NONE, NULL))
		role = g_key_file_get_string (key_file, "GroupWise Folder", "Role", NULL);
	if (role && !*role)
		g_clear_pointer (&role, g_free);
	g_key_file_free (key_file);
	g_free (data);

	return role;
}

static gboolean
is_main_calendar (ESource *source)
{
	gchar *role;
	gboolean main;

	if (!e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR))
		return FALSE;
	role = source_role (source);
	main = !role;
	g_free (role);

	return main;
}

/* The appointments of the series "Evolution Testserie" */
static GSList *
series_uids (ECalClient *client)
{
	GSList *comps = NULL, *link, *uids = NULL;

	e_client_refresh_sync (E_CLIENT (client), NULL, NULL);
	spin (500);
	e_cal_client_get_object_list_sync (client, "(contains? \"summary\" \"Evolution Testserie\")", &comps, NULL, NULL);
	for (link = comps; link; link = g_slist_next (link))
		uids = g_slist_prepend (uids, g_strdup (i_cal_component_get_uid (link->data)));
	g_slist_free_full (comps, g_object_unref);

	return uids;
}

/* A series in a subcalendar: every appointment of it there, none in the Calendar */
static void
write_series (ECalClient *client)
{
	GSList *uids, *in_main, *link;
	gchar *uid;

	uid = create (client,
		"BEGIN:VEVENT\r\nUID:gw-cal-check-3\r\nSUMMARY:Evolution Testserie\r\n"
		"DTSTART;TZID=Europe/Berlin:20261019T080000\r\nDTEND;TZID=Europe/Berlin:20261019T083000\r\n"
		"RRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n");
	g_free (uid);
	uids = series_uids (client);
	check (g_slist_length (uids) == 3, "three appointments of the series in the subcalendar", NULL);
	in_main = series_uids (main_events);
	check (in_main == NULL, "none in the Calendar", NULL);
	for (link = uids; link; link = g_slist_next (link))
		remove_object (client, link->data);
	g_slist_free_full (uids, g_free);
	g_slist_free_full (in_main, g_free);
}

/* Moves @uid from @from to @to as Evolution does (cal_comp_transfer_item_to_sync):
 * created in the target without messages, removed from the source */
static gchar *
transfer (ECalClient *from,
	  ECalClient *to,
	  const gchar *uid,
	  const gchar *what)
{
	ICalComponent *comp = NULL;
	GError *error = NULL;
	gchar *new_uid = NULL;

	if (!e_cal_client_get_object_sync (from, uid, NULL, &comp, NULL, &error)) {
		check (FALSE, what, &error);
		return NULL;
	}
	if (e_cal_client_create_object_sync (to, comp, E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE, &new_uid, NULL, &error))
		e_cal_client_remove_object_sync (from, uid, NULL, E_CAL_OBJ_MOD_THIS, E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE, NULL, &error);
	check (error == NULL && new_uid, what, &error);
	g_object_unref (comp);

	return new_uid;
}

static gboolean
has_object (ECalClient *client,
	    const gchar *uid)
{
	ICalComponent *comp = server_object (client, uid);
	gboolean has = comp != NULL;

	g_clear_object (&comp);

	return has;
}

/* A personal appointment moved Calendar -> subcalendar -> other
 * subcalendar -> Calendar keeps its UID (it is the same item) */
static void
move_test (ECalClient *calendar,
	   ECalClient *sub1,
	   ECalClient *sub2)
{
	gchar *uid, *moved;

	printf ("    move test:\n");
	uid = create (calendar,
		"BEGIN:VEVENT\r\nUID:gw-cal-check-4\r\nSUMMARY:Evolution Verschiebetest\r\n"
		"DTSTART;TZID=Europe/Berlin:20261026T100000\r\nDTEND;TZID=Europe/Berlin:20261026T110000\r\nEND:VEVENT\r\n");
	if (!uid)
		return;

	moved = transfer (calendar, sub1, uid, "into the subcalendar");
	check (g_strcmp0 (moved, uid) == 0 && has_object (sub1, uid) && !has_object (calendar, uid),
		"the same appointment, in the subcalendar only", NULL);
	g_free (moved);

	if (sub2) {
		moved = transfer (sub1, sub2, uid, "into the other subcalendar");
		check (g_strcmp0 (moved, uid) == 0 && has_object (sub2, uid) && !has_object (sub1, uid) && !has_object (calendar, uid),
			"in the other subcalendar only", NULL);
		g_free (moved);
	}

	moved = transfer (sub2 ? sub2 : sub1, calendar, uid, "back into the Calendar");
	check (g_strcmp0 (moved, uid) == 0 && has_object (calendar, uid) && !has_object (sub1, uid) &&
		(!sub2 || !has_object (sub2, uid)), "back in the Calendar only", NULL);
	g_free (moved);

	remove_object (calendar, uid);
	g_free (uid);
}

static void
write_events (ECalClient *client)
{
	ICalComponent *comp;
	ICalTime *tt;
	gchar *uid, *location;

	uid = create (client,
		"BEGIN:VEVENT\r\nUID:gw-cal-check-1\r\nSUMMARY:Evolution Testtermin\r\nLOCATION:Raum 1\r\n"
		"DESCRIPTION:Aus Evolution angelegt\r\nCATEGORIES:Evolution-Kategorie\r\n"
		"DTSTART;TZID=Europe/Berlin:20261012T100000\r\nDTEND;TZID=Europe/Berlin:20261012T110000\r\n"
		"BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT10M\r\nEND:VALARM\r\nEND:VEVENT\r\n");
	if (!uid)
		return;
	/* GroupWise gives its own UID */
	check (!g_str_equal (uid, "gw-cal-check-1"), "UID from GroupWise", NULL);

	comp = server_object (client, uid);
	check (comp != NULL, "read back", NULL);
	if (!comp) {
		g_free (uid);
		return;
	}
	tt = i_cal_component_get_dtstart (comp);
	check (i_cal_time_get_hour (tt) == 10 && has_tzid (comp), "start 10:00 in the local zone", NULL);
	g_object_unref (tt);
	check (g_strcmp0 (i_cal_component_get_location (comp), "Raum 1") == 0, "place", NULL);
	check (g_strcmp0 (i_cal_component_get_description (comp), "Aus Evolution angelegt") == 0, "description", NULL);
	check (i_cal_component_count_components (comp, I_CAL_VALARM_COMPONENT) == 1, "alarm", NULL);
	{
		/* A GroupWise category of that name, created with it */
		ICalProperty *cat = i_cal_component_get_first_property (comp, I_CAL_CATEGORIES_PROPERTY);

		check (cat && g_strcmp0 (i_cal_property_get_categories (cat), "Evolution-Kategorie") == 0, "category", NULL);
		g_clear_object (&cat);
	}
	/* An appointment of a subcalendar is not in the Calendar */
	if (main_events && g_strcmp0 (e_source_get_uid (e_client_get_source (E_CLIENT (main_events))),
		e_source_get_uid (e_client_get_source (E_CLIENT (client)))) != 0) {
		ICalComponent *in_main = server_object (main_events, uid);

		check (in_main == NULL, "not in the Calendar", NULL);
		g_clear_object (&in_main);
	}

	/* Moved by an hour, other place, no alarm */
	tt = i_cal_time_new_from_string ("20261012T090000Z");
	i_cal_component_set_dtstart (comp, tt);
	g_object_unref (tt);
	tt = i_cal_time_new_from_string ("20261012T100000Z");
	i_cal_component_set_dtend (comp, tt);
	g_object_unref (tt);
	i_cal_component_set_location (comp, "Raum 2");
	{
		ICalComponent *alarm = i_cal_component_get_first_component (comp, I_CAL_VALARM_COMPONENT);

		i_cal_component_remove_component (comp, alarm);
		g_object_unref (alarm);
	}
	modify (client, comp, "change");
	g_object_unref (comp);

	comp = server_object (client, uid);
	if (comp) {
		tt = i_cal_component_get_dtstart (comp);
		location = g_strdup (i_cal_component_get_location (comp));
		/* 09:00 UTC is 11:00 CEST */
		check (i_cal_time_get_hour (tt) == 11 && has_tzid (comp) && g_strcmp0 (location, "Raum 2") == 0 &&
			i_cal_component_count_components (comp, I_CAL_VALARM_COMPONENT) == 0, "the change on the server", NULL);
		g_object_unref (tt);
		g_free (location);
		g_object_unref (comp);
	} else {
		check (FALSE, "the change on the server", NULL);
	}

	remove_object (client, uid);
	g_free (uid);

	/* All day */
	uid = create (client,
		"BEGIN:VEVENT\r\nUID:gw-cal-check-2\r\nSUMMARY:Evolution ganztaegig\r\n"
		"DTSTART;VALUE=DATE:20261014\r\nDTEND;VALUE=DATE:20261016\r\nTRANSP:TRANSPARENT\r\nEND:VEVENT\r\n");
	if (!uid)
		return;
	comp = server_object (client, uid);
	if (comp) {
		gchar *start = describe_time (comp, I_CAL_DTSTART_PROPERTY);
		gchar *end = describe_time (comp, I_CAL_DTEND_PROPERTY);

		check (g_str_equal (start, "20261014") && g_str_equal (end, "20261016"), "two days, as dates", NULL);
		printf ("      %s .. %s\n", start, end);
		g_free (start);
		g_free (end);
		g_object_unref (comp);
	} else {
		check (FALSE, "read back", NULL);
	}
	remove_object (client, uid);
	g_free (uid);
}

static void
write_tasks (ECalClient *client)
{
	ICalComponent *comp;
	gchar *uid;

	uid = create (client,
		"BEGIN:VTODO\r\nUID:gw-cal-check-3\r\nSUMMARY:Evolution Testaufgabe\r\n"
		"DTSTART;VALUE=DATE:20261012\r\nDUE;VALUE=DATE:20261016\r\nPRIORITY:1\r\nEND:VTODO\r\n");
	if (!uid)
		return;

	comp = server_object (client, uid);
	check (comp != NULL, "read back", NULL);
	if (comp) {
		gchar *due = describe_time (comp, I_CAL_DUE_PROPERTY);

		check (g_str_equal (due, "20261016"), "due date", NULL);
		g_free (due);

		/* Done */
		i_cal_component_set_status (comp, I_CAL_STATUS_COMPLETED);
		i_cal_component_take_property (comp, i_cal_property_new_percentcomplete (100));
		modify (client, comp, "complete");
		g_object_unref (comp);

		comp = server_object (client, uid);
		check (comp && i_cal_component_get_status (comp) == I_CAL_STATUS_COMPLETED, "completed on the server", NULL);
		g_clear_object (&comp);
	}

	remove_object (client, uid);
	g_free (uid);
}

static void
write_memos (ECalClient *client)
{
	ICalComponent *comp;
	gchar *uid;

	uid = create (client,
		"BEGIN:VJOURNAL\r\nUID:gw-cal-check-4\r\nSUMMARY:Evolution Testnotiz\r\n"
		"DESCRIPTION:Erste Fassung\r\nDTSTART;VALUE=DATE:20261013\r\nEND:VJOURNAL\r\n");
	if (!uid)
		return;

	comp = server_object (client, uid);
	check (comp != NULL, "read back", NULL);
	if (comp) {
		ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_DESCRIPTION_PROPERTY);

		i_cal_component_remove_property (comp, prop);
		g_object_unref (prop);
		i_cal_component_take_property (comp, i_cal_property_new_description ("Zweite Fassung"));
		modify (client, comp, "change the text");
		g_object_unref (comp);

		comp = server_object (client, uid);
		check (comp && g_strcmp0 (i_cal_component_get_description (comp), "Zweite Fassung") == 0, "the text on the server", NULL);
		g_clear_object (&comp);
	}

	remove_object (client, uid);
	g_free (uid);
}

static void
answer (ECalClient *client,
	const gchar *uid,
	const gchar *partstat)
{
	ICalComponent *comp = NULL;
	ICalProperty *prop;
	GError *error = NULL;
	gchar *email = NULL;

	if (!e_cal_client_get_object_sync (client, uid, NULL, &comp, NULL, &error)) {
		check (FALSE, "find the appointment", &error);
		return;
	}
	e_client_get_backend_property_sync (E_CLIENT (client), E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS, &email, NULL, NULL);

	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		const gchar *address = i_cal_property_get_attendee (prop);

		if (email && address && g_ascii_strcasecmp (address + (g_ascii_strncasecmp (address, "mailto:", 7) ? 0 : 7), email) == 0) {
			i_cal_property_remove_parameter_by_kind (prop, I_CAL_PARTSTAT_PARAMETER);
			i_cal_property_take_parameter (prop, i_cal_parameter_new_partstat (
				g_str_equal (partstat, "DECLINED") ? I_CAL_PARTSTAT_DECLINED :
				g_str_equal (partstat, "TENTATIVE") ? I_CAL_PARTSTAT_TENTATIVE : I_CAL_PARTSTAT_ACCEPTED));
			g_object_unref (prop);
			break;
		}
	}
	printf ("    answer %s as %s\n", partstat, email ? email : "?");
	modify (client, comp, "answer");
	g_object_unref (comp);
	g_free (email);
}

static void
receive (ECalClient *client,
	 const gchar *uid,
	 const gchar *partstat)
{
	ICalComponent *comp = NULL, *vcalendar;
	ICalProperty *prop;
	GError *error = NULL;
	gchar *email = NULL;

	if (!e_cal_client_get_object_sync (client, uid, NULL, &comp, NULL, &error)) {
		check (FALSE, "find the appointment", &error);
		return;
	}
	e_client_get_backend_property_sync (E_CLIENT (client), E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS, &email, NULL, NULL);
	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		const gchar *address = i_cal_property_get_attendee (prop);

		if (email && address && g_ascii_strcasecmp (address + (g_ascii_strncasecmp (address, "mailto:", 7) ? 0 : 7), email) == 0) {
			i_cal_property_remove_parameter_by_kind (prop, I_CAL_PARTSTAT_PARAMETER);
			i_cal_property_take_parameter (prop, i_cal_parameter_new_partstat (
				g_str_equal (partstat, "DECLINED") ? I_CAL_PARTSTAT_DECLINED :
				g_str_equal (partstat, "TENTATIVE") ? I_CAL_PARTSTAT_TENTATIVE : I_CAL_PARTSTAT_ACCEPTED));
			g_object_unref (prop);
			break;
		}
	}
	vcalendar = i_cal_component_new_vcalendar ();
	i_cal_component_set_method (vcalendar, I_CAL_METHOD_REQUEST);
	i_cal_component_take_component (vcalendar, comp);
	printf ("    receive REQUEST %s as %s\n", partstat, email ? email : "?");
	check (e_cal_client_receive_objects_sync (client, vcalendar, E_CAL_OPERATION_FLAG_NONE, NULL, &error),
		"receive", &error);
	g_object_unref (vcalendar);
	g_free (email);
}

static void
set_categories (ECalClient *client,
		const gchar *uid,
		const gchar *categories)
{
	ICalComponent *comp = NULL;
	ICalProperty *prop;
	GError *error = NULL;

	if (!e_cal_client_get_object_sync (client, uid, NULL, &comp, NULL, &error)) {
		check (FALSE, "find the appointment", &error);
		return;
	}
	while ((prop = i_cal_component_get_first_property (comp, I_CAL_CATEGORIES_PROPERTY))) {
		i_cal_component_remove_property (comp, prop);
		g_object_unref (prop);
	}
	if (*categories)
		i_cal_component_take_property (comp, i_cal_property_new_categories (categories));
	modify (client, comp, "set the categories");
	g_object_unref (comp);
}

static void
set_alarm (ECalClient *client,
	   const gchar *uid,
	   const gchar *minutes)
{
	ICalComponent *comp = NULL, *alarm;
	ICalTrigger *trigger;
	GError *error = NULL;

	if (!e_cal_client_get_object_sync (client, uid, NULL, &comp, NULL, &error)) {
		check (FALSE, "find the appointment", &error);
		return;
	}
	while ((alarm = i_cal_component_get_first_component (comp, I_CAL_VALARM_COMPONENT))) {
		i_cal_component_remove_component (comp, alarm);
		g_object_unref (alarm);
	}
	alarm = i_cal_component_new_valarm ();
	i_cal_component_take_property (alarm, i_cal_property_new_action (I_CAL_ACTION_DISPLAY));
	trigger = i_cal_trigger_new_from_int (-60 * atoi (minutes));
	i_cal_component_take_property (alarm, i_cal_property_new_trigger (trigger));
	g_object_unref (trigger);
	i_cal_component_take_component (comp, alarm);
	modify (client, comp, "set the alarm");
	g_object_unref (comp);
}

static void
invite (ECalClient *client,
	const gchar *email)
{
	gchar *organizer = NULL, *ical, *uid;
	ICalComponent *comp;

	e_client_get_backend_property_sync (E_CLIENT (client), E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS, &organizer, NULL, NULL);
	/* As Evolution's editor makes a meeting: the organizer takes part */
	ical = g_strdup_printf (
		"BEGIN:VEVENT\r\nUID:gw-cal-check-invite\r\nSUMMARY:Testbesprechung von Karl\r\n"
		"LOCATION:Besprechungsraum\r\nDESCRIPTION:Einladung aus Evolution (evolution-groupwise)\r\n"
		"DTSTART;TZID=Europe/Berlin:20261009T100000\r\nDTEND;TZID=Europe/Berlin:20261009T110000\r\n"
		"ORGANIZER:mailto:%s\r\n"
		"ATTENDEE;PARTSTAT=ACCEPTED;ROLE=CHAIR:mailto:%s\r\n"
		"ATTENDEE;PARTSTAT=NEEDS-ACTION;ROLE=REQ-PARTICIPANT;RSVP=TRUE:mailto:%s\r\n"
		"BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\nEND:VALARM\r\nEND:VEVENT\r\n",
		organizer, organizer, email);
	printf ("    invite %s from %s\n", email, organizer ? organizer : "?");
	uid = create (client, ical);
	if (uid) {
		comp = server_object (client, uid);
		check (comp != NULL, "the meeting from the server", NULL);
		if (comp) {
			gchar *source = e_cal_util_component_dup_x_property (comp, "X-GW-SOURCE");

			check (g_strcmp0 (source, "sent") == 0, "sent to the attendee", NULL);
			g_free (source);
			g_object_unref (comp);
		}
	}
	g_free (uid);
	g_free (ical);
	g_free (organizer);
}

static void
free_busy (ECalClient *client,
	   const gchar *emails)
{
	gchar **list = g_strsplit (emails, ",", -1);
	GSList *users = NULL, *result = NULL, *link;
	GError *error = NULL;
	time_t start = time (NULL);
	guint ii;

	for (ii = 0; list[ii]; ii++)
		users = g_slist_append (users, list[ii]);

	if (!e_cal_client_get_free_busy_sync (client, start, start + 7 * 24 * 3600, users, &result, NULL, &error)) {
		check (FALSE, "free/busy", &error);
	} else {
		check (result != NULL, "free/busy", NULL);
		for (link = result; link; link = g_slist_next (link)) {
			ICalComponent *vfb = e_cal_component_get_icalcomponent (link->data);
			ICalProperty *attendee = i_cal_component_get_first_property (vfb, I_CAL_ATTENDEE_PROPERTY);

			printf ("      %s: %d busy periods\n", attendee ? i_cal_property_get_attendee (attendee) : "?",
				i_cal_component_count_properties (vfb, I_CAL_FREEBUSY_PROPERTY));
			g_clear_object (&attendee);
		}
	}

	g_slist_free_full (result, g_object_unref);
	g_slist_free (users);
	g_strfreev (list);
}

int
main (int argc,
      char **argv)
{
	GError *error = NULL;
	ESourceRegistry *registry;
	ESource *collection;
	GList *sources, *link;
	gboolean write_test, write_test_allowed;

	if (argc < 2) {
		fprintf (stderr, "Usage: %s COLLECTION-UID [--write-test]\n", argv[0]);
		return 2;
	}
	write_test = g_strv_contains ((const gchar * const *) argv, "--write-test");

	registry = e_source_registry_new_sync (NULL, &error);
	if (!registry)
		g_error ("%s", error->message);
	collection = e_source_registry_ref_source (registry, argv[1]);
	if (!collection)
		g_error ("No source %s", argv[1]);

	write_test_allowed = g_strcmp0 (e_source_authentication_get_user (
		e_source_get_extension (collection, E_SOURCE_EXTENSION_AUTHENTICATION)), "karl") == 0;
	if (write_test) {
		if (!write_test_allowed) {
			fprintf (stderr, "--write-test only for the test account karl\n");
			return 2;
		}
	}

	sources = e_source_registry_list_sources (registry, NULL);
	for (link = sources; link && !main_events; link = g_list_next (link)) {
		if (g_strcmp0 (e_source_get_parent (link->data), argv[1]) == 0 && is_main_calendar (link->data))
			main_events = E_CAL_CLIENT (e_cal_client_connect_sync (link->data, E_CAL_CLIENT_SOURCE_TYPE_EVENTS,
				(guint32) -1, NULL, NULL));
	}
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;
		ECalClientSourceType type;
		EClient *client;

		if (g_strcmp0 (e_source_get_parent (source), argv[1]) != 0)
			continue;
		if (e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR))
			type = E_CAL_CLIENT_SOURCE_TYPE_EVENTS;
		else if (e_source_has_extension (source, E_SOURCE_EXTENSION_TASK_LIST))
			type = E_CAL_CLIENT_SOURCE_TYPE_TASKS;
		else if (e_source_has_extension (source, E_SOURCE_EXTENSION_MEMO_LIST))
			type = E_CAL_CLIENT_SOURCE_TYPE_MEMOS;
		else
			continue;

		{
			gchar *role = source_role (source);

			printf ("  %s (%s)%s%s\n", e_source_get_display_name (source), e_source_get_uid (source),
				role ? " role " : "", role ? role : "");
			g_free (role);
		}
		if (g_getenv ("GW_TRACE"))
			fprintf (stderr, "%" G_GINT64_FORMAT " connect\n", g_get_monotonic_time () / 1000);
		/* Not waiting for "connected": that notice arrives through the main
		 * loop, which this tool does not run; the calls wait for the backend */
		client = e_cal_client_connect_sync (source, type, (guint32) -1, NULL, &error);
		if (g_getenv ("GW_TRACE"))
			fprintf (stderr, "%" G_GINT64_FORMAT " connected\n", g_get_monotonic_time () / 1000);
		if (!client) {
			printf ("    open: %s\n", error->message);
			g_clear_error (&error);
			failures++;
			continue;
		}
		e_client_refresh_sync (client, NULL, NULL);
		spin (g_getenv ("GW_SETTLE") ? atoi (g_getenv ("GW_SETTLE")) : 500);
		if (g_getenv ("GW_TRACE"))
			fprintf (stderr, "%" G_GINT64_FORMAT " refreshed\n", g_get_monotonic_time () / 1000);
		list_objects (E_CAL_CLIENT (client));

		for (guint ii = 2; ii + 1 < (guint) argc; ii++) {
			if (g_str_equal (argv[ii], "--delete") && type == E_CAL_CLIENT_SOURCE_TYPE_EVENTS)
				remove_object (E_CAL_CLIENT (client), argv[ii + 1]);
			if (g_str_equal (argv[ii], "--invite") && type == E_CAL_CLIENT_SOURCE_TYPE_EVENTS) {
				if (!write_test_allowed)
					fprintf (stderr, "--invite only for the test account karl\n");
				else
					invite (E_CAL_CLIENT (client), argv[ii + 1]);
			}
			if (g_str_equal (argv[ii], "--free-busy") && type == E_CAL_CLIENT_SOURCE_TYPE_EVENTS)
				free_busy (E_CAL_CLIENT (client), argv[ii + 1]);
		}
		for (guint ii = 2; ii + 2 < (guint) argc; ii++) {
			if (type != E_CAL_CLIENT_SOURCE_TYPE_EVENTS)
				continue;
			if (g_str_equal (argv[ii], "--answer"))
				answer (E_CAL_CLIENT (client), argv[ii + 1], argv[ii + 2]);
			else if (g_str_equal (argv[ii], "--receive"))
				receive (E_CAL_CLIENT (client), argv[ii + 1], argv[ii + 2]);
			else if (g_str_equal (argv[ii], "--set-categories") && write_test_allowed)
				set_categories (E_CAL_CLIENT (client), argv[ii + 1], argv[ii + 2]);
			else if (g_str_equal (argv[ii], "--set-alarm"))
				set_alarm (E_CAL_CLIENT (client), argv[ii + 1], argv[ii + 2]);
			else
				continue;
			e_client_refresh_sync (client, NULL, NULL);
			spin (g_getenv ("GW_SETTLE") ? atoi (g_getenv ("GW_SETTLE")) : 500);
			list_objects (E_CAL_CLIENT (client));
		}

		if (write_test && !e_client_is_readonly (client)) {
			printf ("    write test:\n");
			if (type == E_CAL_CLIENT_SOURCE_TYPE_EVENTS) {
				write_events (E_CAL_CLIENT (client));
				if (main_events && !is_main_calendar (source))
					write_series (E_CAL_CLIENT (client));
			} else if (type == E_CAL_CLIENT_SOURCE_TYPE_TASKS)
				write_tasks (E_CAL_CLIENT (client));
			else
				write_memos (E_CAL_CLIENT (client));
		}
		g_object_unref (client);
	}
	/* Moves between the Calendar and the (first two) subcalendars */
	if (write_test && main_events) {
		ECalClient *subs[2] = { NULL, NULL };
		guint n_subs = 0;

		for (link = sources; link && n_subs < 2; link = g_list_next (link)) {
			gchar *role = g_strcmp0 (e_source_get_parent (link->data), argv[1]) == 0 &&
				e_source_has_extension (link->data, E_SOURCE_EXTENSION_CALENDAR) ? source_role (link->data) : NULL;

			if (g_strcmp0 (role, "own") == 0)
				subs[n_subs++] = E_CAL_CLIENT (e_cal_client_connect_sync (link->data, E_CAL_CLIENT_SOURCE_TYPE_EVENTS,
					(guint32) -1, NULL, NULL));
			g_free (role);
		}
		if (subs[0])
			move_test (main_events, subs[0], subs[1]);
		g_clear_object (&subs[0]);
		g_clear_object (&subs[1]);
	}

	g_clear_object (&main_events);
	g_list_free_full (sources, g_object_unref);
	printf ("%s\n", failures ? "FAILURES" : "all ok");

	g_object_unref (collection);
	g_object_unref (registry);

	return failures ? 1 : 0;
}

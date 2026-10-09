/*
 * e-gw-calendar.h: GroupWise calendar items and iCalendar components
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

#ifndef E_GW_CALENDAR_H
#define E_GW_CALENDAR_H

#include <libecal/libecal.h>
#include <libxml/tree.h>

G_BEGIN_DECLS

/* What a component keeps of its item */
#define E_GW_CALENDAR_X_ITEM_ID		"X-GW-ITEM-ID"
#define E_GW_CALENDAR_X_SOURCE		"X-GW-SOURCE"
#define E_GW_CALENDAR_X_ICAL_ID		"X-GW-ICALID"
#define E_GW_CALENDAR_X_RECURRENCE_KEY	"X-GW-RECURRENCE-KEY"
#define E_GW_CALENDAR_X_TASK_PRIORITY	"X-GW-TASK-PRIORITY"
#define E_GW_CALENDAR_X_BUSY_STATUS	"X-MICROSOFT-CDO-BUSYSTATUS"
/* Preparation and travel time of an appointment, in seconds (the POA keeps
 * an appointment "Before: ..." and "After: ..." for them) */
#define E_GW_CALENDAR_X_TRAVEL_BEFORE	"X-GW-TRAVEL-BEFORE"
#define E_GW_CALENDAR_X_TRAVEL_AFTER	"X-GW-TRAVEL-AFTER"
/* Such an appointment: the item ID of the appointment it belongs to */
#define E_GW_CALENDAR_X_TRAVEL_OF	"X-GW-TRAVEL-OF"

/* VEVENT for an Appointment, VTODO for a Task, VJOURNAL for a Note,
 * I_CAL_NO_COMPONENT for anything else */
ICalComponentKind
		e_gw_calendar_item_kind		(xmlNode *item);

/* The UID of the item's component: its iCalId in the iCalendar form GroupWise
 * uses in invitations (2026-09-25T15:06:24Z_7536008F2ED@example.com becomes
 * 20260925T150624Z_7536008F2ED@example.com), else the item ID. The instances of a
 * series share the iCalId, each gets its start appended. */
gchar *		e_gw_calendar_uid		(xmlNode *item);

/* The item as a component (NULL for other item types). @zone is the user's
 * time zone, for dates GroupWise gives as local midnight in UTC. @user_email
 * finds the user among the attendees. */
ICalComponent *	e_gw_calendar_component_from_item
						(xmlNode *item,
						 ICalTimezone *zone,
						 const gchar *user_email);

/* @comp in a VCALENDAR with the VTIMEZONE of @zone (the zone of its times) */
ICalComponent *	e_gw_calendar_wrap		(ICalComponent *comp,
						 ICalTimezone *zone);

/* What changes when the item changes, including the answers of attendees */
gchar *		e_gw_calendar_revision		(xmlNode *item);

/* The answer of the user's attendee (I_CAL_PARTSTAT_NONE: not among them).
 * GroupWise writes PARTSTAT twice and Evolution replaces only the first
 * when the user answers: the last one counts. */
ICalParameterPartstat
		e_gw_calendar_user_partstat	(ICalComponent *comp,
						 const gchar *user_email);

/* The categories of @comp (CATEGORIES, also comma separated), each once */
GPtrArray *	e_gw_calendar_dup_categories	(ICalComponent *comp);

/* @comp with exactly these categories (NULL-terminated) */
void		e_gw_calendar_set_categories	(ICalComponent *comp,
						 const gchar * const *categories);

/* Whether the task component is completed */
gboolean	e_gw_calendar_is_completed	(ICalComponent *comp);

/* The <item> for sendItemRequest. With attendees other than the user it is
 * sent to them (source "sent", a reply requested), else it is personal.
 * @tz_cache resolves TZIDs (NULL: built-in zones only), @zone is the user's
 * zone for all-day events and floating times. Completion of a task is not
 * part of it (completeRequest). */
gchar *		e_gw_calendar_item_xml		(ICalComponent *comp,
						 ETimezoneCache *tz_cache,
						 ICalTimezone *zone,
						 const gchar *user_email,
						 GError **error);

/* Whether e_gw_calendar_item_xml() sends @comp as a meeting: the user
 * organizes it and there are attendees other than the user */
gboolean	e_gw_calendar_is_meeting	(ICalComponent *comp,
						 const gchar *user_email);

/* The <delete>/<add>/<update> parts for modifyItemRequest that make the item
 * @current into @comp; empty when nothing changed. The attendees and the
 * recurrence of an item are not changed this way. */
gchar *		e_gw_calendar_updates_xml	(xmlNode *current,
						 ICalComponent *comp,
						 ETimezoneCache *tz_cache,
						 ICalTimezone *zone,
						 GError **error);

/* Start and end of an event as GroupWise has them (UTC) */
gboolean	e_gw_calendar_event_range	(ICalComponent *comp,
						 ETimezoneCache *tz_cache,
						 ICalTimezone *zone,
						 gchar **out_start,
						 gchar **out_end);

G_END_DECLS

#endif /* E_GW_CALENDAR_H */

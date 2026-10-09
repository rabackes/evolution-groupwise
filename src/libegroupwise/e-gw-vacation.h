/*
 * e-gw-vacation.h: the out of office rule of GroupWise
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

#ifndef E_GW_VACATION_H
#define E_GW_VACATION_H

#include "e-gw-connection.h"

G_BEGIN_DECLS

/*
 * The out of office rule ("VacationRule", API version 1.08 and later): one
 * per mailbox. Writing it replaces it; the POA keeps an appointment "out of
 * office" in the calendar for its date range, and removes it when the rule
 * is switched off.
 */
typedef struct {
	gboolean enabled;
	gchar *subject;
	gchar *message;			/* plain text */
	gboolean include_sender_message;
	gboolean reply_to_external;	/* also answer Internet senders ... */
	gboolean my_contacts_only;	/* ... only those in the user's contacts */
	gchar *external_subject;	/* NULL: the same as for internal senders */
	gchar *external_message;
	/* The date range, if any: whole days (YYYY-MM-DD, both included) or
	 * from and to a time (UTC) */
	gboolean has_range;
	gboolean all_day;
	gchar *start_day, *end_day;
	GDateTime *start, *end;
} EGwVacation;

EGwVacation *	e_gw_vacation_new		(void);
void		e_gw_vacation_free		(EGwVacation *vacation);

/* The rule of the mailbox; one switched off without text when there is none */
EGwVacation *	e_gw_connection_get_vacation_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);
/* Writes the rule; the times are in the time zone of this computer */
gboolean	e_gw_connection_set_vacation_sync
						(EGwConnection *cnc,
						 const EGwVacation *vacation,
						 GCancellable *cancellable,
						 GError **error);

/* The <timezone> of getTimezoneListRequest that is the local time zone:
 * the same offsets and months of change; NULL when none fits */
gchar *		e_gw_connection_dup_local_timezone_xml_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_VACATION_H */

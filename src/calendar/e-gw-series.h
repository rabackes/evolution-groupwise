/*
 * e-gw-series.h: the appointments of a GroupWise series as one recurring event
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

#ifndef E_GW_SERIES_H
#define E_GW_SERIES_H

#include <libecal/libecal.h>

G_BEGIN_DECLS

/* One appointment of a series: its component as the calendar had it by
 * itself, and what the calendar keeps with it (revision, extra) */
typedef struct {
	ICalComponent *comp;
	gchar *revision;
	gchar *extra;
} EGwSeriesInstance;

EGwSeriesInstance *
		e_gw_series_instance_new	(ICalComponent *comp,
						 const gchar *revision,
						 const gchar *extra);
void		e_gw_series_instance_free	(EGwSeriesInstance *instance);

/* The key of the series @comp is an appointment of, when the series can be
 * shown as one: an appointment with the recurrence key and the rule. */
gchar *		e_gw_series_dup_key		(ICalComponent *comp);

/*
 * The appointments of one series (EGwSeriesInstance, all with the same
 * key) as a recurring event: a VCALENDAR with the master (the rule, EXDATEs
 * for occurrences without an appointment) and a detached instance for every
 * appointment that differs from it. @zone is the zone of their times.
 *
 * @out_extra tells which appointment is which occurrence:
 * "series:KEY", then a line "OCCURRENCE<tab>EXTRA" for each (see
 * e_gw_series_split_extra()).
 *
 * Returns NULL when they are no series to show: fewer than two, no usable
 * rule, more appointments than the rule has occurrences.
 */
ICalComponent *	e_gw_series_build		(GPtrArray *instances,
						 ICalTimezone *zone,
						 gchar **out_uid,
						 gchar **out_revision,
						 gchar **out_extra);

/* Whether @extra is that of a series */
gboolean	e_gw_series_is_extra		(const gchar *extra);

/* The appointments of the extra of a series: string pairs { occurrence (as
 * in iCalendar), extra of the appointment }.
 * Returns: (transfer full) (element-type GStrv) */
GPtrArray *	e_gw_series_split_extra		(const gchar *extra,
						 gchar **out_key);

/* The extra of an appointment as it stands in a line of the series' */
gchar *		e_gw_series_escape_extra	(const gchar *extra);

G_END_DECLS

#endif /* E_GW_SERIES_H */

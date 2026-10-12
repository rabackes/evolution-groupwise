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

/* What an appointment of a series is to be: its extra and the component
 * for it (NULL: the appointment is gone from the series) */
typedef struct {
	gchar *extra;
	ICalComponent *comp;
} EGwSeriesTarget;

void		e_gw_series_target_free		(EGwSeriesTarget *target);

/*
 * The appointments of a series as the recurring event (@master with the
 * rule, @detached: its detached instances, ICalComponent) says them: for
 * each appointment of @pairs (see e_gw_series_split_extra()) the component
 * of its occurrence — the detached instance for it, else the master at that
 * day — or none when the rule no longer has the occurrence (excluded, or
 * past a new end). An appointment keeps its occurrence by the day, so the
 * time of a whole series can change.
 *
 * @out_added counts occurrences the event has that are no appointment yet.
 * Returns: (element-type EGwSeriesTarget), in the order of @pairs
 */
GPtrArray *	e_gw_series_targets		(ICalComponent *master,
						 GPtrArray *detached,
						 GPtrArray *pairs,
						 ICalTimezone *zone,
						 guint *out_added);

/* Whether two components of an appointment say the same: time, length and
 * everything else but what a component has of its own (stamps, IDs) */
gboolean	e_gw_series_same		(ICalComponent *one,
						 ICalComponent *two,
						 ICalTimezone *zone);

G_END_DECLS

#endif /* E_GW_SERIES_H */

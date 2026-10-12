/*
 * e-gw-series.c: the appointments of a GroupWise series as one recurring event
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
 * GroupWise keeps a series as one item per appointment; the items share a
 * recurrenceKey and — series made since about GroupWise 2012 — each carries
 * the rule and the start of the series. What is really there are the items:
 * an appointment of the series may be gone, moved or changed by itself, and
 * the rule may count one more than there are.
 *
 * Here the items of one key become what iCalendar calls a recurring event:
 * a master with the rule, EXDATEs for occurrences of the rule that have no
 * item, and a detached instance (RECURRENCE-ID) for every item that is not
 * what the master says for its occurrence.
 */

#include <string.h>

#include "e-gw-calendar.h"
#include "e-gw-series.h"

/* Trailing occurrences without an item are excluded one by one up to this
 * many; beyond, the rule gets an end */
#define MAX_TRAILING 60
#define MAX_OCCURRENCES 4000

void
e_gw_series_instance_free (EGwSeriesInstance *instance)
{
	if (!instance)
		return;

	g_clear_object (&instance->comp);
	g_free (instance->revision);
	g_free (instance->extra);
	g_free (instance);
}

EGwSeriesInstance *
e_gw_series_instance_new (ICalComponent *comp,
			  const gchar *revision,
			  const gchar *extra)
{
	EGwSeriesInstance *instance = g_new0 (EGwSeriesInstance, 1);

	instance->comp = g_object_ref (comp);
	instance->revision = g_strdup (revision);
	instance->extra = g_strdup (extra);

	return instance;
}

gchar *
e_gw_series_dup_key (ICalComponent *comp)
{
	gchar *key, *rule;

	if (!comp || i_cal_component_isa (comp) != I_CAL_VEVENT_COMPONENT)
		return NULL;

	key = e_cal_util_component_dup_x_property (comp, E_GW_CALENDAR_X_RECURRENCE_KEY);
	rule = e_cal_util_component_dup_x_property (comp, E_GW_CALENDAR_X_RRULE);
	if (!key || !*key || !rule || !*rule)
		g_clear_pointer (&key, g_free);
	g_free (rule);

	return key;
}

/* A day as a number, or the instant: what an occurrence and the item for
 * it have in common */
static gint64
time_key (ICalTime *tt,
	  ICalTimezone *zone)
{
	if (i_cal_time_is_date (tt))
		return (gint64) i_cal_time_get_year (tt) * 10000 + i_cal_time_get_month (tt) * 100 + i_cal_time_get_day (tt);

	return (gint64) i_cal_time_as_timet_with_zone (tt, i_cal_time_get_timezone (tt) ? i_cal_time_get_timezone (tt) : zone);
}

static gint64
day_of (ICalTime *tt)
{
	return (gint64) i_cal_time_get_year (tt) * 10000 + i_cal_time_get_month (tt) * 100 + i_cal_time_get_day (tt);
}

typedef struct {
	EGwSeriesInstance *instance;
	ICalTime *start;	/* its DTSTART */
	gint64 key;
	gint64 seconds;		/* how long it lasts */
	gchar *signature;	/* all that is not its time */
	gint occurrence;	/* index of its occurrence, -1: none yet */
} Item;

static void
item_free (gpointer ptr)
{
	Item *item = ptr;

	g_clear_object (&item->start);
	g_free (item->signature);
	g_free (item);
}

static gint
compare_items (gconstpointer a,
	       gconstpointer b)
{
	const Item *one = *(Item * const *) a, *two = *(Item * const *) b;

	return one->key < two->key ? -1 : one->key > two->key ? 1 : 0;
}

/* The component without what every instance has of its own */
static gchar *
dup_signature (ICalComponent *comp,
	       gint64 seconds)
{
	static const ICalPropertyKind own_kinds[] = {
		I_CAL_UID_PROPERTY, I_CAL_DTSTART_PROPERTY, I_CAL_DTEND_PROPERTY, I_CAL_DURATION_PROPERTY,
		I_CAL_DTSTAMP_PROPERTY, I_CAL_CREATED_PROPERTY, I_CAL_LASTMODIFIED_PROPERTY,
		I_CAL_RECURRENCEID_PROPERTY, I_CAL_SEQUENCE_PROPERTY
	};
	static const gchar *own_x[] = {
		E_GW_CALENDAR_X_ITEM_ID, E_GW_CALENDAR_X_ICAL_ID, E_GW_CALENDAR_X_RECURRENCE_KEY,
		E_GW_CALENDAR_X_RRULE, E_GW_CALENDAR_X_SERIES_START, "X-GW-OWN-COPY-ID"
	};
	ICalComponent *clone = i_cal_component_clone (comp);
	gchar *text, *signature;
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (own_kinds); ii++)
		e_cal_util_component_remove_property_by_kind (clone, own_kinds[ii], TRUE);
	for (ii = 0; ii < G_N_ELEMENTS (own_x); ii++)
		e_cal_util_component_remove_x_property (clone, own_x[ii]);

	text = i_cal_component_as_ical_string (clone);
	signature = g_strdup_printf ("%" G_GINT64_FORMAT "\n%s", seconds, text ? text : "");
	g_free (text);
	g_object_unref (clone);

	return signature;
}

static void
add_time_property (ICalComponent *comp,
		   ICalProperty *prop,
		   ICalTime *tt,
		   ICalTimezone *zone)
{
	if (!i_cal_time_is_date (tt) && zone && zone != i_cal_timezone_get_utc_timezone () && i_cal_timezone_get_tzid (zone))
		i_cal_property_take_parameter (prop, i_cal_parameter_new_tzid (i_cal_timezone_get_tzid (zone)));
	i_cal_component_take_property (comp, prop);
}

/* "2026-10-12T10:00:00Z" */
static ICalTime *
parse_utc (const gchar *text)
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
	if (tt && !i_cal_time_is_date (tt))
		i_cal_time_set_timezone (tt, i_cal_timezone_get_utc_timezone ());

	return tt;
}

gchar *
e_gw_series_escape_extra (const gchar *extra)
{
	gchar *escaped = g_strdup (extra ? extra : "");

	/* (an extra is "ID" or "ID\nOWN-COPY-ID") */
	g_strdelimit (escaped, "\n", '\v');

	return escaped;
}

ICalComponent *
e_gw_series_build (GPtrArray *instances,
		   ICalTimezone *zone,
		   gchar **out_uid,
		   gchar **out_revision,
		   gchar **out_extra)
{
	GPtrArray *items, *occurrences;
	GHashTable *counts;
	GHashTableIter hash_iter;
	gpointer hash_key, hash_value;
	ICalComponent *vcalendar = NULL, *master, *first_comp;
	ICalRecurrence *rule = NULL;
	ICalRecurIterator *iterator = NULL;
	ICalTime *series_start = NULL, *master_start, *master_end, *tt;
	ICalProperty *prop;
	GChecksum *checksum;
	GString *extra;
	Item *representative = NULL;
	gchar *key, *rule_text = NULL, *start_text = NULL;
	const gchar *best_signature = NULL;
	gboolean all_day, finite = FALSE;
	gint64 last_key;
	gint last_matched = -1;
	guint ii, jj, best_count = 0, trailing = 0;

	g_return_val_if_fail (instances != NULL, NULL);

	/* One appointment is no series to show */
	if (instances->len < 2)
		return NULL;

	first_comp = ((EGwSeriesInstance *) instances->pdata[0])->comp;
	key = e_gw_series_dup_key (first_comp);
	if (!key)
		return NULL;

	items = g_ptr_array_new_with_free_func (item_free);
	occurrences = g_ptr_array_new_with_free_func (g_object_unref);
	counts = g_hash_table_new (g_str_hash, g_str_equal);

	for (ii = 0; ii < instances->len; ii++) {
		EGwSeriesInstance *instance = instances->pdata[ii];
		Item *item = g_new0 (Item, 1);
		ICalTime *end;

		item->instance = instance;
		item->occurrence = -1;
		item->start = i_cal_component_get_dtstart (instance->comp);
		end = i_cal_component_get_dtend (instance->comp);
		if (!item->start || i_cal_time_is_null_time (item->start)) {
			g_clear_object (&end);
			item_free (item);
			goto out;
		}
		item->key = time_key (item->start, zone);
		item->seconds = end && !i_cal_time_is_null_time (end) ?
			(i_cal_time_is_date (item->start) ?
			 (gint64) (i_cal_time_as_timet (end) - i_cal_time_as_timet (item->start)) :
			 time_key (end, zone) - item->key) : 0;
		item->signature = dup_signature (instance->comp, item->seconds);
		g_clear_object (&end);
		g_ptr_array_add (items, item);
	}
	g_ptr_array_sort (items, compare_items);
	all_day = i_cal_time_is_date (((Item *) items->pdata[0])->start);
	for (ii = 0; ii < items->len; ii++) {
		if (i_cal_time_is_date (((Item *) items->pdata[ii])->start) != all_day)
			goto out;
	}
	last_key = ((Item *) items->pdata[items->len - 1])->key;

	rule_text = e_cal_util_component_dup_x_property (first_comp, E_GW_CALENDAR_X_RRULE);
	rule = i_cal_recurrence_new_from_string (rule_text);
	if (!rule || i_cal_recurrence_get_freq (rule) == I_CAL_NO_RECURRENCE)
		goto out;

	/* Where the rule starts: the start of the series, in the form the
	 * appointments have (a day, or a time in the user's zone) */
	start_text = e_cal_util_component_dup_x_property (first_comp, E_GW_CALENDAR_X_SERIES_START);
	series_start = parse_utc (start_text);
	if (series_start && !i_cal_time_is_date (series_start) && zone)
		i_cal_time_convert_to_zone_inplace (series_start, zone);
	if (series_start && all_day && !i_cal_time_is_date (series_start)) {
		ICalTime *day = i_cal_time_new_null_time ();

		i_cal_time_set_date (day, i_cal_time_get_year (series_start), i_cal_time_get_month (series_start),
			i_cal_time_get_day (series_start));
		i_cal_time_set_is_date (day, TRUE);
		g_object_unref (series_start);
		series_start = day;
	}
	/* A series that says no start, or one after its first appointment,
	 * starts with that */
	if (!series_start || time_key (series_start, zone) > ((Item *) items->pdata[0])->key) {
		g_clear_object (&series_start);
		series_start = i_cal_time_clone (((Item *) items->pdata[0])->start);
	}

	/* The occurrences of the rule, until past the last appointment */
	iterator = i_cal_recur_iterator_new (rule, series_start);
	if (!iterator)
		goto out;
	finite = TRUE;
	while ((tt = i_cal_recur_iterator_next (iterator)) != NULL) {
		gint64 occurrence_key;

		if (i_cal_time_is_null_time (tt)) {
			g_object_unref (tt);
			break;
		}
		if (!i_cal_time_is_date (tt) && zone && !i_cal_time_get_timezone (tt))
			i_cal_time_set_timezone (tt, zone);
		occurrence_key = time_key (tt, zone);
		if (occurrence_key > last_key && ++trailing > MAX_TRAILING) {
			g_object_unref (tt);
			finite = FALSE;
			break;
		}
		g_ptr_array_add (occurrences, tt);
		if (occurrences->len > MAX_OCCURRENCES) {
			finite = FALSE;
			break;
		}
	}
	if (!occurrences->len)
		goto out;

	/* Which appointment is which occurrence: the one at its time; else
	 * the one of its day (moved within the day); else the next free one */
	for (ii = 0; ii < items->len; ii++) {
		Item *item = items->pdata[ii];

		for (jj = 0; jj < occurrences->len && item->occurrence < 0; jj++) {
			if (time_key (occurrences->pdata[jj], zone) == item->key)
				item->occurrence = jj;
		}
	}
	for (guint pass = 0; pass < 2; pass++) {
		for (ii = 0; ii < items->len; ii++) {
			Item *item = items->pdata[ii];

			for (jj = 0; jj < occurrences->len && item->occurrence < 0; jj++) {
				gboolean taken = FALSE;
				guint kk;

				for (kk = 0; kk < items->len && !taken; kk++)
					taken = ((Item *) items->pdata[kk])->occurrence == (gint) jj;
				if (!taken && (pass == 1 || day_of (occurrences->pdata[jj]) == day_of (item->start)))
					item->occurrence = jj;
			}
			/* More appointments than the rule has: no series to show */
			if (pass == 1 && item->occurrence < 0)
				goto out;
		}
	}
	for (ii = 0; ii < items->len; ii++)
		last_matched = MAX (last_matched, ((Item *) items->pdata[ii])->occurrence);

	/* What most appointments are is what the series is */
	for (ii = 0; ii < items->len; ii++) {
		Item *item = items->pdata[ii];
		guint count;

		if (time_key (occurrences->pdata[item->occurrence], zone) != item->key)
			continue;
		count = GPOINTER_TO_UINT (g_hash_table_lookup (counts, item->signature)) + 1;
		g_hash_table_insert (counts, item->signature, GUINT_TO_POINTER (count));
	}
	g_hash_table_iter_init (&hash_iter, counts);
	while (g_hash_table_iter_next (&hash_iter, &hash_key, &hash_value)) {
		if (GPOINTER_TO_UINT (hash_value) > best_count) {
			best_count = GPOINTER_TO_UINT (hash_value);
			best_signature = hash_key;
		}
	}
	for (ii = 0; ii < items->len && !representative; ii++) {
		Item *item = items->pdata[ii];

		if (best_signature && strcmp (item->signature, best_signature) == 0 &&
		    time_key (occurrences->pdata[item->occurrence], zone) == item->key)
			representative = item;
	}
	if (!representative)
		representative = items->pdata[0];

	*out_uid = g_strconcat ("gw-series-", key, NULL);

	/* The master: the representative at the start of the series */
	master = i_cal_component_clone (representative->instance->comp);
	i_cal_component_set_uid (master, *out_uid);
	e_cal_util_component_remove_x_property (master, E_GW_CALENDAR_X_ITEM_ID);
	e_cal_util_component_remove_x_property (master, "X-GW-OWN-COPY-ID");
	e_cal_util_component_remove_property_by_kind (master, I_CAL_DTSTART_PROPERTY, TRUE);
	e_cal_util_component_remove_property_by_kind (master, I_CAL_DTEND_PROPERTY, TRUE);
	master_start = i_cal_time_clone (occurrences->pdata[0]);
	master_end = i_cal_time_clone (master_start);
	if (all_day)
		i_cal_time_adjust (master_end, (gint) (representative->seconds / 86400), 0, 0, 0);
	else
		i_cal_time_adjust (master_end, 0, 0, 0, (gint) representative->seconds);
	i_cal_component_set_dtstart (master, master_start);
	i_cal_component_set_dtend (master, master_end);
	g_object_unref (master_start);
	g_object_unref (master_end);

	/* The rule ends with the last appointment there is: a rule that
	 * counts gets the number up to it (the POA keeps the number asked
	 * for, also when it made one appointment less), another one an end */
	if ((gint) occurrences->len - 1 > last_matched || !finite) {
		if (i_cal_recurrence_get_count (rule) > 0) {
			i_cal_recurrence_set_count (rule, last_matched + 1);
		} else {
			ICalTime *until = i_cal_time_clone (occurrences->pdata[last_matched]);

			if (!all_day)
				i_cal_time_convert_to_zone_inplace (until, i_cal_timezone_get_utc_timezone ());
			i_cal_recurrence_set_until (rule, until);
			g_object_unref (until);
		}
		g_ptr_array_set_size (occurrences, last_matched + 1);
	}
	i_cal_component_take_property (master, i_cal_property_new_rrule (rule));

	vcalendar = e_gw_calendar_wrap (master, all_day ? NULL : zone);
	g_object_unref (master);
	master = i_cal_component_get_first_component (vcalendar, I_CAL_VEVENT_COMPONENT);

	/* Occurrences without an appointment */
	for (jj = 0; jj < occurrences->len; jj++) {
		gboolean has_item = FALSE;

		for (ii = 0; ii < items->len && !has_item; ii++)
			has_item = ((Item *) items->pdata[ii])->occurrence == (gint) jj;
		if (!has_item) {
			prop = i_cal_property_new_exdate (occurrences->pdata[jj]);
			add_time_property (master, prop, occurrences->pdata[jj], zone);
		}
	}

	/* Appointments that are not what the master says */
	checksum = g_checksum_new (G_CHECKSUM_SHA1);
	extra = g_string_new ("series:");
	g_string_append (extra, key);
	for (ii = 0; ii < items->len; ii++) {
		Item *item = items->pdata[ii];
		ICalTime *occurrence = occurrences->pdata[item->occurrence];
		gchar *when = i_cal_time_as_ical_string (occurrence), *escaped = e_gw_series_escape_extra (item->instance->extra);

		g_string_append_printf (extra, "\n%s\t%s", when, escaped);
		g_checksum_update (checksum, (const guchar *) escaped, -1);
		g_checksum_update (checksum, (const guchar *) (item->instance->revision ? item->instance->revision : ""), -1);
		g_free (escaped);
		g_free (when);

		if (item != representative && (strcmp (item->signature, representative->signature) != 0 ||
		    time_key (occurrence, zone) != item->key)) {
			ICalComponent *detached = i_cal_component_clone (item->instance->comp);

			i_cal_component_set_uid (detached, *out_uid);
			prop = i_cal_property_new_recurrenceid (occurrence);
			add_time_property (detached, prop, occurrence, zone);
			i_cal_component_take_component (vcalendar, detached);
		}
	}
	g_clear_object (&master);

	*out_revision = g_strdup (g_checksum_get_string (checksum));
	*out_extra = g_string_free (extra, FALSE);
	g_checksum_free (checksum);

 out:
	g_clear_object (&iterator);
	g_clear_object (&rule);
	g_clear_object (&series_start);
	g_hash_table_destroy (counts);
	g_ptr_array_unref (occurrences);
	g_ptr_array_unref (items);
	g_free (rule_text);
	g_free (start_text);
	g_free (key);

	return vcalendar;
}

gboolean
e_gw_series_is_extra (const gchar *extra)
{
	return extra && g_str_has_prefix (extra, "series:");
}

GPtrArray *
e_gw_series_split_extra (const gchar *extra,
			 gchar **out_key)
{
	GPtrArray *instances;
	gchar **lines;
	guint ii;

	g_return_val_if_fail (e_gw_series_is_extra (extra), NULL);

	lines = g_strsplit (extra, "\n", -1);
	if (out_key)
		*out_key = g_strdup (lines[0] + strlen ("series:"));

	instances = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
	for (ii = 1; lines[ii]; ii++) {
		gchar **pair = g_strsplit (lines[ii], "\t", 2);

		if (pair[0] && pair[1]) {
			g_strdelimit (pair[1], "\v", '\n');
			g_ptr_array_add (instances, pair);
		} else {
			g_strfreev (pair);
		}
	}
	g_strfreev (lines);

	return instances;
}

void
e_gw_series_target_free (EGwSeriesTarget *target)
{
	if (!target)
		return;

	g_free (target->extra);
	g_clear_object (&target->comp);
	g_free (target);
}

static gint64
comp_seconds (ICalComponent *comp,
	      ICalTimezone *zone)
{
	ICalTime *start = i_cal_component_get_dtstart (comp), *end = i_cal_component_get_dtend (comp);
	gint64 seconds = 0;

	if (start && end && !i_cal_time_is_null_time (start) && !i_cal_time_is_null_time (end))
		seconds = i_cal_time_is_date (start) ?
			(gint64) (i_cal_time_as_timet (end) - i_cal_time_as_timet (start)) :
			time_key (end, zone) - time_key (start, zone);
	g_clear_object (&start);
	g_clear_object (&end);

	return seconds;
}

gboolean
e_gw_series_same (ICalComponent *one,
		  ICalComponent *two,
		  ICalTimezone *zone)
{
	static const ICalPropertyKind series_kinds[] = {
		I_CAL_RRULE_PROPERTY, I_CAL_EXDATE_PROPERTY, I_CAL_RDATE_PROPERTY, I_CAL_EXRULE_PROPERTY
	};
	ICalComponent *clones[2];
	ICalTime *start_one, *start_two;
	gchar *signatures[2];
	gboolean same;
	guint ii, jj;

	if (!one || !two)
		return one == two;

	start_one = i_cal_component_get_dtstart (one);
	start_two = i_cal_component_get_dtstart (two);
	same = start_one && start_two && i_cal_time_is_date (start_one) == i_cal_time_is_date (start_two) &&
		time_key (start_one, zone) == time_key (start_two, zone) &&
		comp_seconds (one, zone) == comp_seconds (two, zone);
	g_clear_object (&start_one);
	g_clear_object (&start_two);
	if (!same)
		return FALSE;

	clones[0] = i_cal_component_clone (one);
	clones[1] = i_cal_component_clone (two);
	for (ii = 0; ii < 2; ii++) {
		for (jj = 0; jj < G_N_ELEMENTS (series_kinds); jj++)
			e_cal_util_component_remove_property_by_kind (clones[ii], series_kinds[jj], TRUE);
		signatures[ii] = dup_signature (clones[ii], 0);
		g_object_unref (clones[ii]);
	}
	same = strcmp (signatures[0], signatures[1]) == 0;
	g_free (signatures[0]);
	g_free (signatures[1]);

	return same;
}

/* The master as the appointment of one of its occurrences */
static ICalComponent *
master_at (ICalComponent *master,
	   ICalTime *occurrence,
	   ICalTimezone *zone)
{
	static const ICalPropertyKind series_kinds[] = {
		I_CAL_RRULE_PROPERTY, I_CAL_EXDATE_PROPERTY, I_CAL_RDATE_PROPERTY, I_CAL_EXRULE_PROPERTY,
		I_CAL_DTSTART_PROPERTY, I_CAL_DTEND_PROPERTY, I_CAL_DURATION_PROPERTY
	};
	ICalComponent *comp = i_cal_component_clone (master);
	ICalTime *start = i_cal_time_clone (occurrence), *end = i_cal_time_clone (occurrence);
	gint64 seconds = comp_seconds (master, zone);
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (series_kinds); ii++)
		e_cal_util_component_remove_property_by_kind (comp, series_kinds[ii], TRUE);
	if (i_cal_time_is_date (start)) {
		i_cal_time_adjust (end, (gint) (seconds / 86400), 0, 0, 0);
	} else {
		if (zone && !i_cal_time_get_timezone (start)) {
			i_cal_time_set_timezone (start, zone);
			i_cal_time_set_timezone (end, zone);
		}
		/* (the length in real time: over a change of the clock too) */
		i_cal_time_adjust (end, 0, 0, 0, (gint) seconds);
	}
	i_cal_component_set_dtstart (comp, start);
	i_cal_component_set_dtend (comp, end);
	g_object_unref (start);
	g_object_unref (end);

	return comp;
}

GPtrArray *
e_gw_series_targets (ICalComponent *master,
		     GPtrArray *detached,
		     GPtrArray *pairs,
		     ICalTimezone *zone,
		     guint *out_added)
{
	GPtrArray *targets, *occurrences;
	GHashTable *excluded, *used_days;
	ICalProperty *prop;
	ICalRecurrence *rule = NULL;
	ICalRecurIterator *iterator = NULL;
	ICalTime *start, *tt;
	guint ii, jj, added = 0;

	g_return_val_if_fail (master != NULL, NULL);
	g_return_val_if_fail (pairs != NULL, NULL);

	targets = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_series_target_free);
	occurrences = g_ptr_array_new_with_free_func (g_object_unref);
	excluded = g_hash_table_new (g_int64_hash, g_int64_equal);
	used_days = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);

	start = i_cal_component_get_dtstart (master);
	if (start && !i_cal_time_is_date (start) && zone && !i_cal_time_get_timezone (start))
		i_cal_time_set_timezone (start, zone);

	for (prop = i_cal_component_get_first_property (master, I_CAL_EXDATE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (master, I_CAL_EXDATE_PROPERTY)) {
		ICalTime *exdate = i_cal_property_get_exdate (prop);
		gint64 *key = g_new (gint64, 1);

		*key = exdate ? day_of (exdate) : 0;
		g_hash_table_add (excluded, key);
		g_clear_object (&exdate);
	}

	/* The occurrences: the rule from the start; without a rule (the user
	 * took it away) only the start */
	prop = i_cal_component_get_first_property (master, I_CAL_RRULE_PROPERTY);
	if (prop) {
		rule = i_cal_property_get_rrule (prop);
		g_object_unref (prop);
	}
	if (rule && start)
		iterator = i_cal_recur_iterator_new (rule, start);
	if (iterator) {
		while ((tt = i_cal_recur_iterator_next (iterator)) != NULL) {
			if (i_cal_time_is_null_time (tt) || occurrences->len >= MAX_OCCURRENCES) {
				g_object_unref (tt);
				break;
			}
			if (!i_cal_time_is_date (tt) && zone && !i_cal_time_get_timezone (tt))
				i_cal_time_set_timezone (tt, zone);
			g_ptr_array_add (occurrences, tt);
		}
	} else if (start) {
		g_ptr_array_add (occurrences, i_cal_time_clone (start));
	}

	for (ii = 0; ii < pairs->len; ii++) {
		gchar **pair = pairs->pdata[ii];
		EGwSeriesTarget *target = g_new0 (EGwSeriesTarget, 1);
		ICalTime *old = i_cal_time_new_from_string (pair[0]);
		gint64 day = old ? day_of (old) : 0, *day_key;

		target->extra = g_strdup (pair[1]);
		day_key = g_new (gint64, 1);
		*day_key = day;
		g_hash_table_add (used_days, day_key);

		/* Its own component, when it has one */
		for (jj = 0; detached && jj < detached->len && !target->comp; jj++) {
			ICalProperty *rid_prop = i_cal_component_get_first_property (detached->pdata[jj], I_CAL_RECURRENCEID_PROPERTY);
			ICalTime *rid = rid_prop ? i_cal_property_get_recurrenceid (rid_prop) : NULL;

			if (rid && day_of (rid) == day) {
				target->comp = i_cal_component_clone (detached->pdata[jj]);
				e_cal_util_component_remove_property_by_kind (target->comp, I_CAL_RECURRENCEID_PROPERTY, TRUE);
			}
			g_clear_object (&rid);
			g_clear_object (&rid_prop);
		}
		/* Else what the master says for its day, if it still has it */
		for (jj = 0; jj < occurrences->len && !target->comp && !g_hash_table_contains (excluded, &day); jj++) {
			if (day_of (occurrences->pdata[jj]) == day)
				target->comp = master_at (master, occurrences->pdata[jj], zone);
		}
		g_clear_object (&old);
		g_ptr_array_add (targets, target);
	}

	/* Occurrences that are no appointment yet */
	for (jj = 0; jj < occurrences->len; jj++) {
		gint64 day = day_of (occurrences->pdata[jj]);

		if (!g_hash_table_contains (excluded, &day) && !g_hash_table_contains (used_days, &day))
			added++;
	}
	if (out_added)
		*out_added = added;

	g_clear_object (&iterator);
	g_clear_object (&rule);
	g_clear_object (&start);
	g_hash_table_destroy (excluded);
	g_hash_table_destroy (used_days);
	g_ptr_array_unref (occurrences);

	return targets;
}

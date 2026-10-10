/*
 * e-groupwise-travel-page.c: the preparation and travel time of GroupWise
 * appointments in Evolution's appointment editor, and the user among the
 * attendees of a new meeting
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
 * A page "Travel Time" (with the red car of the GroupWise client) in the
 * editor of appointments of a GroupWise calendar: hours and minutes before
 * and after the appointment, as in the GroupWise client. The calendar
 * backend keeps them in X-GW-TRAVEL-BEFORE/AFTER (seconds) and lets the POA
 * make the appointments of the travel time; 0 takes the travel time away.
 * Not shown for the appointments of a travel time themselves.
 *
 * A new meeting in a GroupWise calendar gets the owner of the calendar as
 * an attendee, as in the GroupWise client: GroupWise keeps the organizer's
 * own copy then, where the alarm, the categories and the travel time of the
 * organizer live.
 *
 * Saved with a new time (or new), an appointment of a GroupWise calendar is
 * checked against the busy time of the user's own calendars, the travel
 * times of other appointments among it, as the GroupWise client warns.
 *
 * A resource of the kind place (a room) invited to a meeting without
 * location gives it its name as location right away, as in the GroupWise
 * client; the GroupWise address book marks such resources (X-GROUPWISE-PLACE).
 */

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>
#include <calendar/gui/e-comp-editor.h>
#include <calendar/gui/e-comp-editor-event.h>
#include <calendar/gui/e-comp-editor-page.h>
#include <calendar/gui/e-comp-editor-page-general.h>
#include <calendar/gui/e-meeting-list-view.h>
#include <shell/e-shell.h>
#include <calendar/gui/calendar-config.h>
#include <calendar/gui/e-comp-editor-property-part.h>
#include <libebook/libebook.h>

#include "e-source-groupwise-folder.h"

#include "e-groupwise-travel-page.h"

/* As in src/calendar/e-gw-calendar.h (the backend's) */
#define X_TRAVEL_BEFORE "X-GW-TRAVEL-BEFORE"
#define X_TRAVEL_AFTER "X-GW-TRAVEL-AFTER"
#define X_TRAVEL_OF "X-GW-TRAVEL-OF"
#define X_ITEM_ID "X-GW-ITEM-ID"
#define X_OWN_COPY_ID "X-GW-OWN-COPY-ID"

/* At most so many appointments named in the warning */
#define MAX_NAMED_CONFLICTS 8

/* The red car of the GroupWise client */
static const gchar car_svg[] =
	"<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 16 16'>"
	"<path d='M3.2 4.2 Q3.6 3 4.8 3 H11.2 Q12.4 3 12.8 4.2 L13.8 7 H14.2 Q15 7 15 7.8 V11.4 Q15 12 14.4 12 H1.6 Q1 12 1 11.4 V7.8 Q1 7 1.8 7 H2.2 Z' fill='#d01c1c' stroke='#8b0000' stroke-width='0.6'/>"
	"<path d='M4.1 4.6 Q4.3 4 4.9 4 H11.1 Q11.7 4 11.9 4.6 L12.6 6.8 H3.4 Z' fill='#cfe3f5'/>"
	"<circle cx='4.4' cy='12' r='1.8' fill='#303030'/><circle cx='11.6' cy='12' r='1.8' fill='#303030'/>"
	"<circle cx='4.4' cy='12' r='0.7' fill='#c0c0c0'/><circle cx='11.6' cy='12' r='0.7' fill='#c0c0c0'/>"
	"<rect x='1.6' y='8.2' width='1.6' height='1' rx='0.4' fill='#ffe680'/><rect x='12.8' y='8.2' width='1.6' height='1' rx='0.4' fill='#ffe680'/>"
	"</svg>";

#define E_TYPE_GROUPWISE_TRAVEL_PAGE (e_groupwise_travel_page_get_type ())
#define E_GROUPWISE_TRAVEL_PAGE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), E_TYPE_GROUPWISE_TRAVEL_PAGE, EGroupwiseTravelPage))

typedef struct {
	ECompEditorPage parent;

	GtkWidget *before_hours;
	GtkWidget *before_minutes;
	GtkWidget *after_hours;
	GtkWidget *after_minutes;
	GtkWidget *same;
	GtkWidget *hint;

	/* An appointment of the travel time of another one */
	gboolean is_travel;
} EGroupwiseTravelPage;

typedef ECompEditorPageClass EGroupwiseTravelPageClass;

typedef struct {
	EExtension parent;

	/* The user as added to a new meeting (not referenced) */
	EMeetingAttendee *self;

	/* Attendees looked up as a place already (casefolded addresses) */
	GHashTable *checked;
	guint places_idle_id;
	/* Attendees the editor was opened with: no new location for them */
	gboolean as_opened;
} EGroupwiseTravelEditor;

typedef EExtensionClass EGroupwiseTravelEditorClass;

GType e_groupwise_travel_page_get_type (void);
GType e_groupwise_travel_editor_get_type (void);

G_DEFINE_DYNAMIC_TYPE (EGroupwiseTravelPage, e_groupwise_travel_page, E_TYPE_COMP_EDITOR_PAGE)
G_DEFINE_DYNAMIC_TYPE (EGroupwiseTravelEditor, e_groupwise_travel_editor, E_TYPE_EXTENSION)

static GdkPixbuf *
car_pixbuf (void)
{
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new_with_type ("svg", NULL);
	GdkPixbuf *pixbuf = NULL;

	if (!loader)
		return NULL;
	if (gdk_pixbuf_loader_write (loader, (const guchar *) car_svg, sizeof (car_svg) - 1, NULL) &&
	    gdk_pixbuf_loader_close (loader, NULL)) {
		pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
		if (pixbuf)
			g_object_ref (pixbuf);
	} else {
		gdk_pixbuf_loader_close (loader, NULL);
	}
	g_object_unref (loader);

	return pixbuf;
}

static gboolean
target_is_groupwise (ECompEditor *comp_editor)
{
	ECalClient *client = e_comp_editor_get_target_client (comp_editor);
	ESource *source;

	if (!client)
		return FALSE;
	source = e_client_get_source (E_CLIENT (client));

	return source && e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR) &&
		g_strcmp0 (e_source_backend_get_backend_name (
			e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR)), "groupwise") == 0;
}

static void
travel_page_update_visible (EGroupwiseTravelPage *page)
{
	ECompEditor *comp_editor = e_comp_editor_page_ref_editor (E_COMP_EDITOR_PAGE (page));

	if (comp_editor) {
		gtk_widget_set_visible (GTK_WIDGET (page), !page->is_travel && target_is_groupwise (comp_editor));
		g_object_unref (comp_editor);
	}
}

static gint
spin_seconds (GtkWidget *hours,
	      GtkWidget *minutes)
{
	return gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (hours)) * 3600 +
		gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (minutes)) * 60;
}

static void
set_spins (GtkWidget *hours,
	   GtkWidget *minutes,
	   gint seconds)
{
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (hours), seconds / 3600);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (minutes), (seconds % 3600) / 60);
}

static gint
x_seconds (ICalComponent *component,
	   const gchar *name)
{
	gchar *value = e_cal_util_component_dup_x_property (component, name);
	gint seconds = value ? (gint) g_ascii_strtoll (value, NULL, 10) : 0;

	g_free (value);

	return MAX (seconds, 0);
}

/* The same before and after: the time after follows the time before */
static void
travel_page_follow (EGroupwiseTravelPage *page)
{
	gboolean same = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (page->same));

	if (same)
		set_spins (page->after_hours, page->after_minutes, spin_seconds (page->before_hours, page->before_minutes));
}

static void
travel_page_changed_cb (EGroupwiseTravelPage *page)
{
	if (e_comp_editor_page_get_updating (E_COMP_EDITOR_PAGE (page)))
		return;

	e_comp_editor_page_set_updating (E_COMP_EDITOR_PAGE (page), TRUE);
	travel_page_follow (page);
	e_comp_editor_page_set_updating (E_COMP_EDITOR_PAGE (page), FALSE);

	e_comp_editor_page_emit_changed (E_COMP_EDITOR_PAGE (page));
}

static void
travel_page_same_toggled_cb (EGroupwiseTravelPage *page)
{
	gboolean same = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (page->same));

	gtk_widget_set_sensitive (page->after_hours, !same && gtk_widget_is_sensitive (page->before_hours));
	gtk_widget_set_sensitive (page->after_minutes, !same && gtk_widget_is_sensitive (page->before_minutes));
	travel_page_changed_cb (page);
}

static void
travel_page_sensitize_widgets (ECompEditorPage *comp_page,
			       gboolean force_insensitive)
{
	EGroupwiseTravelPage *page = E_GROUPWISE_TRAVEL_PAGE (comp_page);
	gboolean same = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (page->same));

	gtk_widget_set_sensitive (page->before_hours, !force_insensitive);
	gtk_widget_set_sensitive (page->before_minutes, !force_insensitive);
	gtk_widget_set_sensitive (page->after_hours, !force_insensitive && !same);
	gtk_widget_set_sensitive (page->after_minutes, !force_insensitive && !same);
	gtk_widget_set_sensitive (page->same, !force_insensitive);
}

static void
travel_page_fill_widgets (ECompEditorPage *comp_page,
			  ICalComponent *component)
{
	EGroupwiseTravelPage *page = E_GROUPWISE_TRAVEL_PAGE (comp_page);
	gint before = x_seconds (component, X_TRAVEL_BEFORE);
	gint after = x_seconds (component, X_TRAVEL_AFTER);

	page->is_travel = i_cal_component_isa (component) != I_CAL_VEVENT_COMPONENT ||
		e_cal_util_component_has_x_property (component, X_TRAVEL_OF);

	e_comp_editor_page_set_updating (comp_page, TRUE);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->same), before == after);
	set_spins (page->before_hours, page->before_minutes, before);
	set_spins (page->after_hours, page->after_minutes, after);
	e_comp_editor_page_set_updating (comp_page, FALSE);

	travel_page_update_visible (page);
}

static gboolean
travel_page_fill_component (ECompEditorPage *comp_page,
			    ICalComponent *component)
{
	EGroupwiseTravelPage *page = E_GROUPWISE_TRAVEL_PAGE (comp_page);
	struct {
		const gchar *name;
		gint seconds;
	} sides[] = {
		{ X_TRAVEL_BEFORE, spin_seconds (page->before_hours, page->before_minutes) },
		{ X_TRAVEL_AFTER, spin_seconds (page->after_hours, page->after_minutes) }
	};
	guint ii;

	if (page->is_travel)
		return TRUE;

	for (ii = 0; ii < G_N_ELEMENTS (sides); ii++) {
		if (sides[ii].seconds > 0) {
			gchar *value = g_strdup_printf ("%d", sides[ii].seconds);

			e_cal_util_component_set_x_property (component, sides[ii].name, value);
			g_free (value);
		} else {
			/* No travel time (any more): the backend takes it away */
			e_cal_util_component_remove_x_property (component, sides[ii].name);
		}
	}

	return TRUE;
}

static GtkWidget *
time_spin (gdouble upper,
	   gdouble step)
{
	GtkWidget *spin = gtk_spin_button_new_with_range (0, upper, step);

	gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (spin), TRUE);
	gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin), 0);
	gtk_entry_set_width_chars (GTK_ENTRY (spin), 3);

	return spin;
}

/* A row: "Before the appointment:  [h] hours  [min] minutes" */
static void
attach_row (GtkGrid *grid,
	    gint row,
	    const gchar *label_text,
	    GtkWidget *hours,
	    GtkWidget *minutes)
{
	GtkWidget *label = gtk_label_new_with_mnemonic (label_text);

	gtk_label_set_mnemonic_widget (GTK_LABEL (label), hours);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_grid_attach (grid, label, 0, row, 1, 1);
	gtk_grid_attach (grid, hours, 1, row, 1, 1);
	gtk_grid_attach (grid, gtk_label_new (_("hours")), 2, row, 1, 1);
	gtk_grid_attach (grid, minutes, 3, row, 1, 1);
	gtk_grid_attach (grid, gtk_label_new (_("minutes")), 4, row, 1, 1);
}

static void
travel_page_constructed (GObject *object)
{
	EGroupwiseTravelPage *page = E_GROUPWISE_TRAVEL_PAGE (object);
	GtkGrid *grid = GTK_GRID (object);
	GtkWidget *widget, *spins[4];
	guint ii;

	G_OBJECT_CLASS (e_groupwise_travel_page_parent_class)->constructed (object);

	gtk_container_set_border_width (GTK_CONTAINER (grid), 12);
	gtk_grid_set_row_spacing (grid, 6);
	gtk_grid_set_column_spacing (grid, 6);

	widget = gtk_label_new (_("Time blocked in your calendar before and after the appointment, "
		"for example to prepare or to get there. It is only yours: attendees of a meeting "
		"do not get it."));
	gtk_label_set_line_wrap (GTK_LABEL (widget), TRUE);
	gtk_label_set_max_width_chars (GTK_LABEL (widget), 60);
	gtk_label_set_xalign (GTK_LABEL (widget), 0.0);
	gtk_widget_set_margin_bottom (widget, 6);
	gtk_grid_attach (grid, widget, 0, 0, 5, 1);
	page->hint = widget;

	/* Hours as the GroupWise client, minutes in steps of 5 */
	page->before_hours = spins[0] = time_spin (99, 1);
	page->before_minutes = spins[1] = time_spin (55, 5);
	page->after_hours = spins[2] = time_spin (99, 1);
	page->after_minutes = spins[3] = time_spin (55, 5);

	attach_row (grid, 1, _("_Before the appointment:"), page->before_hours, page->before_minutes);
	attach_row (grid, 2, _("_After the appointment:"), page->after_hours, page->after_minutes);

	page->same = gtk_check_button_new_with_mnemonic (_("Travel times before/after the appointment are the _same"));
	gtk_widget_set_margin_top (page->same, 6);
	gtk_grid_attach (grid, page->same, 0, 3, 5, 1);

	for (ii = 0; ii < G_N_ELEMENTS (spins); ii++)
		g_signal_connect_swapped (spins[ii], "value-changed", G_CALLBACK (travel_page_changed_cb), page);
	g_signal_connect_swapped (page->same, "toggled", G_CALLBACK (travel_page_same_toggled_cb), page);

	gtk_widget_show_all (GTK_WIDGET (grid));
}

static void
e_groupwise_travel_page_class_init (EGroupwiseTravelPageClass *class)
{
	G_OBJECT_CLASS (class)->constructed = travel_page_constructed;
	class->sensitize_widgets = travel_page_sensitize_widgets;
	class->fill_widgets = travel_page_fill_widgets;
	class->fill_component = travel_page_fill_component;
}

static void
e_groupwise_travel_page_class_finalize (EGroupwiseTravelPageClass *class)
{
}

static void
e_groupwise_travel_page_init (EGroupwiseTravelPage *page)
{
}

static void
travel_editor_target_client_cb (ECompEditor *comp_editor,
				GParamSpec *param,
				EGroupwiseTravelPage *page)
{
	travel_page_update_visible (page);
}

static void
find_list_view_cb (GtkWidget *widget,
		   gpointer user_data)
{
	GtkWidget **found = user_data;

	if (*found)
		return;
	if (E_IS_MEETING_LIST_VIEW (widget))
		*found = widget;
	else if (GTK_IS_CONTAINER (widget))
		gtk_container_forall (GTK_CONTAINER (widget), find_list_view_cb, found);
}

/* The name of the user's mail identity with that address */
static gchar *
dup_identity_name (const gchar *email)
{
	EShell *shell = e_shell_get_default ();
	GList *sources, *link;
	gchar *name = NULL;

	if (!shell)
		return NULL;
	sources = e_source_registry_list_enabled (e_shell_get_registry (shell), E_SOURCE_EXTENSION_MAIL_IDENTITY);
	for (link = sources; link && !name; link = g_list_next (link)) {
		ESourceMailIdentity *identity = e_source_get_extension (link->data, E_SOURCE_EXTENSION_MAIL_IDENTITY);
		const gchar *address = e_source_mail_identity_get_address (identity);

		if (address && g_ascii_strcasecmp (address, email) == 0)
			name = e_source_mail_identity_dup_name (identity);
	}
	g_list_free_full (sources, g_object_unref);

	return name;
}

/* A new meeting in a GroupWise calendar: its owner is an attendee, as in
 * the GroupWise client (also after choosing another calendar) */
static void
travel_editor_ensure_self (EGroupwiseTravelEditor *editor)
{
	ECompEditor *comp_editor = E_COMP_EDITOR (e_extension_get_extensible (E_EXTENSION (editor)));
	ECompEditorPage *page = e_comp_editor_get_page (comp_editor, E_TYPE_COMP_EDITOR_PAGE_GENERAL);
	ECompEditorPageGeneral *general;
	EMeetingStore *store;
	EMeetingAttendee *attendee;
	GtkWidget *list_view = NULL;
	const gchar *email = e_comp_editor_get_cal_email_address (comp_editor);
	gchar *mailto, *name;

	if (!page)
		return;
	general = E_COMP_EDITOR_PAGE_GENERAL (page);
	store = e_comp_editor_page_general_get_meeting_store (general);
	gtk_container_forall (GTK_CONTAINER (page), find_list_view_cb, &list_view);

	/* The user of another calendar chosen before: not an attendee any more */
	if (editor->self) {
		const GPtrArray *attendees = e_meeting_store_get_attendees (store);
		gboolean still = FALSE;
		guint ii;

		for (ii = 0; attendees && ii < attendees->len && !still; ii++)
			still = g_ptr_array_index (attendees, ii) == editor->self;
		if (still && (!email || !*email || !target_is_groupwise (comp_editor) ||
		    g_ascii_strcasecmp (e_cal_util_strip_mailto (e_meeting_attendee_get_address (editor->self)), email) != 0)) {
			if (list_view)
				e_meeting_list_view_remove_attendee_from_name_selector (E_MEETING_LIST_VIEW (list_view), editor->self);
			e_meeting_store_remove_attendee (store, editor->self);
			still = FALSE;
		}
		if (!still)
			editor->self = NULL;
	}

	if (!(e_comp_editor_get_flags (comp_editor) & E_COMP_EDITOR_FLAG_IS_NEW) ||
	    !e_comp_editor_page_general_get_show_attendees (general) ||
	    !target_is_groupwise (comp_editor) || !email || !*email ||
	    e_meeting_store_find_attendee (store, email, NULL))
		return;

	attendee = E_MEETING_ATTENDEE (e_meeting_attendee_new ());
	mailto = g_strconcat ("mailto:", email, NULL);
	e_meeting_attendee_set_address (attendee, mailto);
	name = dup_identity_name (email);
	if (name && *name)
		e_meeting_attendee_set_cn (attendee, name);
	e_meeting_attendee_set_cutype (attendee, I_CAL_CUTYPE_INDIVIDUAL);
	e_meeting_attendee_set_role (attendee, I_CAL_ROLE_REQPARTICIPANT);
	e_meeting_attendee_set_partstat (attendee, I_CAL_PARTSTAT_ACCEPTED);
	e_meeting_attendee_set_rsvp (attendee, FALSE);
	e_meeting_store_add_attendee (store, attendee);
	/* Else the "Invite Others" dialog would take the user away again */
	if (list_view)
		e_meeting_list_view_add_attendee_to_name_selector (E_MEETING_LIST_VIEW (list_view), attendee);
	editor->self = attendee;
	g_object_unref (attendee);
	g_free (mailto);
	g_free (name);
}

/* The item part of a GroupWise ID ("base@4:container" -> "base") */
static gchar *
dup_id_base (const gchar *id)
{
	return id ? g_strndup (id, strcspn (id, "@:")) : NULL;
}

static gboolean
same_x (ICalComponent *one,
	ICalComponent *other,
	const gchar *name)
{
	gchar *a = e_cal_util_component_dup_x_property (one, name);
	gchar *b = e_cal_util_component_dup_x_property (other, name);
	gboolean same = g_strcmp0 (a, b) == 0;

	g_free (a);
	g_free (b);

	return same;
}

static gboolean
same_time (ICalComponent *one,
	   ICalComponent *other,
	   ICalPropertyKind kind)
{
	ICalProperty *a = i_cal_component_get_first_property (one, kind);
	ICalProperty *b = i_cal_component_get_first_property (other, kind);
	gchar *text_a = a ? i_cal_property_as_ical_string (a) : NULL;
	gchar *text_b = b ? i_cal_property_as_ical_string (b) : NULL;
	gboolean same = g_strcmp0 (text_a, text_b) == 0;

	g_clear_object (&a);
	g_clear_object (&b);
	g_free (text_a);
	g_free (text_b);

	return same;
}

/* The own calendars of the account of @target (the Calendar and the own
 * subcalendars; a proxy or shared calendar only itself) */
static GList *
own_calendar_sources (ESourceRegistry *registry,
		      ESource *target)
{
	GList *sources, *link, *own = NULL;
	const gchar *parent = e_source_get_parent (target);
	gchar *role = NULL;

	if (e_source_has_extension (target, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
		role = e_source_groupwise_folder_dup_role (e_source_get_extension (target, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
	if ((role && g_strcmp0 (role, E_GW_SOURCE_ROLE_OWN) != 0) || !parent) {
		g_free (role);
		return g_list_prepend (NULL, g_object_ref (target));
	}
	g_free (role);

	sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_CALENDAR);
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;

		if (g_strcmp0 (e_source_get_parent (source), parent) != 0 ||
		    !e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
			continue;
		role = e_source_groupwise_folder_dup_role (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
		if (!role || g_strcmp0 (role, E_GW_SOURCE_ROLE_OWN) == 0)
			own = g_list_prepend (own, g_object_ref (source));
		g_free (role);
	}
	g_list_free_full (sources, g_object_unref);

	return own;
}

/* Whether @other blocks time @comp would overlap with */
static gboolean
is_conflict (ICalComponent *comp,
	     ICalComponent *other,
	     const gchar *user_email)
{
	gchar *own_base, *travel_of, *other_base;
	gboolean conflict = TRUE;
	ICalTime *start;

	if (g_strcmp0 (i_cal_component_get_uid (comp), i_cal_component_get_uid (other)) == 0 ||
	    i_cal_component_get_status (other) == I_CAL_STATUS_CANCELLED)
		return FALSE;
	{
		ICalProperty *prop = i_cal_component_get_first_property (other, I_CAL_TRANSP_PROPERTY);

		if (prop) {
			ICalPropertyTransp transp = i_cal_property_get_transp (prop);

			conflict = transp != I_CAL_TRANSP_TRANSPARENT && transp != I_CAL_TRANSP_TRANSPARENTNOCONFLICT;
			g_object_unref (prop);
		}
	}
	/* All-day events do not count, as their day is not busy all day */
	start = conflict ? i_cal_component_get_dtstart (other) : NULL;
	if (start && i_cal_time_is_date (start))
		conflict = FALSE;
	g_clear_object (&start);

	/* Its own travel time */
	if (conflict) {
		travel_of = e_cal_util_component_dup_x_property (other, X_TRAVEL_OF);
		other_base = dup_id_base (travel_of);
		if (other_base) {
			gchar *id = e_cal_util_component_dup_x_property (comp, X_ITEM_ID);
			gchar *own_copy = e_cal_util_component_dup_x_property (comp, X_OWN_COPY_ID);

			own_base = dup_id_base (id);
			conflict = g_strcmp0 (own_base, other_base) != 0;
			g_free (own_base);
			own_base = dup_id_base (own_copy);
			if (conflict && own_base)
				conflict = g_strcmp0 (own_base, other_base) != 0;
			g_free (own_base);
			g_free (id);
			g_free (own_copy);
		}
		g_free (other_base);
		g_free (travel_of);
	}

	/* Declined by the user */
	if (conflict && user_email && *user_email) {
		ICalProperty *prop;

		for (prop = i_cal_component_get_first_property (other, I_CAL_ATTENDEE_PROPERTY); prop && conflict;
		     g_object_unref (prop), prop = i_cal_component_get_next_property (other, I_CAL_ATTENDEE_PROPERTY)) {
			const gchar *address = e_cal_util_strip_mailto (i_cal_property_get_attendee (prop));
			ICalParameter *param;

			if (!address || g_ascii_strcasecmp (address, user_email) != 0)
				continue;
			param = i_cal_property_get_first_parameter (prop, I_CAL_PARTSTAT_PARAMETER);
			if (param && i_cal_parameter_get_partstat (param) == I_CAL_PARTSTAT_DECLINED)
				conflict = FALSE;
			g_clear_object (&param);
		}
		g_clear_object (&prop);
	}

	return conflict;
}

static gchar *
describe_conflict (ICalComponent *other,
		   time_t start,
		   time_t end)
{
	GDateTime *from = g_date_time_new_from_unix_local (start);
	GDateTime *to = g_date_time_new_from_unix_local (end);
	gchar *from_text = from ? g_date_time_format (from, "%x %R") : g_strdup ("");
	gchar *to_text = to ? g_date_time_format (to, "%R") : g_strdup ("");
	const gchar *summary = i_cal_component_get_summary (other);
	gchar *text = g_strdup_printf ("%s – %s  %s", from_text, to_text, summary && *summary ? summary : "");

	g_clear_pointer (&from, g_date_time_unref);
	g_clear_pointer (&to, g_date_time_unref);
	g_free (from_text);
	g_free (to_text);

	return text;
}

/* Saved: the time with the travel time must not collide unnoticed with
 * busy time of the user's calendars, as the GroupWise client warns */
static gboolean
travel_editor_fill_component_cb (ECompEditor *comp_editor,
				 ICalComponent *component,
				 gpointer user_data)
{
	ICalComponent *original = e_comp_editor_get_component (comp_editor);
	ECalClient *target = e_comp_editor_get_target_client (comp_editor);
	EShell *shell = e_shell_get_default ();
	ECalComponent *ecomp;
	GString *names = NULL;
	GList *sources, *link;
	time_t start = 0, end = 0;
	gchar *sexp, *iso_start, *iso_end;
	guint count = 0;
	gint response;

	if (e_comp_editor_get_updating (comp_editor) || !shell || !target || !target_is_groupwise (comp_editor) ||
	    i_cal_component_isa (component) != I_CAL_VEVENT_COMPONENT ||
	    e_cal_util_component_has_x_property (component, X_TRAVEL_OF))
		return TRUE;
	{
		ICalProperty *prop = i_cal_component_get_first_property (component, I_CAL_TRANSP_PROPERTY);
		gboolean free_time = prop && i_cal_property_get_transp (prop) == I_CAL_TRANSP_TRANSPARENT;

		g_clear_object (&prop);
		if (free_time)
			return TRUE;
	}
	/* Unchanged times: no question again */
	if (!(e_comp_editor_get_flags (comp_editor) & E_COMP_EDITOR_FLAG_IS_NEW) && original &&
	    same_time (original, component, I_CAL_DTSTART_PROPERTY) && same_time (original, component, I_CAL_DTEND_PROPERTY) &&
	    same_x (original, component, X_TRAVEL_BEFORE) && same_x (original, component, X_TRAVEL_AFTER))
		return TRUE;

	ecomp = e_cal_component_new_from_icalcomponent (i_cal_component_clone (component));
	if (!ecomp)
		return TRUE;
	e_cal_util_get_component_occur_times (ecomp, &start, &end, e_comp_editor_lookup_timezone_cb, comp_editor,
		calendar_config_get_icaltimezone (), I_CAL_VEVENT_COMPONENT);
	g_object_unref (ecomp);
	if (start <= 0 || end <= start)
		return TRUE;
	start -= x_seconds (component, X_TRAVEL_BEFORE);
	end += x_seconds (component, X_TRAVEL_AFTER);

	iso_start = isodate_from_time_t (start);
	iso_end = isodate_from_time_t (end);
	sexp = g_strdup_printf ("(occur-in-time-range? (make-time \"%s\") (make-time \"%s\"))", iso_start, iso_end);
	g_free (iso_start);
	g_free (iso_end);

	sources = own_calendar_sources (e_shell_get_registry (shell), e_client_get_source (E_CLIENT (target)));
	for (link = sources; link; link = g_list_next (link)) {
		EClient *client = e_client_cache_get_client_sync (e_shell_get_client_cache (shell), link->data,
			E_SOURCE_EXTENSION_CALENDAR, 5, NULL, NULL);
		GSList *comps = NULL, *item;

		if (!client)
			continue;
		if (e_cal_client_get_object_list_as_comps_sync (E_CAL_CLIENT (client), sexp, &comps, NULL, NULL)) {
			for (item = comps; item; item = g_slist_next (item)) {
				ICalComponent *other = e_cal_component_get_icalcomponent (item->data);
				time_t other_start = 0, other_end = 0;

				if (!is_conflict (component, other, e_comp_editor_get_cal_email_address (comp_editor)))
					continue;
				e_cal_util_get_component_occur_times (item->data, &other_start, &other_end,
					e_cal_client_tzlookup_cb, client, calendar_config_get_icaltimezone (), I_CAL_VEVENT_COMPONENT);
				if (other_start >= end || other_end <= start)
					continue;
				if (!names)
					names = g_string_new (NULL);
				if (++count <= MAX_NAMED_CONFLICTS) {
					gchar *text = describe_conflict (other, other_start, other_end);

					g_string_append_printf (names, "%s%s", names->len ? "\n" : "", text);
					g_free (text);
				}
			}
			g_slist_free_full (comps, g_object_unref);
		}
		g_object_unref (client);
	}
	g_list_free_full (sources, g_object_unref);
	g_free (sexp);

	if (!names)
		return TRUE;
	if (count > MAX_NAMED_CONFLICTS)
		g_string_append (names, "\n…");

	{
		GtkWidget *dialog = gtk_message_dialog_new (GTK_WINDOW (comp_editor), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s",
			_("The appointment overlaps with busy time in your calendar"));

		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s\n\n%s", names->str,
			_("The preparation and travel time of the appointment counts, too."));
		gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Cancel"), GTK_RESPONSE_CANCEL);
		gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Save Anyway"), GTK_RESPONSE_ACCEPT);
		gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
		response = gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
	}
	g_string_free (names, TRUE);

	return response == GTK_RESPONSE_ACCEPT;
}

/* The GroupWise address books of the account of @target */
static GList *
account_book_sources (ESourceRegistry *registry,
		      ESource *target)
{
	GList *sources, *link, *books = NULL;
	const gchar *parent = e_source_get_parent (target);

	if (!parent)
		return NULL;

	sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;

		if (g_strcmp0 (e_source_get_parent (source), parent) == 0 &&
		    g_strcmp0 (e_source_backend_get_backend_name (e_source_get_extension (source, E_SOURCE_EXTENSION_ADDRESS_BOOK)), "groupwise") == 0)
			books = g_list_prepend (books, g_object_ref (source));
	}
	g_list_free_full (sources, g_object_unref);

	return books;
}

/* Looks a place up among e-mail addresses in the GroupWise address books of
 * the account, one after the other and without holding up the editor (an
 * address book may first have to be opened, which takes its time), and
 * makes it the location of the meeting if that is still empty then */
typedef struct {
	GWeakRef comp_editor;
	EShell *shell;
	gchar *sexp;
	GList *books;	/* ESource, still to ask */
} PlaceLookup;

static void
place_lookup_free (PlaceLookup *lookup)
{
	g_weak_ref_clear (&lookup->comp_editor);
	g_clear_object (&lookup->shell);
	g_list_free_full (lookup->books, g_object_unref);
	g_free (lookup->sexp);
	g_free (lookup);
}

static void place_lookup_next (PlaceLookup *lookup);

static void
place_lookup_contacts_cb (GObject *source_object,
			  GAsyncResult *result,
			  gpointer user_data)
{
	PlaceLookup *lookup = user_data;
	GSList *contacts = NULL, *item;
	gchar *place = NULL;

	if (e_book_client_get_contacts_finish (E_BOOK_CLIENT (source_object), result, &contacts, NULL)) {
		for (item = contacts; item && !place; item = g_slist_next (item)) {
			EVCardAttribute *attr = e_vcard_get_attribute (item->data, "X-GROUPWISE-PLACE");
			gchar *value = attr ? e_vcard_attribute_get_value (attr) : NULL;

			if (g_strcmp0 (value, "1") == 0) {
				const gchar *name = e_contact_get_const (item->data, E_CONTACT_FILE_AS);

				if (!name || !*name)
					name = e_contact_get_const (item->data, E_CONTACT_FULL_NAME);
				place = name && *name ? g_strdup (name) : NULL;
			}
			g_free (value);
		}
		g_slist_free_full (contacts, g_object_unref);
	}

	if (place) {
		ECompEditor *comp_editor = g_weak_ref_get (&lookup->comp_editor);
		ECompEditorPropertyPart *part = comp_editor ? e_comp_editor_get_property_part (comp_editor, I_CAL_LOCATION_PROPERTY) : NULL;
		GtkWidget *entry = part ? e_comp_editor_property_part_get_edit_widget (part) : NULL;

		if (entry && GTK_IS_ENTRY (entry) && !*gtk_entry_get_text (GTK_ENTRY (entry)))
			gtk_entry_set_text (GTK_ENTRY (entry), place);
		g_clear_object (&comp_editor);
		g_free (place);
		place_lookup_free (lookup);
	} else {
		place_lookup_next (lookup);
	}
}

static void
place_lookup_client_cb (GObject *source_object,
			GAsyncResult *result,
			gpointer user_data)
{
	PlaceLookup *lookup = user_data;
	EClient *client = e_client_cache_get_client_finish (E_CLIENT_CACHE (source_object), result, NULL);

	if (client) {
		e_book_client_get_contacts (E_BOOK_CLIENT (client), lookup->sexp, NULL, place_lookup_contacts_cb, lookup);
		g_object_unref (client);
	} else {
		place_lookup_next (lookup);
	}
}

static void
place_lookup_next (PlaceLookup *lookup)
{
	ECompEditor *comp_editor = g_weak_ref_get (&lookup->comp_editor);
	ESource *book;

	/* Nothing found, or the editor is closed */
	if (!lookup->books || !comp_editor) {
		g_clear_object (&comp_editor);
		place_lookup_free (lookup);
		return;
	}
	g_object_unref (comp_editor);

	book = lookup->books->data;
	lookup->books = g_list_delete_link (lookup->books, lookup->books);
	e_client_cache_get_client (e_shell_get_client_cache (lookup->shell), book, E_SOURCE_EXTENSION_ADDRESS_BOOK, 5, NULL,
		place_lookup_client_cb, lookup);
	g_object_unref (book);
}

static void
lookup_place (ECompEditor *comp_editor,
	      EShell *shell,
	      ESource *target,
	      GPtrArray *emails)
{
	EBookQuery **tests = g_new0 (EBookQuery *, emails->len);
	EBookQuery *query;
	PlaceLookup *lookup;
	guint ii;

	for (ii = 0; ii < emails->len; ii++)
		tests[ii] = e_book_query_field_test (E_CONTACT_EMAIL, E_BOOK_QUERY_IS, emails->pdata[ii]);
	query = e_book_query_or (emails->len, tests, TRUE);
	g_free (tests);

	lookup = g_new0 (PlaceLookup, 1);
	g_weak_ref_init (&lookup->comp_editor, comp_editor);
	lookup->shell = g_object_ref (shell);
	lookup->sexp = e_book_query_to_string (query);
	lookup->books = account_book_sources (e_shell_get_registry (shell), target);
	e_book_query_unref (query);

	place_lookup_next (lookup);
}

/* Attendees came along (the attendee dialog closed): a place among them
 * is the location of a meeting without one */
static gboolean
travel_editor_check_places_cb (gpointer user_data)
{
	EGroupwiseTravelEditor *editor = user_data;
	ECompEditor *comp_editor = E_COMP_EDITOR (e_extension_get_extensible (E_EXTENSION (editor)));
	ECompEditorPage *page = e_comp_editor_get_page (comp_editor, E_TYPE_COMP_EDITOR_PAGE_GENERAL);
	ECompEditorPropertyPart *part = e_comp_editor_get_property_part (comp_editor, I_CAL_LOCATION_PROPERTY);
	GtkWidget *entry = part ? e_comp_editor_property_part_get_edit_widget (part) : NULL;
	ECalClient *target = e_comp_editor_get_target_client (comp_editor);
	EShell *shell = e_shell_get_default ();
	const gchar *own_address = e_comp_editor_get_cal_email_address (comp_editor);
	const GPtrArray *attendees;
	GPtrArray *emails;
	guint ii;

	editor->places_idle_id = 0;
	if (editor->as_opened && page) {
		attendees = e_meeting_store_get_attendees (e_comp_editor_page_general_get_meeting_store (E_COMP_EDITOR_PAGE_GENERAL (page)));
		for (ii = 0; attendees && ii < attendees->len; ii++) {
			const gchar *address = e_cal_util_strip_mailto (e_meeting_attendee_get_address (g_ptr_array_index (attendees, ii)));

			if (address && *address)
				g_hash_table_add (editor->checked, g_ascii_strdown (address, -1));
		}
		editor->as_opened = FALSE;
		return G_SOURCE_REMOVE;
	}
	if (!page || !GTK_IS_ENTRY (entry) || !target || !shell || !target_is_groupwise (comp_editor) ||
	    *gtk_entry_get_text (GTK_ENTRY (entry)))
		return G_SOURCE_REMOVE;

	emails = g_ptr_array_new_with_free_func (g_free);
	attendees = e_meeting_store_get_attendees (e_comp_editor_page_general_get_meeting_store (E_COMP_EDITOR_PAGE_GENERAL (page)));
	for (ii = 0; attendees && ii < attendees->len; ii++) {
		const gchar *address = e_cal_util_strip_mailto (e_meeting_attendee_get_address (g_ptr_array_index (attendees, ii)));
		gchar *key;

		if (!address || !*address)
			continue;
		key = g_ascii_strdown (address, -1);
		/* The user is no place (a new meeting starts with its organizer) */
		if (own_address && g_ascii_strcasecmp (own_address, key) == 0)
			g_hash_table_add (editor->checked, g_strdup (key));
		if (g_hash_table_contains (editor->checked, key)) {
			g_free (key);
			continue;
		}
		g_hash_table_add (editor->checked, g_strdup (key));
		g_ptr_array_add (emails, key);
	}
	if (emails->len)
		lookup_place (comp_editor, shell, e_client_get_source (E_CLIENT (target)), emails);
	g_ptr_array_unref (emails);

	return G_SOURCE_REMOVE;
}

static void
travel_editor_attendees_changed_cb (EGroupwiseTravelEditor *editor)
{
	ECompEditor *comp_editor = E_COMP_EDITOR (e_extension_get_extensible (E_EXTENSION (editor)));

	if (e_comp_editor_get_updating (comp_editor))
		editor->as_opened = TRUE;
	if (!editor->places_idle_id)
		editor->places_idle_id = g_idle_add (travel_editor_check_places_cb, editor);
}

static void
travel_editor_constructed (GObject *object)
{
	ECompEditor *comp_editor = E_COMP_EDITOR (e_extension_get_extensible (E_EXTENSION (object)));
	ECompEditorPage *general;
	GtkWidget *page, *notebook, *box, *label;
	GdkPixbuf *car;
	const gchar *title = _("_Travel Time");

	G_OBJECT_CLASS (e_groupwise_travel_editor_parent_class)->constructed (object);

	/* The owner the backend names as organizer of an appointment of a
	 * proxy calendar (for Evolution's tooltip): no meeting to edit */
	{
		ICalComponent *component = e_comp_editor_get_component (comp_editor);
		ICalProperty *prop = component ? i_cal_component_get_first_property (component, I_CAL_ORGANIZER_PROPERTY) : NULL;
		gchar *owner = prop ? i_cal_property_get_parameter_as_string (prop, "X-GW-OWNER") : NULL;

		if (owner) {
			ECompEditorPage *general_page = e_comp_editor_get_page (comp_editor, E_TYPE_COMP_EDITOR_PAGE_GENERAL);

			i_cal_component_remove_property (component, prop);
			if (general_page && !e_cal_util_component_has_attendee (component))
				e_comp_editor_page_general_set_show_attendees (E_COMP_EDITOR_PAGE_GENERAL (general_page), FALSE);
		}
		g_free (owner);
		g_clear_object (&prop);
	}

	page = g_object_new (E_TYPE_GROUPWISE_TRAVEL_PAGE, "editor", comp_editor, NULL);
	e_comp_editor_add_page (comp_editor, title, E_COMP_EDITOR_PAGE (page));

	/* The tab with the red car, as the button of the GroupWise client */
	notebook = gtk_widget_get_parent (page);
	car = car_pixbuf ();
	if (GTK_IS_NOTEBOOK (notebook) && car) {
		box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
		gtk_box_pack_start (GTK_BOX (box), gtk_image_new_from_pixbuf (car), FALSE, FALSE, 0);
		label = gtk_label_new_with_mnemonic (title);
		gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
		gtk_widget_show_all (box);
		gtk_notebook_set_tab_label (GTK_NOTEBOOK (notebook), page, box);
	}
	g_clear_object (&car);

	g_signal_connect_object (comp_editor, "notify::target-client",
		G_CALLBACK (travel_editor_target_client_cb), page, 0);
	travel_page_update_visible (E_GROUPWISE_TRAVEL_PAGE (page));

	general = e_comp_editor_get_page (comp_editor, E_TYPE_COMP_EDITOR_PAGE_GENERAL);
	if (general) {
		EMeetingStore *store = e_comp_editor_page_general_get_meeting_store (E_COMP_EDITOR_PAGE_GENERAL (general));

		g_signal_connect_object (general, "notify::show-attendees",
			G_CALLBACK (travel_editor_ensure_self), object, G_CONNECT_SWAPPED);
		g_signal_connect_object (store, "row-inserted",
			G_CALLBACK (travel_editor_attendees_changed_cb), object, G_CONNECT_SWAPPED);
		g_signal_connect_object (store, "row-changed",
			G_CALLBACK (travel_editor_attendees_changed_cb), object, G_CONNECT_SWAPPED);
	}
	g_signal_connect_object (comp_editor, "notify::cal-email-address",
		G_CALLBACK (travel_editor_ensure_self), object, G_CONNECT_SWAPPED);
	g_signal_connect_object (comp_editor, "notify::target-client",
		G_CALLBACK (travel_editor_ensure_self), object, G_CONNECT_SWAPPED);
	g_signal_connect_object (comp_editor, "fill-component",
		G_CALLBACK (travel_editor_fill_component_cb), object, 0);
}

static void
travel_editor_dispose (GObject *object)
{
	EGroupwiseTravelEditor *editor = (EGroupwiseTravelEditor *) object;

	if (editor->places_idle_id) {
		g_source_remove (editor->places_idle_id);
		editor->places_idle_id = 0;
	}

	G_OBJECT_CLASS (e_groupwise_travel_editor_parent_class)->dispose (object);
}

static void
travel_editor_finalize (GObject *object)
{
	EGroupwiseTravelEditor *editor = (EGroupwiseTravelEditor *) object;

	g_hash_table_destroy (editor->checked);

	G_OBJECT_CLASS (e_groupwise_travel_editor_parent_class)->finalize (object);
}

static void
e_groupwise_travel_editor_class_init (EGroupwiseTravelEditorClass *class)
{
	G_OBJECT_CLASS (class)->constructed = travel_editor_constructed;
	G_OBJECT_CLASS (class)->dispose = travel_editor_dispose;
	G_OBJECT_CLASS (class)->finalize = travel_editor_finalize;
	class->extensible_type = E_TYPE_COMP_EDITOR_EVENT;
}

static void
e_groupwise_travel_editor_class_finalize (EGroupwiseTravelEditorClass *class)
{
}

static void
e_groupwise_travel_editor_init (EGroupwiseTravelEditor *extension)
{
	extension->checked = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

void
e_groupwise_travel_page_type_register (GTypeModule *type_module)
{
	/* Its extension of the sources is read here */
	g_type_class_unref (g_type_class_ref (E_TYPE_SOURCE_GROUPWISE_FOLDER));
	e_groupwise_travel_page_register_type (type_module);
	e_groupwise_travel_editor_register_type (type_module);
}

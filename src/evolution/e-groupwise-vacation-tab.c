/*
 * e-groupwise-vacation-tab.c: the out of office rule in the GroupWise settings
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
 * The out of office rule as in the GroupWise client (Tools > Out of Office
 * Rule): switched on, the subject and text of the reply, a date range, and
 * a reply of its own to external senders.
 */

#include <stdio.h>
#include <time.h>

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>

#include "e-gw-vacation.h"

#include "e-groupwise-settings-window.h"

typedef struct {
	GtkWidget *widget;
	GtkWidget *enabled;
	GtkWidget *range, *all_day, *start, *end;
	GtkWidget *subject, *message, *include_message;
	GtkWidget *external, *contacts_only, *everyone, *external_subject, *external_message;
	EGwVacation *loaded;
} VacationTab;

/* ------------------------------------------------------------------ */

static GtkWidget *
text_view (void)
{
	GtkWidget *view = gtk_text_view_new ();
	GtkWidget *scrolled = gtk_scrolled_window_new (NULL, NULL);

	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 4);
	gtk_text_view_set_right_margin (GTK_TEXT_VIEW (view), 4);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_widget_set_size_request (scrolled, -1, 110);
	gtk_container_add (GTK_CONTAINER (scrolled), view);
	g_object_set_data (G_OBJECT (scrolled), "text-view", view);

	return scrolled;
}

static gchar *
dup_text (GtkWidget *scrolled)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer (g_object_get_data (G_OBJECT (scrolled), "text-view"));
	GtkTextIter start, end;

	gtk_text_buffer_get_bounds (buffer, &start, &end);

	return gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
}

static void
set_text (GtkWidget *scrolled,
	  const gchar *text)
{
	gtk_text_buffer_set_text (gtk_text_view_get_buffer (g_object_get_data (G_OBJECT (scrolled), "text-view")),
		text ? text : "", -1);
}

static GtkWidget *
labeled (GtkGrid *grid,
	 gint row,
	 const gchar *mnemonic,
	 GtkWidget *widget)
{
	GtkWidget *label = gtk_label_new_with_mnemonic (mnemonic);

	gtk_label_set_xalign (GTK_LABEL (label), 1.0);
	gtk_widget_set_valign (label, GTK_ALIGN_START);
	gtk_widget_set_margin_top (label, 4);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label),
		g_object_get_data (G_OBJECT (widget), "text-view") ? g_object_get_data (G_OBJECT (widget), "text-view") : widget);
	gtk_grid_attach (grid, label, 0, row, 1, 1);
	gtk_widget_set_hexpand (widget, TRUE);
	gtk_grid_attach (grid, widget, 1, row, 1, 1);

	return widget;
}

static void
all_day_toggled_cb (GtkToggleButton *button,
		    VacationTab *tab)
{
	gboolean times = !gtk_toggle_button_get_active (button);

	e_date_edit_set_show_time (E_DATE_EDIT (tab->start), times);
	e_date_edit_set_show_time (E_DATE_EDIT (tab->end), times);
}

static gpointer
vacation_new_tab (void)
{
	VacationTab *tab = g_new0 (VacationTab, 1);
	GtkWidget *box, *notebook, *grid, *row, *label, *page;
	GtkGrid *g;

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	tab->widget = g_object_ref_sink (box);

	label = gtk_label_new (_("GroupWise answers mail with this text while the rule is on and the date range lasts, "
		"each sender once, and shows you as out of office in the calendar for the range."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	tab->enabled = gtk_check_button_new_with_mnemonic (_("_Turn on my out of office rule"));
	gtk_box_pack_start (GTK_BOX (box), tab->enabled, FALSE, FALSE, 0);

	/* The date range */
	tab->range = gtk_check_button_new_with_mnemonic (_("_Only in this date range:"));
	gtk_box_pack_start (GTK_BOX (box), tab->range, FALSE, FALSE, 0);
	grid = gtk_grid_new ();
	g = GTK_GRID (grid);
	gtk_grid_set_row_spacing (g, 6);
	gtk_grid_set_column_spacing (g, 6);
	gtk_widget_set_margin_start (grid, 24);
	tab->start = e_date_edit_new ();
	tab->end = e_date_edit_new ();
	labeled (g, 0, _("_From:"), tab->start);
	labeled (g, 1, _("_To:"), tab->end);
	gtk_widget_set_hexpand (tab->start, FALSE);
	gtk_widget_set_hexpand (tab->end, FALSE);
	tab->all_day = gtk_check_button_new_with_mnemonic (_("_Whole days"));
	gtk_grid_attach (g, tab->all_day, 1, 2, 1, 1);
	gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);
	g_signal_connect (tab->all_day, "toggled", G_CALLBACK (all_day_toggled_cb), tab);
	g_object_bind_property (tab->range, "active", grid, "sensitive", G_BINDING_SYNC_CREATE);

	notebook = gtk_notebook_new ();
	gtk_widget_set_margin_top (notebook, 6);
	gtk_box_pack_start (GTK_BOX (box), notebook, TRUE, TRUE, 0);

	/* The reply */
	grid = gtk_grid_new ();
	g = GTK_GRID (grid);
	gtk_container_set_border_width (GTK_CONTAINER (grid), 6);
	gtk_grid_set_row_spacing (g, 6);
	gtk_grid_set_column_spacing (g, 6);
	tab->subject = labeled (g, 0, _("_Subject:"), gtk_entry_new ());
	tab->message = labeled (g, 1, _("_Message:"), text_view ());
	gtk_widget_set_vexpand (tab->message, TRUE);
	tab->include_message = gtk_check_button_new_with_mnemonic (_("_Include the sender's message in the reply"));
	gtk_grid_attach (g, tab->include_message, 1, 2, 1, 1);
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), grid, gtk_label_new (_("Reply")));

	/* External senders */
	page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (page), 6);
	tab->external = gtk_check_button_new_with_mnemonic (_("Reply to _external users (outside the GroupWise system)"));
	gtk_box_pack_start (GTK_BOX (page), tab->external, FALSE, FALSE, 0);
	grid = gtk_grid_new ();
	g = GTK_GRID (grid);
	gtk_grid_set_row_spacing (g, 6);
	gtk_grid_set_column_spacing (g, 6);
	gtk_widget_set_margin_start (grid, 24);
	row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
	tab->contacts_only = gtk_radio_button_new_with_mnemonic (NULL, _("Only my _contacts"));
	tab->everyone = gtk_radio_button_new_with_mnemonic_from_widget (GTK_RADIO_BUTTON (tab->contacts_only), _("E_veryone"));
	gtk_box_pack_start (GTK_BOX (row), tab->contacts_only, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (row), tab->everyone, FALSE, FALSE, 0);
	labeled (g, 0, _("Reply to:"), row);
	tab->external_subject = labeled (g, 1, _("S_ubject:"), gtk_entry_new ());
	tab->external_message = labeled (g, 2, _("M_essage:"), text_view ());
	gtk_widget_set_vexpand (tab->external_message, TRUE);
	label = gtk_label_new (_("External senders get this subject and text, not the ones of the reply tab."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_grid_attach (g, label, 1, 3, 1, 1);
	gtk_box_pack_start (GTK_BOX (page), grid, TRUE, TRUE, 0);
	g_object_bind_property (tab->external, "active", grid, "sensitive", G_BINDING_SYNC_CREATE);
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), page, gtk_label_new (_("External Users")));

	return tab;
}

static GtkWidget *
vacation_get_widget (gpointer tab)
{
	return ((VacationTab *) tab)->widget;
}

static void
vacation_free_tab (gpointer ptr)
{
	VacationTab *tab = ptr;

	e_gw_vacation_free (tab->loaded);
	g_object_unref (tab->widget);
	g_free (tab);
}

/* ------------------------------------------------------------------ */

static gpointer
vacation_load_sync (EGwConnection *cnc,
		    GCancellable *cancellable,
		    GError **error)
{
	return e_gw_connection_get_vacation_sync (cnc, cancellable, error);
}

static void
set_check (GtkWidget *check,
	   gboolean active)
{
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), active);
}

/* "YYYY-MM-DD" into the date field */
static void
set_day (GtkWidget *date_edit,
	 const gchar *day)
{
	gint year, month, mday;

	if (day && sscanf (day, "%d-%d-%d", &year, &month, &mday) == 3)
		e_date_edit_set_date (E_DATE_EDIT (date_edit), year, month, mday);
}

static void
vacation_fill (gpointer ptr,
	       gpointer data)
{
	VacationTab *tab = ptr;
	EGwVacation *vacation = data;

	tab->loaded = vacation;
	set_check (tab->enabled, vacation->enabled);
	gtk_entry_set_text (GTK_ENTRY (tab->subject), vacation->subject ? vacation->subject : "");
	set_text (tab->message, vacation->message);
	set_check (tab->include_message, vacation->include_sender_message);
	set_check (tab->external, vacation->reply_to_external);
	set_check (vacation->my_contacts_only ? tab->contacts_only : tab->everyone, TRUE);
	gtk_entry_set_text (GTK_ENTRY (tab->external_subject), vacation->external_subject ? vacation->external_subject : "");
	set_text (tab->external_message, vacation->external_message);

	/* Without a range: from today on, as whole days */
	set_check (tab->range, vacation->has_range);
	set_check (tab->all_day, !vacation->has_range || vacation->all_day);
	all_day_toggled_cb (GTK_TOGGLE_BUTTON (tab->all_day), tab);
	if (vacation->has_range && vacation->all_day) {
		set_day (tab->start, vacation->start_day);
		set_day (tab->end, vacation->end_day);
	} else if (vacation->has_range && vacation->start && vacation->end) {
		e_date_edit_set_time (E_DATE_EDIT (tab->start), g_date_time_to_unix (vacation->start));
		e_date_edit_set_time (E_DATE_EDIT (tab->end), g_date_time_to_unix (vacation->end));
	} else {
		e_date_edit_set_time (E_DATE_EDIT (tab->start), time (NULL));
		e_date_edit_set_time (E_DATE_EDIT (tab->end), time (NULL));
	}
}

static gchar *
date_text (GtkWidget *date_edit)
{
	gint year, month, mday;

	if (!e_date_edit_get_date (E_DATE_EDIT (date_edit), &year, &month, &mday))
		return NULL;

	return g_strdup_printf ("%04d-%02d-%02d", year, month, mday);
}

static GDateTime *
date_time (GtkWidget *date_edit)
{
	time_t tt = e_date_edit_get_time (E_DATE_EDIT (date_edit));

	return tt > 0 ? g_date_time_new_from_unix_utc (tt) : NULL;
}

static gboolean
same_text (const gchar *a,
	   const gchar *b)
{
	return g_strcmp0 (a && *a ? a : NULL, b && *b ? b : NULL) == 0;
}

static gboolean
same_time (GDateTime *a,
	   GDateTime *b)
{
	return (!a && !b) || (a && b && g_date_time_equal (a, b));
}

static void
vacation_free_changes (gpointer ptr)
{
	e_gw_vacation_free (ptr);
}

static gpointer
vacation_collect (gpointer ptr)
{
	VacationTab *tab = ptr;
	EGwVacation *vacation = e_gw_vacation_new (), *old = tab->loaded;

#define ACTIVE(w) gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w))
	vacation->enabled = ACTIVE (tab->enabled);
	vacation->subject = g_strdup (gtk_entry_get_text (GTK_ENTRY (tab->subject)));
	vacation->message = dup_text (tab->message);
	vacation->include_sender_message = ACTIVE (tab->include_message);
	vacation->reply_to_external = ACTIVE (tab->external);
	vacation->my_contacts_only = ACTIVE (tab->contacts_only);
	vacation->external_subject = g_strdup (gtk_entry_get_text (GTK_ENTRY (tab->external_subject)));
	vacation->external_message = dup_text (tab->external_message);
	vacation->has_range = ACTIVE (tab->range);
	vacation->all_day = ACTIVE (tab->all_day);
#undef ACTIVE
	if (vacation->has_range && vacation->all_day) {
		vacation->start_day = date_text (tab->start);
		vacation->end_day = date_text (tab->end);
	} else if (vacation->has_range) {
		vacation->start = date_time (tab->start);
		vacation->end = date_time (tab->end);
	}

	/* Unchanged: nothing to write (the server would make the rule anew) */
	if (old && vacation->enabled == old->enabled && same_text (vacation->subject, old->subject) &&
	    same_text (vacation->message, old->message) &&
	    vacation->include_sender_message == old->include_sender_message &&
	    vacation->reply_to_external == old->reply_to_external &&
	    (!vacation->reply_to_external || (vacation->my_contacts_only == old->my_contacts_only &&
	      same_text (vacation->external_subject, old->external_subject) &&
	      same_text (vacation->external_message, old->external_message))) &&
	    vacation->has_range == old->has_range &&
	    (!vacation->has_range || (vacation->all_day == old->all_day &&
	      (vacation->all_day ? same_text (vacation->start_day, old->start_day) && same_text (vacation->end_day, old->end_day) :
	       same_time (vacation->start, old->start) && same_time (vacation->end, old->end))))) {
		e_gw_vacation_free (vacation);
		return NULL;
	}

	return vacation;
}

static gboolean
vacation_apply_sync (gpointer changes,
		     EGwConnection *cnc,
		     GCancellable *cancellable,
		     GError **error)
{
	return e_gw_connection_set_vacation_sync (cnc, changes, cancellable, error);
}

const EGroupwiseSettingsTab *
e_groupwise_vacation_tab (void)
{
	static const EGroupwiseSettingsTab tab = {
		N_("Out of Office"),
		vacation_new_tab,
		vacation_get_widget,
		vacation_load_sync,
		vacation_fill,
		vacation_collect,
		vacation_apply_sync,
		(GDestroyNotify) e_gw_vacation_free,
		vacation_free_changes,
		vacation_free_tab
	};

	return &tab;
}

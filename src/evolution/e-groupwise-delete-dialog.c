/*
 * e-groupwise-delete-dialog.c: the question when an own meeting is deleted
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

/* Deleting a meeting the user set up, Evolution asks whether the attendees
 * are told. For a GroupWise calendar the two answers are those of the
 * GroupWise client: retract the meeting from all mailboxes, or take it out
 * of the user's own calendar only (the attendees keep it). The question is
 * Evolution's own and has no place to hook into; it is recognized when it
 * shows — the one about an own meeting that asks for a deletion reason,
 * which only a calendar that can retract gets — and its second button and
 * its text are put in those words. */

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>

#include "e-groupwise-delete-dialog.h"

static void
find_cb (GtkWidget *widget,
	 gpointer user_data)
{
	gpointer *found = user_data;	/* text view, label with the text looked for, that text */

	if (GTK_IS_TEXT_VIEW (widget))
		found[0] = widget;
	else if (GTK_IS_LABEL (widget) && found[2] && g_strcmp0 (gtk_label_get_text (GTK_LABEL (widget)), found[2]) == 0)
		found[1] = widget;
	else if (GTK_IS_CONTAINER (widget))
		gtk_container_forall (GTK_CONTAINER (widget), find_cb, found);
}

/* "_Delete and Send Notice" -> "Delete and Send Notice" */
static gchar *
dup_plain_label (GtkWidget *button)
{
	const gchar *label = button && GTK_IS_BUTTON (button) ? gtk_button_get_label (GTK_BUTTON (button)) : NULL;
	GString *plain = g_string_new (NULL);

	for (; label && *label; label++) {
		if (*label != '_' || label[1] == '_')
			g_string_append_c (plain, *label);
	}

	return g_string_free (plain, FALSE);
}

static void
adapt (GtkDialog *dialog)
{
	EAlert *alert = e_alert_dialog_get_alert (E_ALERT_DIALOG (dialog));
	const gchar *tag = alert ? e_alert_get_tag (alert) : NULL;
	gpointer found[3] = { NULL, NULL, NULL };
	GtkWidget *only_me, *notice;

	if (g_strcmp0 (tag, "calendar:prompt-delete-meeting-with-notice-organizer") != 0 &&
	    g_strcmp0 (tag, "calendar:prompt-delete-titled-meeting-with-notice-organizer") != 0)
		return;

	found[2] = (gpointer) e_alert_get_secondary_text (alert);
	gtk_container_forall (GTK_CONTAINER (dialog), find_cb, found);
	only_me = gtk_dialog_get_widget_for_response (dialog, GTK_RESPONSE_YES);
	notice = gtk_dialog_get_widget_for_response (dialog, GTK_RESPONSE_APPLY);
	/* With a deletion reason: a calendar that retracts */
	if (!found[0] || !only_me || !GTK_IS_BUTTON (only_me))
		return;

	gtk_button_set_use_underline (GTK_BUTTON (only_me), TRUE);
	gtk_button_set_label (GTK_BUTTON (only_me), _("Delete Only for _Me"));
	if (found[1]) {
		gchar *with_notice = dup_plain_label (notice), *mine = dup_plain_label (only_me), *text;

		/* Translators: the first %s is the label of the button that
		 * retracts the meeting, the second %s that of "Delete Only for Me" */
		text = g_strdup_printf (_("“%s” retracts the meeting from the mailboxes of all attendees. "
			"“%s” takes it out of your calendar only: the attendees keep the meeting, and it stays "
			"in your Sent Items, where it can still be retracted."), with_notice, mine);
		gtk_label_set_text (GTK_LABEL (found[1]), text);
		g_free (text);
		g_free (with_notice);
		g_free (mine);
	}
}

static gboolean
show_hook_cb (GSignalInvocationHint *hint,
	      guint n_param_values,
	      const GValue *param_values,
	      gpointer user_data)
{
	GObject *object = n_param_values ? g_value_peek_pointer (&param_values[0]) : NULL;

	if (object && E_IS_ALERT_DIALOG (object))
		adapt (GTK_DIALOG (object));

	return TRUE;
}

void
e_groupwise_delete_dialog_start (void)
{
	static gboolean started;

	if (started)
		return;
	started = TRUE;

	g_type_class_unref (g_type_class_ref (GTK_TYPE_WIDGET));
	g_signal_add_emission_hook (g_signal_lookup ("show", GTK_TYPE_WIDGET), 0, show_hook_cb, NULL, NULL);
}

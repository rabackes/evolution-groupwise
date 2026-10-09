/*
 * e-groupwise-settings-window.c: the window "GroupWise Settings" of an account
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
 * The settings the GroupWise server keeps for the user (out of office, junk
 * mail handling, the mailboxes the user may open as proxy and who may open
 * the user's), opened from the
 * context menu of the account in the folder list. They apply to every client,
 * except the proxy accounts, which are accounts of this Evolution.
 */

#include <glib/gi18n-lib.h>

#include "e-groupwise-ui-utils.h"

#include "e-groupwise-settings-window.h"

typedef const EGroupwiseSettingsTab *(*TabFunc) (void);

static const TabFunc all_tabs[] = {
	e_groupwise_vacation_tab,
	e_groupwise_rules_tab,
	e_groupwise_junk_tab,
	e_groupwise_signatures_tab,
	e_groupwise_proxy_tab,
	e_groupwise_access_tab
};

typedef struct {
	GtkWidget *window;
	GtkWidget *notebook;
	GtkWidget *status;
	GtkWidget *ok_button;
	ESourceRegistry *registry;
	ESource *account_source;
	GCancellable *cancellable;
	gpointer tabs[G_N_ELEMENTS (all_tabs)];
	gboolean loaded;
} Window;

/* One window per account */
static GHashTable *open_windows;	/* account UID -> Window */

static void
set_status (Window *win,
	    const gchar *text)
{
	gtk_label_set_text (GTK_LABEL (win->status), text ? text : "");
	gtk_widget_set_visible (win->status, text && *text);
}

static void
window_destroyed_cb (GtkWidget *widget,
		     Window *win)
{
	guint ii;

	g_cancellable_cancel (win->cancellable);
	g_hash_table_remove (open_windows, e_source_get_uid (win->account_source));
	for (ii = 0; ii < G_N_ELEMENTS (all_tabs); ii++)
		all_tabs[ii] ()->free_tab (win->tabs[ii]);
	g_object_unref (win->cancellable);
	g_object_unref (win->registry);
	g_object_unref (win->account_source);
	g_free (win);
}

/* What a thread needs: it may outlive the window */
typedef struct {
	ESourceRegistry *registry;
	ESource *account_source;
} Account;

static Account *
account_new (Window *win)
{
	Account *account = g_new0 (Account, 1);

	account->registry = g_object_ref (win->registry);
	account->account_source = g_object_ref (win->account_source);

	return account;
}

static void
account_free (Account *account)
{
	g_object_unref (account->registry);
	g_object_unref (account->account_source);
	g_free (account);
}

/* ------------------------------------------------------------------ */
/* Reading */

static void
load_thread (GTask *task,
	     gpointer source_object,
	     gpointer task_data,
	     GCancellable *cancellable)
{
	Account *account = task_data;
	gpointer *data = g_new0 (gpointer, G_N_ELEMENTS (all_tabs));
	EGwConnection *cnc;
	GError *error = NULL;
	guint ii;

	cnc = e_groupwise_ui_connect_sync (account->registry, account->account_source, cancellable, &error);
	for (ii = 0; cnc && !error && ii < G_N_ELEMENTS (all_tabs); ii++)
		data[ii] = all_tabs[ii] ()->load_sync (cnc, cancellable, &error);
	if (cnc) {
		e_gw_connection_logout_sync (cnc, NULL);
		g_object_unref (cnc);
	}

	if (error) {
		for (ii = 0; ii < G_N_ELEMENTS (all_tabs); ii++) {
			if (data[ii])
				all_tabs[ii] ()->free_data (data[ii]);
		}
		g_free (data);
		g_task_return_error (task, error);
	} else {
		g_task_return_pointer (task, data, g_free);
	}
}

static void
load_done (GObject *source_object,
	   GAsyncResult *result,
	   gpointer user_data)
{
	GError *error = NULL;
	gpointer *data = g_task_propagate_pointer (G_TASK (result), &error);
	Window *win;
	guint ii;

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_clear_error (&error);
		return;
	}

	win = user_data;
	if (!data) {
		gchar *text = g_strdup_printf (_("The settings cannot be read from the server: %s"), error->message);

		set_status (win, text);
		g_free (text);
		g_clear_error (&error);
		return;
	}

	for (ii = 0; ii < G_N_ELEMENTS (all_tabs); ii++)
		all_tabs[ii] ()->fill (win->tabs[ii], data[ii]);
	g_free (data);

	win->loaded = TRUE;
	set_status (win, NULL);
	gtk_widget_set_sensitive (win->notebook, TRUE);
	gtk_widget_set_sensitive (win->ok_button, TRUE);
}

/* ------------------------------------------------------------------ */
/* Writing */

typedef struct {
	Account *account;
	gpointer changes[G_N_ELEMENTS (all_tabs)];
} Changes;

static void
changes_free (Changes *changes)
{
	guint ii;

	account_free (changes->account);
	for (ii = 0; ii < G_N_ELEMENTS (all_tabs); ii++) {
		if (changes->changes[ii])
			all_tabs[ii] ()->free_changes (changes->changes[ii]);
	}
	g_free (changes);
}

static void
apply_thread (GTask *task,
	      gpointer source_object,
	      gpointer task_data,
	      GCancellable *cancellable)
{
	Changes *changes = task_data;
	EGwConnection *cnc;
	GError *error = NULL;
	gboolean success;
	guint ii;

	cnc = e_groupwise_ui_connect_sync (changes->account->registry, changes->account->account_source, cancellable, &error);
	success = cnc != NULL;
	for (ii = 0; success && ii < G_N_ELEMENTS (all_tabs); ii++) {
		if (changes->changes[ii])
			success = all_tabs[ii] ()->apply_sync (changes->changes[ii], cnc, cancellable, &error);
	}
	if (cnc) {
		e_gw_connection_logout_sync (cnc, NULL);
		g_object_unref (cnc);
	}

	if (success)
		g_task_return_boolean (task, TRUE);
	else
		g_task_return_error (task, error);
}

static void
apply_done (GObject *source_object,
	    GAsyncResult *result,
	    gpointer user_data)
{
	GError *error = NULL;
	Window *win;

	if (g_task_propagate_boolean (G_TASK (result), &error)) {
		win = user_data;
		gtk_widget_destroy (win->window);
		return;
	}
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_clear_error (&error);
		return;
	}

	win = user_data;
	{
		gchar *text = g_strdup_printf (_("The settings cannot be saved: %s"), error->message);

		set_status (win, text);
		g_free (text);
	}
	g_clear_error (&error);
	gtk_widget_set_sensitive (win->notebook, TRUE);
	gtk_widget_set_sensitive (win->ok_button, TRUE);
}

static void
ok_clicked_cb (GtkButton *button,
	       Window *win)
{
	Changes *changes;
	gboolean any = FALSE;
	GTask *task;
	guint ii;

	if (!win->loaded) {
		gtk_widget_destroy (win->window);
		return;
	}

	changes = g_new0 (Changes, 1);
	changes->account = account_new (win);
	for (ii = 0; ii < G_N_ELEMENTS (all_tabs); ii++) {
		changes->changes[ii] = all_tabs[ii] ()->collect (win->tabs[ii]);
		any = any || changes->changes[ii];
	}
	if (!any) {
		changes_free (changes);
		gtk_widget_destroy (win->window);
		return;
	}

	set_status (win, _("Saving the settings on the server…"));
	gtk_widget_set_sensitive (win->notebook, FALSE);
	gtk_widget_set_sensitive (win->ok_button, FALSE);

	task = g_task_new (NULL, win->cancellable, apply_done, win);
	g_task_set_task_data (task, changes, (GDestroyNotify) changes_free);
	g_task_run_in_thread (task, apply_thread);
	g_object_unref (task);
}

/* ------------------------------------------------------------------ */

static void
show_tab (Window *win,
	  const EGroupwiseSettingsTab *tab)
{
	guint ii;

	for (ii = 0; tab && ii < G_N_ELEMENTS (all_tabs); ii++) {
		if (all_tabs[ii] () == tab)
			gtk_notebook_set_current_page (GTK_NOTEBOOK (win->notebook), ii);
	}
}

void
e_groupwise_settings_window_show (GtkWindow *parent,
				  ESourceRegistry *registry,
				  ESource *account_source,
				  const EGroupwiseSettingsTab *tab)
{
	GtkWidget *box, *buttons, *button;
	Window *win;
	GTask *task;
	gchar *title;
	guint ii;

	g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));
	g_return_if_fail (E_IS_SOURCE (account_source));

	if (!open_windows)
		open_windows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	win = g_hash_table_lookup (open_windows, e_source_get_uid (account_source));
	if (win) {
		show_tab (win, tab);
		gtk_window_present (GTK_WINDOW (win->window));
		return;
	}

	win = g_new0 (Window, 1);
	win->registry = g_object_ref (registry);
	win->account_source = g_object_ref (account_source);
	win->cancellable = g_cancellable_new ();
	g_hash_table_insert (open_windows, g_strdup (e_source_get_uid (account_source)), win);

	win->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	/* Translators: the title of the settings window; %s is the name of the account */
	title = g_strdup_printf (_("GroupWise Settings – %s"), e_source_get_display_name (account_source));
	gtk_window_set_title (GTK_WINDOW (win->window), title);
	g_free (title);
	gtk_window_set_default_size (GTK_WINDOW (win->window), 620, 560);
	if (parent) {
		gtk_window_set_transient_for (GTK_WINDOW (win->window), parent);
		gtk_window_set_position (GTK_WINDOW (win->window), GTK_WIN_POS_CENTER_ON_PARENT);
	}
	g_signal_connect (win->window, "destroy", G_CALLBACK (window_destroyed_cb), win);

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_container_add (GTK_CONTAINER (win->window), box);

	win->status = gtk_label_new (NULL);
	gtk_label_set_line_wrap (GTK_LABEL (win->status), TRUE);
	gtk_label_set_xalign (GTK_LABEL (win->status), 0.0);
	gtk_box_pack_start (GTK_BOX (box), win->status, FALSE, FALSE, 0);

	win->notebook = gtk_notebook_new ();
	gtk_widget_set_sensitive (win->notebook, FALSE);
	for (ii = 0; ii < G_N_ELEMENTS (all_tabs); ii++) {
		const EGroupwiseSettingsTab *tab = all_tabs[ii] ();

		win->tabs[ii] = tab->new_tab ();
		if (tab->set_account)
			tab->set_account (win->tabs[ii], registry, account_source);
		gtk_notebook_append_page (GTK_NOTEBOOK (win->notebook), tab->get_widget (win->tabs[ii]),
			gtk_label_new (_(tab->title)));
	}
	gtk_box_pack_start (GTK_BOX (box), win->notebook, TRUE, TRUE, 0);

	buttons = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_button_box_set_layout (GTK_BUTTON_BOX (buttons), GTK_BUTTONBOX_END);
	gtk_box_set_spacing (GTK_BOX (buttons), 6);
	button = gtk_button_new_with_mnemonic (_("_Cancel"));
	g_signal_connect_swapped (button, "clicked", G_CALLBACK (gtk_widget_destroy), win->window);
	gtk_container_add (GTK_CONTAINER (buttons), button);
	win->ok_button = gtk_button_new_with_mnemonic (_("_OK"));
	gtk_widget_set_sensitive (win->ok_button, FALSE);
	g_signal_connect (win->ok_button, "clicked", G_CALLBACK (ok_clicked_cb), win);
	gtk_container_add (GTK_CONTAINER (buttons), win->ok_button);
	gtk_box_pack_start (GTK_BOX (box), buttons, FALSE, FALSE, 0);

	gtk_widget_show_all (win->window);
	show_tab (win, tab);
	set_status (win, _("Reading the settings from the server…"));

	task = g_task_new (NULL, win->cancellable, load_done, win);
	g_task_set_task_data (task, account_new (win), (GDestroyNotify) account_free);
	g_task_run_in_thread (task, load_thread);
	g_object_unref (task);
}

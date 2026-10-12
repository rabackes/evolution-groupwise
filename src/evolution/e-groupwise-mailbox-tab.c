/*
 * e-groupwise-mailbox-tab.c: the mailbox in figures, and its password
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
 * What the GroupWise client shows under Tools > Check Mailbox Size, as far
 * as the server tells: the space the mailbox takes and its limit, what the
 * folders hold (the server counts the Mailbox, the Calendar, the Work In
 * Progress folder and the folders the user made; not the Sent Items and
 * other views), the calendars and the address books. And Tools > Options >
 * Security > Password: the password of the mailbox.
 *
 * The page changes nothing with OK: the password is changed in its dialog.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>

#include "e-gw-addressbook.h"
#include "e-gw-backend-utils.h"
#include "e-gw-folder.h"
#include "e-gw-mailbox.h"

#include "e-groupwise-settings-window.h"
#include "e-groupwise-ui-utils.h"

enum {
	COL_NAME,
	COL_COUNT,
	COL_UNREAD,
	COL_UNREAD_TEXT,
	N_COLS
};

typedef struct {
	EGwDiskUsage usage;
	gboolean has_usage;
	GPtrArray *folders;	/* EGwFolder */
	guint n_books;
} MailboxData;

typedef struct {
	GtkWidget *widget;
	GtkWidget *usage_bar, *usage_label;
	GtkWidget *figures[6];
	GtkListStore *model;
	GtkWidget *password_button;
	ESourceRegistry *registry;
	ESource *account;
} MailboxTab;

static void
mailbox_data_free (gpointer ptr)
{
	MailboxData *data = ptr;

	if (!data)
		return;
	g_clear_pointer (&data->folders, g_ptr_array_unref);
	g_free (data);
}

/* ------------------------------------------------------------------ */
/* The password */

typedef struct {
	ESourceRegistry *registry;
	ESource *account;
	gchar *old_password, *new_password;
	GtkWidget *dialog, *message;
} PasswordData;

static void
password_data_free (gpointer ptr)
{
	PasswordData *data = ptr;

	g_clear_object (&data->registry);
	g_clear_object (&data->account);
	e_util_safe_free_string (data->old_password);
	e_util_safe_free_string (data->new_password);
	g_free (data);
}

/* The accounts of the login keep its password each: the user's own and
 * the proxy accounts. Evolution's keyring and the running services get the
 * new one. */
static void
store_password (PasswordData *data,
		GCancellable *cancellable)
{
	ESource *own = e_source_registry_find_extension (data->registry, data->account, E_SOURCE_EXTENSION_COLLECTION);
	ESourceAuthentication *own_auth;
	ENamedParameters *credentials;
	GList *collections, *link;

	if (!own)
		own = g_object_ref (data->account);
	own_auth = e_source_get_extension (own, E_SOURCE_EXTENSION_AUTHENTICATION);
	credentials = e_named_parameters_new ();
	e_named_parameters_set (credentials, E_SOURCE_CREDENTIAL_PASSWORD, data->new_password);

	collections = e_source_registry_list_sources (data->registry, E_SOURCE_EXTENSION_COLLECTION);
	/* (an account without collection keeps it itself) */
	if (!g_list_find (collections, own))
		collections = g_list_prepend (collections, g_object_ref (own));
	for (link = collections; link; link = g_list_next (link)) {
		ESource *source = link->data;
		GError *error = NULL;

		if (source != own) {
			ESourceAuthentication *auth;

			if (!e_source_has_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION) ||
			    g_strcmp0 (e_source_backend_get_backend_name (e_source_get_extension (source, E_SOURCE_EXTENSION_COLLECTION)), "groupwise") != 0)
				continue;
			auth = e_source_get_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION);
			if (g_ascii_strcasecmp (e_source_authentication_get_host (auth) ? e_source_authentication_get_host (auth) : "",
						e_source_authentication_get_host (own_auth) ? e_source_authentication_get_host (own_auth) : "") != 0 ||
			    g_ascii_strcasecmp (e_source_authentication_get_user (auth) ? e_source_authentication_get_user (auth) : "",
						e_source_authentication_get_user (own_auth) ? e_source_authentication_get_user (own_auth) : "") != 0)
				continue;
		}

		if (!e_source_store_password_sync (source, data->new_password, TRUE, cancellable, &error) ||
		    !e_source_invoke_authenticate_sync (source, credentials, cancellable, &error)) {
			g_warning ("GroupWise: the new password is not kept for %s: %s", e_source_get_uid (source),
				error ? error->message : "?");
			g_clear_error (&error);
		}
	}
	g_list_free_full (collections, g_object_unref);
	e_named_parameters_free (credentials);
	g_object_unref (own);
}

static void
password_thread (GTask *task,
		 gpointer source_object,
		 gpointer task_data,
		 GCancellable *cancellable)
{
	PasswordData *data = task_data;
	EGwConnection *cnc;
	GError *error = NULL;
	gboolean success = FALSE;

	cnc = e_groupwise_ui_connect_sync (data->registry, data->account, cancellable, &error);
	if (cnc) {
		success = e_gw_connection_modify_password_sync (cnc, data->old_password, data->new_password, cancellable, &error);
		e_gw_connection_logout_sync (cnc, NULL);
		g_object_unref (cnc);
	}
	if (success) {
		store_password (data, cancellable);
		g_task_return_boolean (task, TRUE);
	} else if (g_error_matches (error, E_GW_ERROR, E_GW_ERROR_WRONG_PASSWORD)) {
		g_clear_error (&error);
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s",
			_("The old password is not right."));
	} else {
		g_task_return_error (task, error);
	}
}

static void
password_done_cb (GObject *source_object,
		  GAsyncResult *result,
		  gpointer user_data)
{
	PasswordData *data = g_task_get_task_data (G_TASK (result));
	GError *error = NULL;

	if (!data->dialog) {
		g_task_propagate_boolean (G_TASK (result), NULL);
		return;
	}

	if (g_task_propagate_boolean (G_TASK (result), &error)) {
		GtkWidget *done = gtk_message_dialog_new (gtk_window_get_transient_for (GTK_WINDOW (data->dialog)),
			GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
			"%s", _("The password of the mailbox is changed"));

		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (done), "%s",
			_("Evolution remembers the new password. Other programs and devices using this mailbox need it as well."));
		g_signal_connect (done, "response", G_CALLBACK (gtk_widget_destroy), NULL);
		gtk_widget_destroy (data->dialog);
		gtk_widget_show (done);
	} else {
		gchar *text = g_strdup_printf (_("The password was not changed: %s"), error->message);

		gtk_label_set_text (GTK_LABEL (data->message), text);
		gtk_widget_show (data->message);
		gtk_widget_set_sensitive (data->dialog, TRUE);
		g_free (text);
		g_clear_error (&error);
	}
}

static void
password_dialog_destroy_cb (GtkWidget *dialog,
			    PasswordData *data)
{
	data->dialog = NULL;
}

static GtkWidget *
password_entry (GtkGrid *grid,
		gint row,
		const gchar *text)
{
	GtkWidget *label = gtk_label_new_with_mnemonic (text), *entry = gtk_entry_new ();

	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
	gtk_entry_set_input_purpose (GTK_ENTRY (entry), GTK_INPUT_PURPOSE_PASSWORD);
	gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
	gtk_entry_set_width_chars (GTK_ENTRY (entry), 28);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
	gtk_grid_attach (grid, label, 0, row, 1, 1);
	gtk_grid_attach (grid, entry, 1, row, 1, 1);

	return entry;
}

typedef struct {
	MailboxTab *tab;
	GtkWidget *dialog, *old_entry, *new_entry, *again_entry, *message;
} PasswordDialog;

static void
password_response_cb (GtkDialog *dialog,
		      gint response,
		      PasswordDialog *pd)
{
	const gchar *old_password, *new_password, *again;
	PasswordData *data;
	GTask *task;

	if (response != GTK_RESPONSE_OK) {
		gtk_widget_destroy (GTK_WIDGET (dialog));
		return;
	}

	old_password = gtk_entry_get_text (GTK_ENTRY (pd->old_entry));
	new_password = gtk_entry_get_text (GTK_ENTRY (pd->new_entry));
	again = gtk_entry_get_text (GTK_ENTRY (pd->again_entry));
	if (!*new_password || strcmp (new_password, again) != 0) {
		gtk_label_set_text (GTK_LABEL (pd->message), *new_password ?
			_("The new password and its repetition differ.") : _("Type the new password."));
		gtk_widget_show (pd->message);
		return;
	}

	data = g_new0 (PasswordData, 1);
	data->registry = g_object_ref (pd->tab->registry);
	data->account = g_object_ref (pd->tab->account);
	data->old_password = g_strdup (old_password);
	data->new_password = g_strdup (new_password);
	data->dialog = GTK_WIDGET (dialog);
	data->message = pd->message;
	g_signal_connect (dialog, "destroy", G_CALLBACK (password_dialog_destroy_cb), data);

	gtk_widget_hide (pd->message);
	gtk_widget_set_sensitive (GTK_WIDGET (dialog), FALSE);
	task = g_task_new (NULL, NULL, password_done_cb, NULL);
	g_task_set_task_data (task, data, password_data_free);
	g_task_run_in_thread (task, password_thread);
	g_object_unref (task);
}

static void
password_clicked_cb (GtkButton *button,
		     MailboxTab *tab)
{
	GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (button));
	GtkWidget *dialog, *box, *grid, *label;
	PasswordDialog *pd;

	if (!tab->registry || !tab->account)
		return;

	dialog = gtk_dialog_new_with_buttons (_("Change the Password of the Mailbox"),
		GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		_("_Cancel"), GTK_RESPONSE_CANCEL, _("C_hange"), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
	box = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_box_set_spacing (GTK_BOX (box), 12);

	label = gtk_label_new (_("The password you log in to GroupWise with. Where your organization keeps the "
		"passwords in a directory, change it there; GroupWise then refuses the change here."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_label_set_max_width_chars (GTK_LABEL (label), 55);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);

	pd = g_new0 (PasswordDialog, 1);
	pd->tab = tab;
	pd->dialog = dialog;
	pd->old_entry = password_entry (GTK_GRID (grid), 0, _("_Old password:"));
	pd->new_entry = password_entry (GTK_GRID (grid), 1, _("_New password:"));
	pd->again_entry = password_entry (GTK_GRID (grid), 2, _("New password _again:"));

	pd->message = gtk_label_new (NULL);
	gtk_label_set_line_wrap (GTK_LABEL (pd->message), TRUE);
	gtk_label_set_xalign (GTK_LABEL (pd->message), 0.0);
	gtk_label_set_max_width_chars (GTK_LABEL (pd->message), 55);
	gtk_widget_set_no_show_all (pd->message, TRUE);
	gtk_box_pack_start (GTK_BOX (box), pd->message, FALSE, FALSE, 0);

	g_object_set_data_full (G_OBJECT (dialog), "groupwise-password-dialog", pd, g_free);
	g_signal_connect (dialog, "response", G_CALLBACK (password_response_cb), pd);
	gtk_widget_show_all (dialog);
}

/* ------------------------------------------------------------------ */
/* The page */

static GtkWidget *
section_label (const gchar *text)
{
	GtkWidget *label = gtk_label_new (NULL);
	gchar *markup = g_markup_printf_escaped ("<b>%s</b>", text);

	gtk_label_set_markup (GTK_LABEL (label), markup);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	g_free (markup);

	return label;
}

static gpointer
mailbox_new_tab (void)
{
	static const gchar *figure_names[] = {
		N_("Messages in the folders:"),
		N_("Unread in the Mailbox:"),
		N_("Entries in the Calendar:"),
		N_("Folders:"),
		N_("Calendars:"),
		N_("Address books:")
	};
	MailboxTab *tab = g_new0 (MailboxTab, 1);
	GtkWidget *box, *grid, *label, *scrolled, *view;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	guint ii;

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	tab->widget = g_object_ref_sink (box);

	gtk_box_pack_start (GTK_BOX (box), section_label (_("Space")), FALSE, FALSE, 0);
	tab->usage_bar = gtk_level_bar_new_for_interval (0.0, 1.0);
	gtk_widget_set_margin_start (tab->usage_bar, 12);
	gtk_box_pack_start (GTK_BOX (box), tab->usage_bar, FALSE, FALSE, 0);
	tab->usage_label = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (tab->usage_label), 0.0);
	gtk_label_set_line_wrap (GTK_LABEL (tab->usage_label), TRUE);
	gtk_widget_set_margin_start (tab->usage_label, 12);
	gtk_box_pack_start (GTK_BOX (box), tab->usage_label, FALSE, FALSE, 0);

	label = section_label (_("Content"));
	gtk_widget_set_margin_top (label, 12);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 3);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_widget_set_margin_start (grid, 12);
	gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);
	for (ii = 0; ii < G_N_ELEMENTS (figure_names); ii++) {
		label = gtk_label_new (_(figure_names[ii]));
		gtk_label_set_xalign (GTK_LABEL (label), 0.0);
		gtk_grid_attach (GTK_GRID (grid), label, 0, ii, 1, 1);
		tab->figures[ii] = gtk_label_new (NULL);
		gtk_label_set_xalign (GTK_LABEL (tab->figures[ii]), 1.0);
		gtk_label_set_selectable (GTK_LABEL (tab->figures[ii]), TRUE);
		gtk_grid_attach (GTK_GRID (grid), tab->figures[ii], 1, ii, 1, 1);
	}

	/* The folders the server counts, the fullest first */
	tab->model = gtk_list_store_new (N_COLS, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_INT64, G_TYPE_STRING);
	view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (tab->model));
	gtk_tree_view_set_enable_search (GTK_TREE_VIEW (view), TRUE);
	gtk_tree_view_set_search_column (GTK_TREE_VIEW (view), COL_NAME);

	renderer = gtk_cell_renderer_text_new ();
	g_object_set (renderer, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
	column = gtk_tree_view_column_new_with_attributes (_("Folder"), renderer, "text", COL_NAME, NULL);
	gtk_tree_view_column_set_expand (column, TRUE);
	gtk_tree_view_column_set_sort_column_id (column, COL_NAME);
	gtk_tree_view_append_column (GTK_TREE_VIEW (view), column);

	renderer = gtk_cell_renderer_text_new ();
	g_object_set (renderer, "xalign", 1.0, NULL);
	column = gtk_tree_view_column_new_with_attributes (_("Messages"), renderer, "text", COL_COUNT, NULL);
	gtk_tree_view_column_set_sort_column_id (column, COL_COUNT);
	gtk_tree_view_append_column (GTK_TREE_VIEW (view), column);

	renderer = gtk_cell_renderer_text_new ();
	g_object_set (renderer, "xalign", 1.0, NULL);
	column = gtk_tree_view_column_new_with_attributes (_("Unread"), renderer, "text", COL_UNREAD_TEXT, NULL);
	gtk_tree_view_column_set_sort_column_id (column, COL_UNREAD);
	gtk_tree_view_append_column (GTK_TREE_VIEW (view), column);

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request (scrolled, -1, 150);
	gtk_widget_set_margin_start (scrolled, 12);
	gtk_widget_set_margin_top (scrolled, 6);
	gtk_container_add (GTK_CONTAINER (scrolled), view);
	gtk_box_pack_start (GTK_BOX (box), scrolled, TRUE, TRUE, 0);

	label = gtk_label_new (_("GroupWise counts the Mailbox, the Calendar, the Work In Progress folder and the folders "
		"you made; the Sent Items and other views onto the mailbox it does not count. Unread messages it "
		"counts in the Mailbox only."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_widget_set_margin_start (label, 12);
	gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	tab->password_button = gtk_button_new_with_mnemonic (_("Change _Password…"));
	gtk_widget_set_tooltip_text (tab->password_button, _("Change the password you log in to GroupWise with"));
	gtk_widget_set_halign (tab->password_button, GTK_ALIGN_START);
	gtk_widget_set_margin_top (tab->password_button, 12);
	gtk_widget_set_no_show_all (tab->password_button, TRUE);
	g_signal_connect (tab->password_button, "clicked", G_CALLBACK (password_clicked_cb), tab);
	gtk_box_pack_start (GTK_BOX (box), tab->password_button, FALSE, FALSE, 0);

	return tab;
}

static GtkWidget *
mailbox_get_widget (gpointer tab)
{
	return ((MailboxTab *) tab)->widget;
}

static void
mailbox_set_account (gpointer ptr,
		     ESourceRegistry *registry,
		     ESource *account_source)
{
	MailboxTab *tab = ptr;
	CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (registry, account_source);
	gchar *proxy = settings ? camel_groupwise_settings_dup_proxy (settings) : NULL;

	g_set_object (&tab->registry, registry);
	g_set_object (&tab->account, account_source);
	/* The password of the user's own mailbox, not of one opened as proxy */
	gtk_widget_set_visible (tab->password_button, settings && !proxy);
	g_clear_object (&settings);
	g_free (proxy);
}

static void
mailbox_free_tab (gpointer ptr)
{
	MailboxTab *tab = ptr;

	g_clear_object (&tab->registry);
	g_clear_object (&tab->account);
	g_clear_object (&tab->model);
	g_object_unref (tab->widget);
	g_free (tab);
}

/* ------------------------------------------------------------------ */

static gpointer
mailbox_load_sync (EGwConnection *cnc,
		   GCancellable *cancellable,
		   GError **error)
{
	MailboxData *data = g_new0 (MailboxData, 1);
	GPtrArray *books;

	/* A figure the server does not give is left out */
	data->has_usage = e_gw_connection_get_disk_usage_sync (cnc, &data->usage, cancellable, NULL);
	data->folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, cancellable, error);
	if (!data->folders) {
		mailbox_data_free (data);
		return NULL;
	}
	books = e_gw_connection_get_address_books_sync (cnc, cancellable, NULL);
	for (guint ii = 0; books && ii < books->len; ii++) {
		EGwAddressBook *book = books->pdata[ii];

		if (book->is_personal && !book->is_system)
			data->n_books++;
	}
	g_clear_pointer (&books, g_ptr_array_unref);

	return data;
}

static void
set_figure (GtkWidget *label,
	    gint64 value)
{
	gchar *text = g_strdup_printf ("%'" G_GINT64_FORMAT, value);

	gtk_label_set_text (GTK_LABEL (label), text);
	g_free (text);
}

static void
mailbox_fill (gpointer ptr,
	      gpointer data_ptr)
{
	MailboxTab *tab = ptr;
	MailboxData *data = data_ptr;
	gint64 messages = 0, unread = 0, calendar = 0;
	guint n_folders = 0, n_calendars = 0, ii;
	gchar *text, *used, *max_size;

	if (!data)
		return;

	used = g_strdup_printf ("%'" G_GINT64_FORMAT, data->usage.used);
	max_size = g_strdup_printf ("%'" G_GINT64_FORMAT, data->usage.max_size);
	if (data->has_usage && data->usage.max_size > 0) {
		gdouble part = CLAMP ((gdouble) data->usage.used / (gdouble) data->usage.max_size, 0.0, 1.0);

		gtk_level_bar_set_value (GTK_LEVEL_BAR (tab->usage_bar), part);
		if (data->usage.threshold > 0)
			/* Translators: megabytes used, megabytes allowed, percent used, percent from which GroupWise warns */
			text = g_strdup_printf (_("%s MB of %s MB used (%d %%). GroupWise warns you from %d %%."),
				used, max_size, (gint) (part * 100 + 0.5), data->usage.threshold);
		else
			/* Translators: megabytes used, megabytes allowed, percent used */
			text = g_strdup_printf (_("%s MB of %s MB used (%d %%)."), used, max_size, (gint) (part * 100 + 0.5));
	} else if (data->has_usage) {
		gtk_widget_hide (tab->usage_bar);
		/* Translators: megabytes used */
		text = g_strdup_printf (_("%s MB used; the mailbox has no limit."), used);
	} else {
		gtk_widget_hide (tab->usage_bar);
		text = g_strdup (_("The server does not tell the size of the mailbox."));
	}
	g_free (used);
	g_free (max_size);
	gtk_label_set_text (GTK_LABEL (tab->usage_label), text);
	g_free (text);

	gtk_list_store_clear (tab->model);
	for (ii = 0; ii < data->folders->len; ii++) {
		EGwFolder *folder = data->folders->pdata[ii];
		gchar *name, *unread_text;

		if (folder->is_calendar) {
			if (folder->type == E_GW_FOLDER_TYPE_CALENDAR && folder->kind == E_GW_FOLDER_KIND_SYSTEM)
				calendar = MAX (folder->count, 0);
			else if (e_gw_folder_get_calendar_role (folder, data->folders) != E_GW_CALENDAR_ROLE_NONE)
				n_calendars++;
			continue;
		}
		if (folder->type == E_GW_FOLDER_TYPE_ROOT || folder->type == E_GW_FOLDER_TYPE_CONTACTS ||
		    folder->type == E_GW_FOLDER_TYPE_QUERY || folder->type == E_GW_FOLDER_TYPE_OTHER ||
		    g_strcmp0 (folder->folder_type, "UserContacts") == 0)
			continue;
		n_folders++;
		if (folder->count < 0)
			continue;

		messages += folder->count;
		unread += MAX (folder->unread_count, 0);
		name = e_groupwise_ui_dup_folder_name (folder);
		unread_text = folder->unread_count > 0 ? g_strdup_printf ("%" G_GINT64_FORMAT, folder->unread_count) : NULL;
		gtk_list_store_insert_with_values (tab->model, NULL, -1, COL_NAME, name, COL_COUNT, folder->count,
			COL_UNREAD, MAX (folder->unread_count, 0), COL_UNREAD_TEXT, unread_text, -1);
		g_free (unread_text);
		g_free (name);
	}
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (tab->model), COL_COUNT, GTK_SORT_DESCENDING);

	set_figure (tab->figures[0], messages);
	set_figure (tab->figures[1], unread);
	set_figure (tab->figures[2], calendar);
	set_figure (tab->figures[3], n_folders);
	/* (the Calendar itself and the user's further calendars) */
	set_figure (tab->figures[4], n_calendars + 1);
	set_figure (tab->figures[5], data->n_books);

	mailbox_data_free (data);
}

/* Nothing here is written with OK */
static gpointer
mailbox_collect (gpointer tab)
{
	return NULL;
}

static gboolean
mailbox_apply_sync (gpointer changes,
		    EGwConnection *cnc,
		    GCancellable *cancellable,
		    GError **error)
{
	return TRUE;
}

static void
mailbox_free_changes (gpointer changes)
{
}

const EGroupwiseSettingsTab *
e_groupwise_mailbox_tab (void)
{
	static const EGroupwiseSettingsTab tab = {
		N_("Mailbox and Password"),
		mailbox_new_tab,
		mailbox_get_widget,
		mailbox_load_sync,
		mailbox_fill,
		mailbox_collect,
		mailbox_apply_sync,
		mailbox_data_free,
		mailbox_free_changes,
		mailbox_free_tab,
		mailbox_set_account
	};

	return &tab;
}

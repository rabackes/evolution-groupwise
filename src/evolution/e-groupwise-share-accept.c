/*
 * e-groupwise-share-accept.c: accepting a folder another user shares
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
 * The notification of a shared folder is a message in the Mailbox. In the
 * GroupWise client opening it starts "Install Shared Folder": who shares
 * what with which rights, then name, description and place of the folder.
 * Here that is one dialog, opened from the message. Accepted, the folder
 * is in the folder list and the notification is gone from the Mailbox.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>
#include <shell/e-shell-content.h>
#include <shell/e-shell-window.h>

#include "e-gw-folder.h"
#include "e-gw-share.h"

#include "e-groupwise-share-accept.h"
#include "e-groupwise-ui-utils.h"

enum {
	COL_NAME,
	COL_ID,
	COL_HOLDS,	/* a folder can be put into it */
	N_COLS
};

typedef struct {
	gint ref_count;		/* main thread only */
	EShellView *shell_view;
	ESourceRegistry *registry;
	ESource *account;
	CamelFolder *folder;
	gchar *uid;

	EGwConnection *cnc;
	EGwShareNotification *offer;
	GPtrArray *folders;	/* EGwFolder */

	gchar *name, *description, *container;
	gint color;		/* of a calendar, as GroupWise keeps it; -1: none */
} AcceptData;

static void
accept_data_unref (gpointer ptr)
{
	AcceptData *data = ptr;

	if (--data->ref_count > 0)
		return;

	g_clear_object (&data->shell_view);
	g_clear_object (&data->registry);
	g_clear_object (&data->account);
	g_clear_object (&data->folder);
	g_clear_object (&data->cnc);
	g_clear_pointer (&data->offer, e_gw_share_notification_free);
	g_clear_pointer (&data->folders, g_ptr_array_unref);
	g_free (data->uid);
	g_free (data->name);
	g_free (data->description);
	g_free (data->container);
	g_free (data);
}

gboolean
e_groupwise_share_accept_is_notification (CamelFolder *folder,
					  const gchar *uid)
{
	CamelMessageInfo *info;
	gboolean offer;

	if (!folder || !uid)
		return FALSE;
	/* The provider marks it; the ID tells only in some listings */
	if (e_gw_share_is_notification_id (uid))
		return TRUE;

	info = camel_folder_get_message_info (folder, uid);
	offer = info && g_strcmp0 (camel_message_info_get_user_tag (info, "groupwise-share"), "1") == 0;
	g_clear_object (&info);

	return offer;
}

static void
report_error (AcceptData *data,
	      const GError *error)
{
	if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		e_alert_submit (E_ALERT_SINK (e_shell_view_get_shell_content (data->shell_view)), "mail:async-error",
			_("accepting the shared folder"), error->message, NULL);
}

/* ------------------------------------------------------------------ */
/* Accepting */

static void
accept_thread (GTask *task,
	       gpointer source_object,
	       gpointer task_data,
	       GCancellable *cancellable)
{
	AcceptData *data = task_data;
	GError *error = NULL;
	gchar *id;

	id = e_gw_connection_accept_share_sync (data->cnc, data->offer->id, data->name, data->description,
		data->container, cancellable, &error);
	/* The color the calendar gets in Evolution is the one it has in GroupWise */
	if (id && data->color >= 0) {
		gchar *updates = g_strdup_printf ("<update><calendarAttribute><color>%d</color></calendarAttribute></update>", data->color);
		GError *local_error = NULL;

		if (!e_gw_connection_modify_item_sync (data->cnc, id, updates, cancellable, &local_error))
			g_debug ("color of %s: %s", id, local_error ? local_error->message : "?");
		g_clear_error (&local_error);
		g_free (updates);
	}
	if (id)
		g_task_return_pointer (task, id, g_free);
	else
		g_task_return_error (task, error);
}

static void
folder_info_cb (GObject *source_object,
		GAsyncResult *result,
		gpointer user_data)
{
	CamelFolderInfo *info = camel_store_get_folder_info_finish (CAMEL_STORE (source_object), result, NULL);

	if (info) {
		camel_folder_info_free (info);
		/* The new folder shows in the folder list */
		camel_store_folder_info_stale (CAMEL_STORE (source_object));
	}
}

static void
accept_done_cb (GObject *source_object,
		GAsyncResult *result,
		gpointer user_data)
{
	AcceptData *data = g_task_get_task_data (G_TASK (result));
	GError *error = NULL;
	gchar *id = g_task_propagate_pointer (G_TASK (result), &error);

	if (!id) {
		report_error (data, error);
		g_clear_error (&error);
		return;
	}
	g_free (id);

	if (data->offer->kind != E_GW_SHARE_KIND_FOLDER) {
		/* The registry lists the calendars and address books of the account anew */
		ESource *collection = e_source_registry_find_extension (data->registry, data->account, E_SOURCE_EXTENSION_COLLECTION);

		if (collection) {
			e_source_registry_refresh_backend (data->registry, e_source_get_uid (collection), NULL, NULL, NULL);
			g_object_unref (collection);
		}
	}
	camel_store_get_folder_info (camel_folder_get_parent_store (data->folder), NULL,
		CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_REFRESH, G_PRIORITY_DEFAULT, NULL, folder_info_cb, NULL);
	/* The notification is gone */
	camel_folder_refresh_info (data->folder, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* The dialog */

static gchar *
rights_text (EGwShareRights rights)
{
	GString *text = g_string_new (_("Read"));

	if (rights & E_GW_SHARE_ADD)
		g_string_append_printf (text, ", %s", _("Add"));
	if (rights & E_GW_SHARE_EDIT)
		g_string_append_printf (text, ", %s", _("Edit"));
	if (rights & E_GW_SHARE_DELETE)
		g_string_append_printf (text, ", %s", _("Delete"));

	return g_string_free (text, FALSE);
}

/* Where a folder can go: beside the Cabinet (into the root), into the
 * Cabinet and into the folders the user made */
static gboolean
can_hold (const EGwFolder *folder)
{
	if (folder->is_calendar || folder->is_shared_to_me)
		return FALSE;

	return folder->type == E_GW_FOLDER_TYPE_ROOT || folder->type == E_GW_FOLDER_TYPE_CABINET ||
		(folder->type == E_GW_FOLDER_TYPE_NORMAL && folder->kind != E_GW_FOLDER_KIND_SYSTEM);
}

/* The name of a folder as the folder list shows it: the system folders in
 * the user's language (the POA names them in English) */
static gchar *
dup_folder_name (const EGwFolder *folder)
{
	static const struct {
		EGwFolderType type;
		const gchar *name;
	} system_folders[] = {
		{ E_GW_FOLDER_TYPE_MAILBOX, N_("Mailbox") },
		{ E_GW_FOLDER_TYPE_JUNK, N_("Junk Mail") },
		{ E_GW_FOLDER_TYPE_TRASH, N_("Trash") },
		{ E_GW_FOLDER_TYPE_SENT_ITEMS, N_("Sent Items") },
		{ E_GW_FOLDER_TYPE_DRAFT, N_("Work In Progress") },
		{ E_GW_FOLDER_TYPE_CABINET, N_("Cabinet") },
		{ E_GW_FOLDER_TYPE_CHECKLIST, N_("Tasklist") },
		{ E_GW_FOLDER_TYPE_CALENDAR, N_("Calendar") },
		{ E_GW_FOLDER_TYPE_CONTACTS, N_("Contacts") },
		{ E_GW_FOLDER_TYPE_DOCUMENTS, N_("Documents") }
	};
	const gchar *name = folder->name && *folder->name ? folder->name : folder->id;
	guint ii;

	if (folder->type == E_GW_FOLDER_TYPE_ROOT) {
		/* "User One Home" */
		if (g_str_has_suffix (name, " Home")) {
			gchar *user = g_strndup (name, strlen (name) - strlen (" Home"));
			/* Translators: the top of the folder list of a mailbox; %s is the name of its user */
			gchar *text = g_strdup_printf (_("%s Home"), user);

			g_free (user);
			return text;
		}
		return g_strdup (name);
	}

	for (ii = 0; folder->kind == E_GW_FOLDER_KIND_SYSTEM && ii < G_N_ELEMENTS (system_folders); ii++) {
		if (folder->type == system_folders[ii].type)
			return g_strdup (_(system_folders[ii].name));
	}

	return g_strdup (name);
}

/* The folders as a tree, parents first as the server lists them: the root
 * with the system folders of the mailbox, as the place is seen there, and
 * the folders that can hold another. Returns the row of the Cabinet. */
static GtkTreePath *
fill_folders (GtkTreeStore *store,
	      GPtrArray *folders)
{
	GHashTable *rows = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) gtk_tree_iter_free);
	GtkTreePath *cabinet = NULL;
	guint ii;

	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];
		GtkTreeIter iter, *parent;
		gboolean holds = can_hold (folder);
		gchar *name;

		parent = folder->parent_id ? g_hash_table_lookup (rows, folder->parent_id) : NULL;
		if (folder->type != E_GW_FOLDER_TYPE_ROOT) {
			/* Below something not shown */
			if (!parent)
				continue;
			/* What cannot hold a folder: only the system folders beside the Cabinet */
			if (!holds && (folder->kind != E_GW_FOLDER_KIND_SYSTEM || folder->type == E_GW_FOLDER_TYPE_QUERY ||
			    folder->type == E_GW_FOLDER_TYPE_OTHER || gtk_tree_store_iter_depth (store, parent) > 0))
				continue;
		}
		name = dup_folder_name (folder);
		gtk_tree_store_insert_with_values (store, &iter, parent, -1,
			COL_NAME, name, COL_ID, folder->id, COL_HOLDS, holds, -1);
		g_free (name);
		/* Only rows that hold folders have rows below them */
		if (holds)
			g_hash_table_insert (rows, folder->id, gtk_tree_iter_copy (&iter));
		if (folder->type == E_GW_FOLDER_TYPE_CABINET && !cabinet)
			cabinet = gtk_tree_model_get_path (GTK_TREE_MODEL (store), &iter);
	}
	g_hash_table_destroy (rows);

	return cabinet;
}

/* Only folders that can hold another are chosen */
static gboolean
select_func (GtkTreeSelection *selection,
	     GtkTreeModel *model,
	     GtkTreePath *path,
	     gboolean currently_selected,
	     gpointer user_data)
{
	GtkTreeIter iter;
	gboolean holds = FALSE;

	if (gtk_tree_model_get_iter (model, &iter, path))
		gtk_tree_model_get (model, &iter, COL_HOLDS, &holds, -1);

	return holds || currently_selected;
}

static GtkWidget *
left_label (const gchar *text,
	    gboolean mnemonic)
{
	GtkWidget *label = mnemonic ? gtk_label_new_with_mnemonic (text) : gtk_label_new (text);

	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_widget_set_valign (label, GTK_ALIGN_START);

	return label;
}

/* TRUE: accept, with name, description and container in @data */
static gboolean
run_dialog (AcceptData *data,
	    GtkWindow *parent)
{
	GtkWidget *dialog, *box, *grid, *label, *name, *scrolled, *description, *view, *color = NULL;
	GtkTreeStore *store;
	GtkTreeSelection *selection = NULL;
	GtkTreePath *cabinet;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	EGwShareNotification *offer = data->offer;
	const gchar *sender = offer->sender_name && *offer->sender_name ? offer->sender_name : offer->sender_email;
	gchar *text;
	gboolean accept, calendar = offer->kind == E_GW_SHARE_KIND_CALENDAR, book = offer->kind == E_GW_SHARE_KIND_BOOK;
	gint row = 0;

	dialog = gtk_dialog_new_with_buttons (book ? _("Install Shared Address Book") :
		calendar ? _("Install Shared Calendar") : _("Install Shared Folder"),
		parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		_("_Cancel"), GTK_RESPONSE_CANCEL, _("_Install"), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

	box = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_box_set_spacing (GTK_BOX (box), 12);

	if (book)
		/* Translators: the first %s is the name of a user, the second %s that of an address book */
		text = g_strdup_printf (_("%s shared the address book “%s” with you."), sender ? sender : "", offer->folder_name ? offer->folder_name : "");
	else if (calendar)
		/* Translators: the first %s is the name of a user, the second %s that of a calendar */
		text = g_strdup_printf (_("%s shared the calendar “%s” with you."), sender ? sender : "", offer->folder_name ? offer->folder_name : "");
	else
		/* Translators: the first %s is the name of a user, the second %s that of a folder */
		text = g_strdup_printf (_("%s shared the folder “%s” with you."), sender ? sender : "", offer->folder_name ? offer->folder_name : "");
	label = left_label (text, FALSE);
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_max_width_chars (GTK_LABEL (label), 60);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
	g_free (text);

	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_box_pack_start (GTK_BOX (box), grid, TRUE, TRUE, 0);

	if (offer->message && *g_strstrip (offer->message)) {
		/* Translators: %s is the name of a user */
		text = g_strdup_printf (_("Message from %s:"), sender ? sender : "");
		gtk_grid_attach (GTK_GRID (grid), left_label (text, FALSE), 0, row, 1, 1);
		g_free (text);
		label = left_label (offer->message, FALSE);
		gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
		gtk_label_set_selectable (GTK_LABEL (label), TRUE);
		gtk_label_set_max_width_chars (GTK_LABEL (label), 50);
		gtk_grid_attach (GTK_GRID (grid), label, 1, row++, 1, 1);
	}

	gtk_grid_attach (GTK_GRID (grid), left_label (_("Your access rights:"), FALSE), 0, row, 1, 1);
	text = rights_text (offer->rights);
	gtk_grid_attach (GTK_GRID (grid), left_label (text, FALSE), 1, row++, 1, 1);
	g_free (text);

	label = left_label (_("_Name:"), TRUE);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top (label, 12);
	gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
	name = gtk_entry_new ();
	gtk_entry_set_text (GTK_ENTRY (name), offer->folder_name ? offer->folder_name : "");
	gtk_entry_set_activates_default (GTK_ENTRY (name), TRUE);
	gtk_widget_set_hexpand (name, TRUE);
	gtk_widget_set_margin_top (name, 12);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), name);
	gtk_grid_attach (GTK_GRID (grid), name, 1, row++, 1, 1);

	label = left_label (_("_Description:"), TRUE);
	gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request (scrolled, 380, 60);
	description = gtk_text_view_new ();
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (description), GTK_WRAP_WORD_CHAR);
	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (description));
	if (offer->description)
		gtk_text_buffer_set_text (buffer, offer->description, -1);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), description);
	gtk_container_add (GTK_CONTAINER (scrolled), description);
	gtk_grid_attach (GTK_GRID (grid), scrolled, 1, row++, 1, 1);

	if (calendar) {
		/* Some of the colors Evolution offers for calendars */
		static const gchar *colors[] = { "#62a0ea", "#57e389", "#f8e45c", "#ffa348", "#ed333b", "#c061cb", "#b5835a", "#5bc8af" };
		GdkRGBA rgba;

		label = left_label (_("C_olor:"), TRUE);
		gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
		gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
		gdk_rgba_parse (&rgba, colors[g_random_int_range (0, G_N_ELEMENTS (colors))]);
		color = gtk_color_button_new_with_rgba (&rgba);
		gtk_widget_set_halign (color, GTK_ALIGN_START);
		gtk_widget_set_tooltip_text (color, _("The color of the calendar, here and in GroupWise"));
		gtk_label_set_mnemonic_widget (GTK_LABEL (label), color);
		gtk_grid_attach (GTK_GRID (grid), color, 1, row++, 1, 1);
	}

	if (!calendar && !book) {
		/* Where the folder goes; a calendar goes beside the others */
		label = left_label (_("_Position:"), TRUE);
		gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
		store = gtk_tree_store_new (N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
		cabinet = fill_folders (store, data->folders);
		view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
		gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (view), FALSE);
		gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (view), -1, NULL, gtk_cell_renderer_text_new (),
			"text", COL_NAME, "sensitive", COL_HOLDS, NULL);
		gtk_widget_set_tooltip_text (view, _("The new folder is put into the folder selected here; the topmost row puts it beside the Cabinet"));
		gtk_label_set_mnemonic_widget (GTK_LABEL (label), view);
		selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (view));
		gtk_tree_selection_set_mode (selection, GTK_SELECTION_BROWSE);
		gtk_tree_selection_set_select_function (selection, select_func, NULL, NULL);
		if (cabinet) {
			gtk_tree_view_expand_to_path (GTK_TREE_VIEW (view), cabinet);
			gtk_tree_view_expand_row (GTK_TREE_VIEW (view), cabinet, FALSE);
			gtk_tree_selection_select_path (selection, cabinet);
			gtk_tree_path_free (cabinet);
		}
		scrolled = gtk_scrolled_window_new (NULL, NULL);
		gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
		gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtk_widget_set_size_request (scrolled, 380, 200);
		gtk_widget_set_vexpand (scrolled, TRUE);
		gtk_container_add (GTK_CONTAINER (scrolled), view);
		gtk_grid_attach (GTK_GRID (grid), scrolled, 1, row++, 1, 1);
		g_object_unref (store);
	}

	gtk_widget_show_all (box);
	gtk_widget_grab_focus (name);

	accept = gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK;
	if (accept) {
		data->name = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (name))));
		gtk_text_buffer_get_bounds (buffer, &start, &end);
		data->description = g_strstrip (gtk_text_buffer_get_text (buffer, &start, &end, FALSE));
		if (color) {
			GdkRGBA rgba;

			/* COLORREF: red in the lowest byte */
			gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (color), &rgba);
			data->color = (gint) (rgba.red * 255 + 0.5) | ((gint) (rgba.green * 255 + 0.5) << 8) |
				((gint) (rgba.blue * 255 + 0.5) << 16);
		}
		if (selection && gtk_tree_selection_get_selected (selection, &model, &iter))
			gtk_tree_model_get (model, &iter, COL_ID, &data->container, -1);
		for (guint ii = 0; calendar && !data->container && ii < data->folders->len; ii++) {
			EGwFolder *folder = data->folders->pdata[ii];

			if (folder->type == E_GW_FOLDER_TYPE_CALENDAR && folder->kind == E_GW_FOLDER_KIND_SYSTEM)
				data->container = g_strdup (folder->id);
		}
	}
	gtk_widget_destroy (dialog);

	return accept;
}

/* ------------------------------------------------------------------ */
/* Loading */

static void
load_thread (GTask *task,
	     gpointer source_object,
	     gpointer task_data,
	     GCancellable *cancellable)
{
	AcceptData *data = task_data;
	GError *error = NULL;

	data->cnc = e_groupwise_ui_connect_items_sync (data->registry, data->account, cancellable, &error);
	if (data->cnc)
		data->offer = e_gw_connection_get_share_notification_sync (data->cnc, data->uid, cancellable, &error);
	if (data->offer)
		data->folders = e_gw_connection_get_folder_list_sync (data->cnc, NULL, TRUE, cancellable, &error);

	if (data->folders)
		g_task_return_boolean (task, TRUE);
	else
		g_task_return_error (task, error);
}

static void
load_done_cb (GObject *source_object,
	      GAsyncResult *result,
	      gpointer user_data)
{
	AcceptData *data = g_task_get_task_data (G_TASK (result));
	GError *error = NULL;
	GTask *task;

	if (!g_task_propagate_boolean (G_TASK (result), &error)) {
		report_error (data, error);
		g_clear_error (&error);
		return;
	}

	if (!run_dialog (data, GTK_WINDOW (e_shell_view_get_shell_window (data->shell_view))))
		return;

	data->ref_count++;
	task = g_task_new (NULL, NULL, accept_done_cb, NULL);
	g_task_set_task_data (task, data, accept_data_unref);
	g_task_run_in_thread (task, accept_thread);
	g_object_unref (task);
}

void
e_groupwise_share_accept_run (EShellView *shell_view,
			      ESourceRegistry *registry,
			      ESource *account,
			      CamelFolder *folder,
			      const gchar *uid)
{
	AcceptData *data;
	GTask *task;

	g_return_if_fail (E_IS_SHELL_VIEW (shell_view));
	g_return_if_fail (E_IS_SOURCE (account));
	g_return_if_fail (CAMEL_IS_FOLDER (folder));
	g_return_if_fail (uid != NULL);

	data = g_new0 (AcceptData, 1);
	data->ref_count = 1;
	data->color = -1;
	data->shell_view = g_object_ref (shell_view);
	data->registry = g_object_ref (registry);
	data->account = g_object_ref (account);
	data->folder = g_object_ref (folder);
	data->uid = g_strdup (uid);

	task = g_task_new (NULL, NULL, load_done_cb, NULL);
	g_task_set_task_data (task, data, accept_data_unref);
	g_task_run_in_thread (task, load_thread);
	g_object_unref (task);
}

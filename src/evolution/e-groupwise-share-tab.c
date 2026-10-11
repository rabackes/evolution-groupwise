/*
 * e-groupwise-share-tab.c: sharing a folder, a page of the folder properties
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
 * As in the GroupWise client, sharing is a page of the properties of a
 * folder: whom the folder is shared with, how far they got with it, and
 * what they may do in it. Evolution has no place for a module to add a page
 * to its folder properties (they are a dialog around an EConfig, which only
 * the old plugins could extend), so the page is added when that dialog
 * shows. The dialog has just the button Close, which applies: so does the
 * page, after asking for the text of the notification when users are new.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>
#include <libemail-engine/libemail-engine.h>
#include <mail/e-mail-backend.h>
#include <mail/em-folder-tree.h>
#include <shell/e-shell.h>
#include <shell/e-shell-content.h>
#include <shell/e-shell-sidebar.h>
#include <shell/e-shell-view.h>
#include <shell/e-shell-window.h>

#include "e-gw-folder.h"
#include "e-gw-proxy.h"
#include "e-gw-share.h"

#include "e-groupwise-share-tab.h"
#include "e-groupwise-ui-utils.h"

#define SECTION "users"

enum {
	COL_NAME,
	COL_EMAIL,
	COL_UUID,
	COL_STATUS,	/* EGwShareStatus, -1: not shared with yet */
	COL_RIGHTS,
	COL_OLD_RIGHTS,
	N_COLS
};

static const struct {
	EGwShareRights right;
	const gchar *label;	/* in the list */
	const gchar *check;	/* of the check box, NULL: always granted */
} rights_boxes[] = {
	{ E_GW_SHARE_READ, N_("Read"), NULL },
	{ E_GW_SHARE_ADD, N_("Add"), N_("_Add") },
	{ E_GW_SHARE_EDIT, N_("Edit"), N_("_Edit") },
	{ E_GW_SHARE_DELETE, N_("Delete"), N_("_Delete") }
};

typedef struct {
	gint ref_count;
	GtkWidget *dialog;		/* NULL once it is gone */
	GtkNotebook *notebook;
	EShellView *shell_view;
	ESourceRegistry *registry;
	ESource *account;
	CamelStore *store;
	gchar *full_name;
	gboolean select;
	GCancellable *cancellable;

	EGwConnection *cnc;
	EGwShare *share;

	GtkWidget *radio_none, *radio_shared;
	GtkWidget *shared_box;
	GtkWidget *entry, *add_button, *remove_button;
	GtkWidget *message;
	GtkWidget *view;
	GtkWidget *rights_box, *boxes[G_N_ELEMENTS (rights_boxes)];
	GtkWidget *tree_box;
	GtkListStore *model;
	ENameSelector *name_selector;
	GPtrArray *removed;		/* EGwShareEntry */
	gboolean updating;
	guint resolving;
} ShareTab;

static gboolean select_next;

static ShareTab *
share_tab_ref (ShareTab *tab)
{
	tab->ref_count++;

	return tab;
}

static void
share_tab_unref (ShareTab *tab)
{
	if (--tab->ref_count > 0)
		return;

	g_clear_object (&tab->shell_view);
	g_clear_object (&tab->registry);
	g_clear_object (&tab->account);
	g_clear_object (&tab->store);
	g_clear_object (&tab->cancellable);
	g_clear_object (&tab->cnc);
	g_clear_object (&tab->model);
	g_clear_object (&tab->name_selector);
	g_clear_pointer (&tab->share, e_gw_share_free);
	g_clear_pointer (&tab->removed, g_ptr_array_unref);
	g_free (tab->full_name);
	g_free (tab);
}

/* ------------------------------------------------------------------ */
/* The list */

static const gchar *
status_text (gint status)
{
	switch (status) {
	case E_GW_SHARE_STATUS_PENDING:
		return _("Pending");
	case E_GW_SHARE_STATUS_OPENED:
		return _("Opened");
	case E_GW_SHARE_STATUS_ACCEPTED:
		return _("Accepted");
	case E_GW_SHARE_STATUS_DELETED:
		return _("Deleted");
	case E_GW_SHARE_STATUS_OWNER:
		return _("Owner");
	}

	/* Translators: a user the folder is not shared with yet */
	return _("New");
}

static void
status_data_func (GtkTreeViewColumn *column,
		  GtkCellRenderer *renderer,
		  GtkTreeModel *model,
		  GtkTreeIter *iter,
		  gpointer user_data)
{
	gint status;

	gtk_tree_model_get (model, iter, COL_STATUS, &status, -1);
	g_object_set (renderer, "text", status_text (status), NULL);
}

static void
rights_data_func (GtkTreeViewColumn *column,
		  GtkCellRenderer *renderer,
		  GtkTreeModel *model,
		  GtkTreeIter *iter,
		  gpointer user_data)
{
	GString *text = g_string_new (NULL);
	guint rights, ii;

	gtk_tree_model_get (model, iter, COL_RIGHTS, &rights, -1);
	for (ii = 0; ii < G_N_ELEMENTS (rights_boxes); ii++) {
		if (!(rights & rights_boxes[ii].right))
			continue;
		if (text->len)
			g_string_append (text, ", ");
		g_string_append (text, _(rights_boxes[ii].label));
	}
	g_object_set (renderer, "text", text->str, NULL);
	g_string_free (text, TRUE);
}

static gboolean
get_selected (ShareTab *tab,
	      GtkTreeIter *iter)
{
	return gtk_tree_selection_get_selected (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), NULL, iter);
}

/* Whether the user may change anything: not in a folder of another user */
static gboolean
is_mine (ShareTab *tab)
{
	return !tab->share->shared_to_me;
}

static gboolean
has_new_users (ShareTab *tab)
{
	GtkTreeModel *model = GTK_TREE_MODEL (tab->model);
	GtkTreeIter iter;
	gboolean valid;

	for (valid = gtk_tree_model_get_iter_first (model, &iter); valid; valid = gtk_tree_model_iter_next (model, &iter)) {
		gint status;

		gtk_tree_model_get (model, &iter, COL_STATUS, &status, -1);
		if (status < 0)
			return TRUE;
	}

	return FALSE;
}

static void
update_sensitivity (ShareTab *tab)
{
	gboolean mine = is_mine (tab);
	gboolean shared = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (tab->radio_shared));
	GtkTreeIter iter;
	gboolean selected = get_selected (tab, &iter);
	guint rights = 0, ii;

	if (selected)
		gtk_tree_model_get (GTK_TREE_MODEL (tab->model), &iter, COL_RIGHTS, &rights, -1);

	tab->updating = TRUE;
	for (ii = 0; ii < G_N_ELEMENTS (rights_boxes); ii++) {
		if (tab->boxes[ii])
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tab->boxes[ii]), (rights & rights_boxes[ii].right) != 0);
	}
	tab->updating = FALSE;

	gtk_widget_set_sensitive (tab->radio_none, mine);
	gtk_widget_set_sensitive (tab->radio_shared, mine);
	gtk_widget_set_sensitive (tab->shared_box, shared);
	gtk_widget_set_sensitive (tab->entry, mine);
	gtk_widget_set_sensitive (tab->add_button, mine && !tab->resolving);
	gtk_widget_set_sensitive (tab->remove_button, mine && selected);
	gtk_widget_set_sensitive (tab->rights_box, mine && selected);
	/* The server takes it only along with new users */
	gtk_widget_set_sensitive (tab->tree_box, mine && !tab->share->share_tree && has_new_users (tab));
}

static void
selection_changed_cb (GtkTreeSelection *selection,
		      ShareTab *tab)
{
	update_sensitivity (tab);
}

static void
radio_toggled_cb (GtkToggleButton *button,
		  ShareTab *tab)
{
	update_sensitivity (tab);
}

static void
box_toggled_cb (GtkToggleButton *button,
		ShareTab *tab)
{
	GtkTreeIter iter;
	guint rights, ii;

	if (tab->updating || !get_selected (tab, &iter))
		return;

	gtk_tree_model_get (GTK_TREE_MODEL (tab->model), &iter, COL_RIGHTS, &rights, -1);
	for (ii = 0; ii < G_N_ELEMENTS (rights_boxes); ii++) {
		if (GTK_WIDGET (button) != tab->boxes[ii])
			continue;
		if (gtk_toggle_button_get_active (button))
			rights |= rights_boxes[ii].right;
		else
			rights &= ~rights_boxes[ii].right;
	}
	gtk_list_store_set (tab->model, &iter, COL_RIGHTS, rights | E_GW_SHARE_READ, -1);
}

static void
set_message (ShareTab *tab,
	     const gchar *text)
{
	gtk_label_set_text (GTK_LABEL (tab->message), text ? text : "");
	gtk_widget_set_visible (tab->message, text && *text);
}

/* ------------------------------------------------------------------ */
/* Adding users: the server says who they are */

typedef struct {
	ShareTab *tab;
	GPtrArray *who;		/* what was typed or chosen */
	GPtrArray *found;	/* EGwShareEntry */
	GString *unknown;
} ResolveData;

static void
resolve_data_free (gpointer ptr)
{
	ResolveData *data = ptr;

	share_tab_unref (data->tab);
	g_ptr_array_unref (data->who);
	g_ptr_array_unref (data->found);
	g_string_free (data->unknown, TRUE);
	g_free (data);
}

static void
resolve_thread (GTask *task,
		gpointer source_object,
		gpointer task_data,
		GCancellable *cancellable)
{
	ResolveData *data = task_data;
	guint ii;

	for (ii = 0; ii < data->who->len; ii++) {
		const gchar *who = data->who->pdata[ii];
		gchar *uuid = NULL, *email = NULL, *name = NULL;
		GError *error = NULL;

		if (e_gw_connection_resolve_user_sync (data->tab->cnc, who, &uuid, &email, &name, cancellable, &error)) {
			EGwShareEntry *entry = e_gw_share_entry_new (name && *name ? name : who, email && *email ? email : who, E_GW_SHARE_READ);

			entry->uuid = uuid;
			g_ptr_array_add (data->found, entry);
			g_free (email);
			g_free (name);
		} else if (g_error_matches (error, E_GW_ERROR, E_GW_ERROR_UNKNOWN_USER)) {
			if (data->unknown->len)
				g_string_append (data->unknown, ", ");
			g_string_append (data->unknown, who);
			g_clear_error (&error);
		} else {
			g_task_return_error (task, error);
			return;
		}
	}

	g_task_return_boolean (task, TRUE);
}

static gboolean
find_email (ShareTab *tab,
	    const gchar *email,
	    GtkTreeIter *iter)
{
	GtkTreeModel *model = GTK_TREE_MODEL (tab->model);
	gboolean valid;

	for (valid = gtk_tree_model_get_iter_first (model, iter); valid; valid = gtk_tree_model_iter_next (model, iter)) {
		gchar *other = NULL;
		gboolean same;

		gtk_tree_model_get (model, iter, COL_EMAIL, &other, -1);
		same = other && email && g_ascii_strcasecmp (other, email) == 0;
		g_free (other);
		if (same)
			return TRUE;
	}

	return FALSE;
}

static void
resolve_done_cb (GObject *source_object,
		 GAsyncResult *result,
		 gpointer user_data)
{
	ResolveData *data = g_task_get_task_data (G_TASK (result));
	ShareTab *tab = data->tab;
	GError *error = NULL;
	guint ii;

	tab->resolving--;
	g_task_propagate_boolean (G_TASK (result), &error);
	if (!tab->dialog) {
		g_clear_error (&error);
		return;
	}

	for (ii = 0; ii < data->found->len; ii++) {
		EGwShareEntry *entry = data->found->pdata[ii];
		GtkTreeIter iter;

		if (!find_email (tab, entry->email, &iter)) {
			gtk_list_store_insert_with_values (tab->model, &iter, -1,
				COL_NAME, entry->display_name, COL_EMAIL, entry->email, COL_UUID, entry->uuid,
				COL_STATUS, -1, COL_RIGHTS, (guint) E_GW_SHARE_READ, COL_OLD_RIGHTS, 0, -1);
		}
		gtk_tree_selection_select_iter (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), &iter);
	}

	if (error) {
		set_message (tab, error->message);
	} else if (data->unknown->len) {
		gchar *text = g_strdup_printf (_("No GroupWise user “%s”"), data->unknown->str);

		set_message (tab, text);
		g_free (text);
	} else {
		set_message (tab, NULL);
	}
	g_clear_error (&error);
	update_sensitivity (tab);
}

static void
add_clicked_cb (GtkWidget *widget,
		ShareTab *tab)
{
	ENameSelectorModel *model = e_name_selector_peek_model (tab->name_selector);
	EDestinationStore *destinations = NULL;
	ResolveData *data;
	GList *list, *link;
	GTask *task;

	if (tab->resolving || !e_name_selector_model_peek_section (model, SECTION, NULL, &destinations) || !destinations)
		return;

	data = g_new0 (ResolveData, 1);
	data->tab = share_tab_ref (tab);
	data->who = g_ptr_array_new_with_free_func (g_free);
	data->found = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);
	data->unknown = g_string_new (NULL);

	list = e_destination_store_list_destinations (destinations);
	for (link = list; link; link = g_list_next (link)) {
		EDestination *destination = link->data;
		const gchar *email = e_destination_get_email (destination);
		const gchar *text = email && strchr (email, '@') ? email : e_destination_get_textrep (destination, FALSE);
		gchar *who = g_strstrip (g_strdup (text ? text : ""));

		if (*who)
			g_ptr_array_add (data->who, who);
		else
			g_free (who);
		e_destination_store_remove_destination (destinations, destination);
	}
	g_list_free (list);

	if (!data->who->len) {
		resolve_data_free (data);
		return;
	}

	tab->resolving++;
	update_sensitivity (tab);
	task = g_task_new (NULL, tab->cancellable, resolve_done_cb, NULL);
	g_task_set_task_data (task, data, resolve_data_free);
	g_task_run_in_thread (task, resolve_thread);
	g_object_unref (task);
}

static void
book_clicked_cb (GtkButton *button,
		 ShareTab *tab)
{
	e_name_selector_show_dialog (tab->name_selector, tab->dialog);
}

static EGwShareEntry *
entry_of_row (ShareTab *tab,
	      GtkTreeIter *iter,
	      gboolean old_rights)
{
	EGwShareEntry *entry;
	gchar *name = NULL, *email = NULL, *uuid = NULL;
	guint rights, old;

	gtk_tree_model_get (GTK_TREE_MODEL (tab->model), iter, COL_NAME, &name, COL_EMAIL, &email, COL_UUID, &uuid,
		COL_RIGHTS, &rights, COL_OLD_RIGHTS, &old, -1);
	entry = e_gw_share_entry_new (name, email, old_rights ? old : rights);
	entry->uuid = uuid;
	g_free (name);
	g_free (email);

	return entry;
}

static void
remove_clicked_cb (GtkButton *button,
		   ShareTab *tab)
{
	GtkTreeIter iter;
	gint status;

	if (!get_selected (tab, &iter))
		return;

	gtk_tree_model_get (GTK_TREE_MODEL (tab->model), &iter, COL_STATUS, &status, -1);
	if (status >= 0)
		g_ptr_array_add (tab->removed, entry_of_row (tab, &iter, TRUE));
	gtk_list_store_remove (tab->model, &iter);
	update_sensitivity (tab);
}

/* ------------------------------------------------------------------ */
/* Applying */

typedef struct {
	ShareTab *tab;
	GPtrArray *added, *changed, *removed;
	gboolean share_tree;
	gchar *subject, *message;
} ApplyData;

static void
apply_data_free (gpointer ptr)
{
	ApplyData *data = ptr;

	share_tab_unref (data->tab);
	g_ptr_array_unref (data->added);
	g_ptr_array_unref (data->changed);
	g_ptr_array_unref (data->removed);
	g_free (data->subject);
	g_free (data->message);
	g_free (data);
}

static void
apply_thread (GTask *task,
	      gpointer source_object,
	      gpointer task_data,
	      GCancellable *cancellable)
{
	ApplyData *data = task_data;
	ShareTab *tab = data->tab;
	GError *error = NULL;

	if (e_gw_connection_modify_share_sync (tab->cnc, tab->share->id, data->added, data->changed, data->removed,
		data->share_tree, data->subject, data->message, NULL, cancellable, &error))
		g_task_return_boolean (task, TRUE);
	else
		g_task_return_error (task, error);
}

static void
folder_info_cb (GObject *source_object,
		GAsyncResult *result,
		gpointer user_data)
{
	CamelFolderInfo *info = camel_store_get_folder_info_finish (CAMEL_STORE (source_object), result, NULL);

	if (info)
		camel_folder_info_free (info);
}

static void
apply_done_cb (GObject *source_object,
	       GAsyncResult *result,
	       gpointer user_data)
{
	ApplyData *data = g_task_get_task_data (G_TASK (result));
	ShareTab *tab = data->tab;
	GError *error = NULL;

	if (!g_task_propagate_boolean (G_TASK (result), &error)) {
		e_alert_submit (E_ALERT_SINK (e_shell_view_get_shell_content (tab->shell_view)), "mail:async-error",
			_("sharing the folder"), error ? error->message : "", NULL);
		g_clear_error (&error);
		return;
	}

	/* Shared, the folder has another ID */
	camel_store_get_folder_info (tab->store, NULL, CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_REFRESH,
		G_PRIORITY_DEFAULT, NULL, folder_info_cb, NULL);
}

/* Subject and text of the message to the new users; FALSE: do not share */
static gboolean
ask_notification (ShareTab *tab,
		  GPtrArray *added,
		  gchar **out_subject,
		  gchar **out_message)
{
	GtkWidget *dialog, *box, *grid, *label, *users, *subject, *scrolled, *text;
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	GString *names = g_string_new (NULL);
	gboolean send;
	guint ii;

	dialog = gtk_dialog_new_with_buttons (_("Shared Folder Notification"), GTK_WINDOW (tab->dialog),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		_("_Cancel"), GTK_RESPONSE_CANCEL, _("_Send"), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

	box = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_box_set_spacing (GTK_BOX (box), 12);

	label = gtk_label_new (_("The new participants are sent a notification that you shared this folder "
		"with them. You may add a message for them."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_label_set_max_width_chars (GTK_LABEL (label), 60);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_box_pack_start (GTK_BOX (box), grid, TRUE, TRUE, 0);

	for (ii = 0; ii < added->len; ii++) {
		EGwShareEntry *entry = added->pdata[ii];

		if (names->len)
			g_string_append_c (names, '\n');
		g_string_append (names, entry->display_name ? entry->display_name : entry->email);
	}
	label = gtk_label_new (_("Participants:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_widget_set_valign (label, GTK_ALIGN_START);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
	users = gtk_label_new (names->str);
	gtk_label_set_xalign (GTK_LABEL (users), 0.0);
	gtk_label_set_selectable (GTK_LABEL (users), TRUE);
	gtk_grid_attach (GTK_GRID (grid), users, 1, 0, 1, 1);
	g_string_free (names, TRUE);

	label = gtk_label_new_with_mnemonic (_("S_ubject:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
	subject = gtk_entry_new ();
	/* The server puts the name of the folder behind it */
	gtk_entry_set_text (GTK_ENTRY (subject), _("Shared folder notification"));
	gtk_entry_set_activates_default (GTK_ENTRY (subject), TRUE);
	gtk_widget_set_hexpand (subject, TRUE);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), subject);
	gtk_grid_attach (GTK_GRID (grid), subject, 1, 1, 1, 1);

	label = gtk_label_new_with_mnemonic (_("_Message:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_widget_set_valign (label, GTK_ALIGN_START);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 2, 1, 1);
	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request (scrolled, 360, 120);
	gtk_widget_set_hexpand (scrolled, TRUE);
	gtk_widget_set_vexpand (scrolled, TRUE);
	text = gtk_text_view_new ();
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (text), GTK_WRAP_WORD_CHAR);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), text);
	gtk_container_add (GTK_CONTAINER (scrolled), text);
	gtk_grid_attach (GTK_GRID (grid), scrolled, 1, 2, 1, 1);

	gtk_widget_show_all (box);
	gtk_widget_grab_focus (text);

	send = gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK;
	if (send) {
		buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (text));
		gtk_text_buffer_get_bounds (buffer, &start, &end);
		*out_subject = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (subject))));
		*out_message = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
	}
	gtk_widget_destroy (dialog);

	return send;
}

static void
response_cb (GtkDialog *dialog,
	     gint response,
	     ShareTab *tab)
{
	GtkTreeModel *model = GTK_TREE_MODEL (tab->model);
	ApplyData *data;
	GtkTreeIter iter;
	gboolean valid, shared;
	GTask *task;
	guint ii;

	if (response != GTK_RESPONSE_OK || !tab->share || !tab->model || !is_mine (tab))
		return;

	data = g_new0 (ApplyData, 1);
	data->tab = share_tab_ref (tab);
	data->added = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);
	data->changed = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);
	data->removed = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);
	data->share_tree = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (tab->tree_box));

	for (ii = 0; ii < tab->removed->len; ii++)
		g_ptr_array_add (data->removed, e_gw_share_entry_copy (tab->removed->pdata[ii]));

	shared = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (tab->radio_shared));
	for (valid = gtk_tree_model_get_iter_first (model, &iter); valid; valid = gtk_tree_model_iter_next (model, &iter)) {
		guint rights, old;
		gint status;

		gtk_tree_model_get (model, &iter, COL_STATUS, &status, COL_RIGHTS, &rights, COL_OLD_RIGHTS, &old, -1);
		if (!shared) {
			/* Not shared: taken from everybody who has it */
			if (status >= 0)
				g_ptr_array_add (data->removed, entry_of_row (tab, &iter, TRUE));
		} else if (status < 0) {
			g_ptr_array_add (data->added, entry_of_row (tab, &iter, FALSE));
		} else if (rights != old) {
			g_ptr_array_add (data->changed, entry_of_row (tab, &iter, FALSE));
		}
	}

	if ((!data->added->len && !data->changed->len && !data->removed->len) ||
	    (data->added->len && !ask_notification (tab, data->added, &data->subject, &data->message))) {
		apply_data_free (data);
		return;
	}

	task = g_task_new (NULL, NULL, apply_done_cb, NULL);
	g_task_set_task_data (task, data, apply_data_free);
	g_task_run_in_thread (task, apply_thread);
	g_object_unref (task);
}

/* ------------------------------------------------------------------ */
/* The page */

static GtkWidget *
build_page (ShareTab *tab,
	    EClientCache *client_cache)
{
	GtkWidget *page, *grid, *label, *box, *scrolled, *frame, *button;
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	ENameSelectorModel *selector_model;
	gboolean mine = is_mine (tab);
	guint ii;
	gint row = 0;

	page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (page), 12);

	if (!mine) {
		const gchar *owner = tab->share->owner_name && *tab->share->owner_name ? tab->share->owner_name : tab->share->owner_email;
		gchar *text = g_strdup_printf (_("%s shares this folder with you."), owner ? owner : "");

		label = gtk_label_new (text);
		gtk_label_set_xalign (GTK_LABEL (label), 0.0);
		gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
		gtk_box_pack_start (GTK_BOX (page), label, FALSE, FALSE, 0);
		g_free (text);
		if (tab->share->description && *tab->share->description) {
			label = gtk_label_new (tab->share->description);
			gtk_label_set_xalign (GTK_LABEL (label), 0.0);
			gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
			gtk_label_set_selectable (GTK_LABEL (label), TRUE);
			gtk_box_pack_start (GTK_BOX (page), label, FALSE, FALSE, 0);
		}
	}

	tab->radio_none = gtk_radio_button_new_with_mnemonic (NULL, _("_Not shared"));
	gtk_box_pack_start (GTK_BOX (page), tab->radio_none, FALSE, FALSE, 0);
	tab->radio_shared = gtk_radio_button_new_with_mnemonic_from_widget (GTK_RADIO_BUTTON (tab->radio_none), _("_Shared with"));
	gtk_box_pack_start (GTK_BOX (page), tab->radio_shared, FALSE, FALSE, 0);

	tab->shared_box = grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
	gtk_widget_set_margin_start (grid, 18);
	gtk_box_pack_start (GTK_BOX (page), grid, TRUE, TRUE, 0);

	/* Whom to add: found in the address books while typing, or picked there */
	tab->name_selector = e_name_selector_new (client_cache);
	selector_model = e_name_selector_peek_model (tab->name_selector);
	e_name_selector_model_add_section (selector_model, SECTION, _("Users"), NULL);
	e_name_selector_load_books (tab->name_selector);

	label = gtk_label_new_with_mnemonic (_("Na_me:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_grid_attach (GTK_GRID (grid), label, 0, row++, 3, 1);
	tab->entry = GTK_WIDGET (e_name_selector_peek_section_entry (tab->name_selector, SECTION));
	gtk_widget_set_hexpand (tab->entry, TRUE);
	gtk_widget_set_tooltip_text (tab->entry, _("The name or the e-mail address of a user of the GroupWise system"));
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), tab->entry);
	g_signal_connect (tab->entry, "activate", G_CALLBACK (add_clicked_cb), tab);
	gtk_grid_attach (GTK_GRID (grid), tab->entry, 0, row, 1, 1);
	button = gtk_button_new_from_icon_name ("x-office-address-book", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text (button, _("Choose users from the address books"));
	g_signal_connect (button, "clicked", G_CALLBACK (book_clicked_cb), tab);
	gtk_grid_attach (GTK_GRID (grid), button, 1, row, 1, 1);
	if (!mine)
		gtk_widget_set_sensitive (button, FALSE);
	tab->add_button = gtk_button_new_with_mnemonic (_("Add _User"));
	g_signal_connect (tab->add_button, "clicked", G_CALLBACK (add_clicked_cb), tab);
	gtk_grid_attach (GTK_GRID (grid), tab->add_button, 2, row++, 1, 1);

	tab->message = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (tab->message), 0.0);
	gtk_label_set_line_wrap (GTK_LABEL (tab->message), TRUE);
	gtk_style_context_add_class (gtk_widget_get_style_context (tab->message), "error");
	gtk_widget_set_no_show_all (tab->message, TRUE);
	gtk_grid_attach (GTK_GRID (grid), tab->message, 0, row++, 3, 1);

	label = gtk_label_new_with_mnemonic (_("Share _list:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_grid_attach (GTK_GRID (grid), label, 0, row++, 3, 1);

	tab->model = gtk_list_store_new (N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_UINT, G_TYPE_UINT);
	tab->view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (tab->model));
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), tab->view);
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Name"), renderer, "text", COL_NAME, NULL);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_column_set_expand (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tab->view), column);
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Status"), renderer, NULL);
	gtk_tree_view_column_set_cell_data_func (column, renderer, status_data_func, NULL, NULL);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tab->view), column);
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Access"), renderer, NULL);
	gtk_tree_view_column_set_cell_data_func (column, renderer, rights_data_func, NULL, NULL);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tab->view), column);

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request (scrolled, 420, 140);
	gtk_widget_set_hexpand (scrolled, TRUE);
	gtk_widget_set_vexpand (scrolled, TRUE);
	gtk_container_add (GTK_CONTAINER (scrolled), tab->view);
	gtk_grid_attach (GTK_GRID (grid), scrolled, 0, row, 2, 1);
	tab->remove_button = gtk_button_new_with_mnemonic (_("_Remove User"));
	gtk_widget_set_valign (tab->remove_button, GTK_ALIGN_START);
	g_signal_connect (tab->remove_button, "clicked", G_CALLBACK (remove_clicked_cb), tab);
	gtk_grid_attach (GTK_GRID (grid), tab->remove_button, 2, row++, 1, 1);

	/* Reading is always granted */
	frame = gtk_frame_new (_("Additional access"));
	tab->rights_box = box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 18);
	gtk_container_set_border_width (GTK_CONTAINER (box), 6);
	gtk_container_add (GTK_CONTAINER (frame), box);
	for (ii = 0; ii < G_N_ELEMENTS (rights_boxes); ii++) {
		if (!rights_boxes[ii].check)
			continue;
		tab->boxes[ii] = gtk_check_button_new_with_mnemonic (_(rights_boxes[ii].check));
		g_signal_connect (tab->boxes[ii], "toggled", G_CALLBACK (box_toggled_cb), tab);
		gtk_box_pack_start (GTK_BOX (box), tab->boxes[ii], FALSE, FALSE, 0);
	}
	gtk_grid_attach (GTK_GRID (grid), frame, 0, row++, 3, 1);

	tab->tree_box = gtk_check_button_new_with_mnemonic (_("Share all su_bfolders"));
	gtk_widget_set_tooltip_text (tab->tree_box, _("The server takes this only together with new users"));
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tab->tree_box), tab->share->share_tree);
	gtk_grid_attach (GTK_GRID (grid), tab->tree_box, 0, row++, 3, 1);

	/* Who has it */
	for (ii = 0; ii < tab->share->entries->len; ii++) {
		EGwShareEntry *entry = tab->share->entries->pdata[ii];

		gtk_list_store_insert_with_values (tab->model, NULL, -1,
			COL_NAME, entry->display_name && *entry->display_name ? entry->display_name : entry->email,
			COL_EMAIL, entry->email, COL_UUID, entry->uuid, COL_STATUS, (gint) entry->status,
			COL_RIGHTS, (guint) (entry->rights | E_GW_SHARE_READ), COL_OLD_RIGHTS, (guint) (entry->rights | E_GW_SHARE_READ), -1);
	}
	if (!mine) {
		gtk_list_store_insert_with_values (tab->model, NULL, -1,
			COL_NAME, tab->share->owner_name && *tab->share->owner_name ? tab->share->owner_name : tab->share->owner_email,
			COL_EMAIL, tab->share->owner_email, COL_STATUS, (gint) E_GW_SHARE_STATUS_OWNER,
			COL_RIGHTS, (guint) E_GW_SHARE_ALL, COL_OLD_RIGHTS, (guint) E_GW_SHARE_ALL, -1);
	}

	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tab->share->shared_by_me || tab->share->shared_to_me ?
		tab->radio_shared : tab->radio_none), TRUE);
	g_signal_connect (tab->radio_shared, "toggled", G_CALLBACK (radio_toggled_cb), tab);
	g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), "changed", G_CALLBACK (selection_changed_cb), tab);
	update_sensitivity (tab);

	gtk_widget_show_all (page);

	return page;
}

/* ------------------------------------------------------------------ */
/* Loading */

static void
load_thread (GTask *task,
	     gpointer source_object,
	     gpointer task_data,
	     GCancellable *cancellable)
{
	ShareTab *tab = task_data;
	EGwConnection *cnc;
	GPtrArray *folders;
	GHashTable *ids;
	EGwShare *share = NULL;
	const gchar *id;
	GError *error = NULL;
	guint ii;

	cnc = e_groupwise_ui_connect_items_sync (tab->registry, tab->account, cancellable, &error);
	folders = cnc ? e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, cancellable, &error) : NULL;
	if (!folders) {
		g_clear_object (&cnc);
		g_task_return_error (task, error);
		return;
	}

	ids = e_groupwise_ui_folder_ids (folders);
	id = g_hash_table_lookup (ids, tab->full_name);
	for (ii = 0; id && ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];

		if (g_strcmp0 (folder->id, id) != 0)
			continue;
		/* A folder the user made, or one shared already; calendars
		 * and address books are not shared here */
		if (!folder->is_calendar &&
		    ((folder->kind == E_GW_FOLDER_KIND_FOLDER && folder->type == E_GW_FOLDER_TYPE_NORMAL) ||
		     folder->kind == E_GW_FOLDER_KIND_SHARED))
			share = e_gw_connection_get_share_sync (cnc, id, cancellable, &error);
		break;
	}
	g_hash_table_destroy (ids);
	g_ptr_array_unref (folders);

	if (error) {
		g_clear_object (&cnc);
		g_task_return_error (task, error);
		return;
	}

	tab->cnc = cnc;
	g_task_return_pointer (task, share, (GDestroyNotify) e_gw_share_free);
}

static void
load_done_cb (GObject *source_object,
	      GAsyncResult *result,
	      gpointer user_data)
{
	ShareTab *tab = g_task_get_task_data (G_TASK (result));
	GError *error = NULL;
	GtkWidget *page;
	gint number;

	tab->share = g_task_propagate_pointer (G_TASK (result), &error);
	if (error)
		g_debug ("share tab: %s", error->message);
	g_clear_error (&error);
	/* A folder that cannot be shared has no page */
	if (!tab->share || !tab->dialog)
		return;

	tab->removed = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);
	page = build_page (tab, e_shell_get_client_cache (
		e_shell_window_get_shell (E_SHELL_WINDOW (e_shell_view_get_shell_window (tab->shell_view)))));
	number = gtk_notebook_append_page (tab->notebook, page, gtk_label_new (_("Sharing")));
	if (tab->select)
		gtk_notebook_set_current_page (tab->notebook, number);
	g_signal_connect (tab->dialog, "response", G_CALLBACK (response_cb), tab);
}

static void
dialog_destroy_cb (GtkWidget *dialog,
		   ShareTab *tab)
{
	tab->dialog = NULL;
	tab->notebook = NULL;
	g_cancellable_cancel (tab->cancellable);
	if (tab->name_selector)
		e_name_selector_cancel_loading (tab->name_selector);
	share_tab_unref (tab);
}

static void
find_notebook_cb (GtkWidget *widget,
		  gpointer user_data)
{
	GtkWidget **notebook = user_data;

	if (!*notebook && GTK_IS_NOTEBOOK (widget))
		*notebook = widget;
}

/* The properties of the folder selected in the mail view of @dialog's window */
static void
adapt (GtkDialog *dialog)
{
	GtkWindow *parent = gtk_window_get_transient_for (GTK_WINDOW (dialog));
	GtkWidget *notebook = NULL;
	EShellView *shell_view;
	EShellBackend *backend;
	EMFolderTree *folder_tree = NULL;
	ESourceRegistry *registry;
	CamelStore *store = NULL;
	ESource *account;
	gchar *full_name = NULL;
	gboolean select = select_next;
	ShareTab *tab;
	GTask *task;

	if (!parent || !E_IS_SHELL_WINDOW (parent) || g_object_get_data (G_OBJECT (dialog), "groupwise-share-tab") ||
	    g_strcmp0 (gtk_window_get_title (GTK_WINDOW (dialog)), g_dgettext ("evolution", "Folder Properties")) != 0)
		return;

	select_next = FALSE;
	gtk_container_foreach (GTK_CONTAINER (gtk_dialog_get_content_area (dialog)), find_notebook_cb, &notebook);
	shell_view = e_shell_window_peek_shell_view (E_SHELL_WINDOW (parent), "mail");
	if (!notebook || !shell_view)
		return;

	g_object_get (e_shell_view_get_shell_sidebar (shell_view), "folder-tree", &folder_tree, NULL);
	if (!folder_tree || !em_folder_tree_get_selected (folder_tree, &store, &full_name) || !store || !full_name) {
		g_clear_object (&folder_tree);
		g_clear_object (&store);
		g_free (full_name);
		return;
	}
	g_object_unref (folder_tree);

	backend = e_shell_view_get_shell_backend (shell_view);
	registry = e_mail_session_get_registry (e_mail_backend_get_session (E_MAIL_BACKEND (backend)));
	account = e_groupwise_ui_ref_account_source (registry, CAMEL_SERVICE (store));
	/* The folders of another mailbox are for its user to share */
	if (!account || e_groupwise_ui_is_proxy_store (CAMEL_SERVICE (store))) {
		g_clear_object (&account);
		g_object_unref (store);
		g_free (full_name);
		return;
	}

	g_object_set_data (G_OBJECT (dialog), "groupwise-share-tab", GINT_TO_POINTER (1));
	tab = g_new0 (ShareTab, 1);
	tab->ref_count = 1;
	tab->dialog = GTK_WIDGET (dialog);
	tab->notebook = GTK_NOTEBOOK (notebook);
	tab->shell_view = g_object_ref (shell_view);
	tab->registry = g_object_ref (registry);
	tab->account = account;
	tab->store = store;
	tab->full_name = full_name;
	tab->select = select;
	tab->cancellable = g_cancellable_new ();
	g_signal_connect (dialog, "destroy", G_CALLBACK (dialog_destroy_cb), tab);

	task = g_task_new (NULL, tab->cancellable, load_done_cb, NULL);
	g_task_set_task_data (task, share_tab_ref (tab), (GDestroyNotify) share_tab_unref);
	g_task_run_in_thread (task, load_thread);
	g_object_unref (task);
}

static gboolean
show_hook_cb (GSignalInvocationHint *hint,
	      guint n_param_values,
	      const GValue *param_values,
	      gpointer user_data)
{
	GObject *object = n_param_values ? g_value_peek_pointer (&param_values[0]) : NULL;

	if (object && GTK_IS_DIALOG (object) && !E_IS_ALERT_DIALOG (object))
		adapt (GTK_DIALOG (object));

	return TRUE;
}

void
e_groupwise_share_tab_start (void)
{
	static gboolean started;

	if (started)
		return;
	started = TRUE;

	g_type_class_unref (g_type_class_ref (GTK_TYPE_WIDGET));
	g_signal_add_emission_hook (g_signal_lookup ("show", GTK_TYPE_WIDGET), 0, show_hook_cb, NULL, NULL);
}

void
e_groupwise_share_tab_select_next (void)
{
	select_next = TRUE;
}

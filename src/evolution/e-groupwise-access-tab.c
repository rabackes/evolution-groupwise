/*
 * e-groupwise-access-tab.c: who may open the user's mailbox as proxy
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
 * The proxy access list of the mailbox, as in the GroupWise client (Tools >
 * Options > Security > Proxy Access): the users who may log in as proxy,
 * each with the rights to mail, appointments, reminder notes and tasks and
 * the other rights; the entry for all users of the system on top.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-backend-utils.h"
#include "e-gw-proxy.h"

#include "e-groupwise-settings-window.h"

enum {
	COL_NAME,
	COL_EMAIL,
	COL_ID,		/* NULL: added here */
	COL_OLD_RIGHTS,
	COL_RIGHTS,
	COL_ALL_USERS,
	N_COLUMNS
};

/* The check boxes of the rights */
static const struct {
	EGwProxyRights right;
	EGwProxyRights implies;		/* writing needs reading */
	const gchar *label;
} rights_boxes[] = {
	{ E_GW_PROXY_MAIL_READ, 0, NULL },
	{ E_GW_PROXY_MAIL_WRITE, E_GW_PROXY_MAIL_READ, NULL },
	{ E_GW_PROXY_APPOINTMENT_READ, 0, NULL },
	{ E_GW_PROXY_APPOINTMENT_WRITE, E_GW_PROXY_APPOINTMENT_READ, NULL },
	{ E_GW_PROXY_NOTE_READ, 0, NULL },
	{ E_GW_PROXY_NOTE_WRITE, E_GW_PROXY_NOTE_READ, NULL },
	{ E_GW_PROXY_TASK_READ, 0, NULL },
	{ E_GW_PROXY_TASK_WRITE, E_GW_PROXY_TASK_READ, NULL },
	{ E_GW_PROXY_ALARMS, 0, N_("Subscribe to my _alarms") },
	{ E_GW_PROXY_NOTIFY, 0, N_("Subscribe to my _notifications") },
	{ E_GW_PROXY_SETUP, 0, N_("_Modify options, rules and folders") },
	{ E_GW_PROXY_READ_HIDDEN, 0, N_("Read items marked _private") },
	{ E_GW_PROXY_SETUP_SECURITY, 0, N_("Modify _security options") }
};

typedef struct {
	GtkWidget *widget;
	GtkListStore *store;
	GtkWidget *view, *rights_box, *remove_button;
	GtkWidget *boxes[G_N_ELEMENTS (rights_boxes)];
	GPtrArray *removed;	/* IDs */
	gboolean updating;	/* setting the boxes from the list */
	gboolean is_proxy;	/* the window of a proxy account */
} AccessTab;

typedef struct {
	GPtrArray *removed;	/* IDs */
	GPtrArray *modified;	/* "ID\nOLD\nNEW" */
	GPtrArray *added;	/* "RIGHTS\nWHO" */
} AccessChanges;

/* ------------------------------------------------------------------ */

static gboolean
get_selected (AccessTab *tab,
	      GtkTreeIter *iter)
{
	return gtk_tree_selection_get_selected (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), NULL, iter);
}

static void
selection_changed_cb (GtkTreeSelection *selection,
		      AccessTab *tab)
{
	GtkTreeIter iter;
	guint rights = 0;
	gboolean selected, all_users = FALSE;
	guint ii;

	selected = get_selected (tab, &iter);
	if (selected)
		gtk_tree_model_get (GTK_TREE_MODEL (tab->store), &iter, COL_RIGHTS, &rights, COL_ALL_USERS, &all_users, -1);

	tab->updating = TRUE;
	for (ii = 0; ii < G_N_ELEMENTS (rights_boxes); ii++)
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tab->boxes[ii]), (rights & rights_boxes[ii].right) != 0);
	tab->updating = FALSE;

	gtk_widget_set_sensitive (tab->rights_box, selected);
	/* The entry for all users stays; its rights can be taken away */
	gtk_widget_set_sensitive (tab->remove_button, selected && !all_users);
}

static void
box_toggled_cb (GtkToggleButton *button,
		AccessTab *tab)
{
	GtkTreeIter iter;
	guint rights, ii;

	if (tab->updating || !get_selected (tab, &iter))
		return;

	gtk_tree_model_get (GTK_TREE_MODEL (tab->store), &iter, COL_RIGHTS, &rights, -1);
	for (ii = 0; ii < G_N_ELEMENTS (rights_boxes); ii++) {
		if (GTK_WIDGET (button) != tab->boxes[ii])
			continue;
		if (gtk_toggle_button_get_active (button)) {
			rights |= rights_boxes[ii].right | rights_boxes[ii].implies;
		} else {
			guint jj;

			rights &= ~rights_boxes[ii].right;
			/* Without reading no writing */
			for (jj = 0; jj < G_N_ELEMENTS (rights_boxes); jj++) {
				if (rights_boxes[jj].implies == rights_boxes[ii].right)
					rights &= ~rights_boxes[jj].right;
			}
		}
	}
	gtk_list_store_set (tab->store, &iter, COL_RIGHTS, rights, -1);

	/* Shows what reading and writing imply */
	selection_changed_cb (NULL, tab);
}

static void
add_clicked_cb (GtkButton *button,
		AccessTab *tab)
{
	GtkWidget *dialog, *entry, *label, *box;
	GtkTreeIter iter;
	gchar *text;

	dialog = gtk_dialog_new_with_buttons (_("Grant Proxy Access"),
		GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (button))), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		_("_Cancel"), GTK_RESPONSE_CANCEL, _("_Add"), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

	box = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_box_set_spacing (GTK_BOX (box), 6);
	label = gtk_label_new (_("The e-mail address or the name of a user of the GroupWise system:"));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_label_set_max_width_chars (GTK_LABEL (label), 50);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
	entry = gtk_entry_new ();
	gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
	gtk_box_pack_start (GTK_BOX (box), entry, FALSE, FALSE, 0);
	gtk_widget_show_all (box);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		text = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (entry))));
		if (*text) {
			/* Reading mail and appointments to start with */
			gtk_list_store_insert_with_values (tab->store, &iter, -1,
				COL_NAME, text, COL_EMAIL, strchr (text, '@') ? text : "", COL_ID, NULL,
				COL_OLD_RIGHTS, 0, COL_RIGHTS, E_GW_PROXY_MAIL_READ | E_GW_PROXY_APPOINTMENT_READ,
				COL_ALL_USERS, FALSE, -1);
			gtk_tree_selection_select_iter (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), &iter);
		}
		g_free (text);
	}
	gtk_widget_destroy (dialog);
}

static void
remove_clicked_cb (GtkButton *button,
		   AccessTab *tab)
{
	GtkTreeIter iter;
	gchar *id = NULL;

	if (!get_selected (tab, &iter))
		return;

	gtk_tree_model_get (GTK_TREE_MODEL (tab->store), &iter, COL_ID, &id, -1);
	if (id)
		g_ptr_array_add (tab->removed, id);
	gtk_list_store_remove (tab->store, &iter);
}

static GtkWidget *
rights_label (const gchar *text)
{
	GtkWidget *label = gtk_label_new (text);

	gtk_label_set_xalign (GTK_LABEL (label), 0.0);

	return label;
}

static gpointer
access_new_tab (void)
{
	AccessTab *tab = g_new0 (AccessTab, 1);
	GtkWidget *box, *label, *scrolled, *buttons, *grid, *column;
	const gchar *kinds[] = { N_("Mail/Phone"), N_("Appointments"), N_("Reminder Notes"), N_("Tasks") };
	guint ii;

	tab->removed = g_ptr_array_new_with_free_func (g_free);

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	tab->widget = g_object_ref_sink (box);

	label = gtk_label_new (_("These users may open your mailbox as proxy, with the rights below. "
		"“All users” applies to everybody in the GroupWise system."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	tab->store = gtk_list_store_new (N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_UINT, G_TYPE_UINT, G_TYPE_BOOLEAN);
	tab->view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (tab->store));
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("Name"),
		gtk_cell_renderer_text_new (), "text", COL_NAME, NULL);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("E-mail address"),
		gtk_cell_renderer_text_new (), "text", COL_EMAIL, NULL);
	g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), "changed",
		G_CALLBACK (selection_changed_cb), tab);

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_container_add (GTK_CONTAINER (scrolled), tab->view);
	gtk_box_pack_start (GTK_BOX (box), scrolled, TRUE, TRUE, 0);

	buttons = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_button_box_set_layout (GTK_BUTTON_BOX (buttons), GTK_BUTTONBOX_START);
	gtk_box_set_spacing (GTK_BOX (buttons), 6);
	{
		GtkWidget *button = gtk_button_new_with_mnemonic (_("A_dd User…"));

		g_signal_connect (button, "clicked", G_CALLBACK (add_clicked_cb), tab);
		gtk_container_add (GTK_CONTAINER (buttons), button);
	}
	tab->remove_button = gtk_button_new_with_mnemonic (_("_Remove User"));
	g_signal_connect (tab->remove_button, "clicked", G_CALLBACK (remove_clicked_cb), tab);
	gtk_container_add (GTK_CONTAINER (buttons), tab->remove_button);
	gtk_box_pack_start (GTK_BOX (box), buttons, FALSE, FALSE, 0);

	/* The rights of the selected user: read and write per kind, then the others */
	tab->rights_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
	gtk_widget_set_margin_top (tab->rights_box, 6);
	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 2);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_grid_attach (GTK_GRID (grid), rights_label (_("Read")), 1, 0, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), rights_label (_("Write")), 2, 0, 1, 1);
	for (ii = 0; ii < G_N_ELEMENTS (kinds); ii++) {
		gtk_grid_attach (GTK_GRID (grid), rights_label (_(kinds[ii])), 0, ii + 1, 1, 1);
		tab->boxes[2 * ii] = gtk_check_button_new ();
		tab->boxes[2 * ii + 1] = gtk_check_button_new ();
		gtk_widget_set_halign (tab->boxes[2 * ii], GTK_ALIGN_CENTER);
		gtk_widget_set_halign (tab->boxes[2 * ii + 1], GTK_ALIGN_CENTER);
		gtk_grid_attach (GTK_GRID (grid), tab->boxes[2 * ii], 1, ii + 1, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), tab->boxes[2 * ii + 1], 2, ii + 1, 1, 1);
	}
	gtk_box_pack_start (GTK_BOX (tab->rights_box), grid, FALSE, FALSE, 0);

	column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
	for (ii = 2 * G_N_ELEMENTS (kinds); ii < G_N_ELEMENTS (rights_boxes); ii++) {
		tab->boxes[ii] = gtk_check_button_new_with_mnemonic (_(rights_boxes[ii].label));
		gtk_box_pack_start (GTK_BOX (column), tab->boxes[ii], FALSE, FALSE, 0);
	}
	gtk_box_pack_start (GTK_BOX (tab->rights_box), column, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), tab->rights_box, FALSE, FALSE, 0);

	for (ii = 0; ii < G_N_ELEMENTS (rights_boxes); ii++)
		g_signal_connect (tab->boxes[ii], "toggled", G_CALLBACK (box_toggled_cb), tab);
	selection_changed_cb (NULL, tab);

	return tab;
}

static GtkWidget *
access_get_widget (gpointer tab)
{
	return ((AccessTab *) tab)->widget;
}

static void
access_free_tab (gpointer ptr)
{
	AccessTab *tab = ptr;

	g_ptr_array_unref (tab->removed);
	g_object_unref (tab->store);
	g_object_unref (tab->widget);
	g_free (tab);
}

/* ------------------------------------------------------------------ */

static void
access_set_account (gpointer ptr,
		    ESourceRegistry *registry,
		    ESource *account_source)
{
	AccessTab *tab = ptr;
	CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (registry, account_source);
	gchar *proxy = settings ? camel_groupwise_settings_dup_proxy (settings) : NULL;

	/* Who may open another user's mailbox is that user's business */
	tab->is_proxy = proxy != NULL;
	if (tab->is_proxy)
		gtk_widget_hide (tab->widget);
	g_free (proxy);
	g_clear_object (&settings);
}

static gpointer
access_load_sync (EGwConnection *cnc,
		  GCancellable *cancellable,
		  GError **error)
{
	/* The window of a proxy account: the tab is hidden */
	if (e_gw_connection_get_proxy (cnc))
		return g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_proxy_access_free);

	return e_gw_connection_get_proxy_access_list_sync (cnc, cancellable, error);
}

static void
access_free_data (gpointer data)
{
	g_ptr_array_unref (data);
}

static void
access_fill (gpointer ptr,
	     gpointer data)
{
	AccessTab *tab = ptr;
	GPtrArray *list = data;
	guint ii;

	for (ii = 0; ii < list->len; ii++) {
		EGwProxyAccess *access = list->pdata[ii];
		gboolean all_users = e_gw_proxy_access_is_all_users (access);

		gtk_list_store_insert_with_values (tab->store, NULL, all_users ? 0 : -1,
			COL_NAME, all_users ? _("All users") : access->display_name ? access->display_name : access->email,
			COL_EMAIL, access->email ? access->email : "",
			COL_ID, access->id,
			COL_OLD_RIGHTS, access->rights,
			COL_RIGHTS, access->rights,
			COL_ALL_USERS, all_users,
			-1);
	}

	g_ptr_array_unref (list);
}

static void
access_free_changes (gpointer ptr)
{
	AccessChanges *changes = ptr;

	g_ptr_array_unref (changes->removed);
	g_ptr_array_unref (changes->modified);
	g_ptr_array_unref (changes->added);
	g_free (changes);
}

static gpointer
access_collect (gpointer ptr)
{
	AccessTab *tab = ptr;
	GtkTreeModel *model = GTK_TREE_MODEL (tab->store);
	AccessChanges *changes;
	GtkTreeIter iter;
	gboolean valid;
	guint ii;

	if (tab->is_proxy)
		return NULL;

	changes = g_new0 (AccessChanges, 1);
	changes->removed = g_ptr_array_new_with_free_func (g_free);
	changes->modified = g_ptr_array_new_with_free_func (g_free);
	changes->added = g_ptr_array_new_with_free_func (g_free);

	for (ii = 0; ii < tab->removed->len; ii++)
		g_ptr_array_add (changes->removed, g_strdup (tab->removed->pdata[ii]));

	for (valid = gtk_tree_model_get_iter_first (model, &iter); valid; valid = gtk_tree_model_iter_next (model, &iter)) {
		gchar *name = NULL, *id = NULL;
		guint old_rights, rights;

		gtk_tree_model_get (model, &iter, COL_NAME, &name, COL_ID, &id,
			COL_OLD_RIGHTS, &old_rights, COL_RIGHTS, &rights, -1);
		if (!id)
			g_ptr_array_add (changes->added, g_strdup_printf ("%x\n%s", rights, name));
		else if (rights != old_rights)
			g_ptr_array_add (changes->modified, g_strdup_printf ("%s\n%x\n%x", id, old_rights, rights));
		g_free (name);
		g_free (id);
	}

	if (!changes->removed->len && !changes->modified->len && !changes->added->len) {
		access_free_changes (changes);
		return NULL;
	}

	return changes;
}

static gboolean
access_apply_sync (gpointer ptr,
		   EGwConnection *cnc,
		   GCancellable *cancellable,
		   GError **error)
{
	AccessChanges *changes = ptr;
	gboolean success = TRUE;
	guint ii;

	for (ii = 0; success && ii < changes->removed->len; ii++)
		success = e_gw_connection_remove_proxy_access_sync (cnc, changes->removed->pdata[ii], cancellable, error);
	for (ii = 0; success && ii < changes->modified->len; ii++) {
		gchar **parts = g_strsplit (changes->modified->pdata[ii], "\n", 3);

		success = e_gw_connection_modify_proxy_access_sync (cnc, parts[0],
			(EGwProxyRights) g_ascii_strtoull (parts[1], NULL, 16),
			(EGwProxyRights) g_ascii_strtoull (parts[2], NULL, 16), cancellable, error);
		g_strfreev (parts);
	}
	for (ii = 0; success && ii < changes->added->len; ii++) {
		gchar **parts = g_strsplit (changes->added->pdata[ii], "\n", 2);
		gchar *id = e_gw_connection_create_proxy_access_sync (cnc, parts[1],
			(EGwProxyRights) g_ascii_strtoull (parts[0], NULL, 16), cancellable, error);

		success = id != NULL;
		g_free (id);
		g_strfreev (parts);
	}

	return success;
}

const EGroupwiseSettingsTab *
e_groupwise_access_tab (void)
{
	static const EGroupwiseSettingsTab tab = {
		N_("Proxy Access"),
		access_new_tab,
		access_get_widget,
		access_load_sync,
		access_fill,
		access_collect,
		access_apply_sync,
		access_free_data,
		access_free_changes,
		access_free_tab,
		access_set_account
	};

	return &tab;
}

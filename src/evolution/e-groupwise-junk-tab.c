/*
 * e-groupwise-junk-tab.c: the junk mail handling in the GroupWise settings
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
 * The settings and the junk, block and trust lists of the server, as in
 * the GroupWise client (Tools > Junk Mail Handling).
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-junk.h"

#include "e-groupwise-settings-window.h"

enum {
	COL_MATCH,
	COL_ID,		/* NULL for an entry added here */
	N_COLUMNS
};

typedef struct {
	GtkWidget *widget;
	GtkWidget *use_junk, *use_block, *use_pab, *ical_pab, *auto_delete, *days;
	GtkListStore *lists[3];		/* EGwJunkList */
	GtkWidget *views[3];
	GHashTable *settings;		/* as read: field -> value */
	GPtrArray *removed;		/* IDs of entries removed here */
} JunkTab;

typedef struct {
	GHashTable *settings;
	GPtrArray *entries;
} JunkData;

typedef struct {
	GHashTable *settings;	/* changed fields */
	GPtrArray *removed;	/* entry IDs */
	GPtrArray *added;	/* "<list>\n<match>" */
} JunkChanges;

/* ------------------------------------------------------------------ */

static void
add_clicked_cb (GtkButton *button,
		JunkTab *tab)
{
	guint list = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "junk-list"));
	GtkWidget *dialog, *entry, *label, *box;
	gchar *text;

	dialog = gtk_dialog_new_with_buttons (_("Add an Address or Domain"),
		GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (button))), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		_("_Cancel"), GTK_RESPONSE_CANCEL, _("_Add"), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

	box = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_box_set_spacing (GTK_BOX (box), 6);
	label = gtk_label_new (_("An e-mail address (name@example.com) or an Internet domain (example.com; "
		"it takes its subdomains along):"));
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
		if (*text)
			gtk_list_store_insert_with_values (tab->lists[list], NULL, -1, COL_MATCH, text, COL_ID, NULL, -1);
		g_free (text);
	}
	gtk_widget_destroy (dialog);
}

/* Takes the selected entries off @list; with @into, they go onto that list */
static void
take_selected (JunkTab *tab,
	       guint list,
	       gint into)
{
	GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->views[list]));
	GtkTreeModel *sorted;
	GList *rows, *link, *refs = NULL;

	/* The view shows the list sorted: its rows point into the sorted model */
	rows = gtk_tree_selection_get_selected_rows (selection, &sorted);
	for (link = rows; link; link = g_list_next (link)) {
		GtkTreePath *child = gtk_tree_model_sort_convert_path_to_child_path (GTK_TREE_MODEL_SORT (sorted), link->data);

		if (child) {
			refs = g_list_prepend (refs, gtk_tree_row_reference_new (GTK_TREE_MODEL (tab->lists[list]), child));
			gtk_tree_path_free (child);
		}
	}
	g_list_free_full (rows, (GDestroyNotify) gtk_tree_path_free);

	for (link = refs; link; link = g_list_next (link)) {
		GtkTreePath *path = gtk_tree_row_reference_get_path (link->data);
		GtkTreeIter iter;
		gchar *id = NULL;

		if (path && gtk_tree_model_get_iter (GTK_TREE_MODEL (tab->lists[list]), &iter, path)) {
			gchar *match = NULL;

			gtk_tree_model_get (GTK_TREE_MODEL (tab->lists[list]), &iter, COL_ID, &id, COL_MATCH, &match, -1);
			if (id)
				g_ptr_array_add (tab->removed, id);
			gtk_list_store_remove (tab->lists[list], &iter);
			/* A new entry of the other list (the server's goes above) */
			if (into >= 0 && match)
				gtk_list_store_insert_with_values (tab->lists[into], NULL, -1, COL_MATCH, match, COL_ID, NULL, -1);
			g_free (match);
		}
		gtk_tree_path_free (path);
	}
	g_list_free_full (refs, (GDestroyNotify) gtk_tree_row_reference_free);
}

static void
remove_clicked_cb (GtkButton *button,
		   JunkTab *tab)
{
	take_selected (tab, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "junk-list")), -1);
}

static void
move_activate_cb (GtkMenuItem *item,
		  JunkTab *tab)
{
	take_selected (tab, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (item), "junk-list")),
		GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "junk-into")));
}

/* The context menu of a list: its entries onto one of the other lists */
static void
show_menu (JunkTab *tab,
	   guint list,
	   const GdkEvent *event)
{
	static const gchar *labels[] = {
		N_("Move to the _Junk List"),	/* E_GW_JUNK_LIST_JUNK */
		N_("Move to the _Block List"),	/* E_GW_JUNK_LIST_BLOCK */
		N_("Move to the _Trust List")	/* E_GW_JUNK_LIST_TRUST */
	};
	GtkWidget *menu = gtk_menu_new ();
	gboolean any = gtk_tree_selection_count_selected_rows (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->views[list]))) > 0;
	guint into;

	for (into = 0; into < G_N_ELEMENTS (labels); into++) {
		GtkWidget *item;

		if (into == list)
			continue;
		item = gtk_menu_item_new_with_mnemonic (_(labels[into]));
		g_object_set_data (G_OBJECT (item), "junk-list", GUINT_TO_POINTER (list));
		g_object_set_data (G_OBJECT (item), "junk-into", GINT_TO_POINTER (into));
		gtk_widget_set_sensitive (item, any);
		g_signal_connect (item, "activate", G_CALLBACK (move_activate_cb), tab);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	}
	gtk_widget_show_all (menu);
	gtk_menu_attach_to_widget (GTK_MENU (menu), tab->views[list], NULL);
	g_signal_connect (menu, "deactivate", G_CALLBACK (gtk_widget_destroy), NULL);
	gtk_menu_popup_at_pointer (GTK_MENU (menu), event);
}

static gboolean
view_button_press_cb (GtkWidget *view,
		      GdkEventButton *event,
		      JunkTab *tab)
{
	guint list = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (view), "junk-list"));
	GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (view));
	GtkTreePath *path = NULL;

	if (!gdk_event_triggers_context_menu ((GdkEvent *) event))
		return FALSE;

	/* The row clicked on, unless it is among the selected ones */
	if (gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (view), event->x, event->y, &path, NULL, NULL, NULL)) {
		if (!gtk_tree_selection_path_is_selected (selection, path)) {
			gtk_tree_selection_unselect_all (selection);
			gtk_tree_selection_select_path (selection, path);
		}
		gtk_tree_path_free (path);
	}
	show_menu (tab, list, (GdkEvent *) event);

	return TRUE;
}

static gboolean
view_popup_menu_cb (GtkWidget *view,
		    JunkTab *tab)
{
	show_menu (tab, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (view), "junk-list")), NULL);

	return TRUE;
}

static GtkWidget *
list_page (JunkTab *tab,
	   EGwJunkList list,
	   const gchar *explanation)
{
	GtkWidget *box, *label, *scrolled, *view, *buttons, *button;
	GtkTreeModel *sorted;

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (box), 6);

	label = gtk_label_new (explanation);
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	tab->lists[list] = gtk_list_store_new (N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING);
	sorted = gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (tab->lists[list]));
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (sorted), COL_MATCH, GTK_SORT_ASCENDING);
	view = gtk_tree_view_new_with_model (sorted);
	g_object_unref (sorted);
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (view), FALSE);
	gtk_tree_view_set_search_column (GTK_TREE_VIEW (view), COL_MATCH);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (view), -1, _("Address or domain"),
		gtk_cell_renderer_text_new (), "text", COL_MATCH, NULL);
	gtk_tree_selection_set_mode (gtk_tree_view_get_selection (GTK_TREE_VIEW (view)), GTK_SELECTION_MULTIPLE);
	tab->views[list] = view;
	g_object_set_data (G_OBJECT (view), "junk-list", GUINT_TO_POINTER (list));
	g_signal_connect (view, "button-press-event", G_CALLBACK (view_button_press_cb), tab);
	g_signal_connect (view, "popup-menu", G_CALLBACK (view_popup_menu_cb), tab);

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_container_add (GTK_CONTAINER (scrolled), view);
	gtk_box_pack_start (GTK_BOX (box), scrolled, TRUE, TRUE, 0);

	buttons = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_button_box_set_layout (GTK_BUTTON_BOX (buttons), GTK_BUTTONBOX_START);
	gtk_box_set_spacing (GTK_BOX (buttons), 6);
	button = gtk_button_new_with_mnemonic (_("A_dd…"));
	g_object_set_data (G_OBJECT (button), "junk-list", GUINT_TO_POINTER (list));
	g_signal_connect (button, "clicked", G_CALLBACK (add_clicked_cb), tab);
	gtk_container_add (GTK_CONTAINER (buttons), button);
	button = gtk_button_new_with_mnemonic (_("_Remove"));
	g_object_set_data (G_OBJECT (button), "junk-list", GUINT_TO_POINTER (list));
	g_signal_connect (button, "clicked", G_CALLBACK (remove_clicked_cb), tab);
	gtk_container_add (GTK_CONTAINER (buttons), button);
	gtk_box_pack_start (GTK_BOX (box), buttons, FALSE, FALSE, 0);

	return box;
}

static GtkWidget *
check_button (GtkWidget *box,
	      const gchar *text)
{
	GtkWidget *check = gtk_check_button_new_with_mnemonic (text);

	gtk_box_pack_start (GTK_BOX (box), check, FALSE, FALSE, 0);

	return check;
}

static gpointer
junk_new_tab (void)
{
	JunkTab *tab = g_new0 (JunkTab, 1);
	GtkWidget *box, *label, *row, *notebook;

	tab->removed = g_ptr_array_new_with_free_func (g_free);

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	tab->widget = g_object_ref_sink (box);

	label = gtk_label_new (_("GroupWise sorts Internet mail with these lists and settings, for every client. "
		"The context menu of a message offers them too (Junk Mail)."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	tab->use_junk = check_button (box, _("Use the _junk list (into the Junk Mail folder)"));
	tab->use_block = check_button (box, _("Use the _block list (not delivered)"));
	tab->use_pab = check_button (box, _("Mail from senders in no _personal address book is junk"));
	tab->ical_pab = check_button (box, _("_Appointments from senders in no personal address book are junk"));

	row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	tab->auto_delete = gtk_check_button_new_with_mnemonic (_("_Delete junk mail automatically after"));
	gtk_box_pack_start (GTK_BOX (row), tab->auto_delete, FALSE, FALSE, 0);
	tab->days = gtk_spin_button_new_with_range (1, 999, 1);
	gtk_box_pack_start (GTK_BOX (row), tab->days, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (row), gtk_label_new (_("days")), FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), row, FALSE, FALSE, 0);
	g_object_bind_property (tab->auto_delete, "active", tab->days, "sensitive", G_BINDING_SYNC_CREATE);

	notebook = gtk_notebook_new ();
	gtk_widget_set_margin_top (notebook, 6);
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook),
		list_page (tab, E_GW_JUNK_LIST_JUNK, _("Mail from these addresses and domains goes into the Junk Mail folder.")),
		gtk_label_new (_("Junk List")));
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook),
		list_page (tab, E_GW_JUNK_LIST_BLOCK, _("Mail from these addresses and domains is not delivered.")),
		gtk_label_new (_("Block List")));
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook),
		list_page (tab, E_GW_JUNK_LIST_TRUST, _("Mail from these addresses and domains is never junk, "
			"whatever the other lists say.")),
		gtk_label_new (_("Trust List")));
	gtk_box_pack_start (GTK_BOX (box), notebook, TRUE, TRUE, 0);

	return tab;
}

static GtkWidget *
junk_get_widget (gpointer tab)
{
	return ((JunkTab *) tab)->widget;
}

static void
junk_free_tab (gpointer ptr)
{
	JunkTab *tab = ptr;
	guint ii;

	g_clear_pointer (&tab->settings, g_hash_table_destroy);
	g_ptr_array_unref (tab->removed);
	for (ii = 0; ii < G_N_ELEMENTS (tab->lists); ii++)
		g_clear_object (&tab->lists[ii]);
	g_object_unref (tab->widget);
	g_free (tab);
}

/* ------------------------------------------------------------------ */

static void
junk_free_data (gpointer ptr)
{
	JunkData *data = ptr;

	g_clear_pointer (&data->settings, g_hash_table_destroy);
	g_clear_pointer (&data->entries, g_ptr_array_unref);
	g_free (data);
}

static gpointer
junk_load_sync (EGwConnection *cnc,
		GCancellable *cancellable,
		GError **error)
{
	JunkData *data = g_new0 (JunkData, 1);

	data->settings = e_gw_connection_get_junk_settings_sync (cnc, cancellable, error);
	if (data->settings)
		data->entries = e_gw_connection_get_junk_entries_sync (cnc, cancellable, error);
	if (!data->entries) {
		junk_free_data (data);
		return NULL;
	}

	return data;
}

static const gchar *
setting (JunkTab *tab,
	 const gchar *field)
{
	return tab->settings ? g_hash_table_lookup (tab->settings, field) : NULL;
}

static void
set_check (GtkWidget *check,
	   const gchar *value)
{
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), g_strcmp0 (value, "1") == 0);
}

static void
junk_fill (gpointer ptr,
	   gpointer data_ptr)
{
	JunkTab *tab = ptr;
	JunkData *data = data_ptr;
	const gchar *days;
	guint ii;

	tab->settings = g_steal_pointer (&data->settings);
	set_check (tab->use_junk, setting (tab, E_GW_JUNK_SETTING_USE_JUNK_LIST));
	set_check (tab->use_block, setting (tab, E_GW_JUNK_SETTING_USE_BLOCK_LIST));
	set_check (tab->use_pab, setting (tab, E_GW_JUNK_SETTING_USE_PAB));
	set_check (tab->ical_pab, setting (tab, E_GW_JUNK_SETTING_ICAL_USE_PAB));
	set_check (tab->auto_delete, setting (tab, E_GW_JUNK_SETTING_AUTO_DELETE));
	days = setting (tab, E_GW_JUNK_SETTING_PERSISTENCE);
	if (days && *days)
		gtk_spin_button_set_value (GTK_SPIN_BUTTON (tab->days), g_ascii_strtod (days, NULL));

	for (ii = 0; ii < data->entries->len; ii++) {
		EGwJunkEntry *entry = data->entries->pdata[ii];

		gtk_list_store_insert_with_values (tab->lists[entry->list], NULL, -1,
			COL_MATCH, entry->match, COL_ID, entry->id, -1);
	}

	junk_free_data (data);
}

static void
changed_setting (JunkTab *tab,
		 GHashTable *changes,
		 const gchar *field,
		 const gchar *value)
{
	if (g_strcmp0 (setting (tab, field), value) != 0)
		g_hash_table_insert (changes, g_strdup (field), g_strdup (value));
}

static void
junk_free_changes (gpointer ptr)
{
	JunkChanges *changes = ptr;

	g_hash_table_destroy (changes->settings);
	g_ptr_array_unref (changes->removed);
	g_ptr_array_unref (changes->added);
	g_free (changes);
}

static gpointer
junk_collect (gpointer ptr)
{
	JunkTab *tab = ptr;
	JunkChanges *changes = g_new0 (JunkChanges, 1);
	gchar *days;
	guint list, ii;

	changes->settings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	changes->removed = g_ptr_array_new_with_free_func (g_free);
	changes->added = g_ptr_array_new_with_free_func (g_free);

#define ACTIVE(w) (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w)) ? "1" : "0")
	changed_setting (tab, changes->settings, E_GW_JUNK_SETTING_USE_JUNK_LIST, ACTIVE (tab->use_junk));
	changed_setting (tab, changes->settings, E_GW_JUNK_SETTING_USE_BLOCK_LIST, ACTIVE (tab->use_block));
	changed_setting (tab, changes->settings, E_GW_JUNK_SETTING_USE_PAB, ACTIVE (tab->use_pab));
	changed_setting (tab, changes->settings, E_GW_JUNK_SETTING_ICAL_USE_PAB, ACTIVE (tab->ical_pab));
	changed_setting (tab, changes->settings, E_GW_JUNK_SETTING_AUTO_DELETE, ACTIVE (tab->auto_delete));
#undef ACTIVE
	days = g_strdup_printf ("%d", gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (tab->days)));
	changed_setting (tab, changes->settings, E_GW_JUNK_SETTING_PERSISTENCE, days);
	g_free (days);

	for (ii = 0; ii < tab->removed->len; ii++)
		g_ptr_array_add (changes->removed, g_strdup (tab->removed->pdata[ii]));
	for (list = 0; list < G_N_ELEMENTS (tab->lists); list++) {
		GtkTreeModel *model = GTK_TREE_MODEL (tab->lists[list]);
		GtkTreeIter iter;
		gboolean valid;

		for (valid = gtk_tree_model_get_iter_first (model, &iter); valid; valid = gtk_tree_model_iter_next (model, &iter)) {
			gchar *match = NULL, *id = NULL;

			gtk_tree_model_get (model, &iter, COL_MATCH, &match, COL_ID, &id, -1);
			if (!id)
				g_ptr_array_add (changes->added, g_strdup_printf ("%u\n%s", list, match));
			g_free (match);
			g_free (id);
		}
	}

	if (g_hash_table_size (changes->settings) == 0 && changes->removed->len == 0 && changes->added->len == 0) {
		junk_free_changes (changes);
		return NULL;
	}

	return changes;
}

static gboolean
junk_apply_sync (gpointer ptr,
		 EGwConnection *cnc,
		 GCancellable *cancellable,
		 GError **error)
{
	JunkChanges *changes = ptr;
	gboolean success = TRUE;
	guint ii;

	if (g_hash_table_size (changes->settings) > 0)
		success = e_gw_connection_modify_junk_settings_sync (cnc, changes->settings, cancellable, error);
	for (ii = 0; success && ii < changes->removed->len; ii++)
		success = e_gw_connection_remove_junk_entry_sync (cnc, changes->removed->pdata[ii], cancellable, error);
	for (ii = 0; success && ii < changes->added->len; ii++) {
		const gchar *spec = changes->added->pdata[ii];
		const gchar *match = strchr (spec, '\n') + 1;

		/* An address has an "@", anything else is a domain */
		success = e_gw_connection_put_junk_entry_sync (cnc, match, !strchr (match, '@'),
			(EGwJunkList) (spec[0] - '0'), NULL, cancellable, error);
	}

	return success;
}

const EGroupwiseSettingsTab *
e_groupwise_junk_tab (void)
{
	static const EGroupwiseSettingsTab tab = {
		N_("Junk Mail"),
		junk_new_tab,
		junk_get_widget,
		junk_load_sync,
		junk_fill,
		junk_collect,
		junk_apply_sync,
		junk_free_data,
		junk_free_changes,
		junk_free_tab
	};

	return &tab;
}

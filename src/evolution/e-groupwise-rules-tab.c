/*
 * e-groupwise-rules-tab.c: the rules in the GroupWise settings
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
 * The rules of the mailbox, as in the GroupWise client (Tools > Rules):
 * switched on and off, edited, copied, removed, put in order, run. The
 * changes go to GroupWise on OK; running works on the rules as saved.
 * Two settings of the account: whether Evolution runs the rules of the
 * client's events (startup and exit, folders opened and closed).
 */

#include <glib/gi18n-lib.h>

#include "e-gw-backend-utils.h"
#include "e-gw-category.h"
#include "e-gw-folder.h"
#include "e-gw-rule.h"

#include "e-groupwise-rule-editor.h"
#include "e-groupwise-rule-runner.h"
#include "e-groupwise-settings-window.h"
#include "e-groupwise-ui-utils.h"

enum {
	COL_ENABLED,
	COL_NAME,
	COL_EVENT,
	COL_RULE,	/* EGwRule, owned by the tab */
	N_COLUMNS
};

typedef struct {
	GtkWidget *widget;
	GtkListStore *store;
	GtkWidget *view, *edit_button, *copy_button, *remove_button, *up_button, *down_button, *run_button;
	GtkWidget *run_startup, *run_folders;
	GHashTable *originals;	/* ID -> EGwRule as read */
	GPtrArray *rules;	/* EGwRule, owned: the rows point to them */
	GPtrArray *removed;	/* IDs */
	EGroupwiseRuleContext *context;
	ESourceRegistry *registry;
	ESource *account_source;
	gboolean startup_rules, folder_rules;	/* as read */
	gboolean loaded;
} RulesTab;

typedef struct {
	GPtrArray *folders;
	GPtrArray *categories;
	GPtrArray *rules;
} RulesData;

typedef struct {
	GPtrArray *pairs;	/* old (NULL: new), new; EGwRule copies */
	GPtrArray *removed;	/* IDs */
	ESourceRegistry *registry;
	ESource *account_source;
	gint startup_rules, folder_rules;	/* -1: unchanged */
} RulesChanges;

/* ------------------------------------------------------------------ */

static gboolean
get_selected (RulesTab *tab,
	      GtkTreeIter *iter)
{
	return gtk_tree_selection_get_selected (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), NULL, iter);
}

static EGwRule *
selected_rule (RulesTab *tab,
	       GtkTreeIter *iter)
{
	EGwRule *rule = NULL;

	if (get_selected (tab, iter))
		gtk_tree_model_get (GTK_TREE_MODEL (tab->store), iter, COL_RULE, &rule, -1);

	return rule;
}

static gboolean
rule_changed (RulesTab *tab,
	      const EGwRule *rule)
{
	EGwRule *original = rule->id ? g_hash_table_lookup (tab->originals, rule->id) : NULL;
	gchar *a, *b;
	gboolean changed;

	if (!original)
		return TRUE;

	if (g_strcmp0 (original->name, rule->name) != 0 || original->enabled != rule->enabled ||
	    g_strcmp0 (original->execution, rule->execution) != 0 || g_strcmp0 (original->types, rule->types) != 0 ||
	    g_strcmp0 (original->source, rule->source) != 0 || g_strcmp0 (original->container, rule->container) != 0 ||
	    g_strcmp0 (original->conflict, rule->conflict) != 0)
		return TRUE;

	a = e_gw_filter_node_to_xml (original->filter);
	b = e_gw_filter_node_to_xml (rule->filter);
	changed = g_strcmp0 (a, b) != 0;
	g_free (a);
	g_free (b);
	if (changed)
		return TRUE;

	a = e_gw_rule_actions_to_xml (original->actions);
	b = e_gw_rule_actions_to_xml (rule->actions);
	changed = g_strcmp0 (a, b) != 0;
	g_free (a);
	g_free (b);

	return changed;
}

static void
selection_changed_cb (GtkTreeSelection *selection,
		      RulesTab *tab)
{
	GtkTreeIter iter;
	EGwRule *rule = selected_rule (tab, &iter);
	gint n = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (tab->store), NULL);
	gint index = -1;

	if (rule) {
		GtkTreePath *path = gtk_tree_model_get_path (GTK_TREE_MODEL (tab->store), &iter);

		index = gtk_tree_path_get_indices (path)[0];
		gtk_tree_path_free (path);
	}

	gtk_widget_set_sensitive (tab->edit_button, rule != NULL);
	gtk_widget_set_sensitive (tab->copy_button, rule && e_groupwise_rule_is_editable (rule));
	gtk_widget_set_sensitive (tab->remove_button, rule != NULL);
	gtk_widget_set_sensitive (tab->up_button, rule && index > 0);
	gtk_widget_set_sensitive (tab->down_button, rule && index >= 0 && index + 1 < n);
	/* Runs what GroupWise has: only a saved, unchanged rule */
	gtk_widget_set_sensitive (tab->run_button, rule && rule->id && !rule_changed (tab, rule));
}

static void
set_row (RulesTab *tab,
	 GtkTreeIter *iter,
	 EGwRule *rule)
{
	gchar *event = e_groupwise_rule_dup_event_label (rule, tab->context);

	gtk_list_store_set (tab->store, iter,
		COL_ENABLED, rule->enabled,
		COL_NAME, rule->name,
		COL_EVENT, event,
		COL_RULE, rule,
		-1);
	g_free (event);
}

static void
enabled_toggled_cb (GtkCellRendererToggle *renderer,
		    gchar *path,
		    RulesTab *tab)
{
	GtkTreeIter iter;
	EGwRule *rule = NULL;

	if (!gtk_tree_model_get_iter_from_string (GTK_TREE_MODEL (tab->store), &iter, path))
		return;

	gtk_tree_model_get (GTK_TREE_MODEL (tab->store), &iter, COL_RULE, &rule, -1);
	rule->enabled = !rule->enabled;
	set_row (tab, &iter, rule);
	selection_changed_cb (NULL, tab);
}

static void
edit_rule (RulesTab *tab)
{
	GtkTreeIter iter;
	EGwRule *rule = selected_rule (tab, &iter), *edited;

	if (!rule)
		return;

	edited = e_groupwise_rule_editor_run (GTK_WINDOW (gtk_widget_get_toplevel (tab->widget)), rule, tab->context);
	if (edited) {
		g_ptr_array_remove (tab->rules, rule);
		g_ptr_array_add (tab->rules, edited);
		set_row (tab, &iter, edited);
		selection_changed_cb (NULL, tab);
	}
}

static void
edit_clicked_cb (GtkButton *button,
		 RulesTab *tab)
{
	edit_rule (tab);
}

static void
row_activated_cb (GtkTreeView *view,
		  GtkTreePath *path,
		  GtkTreeViewColumn *column,
		  RulesTab *tab)
{
	edit_rule (tab);
}

static void
insert_rule (RulesTab *tab,
	     EGwRule *rule,
	     GtkTreeIter *after)
{
	GtkTreeIter iter;

	g_ptr_array_add (tab->rules, rule);
	gtk_list_store_insert_after (tab->store, &iter, after);
	set_row (tab, &iter, rule);
	gtk_tree_selection_select_iter (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), &iter);
}

static void
new_clicked_cb (GtkButton *button,
		RulesTab *tab)
{
	EGwRule *rule = e_groupwise_rule_editor_run (GTK_WINDOW (gtk_widget_get_toplevel (tab->widget)), NULL, tab->context);
	GtkTreeIter iter;

	if (rule) {
		/* New rules are switched on, as in the GroupWise client */
		rule->enabled = TRUE;
		insert_rule (tab, rule, get_selected (tab, &iter) ? &iter : NULL);
	}
}

static void
copy_clicked_cb (GtkButton *button,
		 RulesTab *tab)
{
	GtkTreeIter iter;
	EGwRule *rule = selected_rule (tab, &iter), *copy;

	if (!rule)
		return;

	copy = e_gw_rule_copy (rule);
	g_clear_pointer (&copy->id, g_free);
	g_free (copy->name);
	/* Translators: the name of a copied rule; %s is the name of the rule */
	copy->name = g_strdup_printf (_("Copy of %s"), rule->name ? rule->name : "");
	insert_rule (tab, copy, &iter);
}

static void
remove_clicked_cb (GtkButton *button,
		   RulesTab *tab)
{
	GtkTreeIter iter;
	EGwRule *rule = selected_rule (tab, &iter);

	if (!rule)
		return;

	if (rule->id)
		g_ptr_array_add (tab->removed, g_strdup (rule->id));
	gtk_list_store_remove (tab->store, &iter);
	g_ptr_array_remove (tab->rules, rule);
	selection_changed_cb (NULL, tab);
}

static void
move_selected (RulesTab *tab,
	       gboolean up)
{
	GtkTreeIter iter, other;

	if (!get_selected (tab, &iter))
		return;

	other = iter;
	if (up ? gtk_tree_model_iter_previous (GTK_TREE_MODEL (tab->store), &other) :
		 gtk_tree_model_iter_next (GTK_TREE_MODEL (tab->store), &other))
		gtk_list_store_swap (tab->store, &iter, &other);
	selection_changed_cb (NULL, tab);
}

static void
up_clicked_cb (GtkButton *button,
	       RulesTab *tab)
{
	move_selected (tab, TRUE);
}

static void
down_clicked_cb (GtkButton *button,
		 RulesTab *tab)
{
	move_selected (tab, FALSE);
}

static void
run_done_cb (GObject *source_object,
	     GAsyncResult *result,
	     gpointer user_data)
{
	GtkWidget *toplevel = user_data;
	GError *error = NULL;

	if (!e_groupwise_rule_runner_run_finish (result, &error)) {
		GtkWidget *message = gtk_message_dialog_new (GTK_WINDOW (toplevel), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, _("The rule cannot be run: %s"), error->message);

		gtk_dialog_run (GTK_DIALOG (message));
		gtk_widget_destroy (message);
		g_clear_error (&error);
	}
	g_object_unref (toplevel);
}

static void
run_clicked_cb (GtkButton *button,
		RulesTab *tab)
{
	GtkTreeIter iter;
	EGwRule *rule = selected_rule (tab, &iter);
	GtkWidget *toplevel = gtk_widget_get_toplevel (tab->widget), *dialog;
	gint response;

	if (!rule || !rule->id)
		return;

	/* As GroupWise Web asks */
	dialog = gtk_message_dialog_new (GTK_WINDOW (toplevel), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, _("Run the rule “%s” now?"), rule->name ? rule->name : "");
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
		_("GroupWise applies it to the items of your mailbox at once; what it does cannot be undone here."));
	gtk_dialog_add_buttons (GTK_DIALOG (dialog), _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Run"), GTK_RESPONSE_OK, NULL);
	response = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
	if (response != GTK_RESPONSE_OK)
		return;

	e_groupwise_rule_runner_run (tab->registry, tab->account_source, rule->id, NULL, run_done_cb, g_object_ref (toplevel));
}

static GtkWidget *
add_button (GtkWidget *box,
	    const gchar *label,
	    GCallback callback,
	    RulesTab *tab)
{
	GtkWidget *button = gtk_button_new_with_mnemonic (label);

	g_signal_connect (button, "clicked", callback, tab);
	gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);

	return button;
}

static gpointer
rules_new_tab (void)
{
	RulesTab *tab = g_new0 (RulesTab, 1);
	GtkWidget *box, *label, *scrolled, *hbox, *buttons;
	GtkCellRenderer *renderer;

	tab->originals = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) e_gw_rule_free);
	tab->rules = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_rule_free);
	tab->removed = g_ptr_array_new_with_free_func (g_free);

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	tab->widget = g_object_ref_sink (box);

	label = gtk_label_new (_("GroupWise runs these rules in the given order when their event happens, for every "
		"client. The changes go to GroupWise on OK."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_pack_start (GTK_BOX (box), hbox, TRUE, TRUE, 0);

	tab->store = gtk_list_store_new (N_COLUMNS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);
	tab->view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (tab->store));
	renderer = gtk_cell_renderer_toggle_new ();
	g_signal_connect (renderer, "toggled", G_CALLBACK (enabled_toggled_cb), tab);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("On"),
		renderer, "active", COL_ENABLED, NULL);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("Name"),
		gtk_cell_renderer_text_new (), "text", COL_NAME, NULL);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("Event"),
		gtk_cell_renderer_text_new (), "text", COL_EVENT, NULL);
	g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), "changed",
		G_CALLBACK (selection_changed_cb), tab);
	g_signal_connect (tab->view, "row-activated", G_CALLBACK (row_activated_cb), tab);

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_container_add (GTK_CONTAINER (scrolled), tab->view);
	gtk_box_pack_start (GTK_BOX (hbox), scrolled, TRUE, TRUE, 0);

	buttons = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	add_button (buttons, _("_New…"), G_CALLBACK (new_clicked_cb), tab);
	tab->edit_button = add_button (buttons, _("_Edit…"), G_CALLBACK (edit_clicked_cb), tab);
	tab->copy_button = add_button (buttons, _("C_opy"), G_CALLBACK (copy_clicked_cb), tab);
	tab->remove_button = add_button (buttons, _("_Remove"), G_CALLBACK (remove_clicked_cb), tab);
	tab->up_button = add_button (buttons, _("_Up"), G_CALLBACK (up_clicked_cb), tab);
	tab->down_button = add_button (buttons, _("Do_wn"), G_CALLBACK (down_clicked_cb), tab);
	tab->run_button = add_button (buttons, _("R_un…"), G_CALLBACK (run_clicked_cb), tab);
	gtk_box_pack_start (GTK_BOX (hbox), buttons, FALSE, FALSE, 0);

	/* The client's events, which GroupWise leaves to the client */
	tab->run_startup = gtk_check_button_new_with_mnemonic (
		_("Evolution runs the rules for _startup and exit when it starts and quits"));
	gtk_box_pack_start (GTK_BOX (box), tab->run_startup, FALSE, FALSE, 0);
	tab->run_folders = gtk_check_button_new_with_mnemonic (
		_("Evolution runs the rules for opening and closing _folders"));
	gtk_box_pack_start (GTK_BOX (box), tab->run_folders, FALSE, FALSE, 0);

	selection_changed_cb (NULL, tab);

	return tab;
}

static GtkWidget *
rules_get_widget (gpointer tab)
{
	return ((RulesTab *) tab)->widget;
}

static void
rules_set_account (gpointer ptr,
		   ESourceRegistry *registry,
		   ESource *account_source)
{
	RulesTab *tab = ptr;
	CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (registry, account_source);

	tab->registry = g_object_ref (registry);
	tab->account_source = g_object_ref (account_source);
	if (settings) {
		tab->startup_rules = camel_groupwise_settings_get_run_startup_rules (settings);
		tab->folder_rules = camel_groupwise_settings_get_run_folder_rules (settings);
		g_object_unref (settings);
	}
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tab->run_startup), tab->startup_rules);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tab->run_folders), tab->folder_rules);
}

static void
rules_free_tab (gpointer ptr)
{
	RulesTab *tab = ptr;

	g_hash_table_destroy (tab->originals);
	g_ptr_array_unref (tab->rules);
	g_ptr_array_unref (tab->removed);
	e_groupwise_rule_context_free (tab->context);
	g_clear_object (&tab->registry);
	g_clear_object (&tab->account_source);
	g_object_unref (tab->store);
	g_object_unref (tab->widget);
	g_free (tab);
}

/* ------------------------------------------------------------------ */

static void
rules_free_data (gpointer ptr)
{
	RulesData *data = ptr;

	g_clear_pointer (&data->folders, g_ptr_array_unref);
	g_clear_pointer (&data->categories, g_ptr_array_unref);
	g_clear_pointer (&data->rules, g_ptr_array_unref);
	g_free (data);
}

static gpointer
rules_load_sync (EGwConnection *cnc,
		 GCancellable *cancellable,
		 GError **error)
{
	RulesData *data = g_new0 (RulesData, 1);

	data->rules = e_gw_connection_get_rules_sync (cnc, cancellable, error);
	if (!data->rules) {
		rules_free_data (data);
		return NULL;
	}
	/* For the editor; without them it offers less */
	data->folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, cancellable, NULL);
	data->categories = e_gw_connection_get_categories_sync (cnc, cancellable, NULL);

	return data;
}

static void
rules_fill (gpointer ptr,
	    gpointer data_ptr)
{
	RulesTab *tab = ptr;
	RulesData *data = data_ptr;
	guint ii;

	tab->context = e_groupwise_rule_context_new (data->folders, data->categories);
	for (ii = 0; ii < data->rules->len; ii++) {
		EGwRule *rule = data->rules->pdata[ii];
		GtkTreeIter iter;

		g_hash_table_insert (tab->originals, g_strdup (rule->id), e_gw_rule_copy (rule));
		rule = e_gw_rule_copy (rule);
		g_ptr_array_add (tab->rules, rule);
		gtk_list_store_append (tab->store, &iter);
		set_row (tab, &iter, rule);
	}
	tab->loaded = TRUE;
	selection_changed_cb (NULL, tab);

	rules_free_data (data);
}

static void
rules_free_changes (gpointer ptr)
{
	RulesChanges *changes = ptr;

	g_ptr_array_unref (changes->pairs);
	g_ptr_array_unref (changes->removed);
	g_clear_object (&changes->registry);
	g_clear_object (&changes->account_source);
	g_free (changes);
}

static gpointer
rules_collect (gpointer ptr)
{
	RulesTab *tab = ptr;
	RulesChanges *changes = g_new0 (RulesChanges, 1);
	GtkTreeModel *model = GTK_TREE_MODEL (tab->store);
	GtkTreeIter iter;
	gboolean valid, startup, folders;
	gint sequence = 0;
	guint ii;

	changes->pairs = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_rule_free);
	changes->removed = g_ptr_array_new_with_free_func (g_free);
	changes->registry = tab->registry ? g_object_ref (tab->registry) : NULL;
	changes->account_source = tab->account_source ? g_object_ref (tab->account_source) : NULL;

	startup = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (tab->run_startup));
	folders = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (tab->run_folders));
	changes->startup_rules = startup != tab->startup_rules ? startup : -1;
	changes->folder_rules = folders != tab->folder_rules ? folders : -1;

	if (tab->loaded) {
		for (ii = 0; ii < tab->removed->len; ii++)
			g_ptr_array_add (changes->removed, g_strdup (tab->removed->pdata[ii]));

		/* The order of the list is the order of the rules */
		for (valid = gtk_tree_model_get_iter_first (model, &iter); valid; valid = gtk_tree_model_iter_next (model, &iter)) {
			EGwRule *rule = NULL, *original;

			gtk_tree_model_get (model, &iter, COL_RULE, &rule, -1);
			rule->sequence = sequence++;
			original = rule->id ? g_hash_table_lookup (tab->originals, rule->id) : NULL;
			if (original && original->sequence == rule->sequence && !rule_changed (tab, rule))
				continue;
			g_ptr_array_add (changes->pairs, original ? e_gw_rule_copy (original) : NULL);
			g_ptr_array_add (changes->pairs, e_gw_rule_copy (rule));
		}
	}

	if (!changes->pairs->len && !changes->removed->len && changes->startup_rules < 0 && changes->folder_rules < 0) {
		rules_free_changes (changes);
		return NULL;
	}

	return changes;
}

/* The two settings of the account (its collection, or the mail account) */
static void
save_settings (RulesChanges *changes)
{
	const gchar *extension_name = e_source_camel_get_extension_name ("groupwise");
	ESource *source = e_source_registry_find_extension (changes->registry, changes->account_source, extension_name);
	CamelSettings *settings;
	GError *error = NULL;

	if (!source)
		return;

	settings = e_source_camel_get_settings (e_source_get_extension (source, extension_name));
	if (changes->startup_rules >= 0)
		camel_groupwise_settings_set_run_startup_rules (CAMEL_GROUPWISE_SETTINGS (settings), changes->startup_rules);
	if (changes->folder_rules >= 0)
		camel_groupwise_settings_set_run_folder_rules (CAMEL_GROUPWISE_SETTINGS (settings), changes->folder_rules);
	if (!e_source_write_sync (source, NULL, &error)) {
		g_warning ("GroupWise: cannot store %s: %s", e_source_get_uid (source), error->message);
		g_clear_error (&error);
	}
	g_object_unref (source);
}

static gboolean
rules_apply_sync (gpointer ptr,
		  EGwConnection *cnc,
		  GCancellable *cancellable,
		  GError **error)
{
	RulesChanges *changes = ptr;
	gboolean success = TRUE;
	guint ii;

	for (ii = 0; success && ii < changes->removed->len; ii++)
		success = e_gw_connection_remove_rule_sync (cnc, changes->removed->pdata[ii], cancellable, error);
	for (ii = 0; success && ii + 1 < changes->pairs->len; ii += 2) {
		EGwRule *old_rule = changes->pairs->pdata[ii], *new_rule = changes->pairs->pdata[ii + 1];

		if (old_rule) {
			success = e_gw_connection_modify_rule_sync (cnc, old_rule, new_rule, cancellable, error);
		} else {
			gchar *id = e_gw_connection_create_rule_sync (cnc, new_rule, cancellable, error);

			success = id != NULL;
			g_free (id);
		}
	}

	if (success && (changes->startup_rules >= 0 || changes->folder_rules >= 0) && changes->registry)
		save_settings (changes);
	/* The runner reads the rules anew */
	if (success && changes->account_source)
		e_groupwise_rule_runner_forget (e_source_get_uid (changes->account_source));

	return success;
}

const EGroupwiseSettingsTab *
e_groupwise_rules_tab (void)
{
	static const EGroupwiseSettingsTab tab = {
		N_("Rules"),
		rules_new_tab,
		rules_get_widget,
		rules_load_sync,
		rules_fill,
		rules_collect,
		rules_apply_sync,
		rules_free_data,
		rules_free_changes,
		rules_free_tab,
		rules_set_account
	};

	return &tab;
}

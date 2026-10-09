/*
 * e-groupwise-signatures-tab.c: the signatures in the GroupWise settings
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
 * The signatures of the mailbox, as the registry keeps them below the
 * account's collection. Adding, editing and removing work at once with
 * Evolution's signature editor (e-groupwise-signature-sync.c sends the
 * changes to GroupWise); the default goes to GroupWise and to the identity
 * of the account on OK.
 */

#include <glib/gi18n-lib.h>
#include <e-util/e-util.h>

#include "e-gw-backend-utils.h"
#include "e-gw-signature.h"
#include "e-source-groupwise-folder.h"

#include "e-groupwise-settings-window.h"

enum {
	COL_NAME,
	COL_UID,
	COL_GW_ID,	/* NULL: not yet in GroupWise */
	COL_DEFAULT,
	N_COLUMNS
};

typedef struct {
	GtkWidget *widget;
	GtkListStore *store;
	GtkWidget *view, *edit_button, *remove_button, *default_button;
	ESourceRegistry *registry;
	gchar *collection_uid;
	gchar *default_id;	/* the default in GroupWise as read */
	gchar *wanted_default;	/* GroupWise ID chosen here */
	gulong handlers[3];
	gboolean loaded;
} SignaturesTab;

typedef struct {
	ESourceRegistry *registry;
	gchar *collection_uid;
	gchar *default_id;
	gchar *default_uid;
} SignaturesChanges;

/* ------------------------------------------------------------------ */

static gboolean
get_selected (SignaturesTab *tab,
	      GtkTreeIter *iter)
{
	return gtk_tree_selection_get_selected (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), NULL, iter);
}

static ESource *
ref_selected_source (SignaturesTab *tab)
{
	GtkTreeIter iter;
	gchar *uid = NULL;
	ESource *source = NULL;

	if (get_selected (tab, &iter)) {
		gtk_tree_model_get (GTK_TREE_MODEL (tab->store), &iter, COL_UID, &uid, -1);
		source = uid ? e_source_registry_ref_source (tab->registry, uid) : NULL;
		g_free (uid);
	}

	return source;
}

static void
selection_changed_cb (GtkTreeSelection *selection,
		      SignaturesTab *tab)
{
	GtkTreeIter iter;
	gchar *gw_id = NULL;
	gboolean selected = get_selected (tab, &iter);

	if (selected)
		gtk_tree_model_get (GTK_TREE_MODEL (tab->store), &iter, COL_GW_ID, &gw_id, -1);

	gtk_widget_set_sensitive (tab->edit_button, selected);
	gtk_widget_set_sensitive (tab->remove_button, selected);
	/* A signature not yet in GroupWise cannot be its default */
	gtk_widget_set_sensitive (tab->default_button, selected && gw_id);
	g_free (gw_id);
}

/* The rows from the registry: the signatures below the collection */
static void
refill (SignaturesTab *tab)
{
	GList *sources, *link;
	gchar *selected_uid = NULL;
	GtkTreeIter iter;

	if (!tab->collection_uid || !tab->loaded)
		return;

	if (get_selected (tab, &iter))
		gtk_tree_model_get (GTK_TREE_MODEL (tab->store), &iter, COL_UID, &selected_uid, -1);
	gtk_list_store_clear (tab->store);

	sources = e_source_registry_list_sources (tab->registry, E_SOURCE_EXTENSION_MAIL_SIGNATURE);
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;
		gchar *gw_id = NULL;

		if (g_strcmp0 (e_source_get_parent (source), tab->collection_uid) != 0)
			continue;
		if (e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
			gw_id = e_source_groupwise_folder_dup_id (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
		if (gw_id && !*gw_id)
			g_clear_pointer (&gw_id, g_free);

		gtk_list_store_insert_with_values (tab->store, &iter, -1,
			COL_NAME, e_source_get_display_name (source),
			COL_UID, e_source_get_uid (source),
			COL_GW_ID, gw_id,
			COL_DEFAULT, gw_id && g_strcmp0 (gw_id, tab->wanted_default) == 0,
			-1);
		if (g_strcmp0 (selected_uid, e_source_get_uid (source)) == 0)
			gtk_tree_selection_select_iter (gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view)), &iter);
		g_free (gw_id);
	}
	g_list_free_full (sources, g_object_unref);
	g_free (selected_uid);

	selection_changed_cb (NULL, tab);
}

static void
registry_changed_cb (ESourceRegistry *registry,
		     ESource *source,
		     SignaturesTab *tab)
{
	if (e_source_has_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE) &&
	    g_strcmp0 (e_source_get_parent (source), tab->collection_uid) == 0)
		refill (tab);
}

static void
editor_ready_cb (GObject *source_object,
		 GAsyncResult *result,
		 gpointer user_data)
{
	GError *error = NULL;
	GtkWidget *editor = e_mail_signature_editor_new_finish (result, &error);

	if (!editor) {
		g_warning ("GroupWise: signature editor: %s", error ? error->message : "?");
		g_clear_error (&error);
		return;
	}

	gtk_window_set_position (GTK_WINDOW (editor), GTK_WIN_POS_CENTER);
	gtk_widget_show (editor);
}

static void
add_clicked_cb (GtkButton *button,
		SignaturesTab *tab)
{
	ESource *source = e_source_new (NULL, NULL, NULL);

	/* Below the collection: then it goes to GroupWise */
	e_source_set_parent (source, tab->collection_uid);
	e_source_mail_signature_set_mime_type (e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE), "text/html");
	e_mail_signature_editor_new (tab->registry, source, editor_ready_cb, NULL);
	g_object_unref (source);
}

static void
edit_clicked_cb (GtkButton *button,
		 SignaturesTab *tab)
{
	ESource *source = ref_selected_source (tab);

	if (source) {
		e_mail_signature_editor_new (tab->registry, source, editor_ready_cb, NULL);
		g_object_unref (source);
	}
}

static void
remove_done_cb (GObject *source_object,
		GAsyncResult *result,
		gpointer user_data)
{
	GError *error = NULL;

	if (!e_source_remove_finish (E_SOURCE (source_object), result, &error)) {
		g_warning ("GroupWise: cannot remove the signature: %s", error->message);
		g_clear_error (&error);
	}
}

static void
remove_clicked_cb (GtkButton *button,
		   SignaturesTab *tab)
{
	ESource *source = ref_selected_source (tab);
	GtkWidget *dialog;

	if (!source)
		return;

	dialog = gtk_message_dialog_new (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (button))),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
		_("Remove the signature “%s” from GroupWise?"), e_source_get_display_name (source));
	gtk_dialog_add_buttons (GTK_DIALOG (dialog), _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Remove"), GTK_RESPONSE_OK, NULL);
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK)
		e_source_remove (source, NULL, remove_done_cb, NULL);
	gtk_widget_destroy (dialog);
	g_object_unref (source);
}

static void
default_clicked_cb (GtkButton *button,
		    SignaturesTab *tab)
{
	GtkTreeIter iter;
	gchar *gw_id = NULL;

	if (!get_selected (tab, &iter))
		return;

	gtk_tree_model_get (GTK_TREE_MODEL (tab->store), &iter, COL_GW_ID, &gw_id, -1);
	g_free (tab->wanted_default);
	tab->wanted_default = gw_id;
	refill (tab);
}

static GtkWidget *
add_button (GtkWidget *buttons,
	    const gchar *label,
	    GCallback callback,
	    SignaturesTab *tab)
{
	GtkWidget *button = gtk_button_new_with_mnemonic (label);

	g_signal_connect (button, "clicked", callback, tab);
	gtk_container_add (GTK_CONTAINER (buttons), button);

	return button;
}

static gpointer
signatures_new_tab (void)
{
	SignaturesTab *tab = g_new0 (SignaturesTab, 1);
	GtkWidget *box, *label, *scrolled, *buttons;
	GtkCellRenderer *renderer;

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	tab->widget = g_object_ref_sink (box);

	label = gtk_label_new (_("These signatures are kept in GroupWise and apply to every client. Changes made here "
		"or in Evolution's signature preferences go to GroupWise. Evolution adds the default signature "
		"to new messages of this account."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	tab->store = gtk_list_store_new (N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
	tab->view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (tab->store));
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("Name"),
		gtk_cell_renderer_text_new (), "text", COL_NAME, NULL);
	renderer = gtk_cell_renderer_toggle_new ();
	gtk_cell_renderer_toggle_set_radio (GTK_CELL_RENDERER_TOGGLE (renderer), TRUE);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("Default"),
		renderer, "active", COL_DEFAULT, NULL);
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
	add_button (buttons, _("A_dd…"), G_CALLBACK (add_clicked_cb), tab);
	tab->edit_button = add_button (buttons, _("_Edit…"), G_CALLBACK (edit_clicked_cb), tab);
	tab->remove_button = add_button (buttons, _("_Remove"), G_CALLBACK (remove_clicked_cb), tab);
	tab->default_button = add_button (buttons, _("Make _Default"), G_CALLBACK (default_clicked_cb), tab);
	gtk_box_pack_start (GTK_BOX (box), buttons, FALSE, FALSE, 0);
	selection_changed_cb (NULL, tab);

	return tab;
}

static GtkWidget *
signatures_get_widget (gpointer tab)
{
	return ((SignaturesTab *) tab)->widget;
}

static void
signatures_set_account (gpointer ptr,
			ESourceRegistry *registry,
			ESource *account_source)
{
	SignaturesTab *tab = ptr;
	ESource *collection = e_source_registry_find_extension (registry, account_source, E_SOURCE_EXTENSION_COLLECTION);

	e_gw_backend_ensure_types ();
	tab->registry = g_object_ref (registry);
	if (collection) {
		tab->collection_uid = g_strdup (e_source_get_uid (collection));
		g_object_unref (collection);
	} else {
		gtk_widget_hide (tab->widget);
	}
	tab->handlers[0] = g_signal_connect (registry, "source-added", G_CALLBACK (registry_changed_cb), tab);
	tab->handlers[1] = g_signal_connect (registry, "source-changed", G_CALLBACK (registry_changed_cb), tab);
	tab->handlers[2] = g_signal_connect (registry, "source-removed", G_CALLBACK (registry_changed_cb), tab);
}

static void
signatures_free_tab (gpointer ptr)
{
	SignaturesTab *tab = ptr;
	guint ii;

	for (ii = 0; tab->registry && ii < G_N_ELEMENTS (tab->handlers); ii++)
		g_signal_handler_disconnect (tab->registry, tab->handlers[ii]);
	g_clear_object (&tab->registry);
	g_free (tab->collection_uid);
	g_free (tab->default_id);
	g_free (tab->wanted_default);
	g_object_unref (tab->store);
	g_object_unref (tab->widget);
	g_free (tab);
}

/* ------------------------------------------------------------------ */

static gpointer
signatures_load_sync (EGwConnection *cnc,
		      GCancellable *cancellable,
		      GError **error)
{
	return e_gw_connection_get_signatures_sync (cnc, cancellable, error);
}

static void
signatures_free_data (gpointer data)
{
	e_gw_signatures_free (data);
}

static void
signatures_fill (gpointer ptr,
		 gpointer data)
{
	SignaturesTab *tab = ptr;
	EGwSignatures *signatures = data;
	guint ii;

	for (ii = 0; ii < signatures->signatures->len; ii++) {
		EGwSignature *signature = signatures->signatures->pdata[ii];

		if (signature->is_default)
			tab->default_id = g_strdup (signature->id);
	}
	tab->wanted_default = g_strdup (tab->default_id);
	tab->loaded = TRUE;
	refill (tab);

	e_gw_signatures_free (signatures);
}

static void
signatures_free_changes (gpointer ptr)
{
	SignaturesChanges *changes = ptr;

	g_object_unref (changes->registry);
	g_free (changes->collection_uid);
	g_free (changes->default_id);
	g_free (changes->default_uid);
	g_free (changes);
}

static gpointer
signatures_collect (gpointer ptr)
{
	SignaturesTab *tab = ptr;
	SignaturesChanges *changes;
	GtkTreeModel *model = GTK_TREE_MODEL (tab->store);
	GtkTreeIter iter;
	gboolean valid;

	if (!tab->loaded || !tab->wanted_default || g_strcmp0 (tab->wanted_default, tab->default_id) == 0)
		return NULL;

	changes = g_new0 (SignaturesChanges, 1);
	changes->registry = g_object_ref (tab->registry);
	changes->collection_uid = g_strdup (tab->collection_uid);
	changes->default_id = g_strdup (tab->wanted_default);
	for (valid = gtk_tree_model_get_iter_first (model, &iter); valid; valid = gtk_tree_model_iter_next (model, &iter)) {
		gchar *gw_id = NULL, *uid = NULL;

		gtk_tree_model_get (model, &iter, COL_GW_ID, &gw_id, COL_UID, &uid, -1);
		if (g_strcmp0 (gw_id, tab->wanted_default) == 0)
			changes->default_uid = g_steal_pointer (&uid);
		g_free (gw_id);
		g_free (uid);
	}

	return changes;
}

static gboolean
signatures_apply_sync (gpointer ptr,
		       EGwConnection *cnc,
		       GCancellable *cancellable,
		       GError **error)
{
	SignaturesChanges *changes = ptr;
	GList *sources, *link;

	if (!e_gw_connection_modify_signature_sync (cnc, changes->default_id, NULL, NULL, 1, cancellable, error))
		return FALSE;

	/* ... and the signature of the account's identity */
	sources = e_source_registry_list_sources (changes->registry, E_SOURCE_EXTENSION_MAIL_IDENTITY);
	for (link = sources; link && changes->default_uid; link = g_list_next (link)) {
		ESource *identity = link->data;
		GError *local_error = NULL;

		if (g_strcmp0 (e_source_get_parent (identity), changes->collection_uid) != 0)
			continue;
		e_source_mail_identity_set_signature_uid (e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY),
			changes->default_uid);
		if (!e_source_write_sync (identity, cancellable, &local_error)) {
			g_warning ("GroupWise: cannot store %s: %s", e_source_get_uid (identity), local_error->message);
			g_clear_error (&local_error);
		}
	}
	g_list_free_full (sources, g_object_unref);

	return TRUE;
}

const EGroupwiseSettingsTab *
e_groupwise_signatures_tab (void)
{
	static const EGroupwiseSettingsTab tab = {
		N_("Signatures"),
		signatures_new_tab,
		signatures_get_widget,
		signatures_load_sync,
		signatures_fill,
		signatures_collect,
		signatures_apply_sync,
		signatures_free_data,
		signatures_free_changes,
		signatures_free_tab,
		signatures_set_account
	};

	return &tab;
}

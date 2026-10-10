/*
 * e-groupwise-name-selector.c: the dialog to pick recipients searches the chosen address books
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

/* The dialog behind "To:", "Cc:" and "Attendees…" lists one address book,
 * the one picked at its top, and starts with Evolution's default address
 * book. The GroupWise client searches the address books the user chose for
 * that. Evolution has such a choice, too — the address books marked for
 * autocompletion (Preferences, Contacts), which typing a name searches —
 * but not in this dialog: here its list of address books gets a first
 * entry "Automatic", selected whenever the dialog opens, which lists and
 * searches all those address books at once. Picking an address book works
 * as before. */

#include <glib/gi18n-lib.h>

#include <libebook/libebook.h>
#include <e-util/e-util.h>

#include "e-groupwise-name-selector.h"

/* The ID of the entry; no address book has it */
#define AUTOMATIC_UID "groupwise-automatic"

/* The columns of an ESourceComboBox (not public) */
enum {
	COLUMN_COLOR,
	COLUMN_NAME,
	COLUMN_FULL_NAME,
	COLUMN_SENSITIVE,
	COLUMN_UID,
	N_COLUMNS
};

typedef struct _EGroupwiseNameSelector {
	EExtension parent;

	GtkComboBox *combo;		/* the dialog's; not referenced */
	GCancellable *cancellable;	/* of the address books being opened */
	gboolean automatic;		/* the entry is the one selected */
	gboolean restore;		/* ... and was when the list was built anew */
	guint idle;
} EGroupwiseNameSelector;

typedef struct _EGroupwiseNameSelectorClass {
	EExtensionClass parent_class;
} EGroupwiseNameSelectorClass;

GType e_groupwise_name_selector_get_type (void);

G_DEFINE_DYNAMIC_TYPE (EGroupwiseNameSelector, e_groupwise_name_selector, E_TYPE_EXTENSION)

static void
find_combo_cb (GtkWidget *widget,
	       gpointer user_data)
{
	GtkComboBox **combo = user_data;

	if (E_IS_CLIENT_COMBO_BOX (widget))
		*combo = GTK_COMBO_BOX (widget);
	else if (!*combo && GTK_IS_CONTAINER (widget))
		gtk_container_forall (GTK_CONTAINER (widget), find_combo_cb, combo);
}

/* The list of the combo box, when it is what this was written for */
static GtkListStore *
get_list (EGroupwiseNameSelector *self)
{
	GtkTreeModel *model = self->combo ? gtk_combo_box_get_model (self->combo) : NULL;

	if (!model || !GTK_IS_LIST_STORE (model) || gtk_tree_model_get_n_columns (model) != N_COLUMNS ||
	    gtk_tree_model_get_column_type (model, COLUMN_NAME) != G_TYPE_STRING ||
	    gtk_tree_model_get_column_type (model, COLUMN_FULL_NAME) != G_TYPE_STRING ||
	    gtk_tree_model_get_column_type (model, COLUMN_SENSITIVE) != G_TYPE_BOOLEAN ||
	    gtk_tree_model_get_column_type (model, COLUMN_UID) != G_TYPE_STRING ||
	    gtk_combo_box_get_id_column (self->combo) != COLUMN_UID)
		return NULL;

	return GTK_LIST_STORE (model);
}

/* The entry is the first of the list (again) */
static gboolean
ensure_entry (EGroupwiseNameSelector *self)
{
	GtkListStore *list = get_list (self);
	GtkTreeIter iter;
	gchar *uid = NULL;
	gboolean there;

	if (!list)
		return FALSE;

	if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (list), &iter))
		gtk_tree_model_get (GTK_TREE_MODEL (list), &iter, COLUMN_UID, &uid, -1);
	there = g_strcmp0 (uid, AUTOMATIC_UID) == 0;
	g_free (uid);
	if (!there) {
		/* Translators: the entry in the list of address books that
		 * searches all address books marked for autocompletion */
		const gchar *name = _("Automatic");

		gtk_list_store_insert_with_values (list, NULL, 0,
			COLUMN_NAME, name,
			COLUMN_FULL_NAME, name,
			COLUMN_SENSITIVE, TRUE,
			COLUMN_UID, AUTOMATIC_UID,
			-1);
	}

	return TRUE;
}

static EContactStore *
get_contact_store (EGroupwiseNameSelector *self)
{
	ENameSelectorDialog *dialog = E_NAME_SELECTOR_DIALOG (e_extension_get_extensible (E_EXTENSION (self)));
	ENameSelectorModel *model = e_name_selector_dialog_peek_model (dialog);

	return model ? e_name_selector_model_peek_contact_store (model) : NULL;
}

typedef struct {
	GWeakRef self;
	GCancellable *cancellable;
} OpenData;

static void
book_opened_cb (GObject *source_object,
		GAsyncResult *result,
		gpointer user_data)
{
	OpenData *data = user_data;
	EGroupwiseNameSelector *self = g_weak_ref_get (&data->self);
	EClient *client = e_client_combo_box_get_client_finish (E_CLIENT_COMBO_BOX (source_object), result, NULL);

	/* Still wanted: the selection may have changed meanwhile */
	if (client && self && self->automatic && self->cancellable == data->cancellable &&
	    !g_cancellable_is_cancelled (data->cancellable)) {
		EContactStore *store = get_contact_store (self);

		if (store)
			e_contact_store_add_client (store, E_BOOK_CLIENT (client));
	}

	g_clear_object (&client);
	g_clear_object (&self);
	g_weak_ref_clear (&data->self);
	g_object_unref (data->cancellable);
	g_free (data);
}

/* The address books marked for autocompletion, all in the list at once */
static void
load_automatic (EGroupwiseNameSelector *self)
{
	ENameSelectorDialog *dialog = E_NAME_SELECTOR_DIALOG (e_extension_get_extensible (E_EXTENSION (self)));
	EContactStore *store = get_contact_store (self);
	EClientCache *client_cache;
	ESourceRegistry *registry;
	GSList *clients, *slink;
	GList *sources, *link;

	if (self->cancellable) {
		g_cancellable_cancel (self->cancellable);
		g_clear_object (&self->cancellable);
	}
	if (!store)
		return;

	clients = e_contact_store_get_clients (store);
	for (slink = clients; slink; slink = g_slist_next (slink))
		e_contact_store_remove_client (store, slink->data);
	g_slist_free (clients);

	self->cancellable = g_cancellable_new ();
	client_cache = e_name_selector_dialog_ref_client_cache (dialog);
	registry = e_client_cache_ref_registry (client_cache);
	sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;
		OpenData *data;

		if (!e_source_has_extension (source, E_SOURCE_EXTENSION_AUTOCOMPLETE) ||
		    !e_source_autocomplete_get_include_me (e_source_get_extension (source, E_SOURCE_EXTENSION_AUTOCOMPLETE)))
			continue;

		data = g_new0 (OpenData, 1);
		g_weak_ref_init (&data->self, self);
		data->cancellable = g_object_ref (self->cancellable);
		e_client_combo_box_get_client (E_CLIENT_COMBO_BOX (self->combo), source, self->cancellable,
			book_opened_cb, data);
	}
	g_list_free_full (sources, g_object_unref);
	g_object_unref (registry);
	g_object_unref (client_cache);
}

/* After the dialog's own handler, which has emptied the list for an entry
 * that is no address book */
static void
changed_cb (GtkComboBox *combo,
	    gpointer user_data)
{
	EGroupwiseNameSelector *self = user_data;

	/* The list is being built anew: not the user's choice */
	if (self->idle)
		return;

	self->automatic = g_strcmp0 (gtk_combo_box_get_active_id (combo), AUTOMATIC_UID) == 0;
	if (self->automatic) {
		load_automatic (self);
	} else if (self->cancellable) {
		g_cancellable_cancel (self->cancellable);
		g_clear_object (&self->cancellable);
	}
}

static gboolean
rebuilt_idle_cb (gpointer user_data)
{
	EGroupwiseNameSelector *self = user_data;

	self->idle = 0;
	if (ensure_entry (self) && self->restore)
		gtk_combo_box_set_active_id (self->combo, AUTOMATIC_UID);
	/* Whatever is selected now is the choice */
	changed_cb (self->combo, self);

	return G_SOURCE_REMOVE;
}

/* Evolution builds the list anew when an address book comes or goes: the
 * entry is gone with it */
static void
row_deleted_cb (GtkTreeModel *model,
		GtkTreePath *path,
		gpointer user_data)
{
	EGroupwiseNameSelector *self = user_data;

	if (!self->idle) {
		self->restore = self->automatic;
		self->idle = g_idle_add (rebuilt_idle_cb, self);
	}
}

/* Opened (again): the search over the chosen address books */
static void
show_cb (GtkWidget *dialog,
	 gpointer user_data)
{
	EGroupwiseNameSelector *self = user_data;

	if (self->idle || !ensure_entry (self))
		return;

	if (g_strcmp0 (gtk_combo_box_get_active_id (self->combo), AUTOMATIC_UID) != 0) {
		gtk_combo_box_set_active_id (self->combo, AUTOMATIC_UID);
	} else {
		self->automatic = TRUE;
		load_automatic (self);
	}
}

static void
e_groupwise_name_selector_constructed (GObject *object)
{
	EGroupwiseNameSelector *self = (EGroupwiseNameSelector *) object;
	GtkWidget *dialog;

	G_OBJECT_CLASS (e_groupwise_name_selector_parent_class)->constructed (object);

	dialog = GTK_WIDGET (e_extension_get_extensible (E_EXTENSION (object)));
	gtk_container_forall (GTK_CONTAINER (dialog), find_combo_cb, &self->combo);
	if (!get_list (self)) {
		self->combo = NULL;
		return;
	}

	g_signal_connect_object (self->combo, "changed", G_CALLBACK (changed_cb), self, G_CONNECT_AFTER);
	g_signal_connect_object (gtk_combo_box_get_model (self->combo), "row-deleted", G_CALLBACK (row_deleted_cb), self, 0);
	g_signal_connect_object (dialog, "show", G_CALLBACK (show_cb), self, 0);
}

static void
e_groupwise_name_selector_dispose (GObject *object)
{
	EGroupwiseNameSelector *self = (EGroupwiseNameSelector *) object;

	if (self->idle) {
		g_source_remove (self->idle);
		self->idle = 0;
	}
	if (self->cancellable) {
		g_cancellable_cancel (self->cancellable);
		g_clear_object (&self->cancellable);
	}

	G_OBJECT_CLASS (e_groupwise_name_selector_parent_class)->dispose (object);
}

static void
e_groupwise_name_selector_class_init (EGroupwiseNameSelectorClass *class)
{
	G_OBJECT_CLASS (class)->constructed = e_groupwise_name_selector_constructed;
	G_OBJECT_CLASS (class)->dispose = e_groupwise_name_selector_dispose;
	E_EXTENSION_CLASS (class)->extensible_type = E_TYPE_NAME_SELECTOR_DIALOG;
}

static void
e_groupwise_name_selector_class_finalize (EGroupwiseNameSelectorClass *class)
{
}

static void
e_groupwise_name_selector_init (EGroupwiseNameSelector *self)
{
}

void
e_groupwise_name_selector_type_register (GTypeModule *type_module)
{
	e_groupwise_name_selector_register_type (type_module);
}

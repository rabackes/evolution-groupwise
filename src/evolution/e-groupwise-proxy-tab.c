/*
 * e-groupwise-proxy-tab.c: the proxy accounts in the GroupWise settings
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
 * The users who granted the user proxy rights (getProxyList). Where the
 * GroupWise client switches the whole window to the other mailbox, Evolution
 * shows it as an account of its own beside the user's: a collection with the
 * login of the user's account and the setting Proxy, and below it a mail
 * account, an identity with the other user's address and a transport. The
 * registry fills the collection with the other user's calendars, task and
 * memo lists and personal address books. It logs in with the user's
 * password (stored for the proxy account as well), as the other user's
 * proxy.
 *
 * A collection of its own because Evolution takes every child of a
 * collection for the collection (renaming, enabling, removing act on it).
 * Proxy accounts of version 0.3 (children of the user's collection) and 0.4
 * (mail accounts of their own) are made anew on OK.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-backend-utils.h"

#include "e-groupwise-settings-window.h"
#include "e-groupwise-ui-utils.h"

enum {
	COL_NAME,
	COL_EMAIL,
	COL_ACCOUNT_UID,	/* the proxy account there is, or NULL */
	COL_WANTED,		/* an account after OK */
	COL_GRANTED,		/* in the proxy list of the server */
	COL_LEGACY,		/* the account is a child of the collection */
	COL_STATE,
	N_COLUMNS
};

typedef struct {
	GtkWidget *widget;
	GtkListStore *store;
	GtkWidget *view, *add_button, *remove_button;
	gboolean is_proxy;		/* the window of a proxy account */
	ESourceRegistry *registry;
	ESource *account_source;
} ProxyTab;

typedef struct {
	ESourceRegistry *registry;
	ESource *account_source;
	GPtrArray *added;	/* "email\nname" */
	GPtrArray *removed;	/* account UIDs */
} ProxyChanges;

/* ------------------------------------------------------------------ */

static void
update_row (ProxyTab *tab,
	    GtkTreeIter *iter)
{
	GtkTreeModel *model = GTK_TREE_MODEL (tab->store);
	gchar *account_uid = NULL;
	gboolean wanted, granted, legacy;
	const gchar *state;

	gtk_tree_model_get (model, iter, COL_ACCOUNT_UID, &account_uid, COL_WANTED, &wanted, COL_GRANTED, &granted,
		COL_LEGACY, &legacy, -1);
	if (account_uid && wanted && legacy && granted)
		state = _("Account is made anew on OK");
	else if (account_uid && wanted)
		state = granted ? _("Account") : _("Account (no longer granted)");
	else if (account_uid)
		state = _("Account is removed on OK");
	else if (wanted)
		state = _("Account is added on OK");
	else
		state = "";
	gtk_list_store_set (tab->store, iter, COL_STATE, state, -1);
	g_free (account_uid);
}

static void
selection_changed_cb (GtkTreeSelection *selection,
		      ProxyTab *tab)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gboolean wanted = FALSE, granted = FALSE, selected;

	selected = gtk_tree_selection_get_selected (selection, &model, &iter);
	if (selected)
		gtk_tree_model_get (model, &iter, COL_WANTED, &wanted, COL_GRANTED, &granted, -1);

	gtk_widget_set_sensitive (tab->add_button, selected && !wanted && granted);
	gtk_widget_set_sensitive (tab->remove_button, selected && wanted);
}

static void
set_wanted (ProxyTab *tab,
	    gboolean wanted)
{
	GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view));
	GtkTreeIter iter;

	if (!gtk_tree_selection_get_selected (selection, NULL, &iter))
		return;

	gtk_list_store_set (tab->store, &iter, COL_WANTED, wanted, -1);
	update_row (tab, &iter);
	selection_changed_cb (selection, tab);
}

static void
add_clicked_cb (GtkButton *button,
		ProxyTab *tab)
{
	set_wanted (tab, TRUE);
}

static void
remove_clicked_cb (GtkButton *button,
		   ProxyTab *tab)
{
	set_wanted (tab, FALSE);
}

static gpointer
proxy_new_tab (void)
{
	ProxyTab *tab = g_new0 (ProxyTab, 1);
	GtkWidget *box, *label, *scrolled, *buttons;
	GtkTreeSelection *selection;

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	tab->widget = g_object_ref_sink (box);

	label = gtk_label_new (_("These users have granted you proxy access to their mailboxes. "
		"Added as an account, a mailbox appears beside your own, in the folder list in grey, "
		"with its calendars (not shown until you tick them), task and memo lists and address books. "
		"You read it and send in its name with the rights its user granted you."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	tab->store = gtk_list_store_new (N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING);
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (tab->store), COL_NAME, GTK_SORT_ASCENDING);
	tab->view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (tab->store));
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("Name"),
		gtk_cell_renderer_text_new (), "text", COL_NAME, NULL);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("E-mail address"),
		gtk_cell_renderer_text_new (), "text", COL_EMAIL, NULL);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tab->view), -1, _("In Evolution"),
		gtk_cell_renderer_text_new (), "text", COL_STATE, NULL);
	gtk_tree_view_set_search_column (GTK_TREE_VIEW (tab->view), COL_NAME);
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tab->view));
	g_signal_connect (selection, "changed", G_CALLBACK (selection_changed_cb), tab);

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_container_add (GTK_CONTAINER (scrolled), tab->view);
	gtk_box_pack_start (GTK_BOX (box), scrolled, TRUE, TRUE, 0);

	buttons = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_button_box_set_layout (GTK_BUTTON_BOX (buttons), GTK_BUTTONBOX_START);
	gtk_box_set_spacing (GTK_BOX (buttons), 6);
	tab->add_button = gtk_button_new_with_mnemonic (_("_Add as Account"));
	g_signal_connect (tab->add_button, "clicked", G_CALLBACK (add_clicked_cb), tab);
	gtk_container_add (GTK_CONTAINER (buttons), tab->add_button);
	tab->remove_button = gtk_button_new_with_mnemonic (_("_Remove Account"));
	g_signal_connect (tab->remove_button, "clicked", G_CALLBACK (remove_clicked_cb), tab);
	gtk_container_add (GTK_CONTAINER (buttons), tab->remove_button);
	gtk_box_pack_start (GTK_BOX (box), buttons, FALSE, FALSE, 0);
	selection_changed_cb (selection, tab);

	return tab;
}

static GtkWidget *
proxy_get_widget (gpointer tab)
{
	return ((ProxyTab *) tab)->widget;
}

static void
proxy_set_account (gpointer ptr,
		   ESourceRegistry *registry,
		   ESource *account_source)
{
	ProxyTab *tab = ptr;

	CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (registry, account_source);
	gchar *proxy = settings ? camel_groupwise_settings_dup_proxy (settings) : NULL;

	g_clear_object (&settings);
	tab->registry = g_object_ref (registry);
	tab->account_source = g_object_ref (account_source);

	/* A proxy account has no proxy accounts of its own */
	tab->is_proxy = proxy != NULL;
	if (tab->is_proxy)
		gtk_widget_hide (tab->widget);
	g_free (proxy);
}

static void
proxy_free_tab (gpointer ptr)
{
	ProxyTab *tab = ptr;

	g_clear_object (&tab->registry);
	g_clear_object (&tab->account_source);
	g_object_unref (tab->store);
	g_object_unref (tab->widget);
	g_free (tab);
}

/* ------------------------------------------------------------------ */

/* The collection the accounts of the user hang below */
static ESource *
ref_collection (ESourceRegistry *registry,
		ESource *account_source)
{
	return e_source_registry_find_extension (registry, account_source, E_SOURCE_EXTENSION_COLLECTION);
}

static gboolean
same_login (ESource *a,
	    ESource *b)
{
	ESourceAuthentication *aa, *ab;
	gchar *ha, *hb, *ua, *ub;
	gboolean same;

	if (!e_source_has_extension (a, E_SOURCE_EXTENSION_AUTHENTICATION) ||
	    !e_source_has_extension (b, E_SOURCE_EXTENSION_AUTHENTICATION))
		return FALSE;

	aa = e_source_get_extension (a, E_SOURCE_EXTENSION_AUTHENTICATION);
	ab = e_source_get_extension (b, E_SOURCE_EXTENSION_AUTHENTICATION);
	ha = e_source_authentication_dup_host (aa);
	hb = e_source_authentication_dup_host (ab);
	ua = e_source_authentication_dup_user (aa);
	ub = e_source_authentication_dup_user (ab);
	same = ha && hb && ua && ub && !g_ascii_strcasecmp (ha, hb) && !g_ascii_strcasecmp (ua, ub);
	g_free (ha);
	g_free (hb);
	g_free (ua);
	g_free (ub);

	return same;
}

/* The proxy accounts of the user (the same login as the collection):
 * e-mail address (folded) -> the collection of the account, or the mail
 * account of one made by version 0.3 or 0.4 */
static GHashTable *
list_proxy_accounts (ESourceRegistry *registry,
		     ESource *account_source)
{
	GHashTable *accounts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
	ESource *collection = ref_collection (registry, account_source);
	GList *sources, *link;

	if (!collection)
		return accounts;

	/* Older ones first: a collection of the same user replaces them */
	sources = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_MAIL_ACCOUNT);
	sources = g_list_concat (sources, e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_COLLECTION));
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;
		gchar *proxy;

		if (source == collection ||
		    (g_strcmp0 (e_source_get_parent (source), e_source_get_uid (collection)) != 0 &&
		     !same_login (source, collection)))
			continue;
		proxy = e_groupwise_ui_dup_proxy (source);
		if (proxy)
			g_hash_table_insert (accounts, g_utf8_casefold (proxy, -1), g_object_ref (source));
		g_free (proxy);
	}
	g_list_free_full (sources, g_object_unref);
	g_object_unref (collection);

	return accounts;
}

/* A proxy account of version 0.3 or 0.4: a mail account, no collection */
static gboolean
is_legacy (ESource *account)
{
	return !e_source_has_extension (account, E_SOURCE_EXTENSION_COLLECTION);
}

static gpointer
proxy_load_sync (EGwConnection *cnc,
		 GCancellable *cancellable,
		 GError **error)
{
	/* The window of a proxy account: the tab is hidden */
	if (e_gw_connection_get_proxy (cnc))
		return g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_proxy_user_free);

	return e_gw_connection_get_proxy_users_sync (cnc, cancellable, error);
}

static void
proxy_free_data (gpointer data)
{
	g_ptr_array_unref (data);
}

static void
proxy_fill (gpointer ptr,
	    gpointer data)
{
	ProxyTab *tab = ptr;
	GPtrArray *users = data;
	GHashTable *accounts = list_proxy_accounts (tab->registry, tab->account_source);
	GHashTableIter hash_iter;
	gpointer key, value;
	GtkTreeIter iter;
	guint ii;

	if (tab->is_proxy) {
		g_hash_table_destroy (accounts);
		g_ptr_array_unref (users);
		return;
	}

	for (ii = 0; ii < users->len; ii++) {
		EGwProxyUser *user = users->pdata[ii];
		gchar *key = g_utf8_casefold (user->email, -1);
		ESource *account = g_hash_table_lookup (accounts, key);

		gtk_list_store_insert_with_values (tab->store, &iter, -1,
			COL_NAME, user->display_name,
			COL_EMAIL, user->email,
			COL_ACCOUNT_UID, account ? e_source_get_uid (account) : NULL,
			COL_WANTED, account != NULL,
			COL_GRANTED, TRUE,
			COL_LEGACY, account && is_legacy (account),
			-1);
		update_row (tab, &iter);
		g_hash_table_remove (accounts, key);
		g_free (key);
	}

	/* Accounts of users who took the rights back: to be removed here */
	g_hash_table_iter_init (&hash_iter, accounts);
	while (g_hash_table_iter_next (&hash_iter, &key, &value)) {
		ESource *account = value;
		gchar *proxy = e_groupwise_ui_dup_proxy (account);

		gtk_list_store_insert_with_values (tab->store, &iter, -1,
			COL_NAME, e_source_get_display_name (account),
			COL_EMAIL, proxy,
			COL_ACCOUNT_UID, e_source_get_uid (account),
			COL_WANTED, TRUE,
			COL_GRANTED, FALSE,
			COL_LEGACY, is_legacy (account),
			-1);
		update_row (tab, &iter);
		g_free (proxy);
	}

	g_hash_table_destroy (accounts);
	g_ptr_array_unref (users);
}

/* ------------------------------------------------------------------ */

static void
proxy_free_changes (gpointer ptr)
{
	ProxyChanges *changes = ptr;

	g_object_unref (changes->registry);
	g_object_unref (changes->account_source);
	g_ptr_array_unref (changes->added);
	g_ptr_array_unref (changes->removed);
	g_free (changes);
}

static gpointer
proxy_collect (gpointer ptr)
{
	ProxyTab *tab = ptr;
	GtkTreeModel *model = GTK_TREE_MODEL (tab->store);
	ProxyChanges *changes;
	GtkTreeIter iter;
	gboolean valid;

	if (!tab->registry || tab->is_proxy)
		return NULL;

	changes = g_new0 (ProxyChanges, 1);
	changes->registry = g_object_ref (tab->registry);
	changes->account_source = g_object_ref (tab->account_source);
	changes->added = g_ptr_array_new_with_free_func (g_free);
	changes->removed = g_ptr_array_new_with_free_func (g_free);

	for (valid = gtk_tree_model_get_iter_first (model, &iter); valid; valid = gtk_tree_model_iter_next (model, &iter)) {
		gchar *name = NULL, *email = NULL, *account_uid = NULL;
		gboolean wanted, granted, legacy;

		gtk_tree_model_get (model, &iter, COL_NAME, &name, COL_EMAIL, &email,
			COL_ACCOUNT_UID, &account_uid, COL_WANTED, &wanted, COL_GRANTED, &granted,
			COL_LEGACY, &legacy, -1);
		if (wanted && (!account_uid || (legacy && granted)))
			g_ptr_array_add (changes->added, g_strdup_printf ("%s\n%s", email, name));
		if (account_uid && (!wanted || (legacy && granted)))
			g_ptr_array_add (changes->removed, g_steal_pointer (&account_uid));
		g_free (name);
		g_free (email);
		g_free (account_uid);
	}

	if (changes->added->len == 0 && changes->removed->len == 0) {
		proxy_free_changes (changes);
		return NULL;
	}

	return changes;
}

/* Login data as with the collection: the proxy accounts use its password */
static void
copy_login (ESource *collection,
	    ESource *source)
{
	ESourceAuthentication *from = e_source_get_extension (collection, E_SOURCE_EXTENSION_AUTHENTICATION);
	ESourceAuthentication *to = e_source_get_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION);
	gchar *value;

	value = e_source_authentication_dup_host (from);
	e_source_authentication_set_host (to, value);
	g_free (value);
	e_source_authentication_set_port (to, e_source_authentication_get_port (from));
	value = e_source_authentication_dup_user (from);
	e_source_authentication_set_user (to, value);
	g_free (value);
	value = e_source_authentication_dup_method (from);
	e_source_authentication_set_method (to, value);
	g_free (value);

	if (e_source_has_extension (collection, E_SOURCE_EXTENSION_SECURITY)) {
		value = e_source_security_dup_method (e_source_get_extension (collection, E_SOURCE_EXTENSION_SECURITY));
		e_source_security_set_method (e_source_get_extension (source, E_SOURCE_EXTENSION_SECURITY), value);
		g_free (value);
	}
}

static void
set_proxy (ESource *source,
	   const gchar *email)
{
	ESourceExtension *extension = e_source_get_extension (source, e_source_camel_get_extension_name ("groupwise"));

	camel_groupwise_settings_set_proxy (CAMEL_GROUPWISE_SETTINGS (
		e_source_camel_get_settings (E_SOURCE_CAMEL (extension))), email);
}

static ESource *
new_source (ESource *parent,
	    const gchar *display_name)
{
	ESource *source = e_source_new (NULL, NULL, NULL);

	if (parent)
		e_source_set_parent (source, e_source_get_uid (parent));
	e_source_set_display_name (source, display_name);

	return source;
}

static gboolean
add_account_sync (ESourceRegistry *registry,
		  ESource *collection,
		  ESource *account_source,
		  const gchar *spec,
		  GCancellable *cancellable,
		  GError **error)
{
	const gchar *newline = strchr (spec, '\n');
	gchar *email = g_strndup (spec, newline - spec);
	const gchar *name = newline + 1;
	ESource *proxy_collection, *account, *identity, *transport;
	ESourceCollection *collection_extension;
	ESourceMailSubmission *submission;
	gchar *display_name, *password = NULL, *user;
	GList *sources;
	gboolean success;

	/* Translators: the name of a proxy account in the folder list; %s is
	 * the name of the user whose mailbox it shows */
	display_name = g_strdup_printf (_("%s (Proxy)"), name);

	/* The account: the login of the user's, the other mailbox */
	proxy_collection = new_source (NULL, display_name);
	collection_extension = e_source_get_extension (proxy_collection, E_SOURCE_EXTENSION_COLLECTION);
	e_source_backend_set_backend_name (E_SOURCE_BACKEND (collection_extension), "groupwise");
	user = e_source_authentication_dup_user (e_source_get_extension (collection, E_SOURCE_EXTENSION_AUTHENTICATION));
	e_source_collection_set_identity (collection_extension, user);
	g_free (user);
	e_source_collection_set_mail_enabled (collection_extension, TRUE);
	e_source_collection_set_calendar_enabled (collection_extension, TRUE);
	e_source_collection_set_contacts_enabled (collection_extension, TRUE);
	copy_login (collection, proxy_collection);
	set_proxy (proxy_collection, email);
	/* The trust in the server certificate, as confirmed for the user's account */
	if (e_source_has_extension (collection, E_SOURCE_EXTENSION_WEBDAV_BACKEND)) {
		gchar *trust = e_source_webdav_dup_ssl_trust (e_source_get_extension (collection, E_SOURCE_EXTENSION_WEBDAV_BACKEND));

		e_source_webdav_set_ssl_trust (e_source_get_extension (proxy_collection, E_SOURCE_EXTENSION_WEBDAV_BACKEND),
			trust ? trust : "");
		g_free (trust);
	}

	transport = new_source (proxy_collection, display_name);
	e_source_backend_set_backend_name (e_source_get_extension (transport, E_SOURCE_EXTENSION_MAIL_TRANSPORT), "groupwise");

	identity = new_source (proxy_collection, display_name);
	e_source_mail_identity_set_name (e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY), name);
	e_source_mail_identity_set_address (e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY), email);
	submission = e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION);
	e_source_mail_submission_set_transport_uid (submission, e_source_get_uid (transport));
	/* GroupWise keeps the sent message in the Sent Items of the mailbox */
	e_source_mail_submission_set_use_sent_folder (submission, FALSE);

	account = new_source (proxy_collection, display_name);
	e_source_backend_set_backend_name (e_source_get_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT), "groupwise");
	e_source_mail_account_set_identity_uid (e_source_get_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT),
		e_source_get_uid (identity));
	e_source_mail_account_set_needs_initial_setup (e_source_get_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT), FALSE);
	/* Checked for new mail like the own account */
	if (e_source_has_extension (account_source, E_SOURCE_EXTENSION_REFRESH)) {
		ESourceRefresh *from = e_source_get_extension (account_source, E_SOURCE_EXTENSION_REFRESH);
		ESourceRefresh *to = e_source_get_extension (account, E_SOURCE_EXTENSION_REFRESH);

		e_source_refresh_set_enabled (to, e_source_refresh_get_enabled (from));
		e_source_refresh_set_interval_minutes (to, e_source_refresh_get_interval_minutes (from));
	}

	/* The password of the user, for the proxy account; stored first, so
	 * that Evolution and the registry find it when they open it */
	if (e_source_lookup_password_sync (collection, cancellable, &password, NULL) && password && *password)
		e_source_store_password_sync (proxy_collection, password, TRUE, cancellable, NULL);
	e_util_safe_free_string (password);

	sources = g_list_append (NULL, proxy_collection);
	sources = g_list_append (sources, transport);
	sources = g_list_append (sources, identity);
	sources = g_list_append (sources, account);
	success = e_source_registry_create_sources_sync (registry, sources, cancellable, error);
	g_debug ("proxy account for %s: %s", email, success ? e_source_get_uid (proxy_collection) : "failed");

	g_list_free_full (sources, g_object_unref);
	g_free (display_name);
	g_free (email);

	return success;
}

static gboolean
remove_source_sync (ESourceRegistry *registry,
		    const gchar *uid,
		    GCancellable *cancellable,
		    GError **error)
{
	ESource *source = uid ? e_source_registry_ref_source (registry, uid) : NULL;
	gboolean success = TRUE;

	if (source) {
		success = e_source_remove_sync (source, cancellable, error);
		g_object_unref (source);
	}

	return success;
}

/* The account: a collection with its children, or a mail account of
 * version 0.3 or 0.4 with its identity and transport */
static gboolean
remove_account_sync (ESourceRegistry *registry,
		     const gchar *account_uid,
		     GCancellable *cancellable,
		     GError **error)
{
	ESource *account = e_source_registry_ref_source (registry, account_uid);
	ESource *identity = NULL;
	gchar *identity_uid = NULL, *transport_uid = NULL;
	gboolean success;

	if (!account)
		return TRUE;

	/* A collection goes with its children */
	if (!is_legacy (account)) {
		e_source_delete_password_sync (account, cancellable, NULL);
		success = e_source_remove_sync (account, cancellable, error);
		g_debug ("proxy account %s removed: %s", account_uid, success ? "yes" : "failed");
		g_object_unref (account);
		return success;
	}

	identity_uid = e_source_mail_account_dup_identity_uid (e_source_get_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT));
	identity = identity_uid ? e_source_registry_ref_source (registry, identity_uid) : NULL;
	if (identity && e_source_has_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION))
		transport_uid = e_source_mail_submission_dup_transport_uid (
			e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION));

	/* A mail account of its own (0.4) had the password */
	if (!e_source_get_parent (account) || !*e_source_get_parent (account))
		e_source_delete_password_sync (account, cancellable, NULL);
	success = e_source_remove_sync (account, cancellable, error) &&
		remove_source_sync (registry, identity_uid, cancellable, error) &&
		remove_source_sync (registry, transport_uid, cancellable, error);
	g_debug ("proxy account %s removed: %s", account_uid, success ? "yes" : "failed");

	g_clear_object (&identity);
	g_object_unref (account);
	g_free (identity_uid);
	g_free (transport_uid);

	return success;
}

static gboolean
proxy_apply_sync (gpointer ptr,
		  EGwConnection *cnc,
		  GCancellable *cancellable,
		  GError **error)
{
	ProxyChanges *changes = ptr;
	ESource *collection = ref_collection (changes->registry, changes->account_source);
	gboolean success = TRUE;
	guint ii;

	if (!collection) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			_("Proxy accounts need an account made as a GroupWise collection"));
		return FALSE;
	}

	for (ii = 0; success && ii < changes->removed->len; ii++)
		success = remove_account_sync (changes->registry, changes->removed->pdata[ii], cancellable, error);
	for (ii = 0; success && ii < changes->added->len; ii++)
		success = add_account_sync (changes->registry, collection, changes->account_source,
			changes->added->pdata[ii], cancellable, error);

	g_object_unref (collection);

	return success;
}

const EGroupwiseSettingsTab *
e_groupwise_proxy_tab (void)
{
	static const EGroupwiseSettingsTab tab = {
		N_("Proxy Accounts"),
		proxy_new_tab,
		proxy_get_widget,
		proxy_load_sync,
		proxy_fill,
		proxy_collect,
		proxy_apply_sync,
		proxy_free_data,
		proxy_free_changes,
		proxy_free_tab,
		proxy_set_account
	};

	return &tab;
}

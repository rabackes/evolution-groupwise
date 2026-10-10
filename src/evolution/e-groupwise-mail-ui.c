/*
 * e-groupwise-mail-ui.c: GroupWise actions in Evolution's mail view
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
 * "Restore" in the Trash of a GroupWise account: the messages go back where
 * they were deleted from, as "Undelete" in the GroupWise clients. Evolution's
 * own "Undelete" only clears a deletion mark, which GroupWise Trash items do
 * not have. The action flags the messages (CAMEL_GROUPWISE_RESTORE_FLAG of
 * the provider) and synchronizes the folder, the provider does the rest.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>
#include <libemail-engine/libemail-engine.h>
#include <mail/e-mail-backend.h>
#include <mail/e-mail-reader.h>
#include <mail/em-folder-tree.h>
#include <shell/e-shell-sidebar.h>
#include <shell/e-shell-window.h>
#include <shell/e-shell-view.h>

#include "e-gw-junk.h"
#include "e-groupwise-events-port.h"
#include "e-groupwise-proxy-options.h"
#include "e-groupwise-sent-items.h"

#include "e-groupwise-mail-ui.h"
#include "e-groupwise-settings-window.h"
#include "e-groupwise-rule-runner.h"
#include "e-groupwise-signature-sync.h"
#include "e-groupwise-ui-utils.h"

/* As in camel-groupwise-folder.h */
#define RESTORE_FLAG "gw-restore"
#define RESTORE_ACTION "groupwise-mail-restore"
#define SETTINGS_ACTION "groupwise-account-settings"
#define JUNK_MENU "groupwise-junk-menu"
#define JUNK_ACTION "groupwise-junk-sender"
#define BLOCK_ACTION "groupwise-block-sender"
#define TRUST_ACTION "groupwise-trust-sender"
#define JUNK_SETTINGS_ACTION "groupwise-junk-settings"
#define RETRACT_ACTION "groupwise-mail-retract"
#define STATUS_ACTION "groupwise-mail-status"
#define RESEND_ACTION "groupwise-mail-resend"

typedef struct {
	EExtension parent;
} EGroupwiseMailUI;

typedef struct {
	EExtensionClass parent_class;
} EGroupwiseMailUIClass;

GType e_groupwise_mail_ui_get_type (void);

G_DEFINE_DYNAMIC_TYPE (EGroupwiseMailUI, e_groupwise_mail_ui, E_TYPE_EXTENSION)

static EMailReader *
ref_reader (EShellView *shell_view)
{
	EShellContent *shell_content = e_shell_view_get_shell_content (shell_view);
	GObject *mail_view = NULL;

	if (shell_content && g_object_class_find_property (G_OBJECT_GET_CLASS (shell_content), "mail-view"))
		g_object_get (shell_content, "mail-view", &mail_view, NULL);
	if (mail_view && !E_IS_MAIL_READER (mail_view))
		g_clear_object (&mail_view);

	return (EMailReader *) mail_view;
}

/* The folder of the reader when it is the Trash of a writable GroupWise
 * account */
static CamelFolder *
ref_groupwise_trash (EMailReader *reader)
{
	CamelFolder *folder = e_mail_reader_ref_folder (reader);
	CamelFolderInfoFlags flags = 0;
	CamelProvider *provider;
	CamelSettings *settings;
	CamelStore *store;
	EMailSession *session;
	gboolean read_only = FALSE;

	if (!folder)
		return NULL;

	store = camel_folder_get_parent_store (folder);
	provider = store ? camel_service_get_provider (CAMEL_SERVICE (store)) : NULL;
	if (!provider || g_strcmp0 (provider->protocol, "groupwise") != 0) {
		g_object_unref (folder);
		return NULL;
	}

	session = e_mail_backend_get_session (e_mail_reader_get_backend (reader));
	if (!mail_folder_cache_get_folder_info_flags (e_mail_session_get_folder_cache (session), store,
		camel_folder_get_full_name (folder), &flags) ||
	    (flags & CAMEL_FOLDER_TYPE_MASK) != CAMEL_FOLDER_TYPE_TRASH) {
		g_object_unref (folder);
		return NULL;
	}

	settings = camel_service_ref_settings (CAMEL_SERVICE (store));
	if (settings && g_object_class_find_property (G_OBJECT_GET_CLASS (settings), "read-only"))
		g_object_get (settings, "read-only", &read_only, NULL);
	g_clear_object (&settings);
	if (read_only) {
		g_object_unref (folder);
		return NULL;
	}

	return folder;
}

typedef struct {
	EShellView *shell_view;
} RestoreData;

static void
restore_done_cb (GObject *source,
		 GAsyncResult *result,
		 gpointer user_data)
{
	RestoreData *data = user_data;
	GError *error = NULL;

	if (!camel_folder_synchronize_finish (CAMEL_FOLDER (source), result, &error) &&
	    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		EShellContent *shell_content = e_shell_view_get_shell_content (data->shell_view);

		e_alert_submit (E_ALERT_SINK (shell_content), "mail:async-error",
			_("restoring messages"), error ? error->message : "", NULL);
	}

	g_clear_error (&error);
	g_object_unref (data->shell_view);
	g_free (data);
}

static void
action_restore_cb (EUIAction *action,
		   GVariant *parameter,
		   gpointer user_data)
{
	EShellView *shell_view = user_data;
	EMailReader *reader = ref_reader (shell_view);
	CamelFolder *folder = reader ? ref_groupwise_trash (reader) : NULL;
	GPtrArray *uids;
	RestoreData *data;
	guint ii;

	if (!folder) {
		g_clear_object (&reader);
		return;
	}

	uids = e_mail_reader_get_selected_uids (reader);
	for (ii = 0; uids && ii < uids->len; ii++)
		camel_folder_set_message_user_flag (folder, uids->pdata[ii], RESTORE_FLAG, TRUE);

	if (uids && uids->len > 0) {
		data = g_new0 (RestoreData, 1);
		data->shell_view = g_object_ref (shell_view);
		camel_folder_synchronize (folder, FALSE, G_PRIORITY_DEFAULT, NULL, restore_done_cb, data);
	}

	if (uids)
		g_ptr_array_unref (uids);
	g_object_unref (folder);
	g_object_unref (reader);
}

static ESourceRegistry *
shell_view_get_registry (EShellView *shell_view)
{
	EShellBackend *backend = e_shell_view_get_shell_backend (shell_view);

	return e_mail_session_get_registry (e_mail_backend_get_session (E_MAIL_BACKEND (backend)));
}

/* ------------------------------------------------------------------ */
/* GroupWise Settings: the account in the folder list */

/* The account of the GroupWise account node selected in the folder list */
static ESource *
ref_selected_account (EShellView *shell_view)
{
	EShellSidebar *sidebar = e_shell_view_get_shell_sidebar (shell_view);
	EMFolderTree *folder_tree = NULL;
	CamelStore *store = NULL;
	ESource *account = NULL;

	g_object_get (sidebar, "folder-tree", &folder_tree, NULL);
	if (folder_tree && em_folder_tree_store_root_selected (folder_tree, &store) && store)
		account = e_groupwise_ui_ref_account_source (shell_view_get_registry (shell_view), CAMEL_SERVICE (store));
	g_clear_object (&store);
	g_clear_object (&folder_tree);

	return account;
}

static void
action_settings_cb (EUIAction *action,
		    GVariant *parameter,
		    gpointer user_data)
{
	EShellView *shell_view = user_data;
	ESource *account = ref_selected_account (shell_view);

	if (account) {
		e_groupwise_settings_window_show (GTK_WINDOW (e_shell_view_get_shell_window (shell_view)),
			shell_view_get_registry (shell_view), account, NULL);
		g_object_unref (account);
	}
}

/* ------------------------------------------------------------------ */
/* Junk Mail: the senders of messages onto the lists of GroupWise */

/* The folder of the reader when it is one of a writable GroupWise account
 * (of a proxy account: the lists of the other mailbox) */
static CamelFolder *
ref_groupwise_folder (EMailReader *reader)
{
	CamelFolder *folder = e_mail_reader_ref_folder (reader);
	CamelStore *store = folder ? camel_folder_get_parent_store (folder) : NULL;
	CamelProvider *provider = store ? camel_service_get_provider (CAMEL_SERVICE (store)) : NULL;
	CamelSettings *settings;
	gboolean read_only = FALSE;

	if (!provider || g_strcmp0 (provider->protocol, "groupwise") != 0) {
		g_clear_object (&folder);
		return NULL;
	}

	settings = camel_service_ref_settings (CAMEL_SERVICE (store));
	if (settings && g_object_class_find_property (G_OBJECT_GET_CLASS (settings), "read-only"))
		g_object_get (settings, "read-only", &read_only, NULL);
	g_clear_object (&settings);
	if (read_only)
		g_clear_object (&folder);

	return folder;
}

/* The sender addresses of the messages, each once */
static GPtrArray *
dup_senders (CamelFolder *folder,
	     GPtrArray *uids)
{
	GPtrArray *senders = g_ptr_array_new_with_free_func (g_free);
	guint ii;

	for (ii = 0; uids && ii < uids->len; ii++) {
		CamelMessageInfo *info = camel_folder_get_message_info (folder, uids->pdata[ii]);
		CamelInternetAddress *address = camel_internet_address_new ();
		const gchar *email = NULL;

		if (info && camel_address_decode (CAMEL_ADDRESS (address), camel_message_info_get_from (info)) > 0 &&
		    camel_internet_address_get (address, 0, NULL, &email) && email && strchr (email, '@')) {
			guint jj;
			gboolean known = FALSE;

			for (jj = 0; jj < senders->len && !known; jj++)
				known = g_ascii_strcasecmp (senders->pdata[jj], email) == 0;
			if (!known)
				g_ptr_array_add (senders, g_ascii_strdown (email, -1));
		}
		g_object_unref (address);
		g_clear_object (&info);
	}

	return senders;
}

typedef struct {
	EShellView *shell_view;
	CamelFolder *folder;
	GPtrArray *uids;
	GPtrArray *matches;
	gboolean domains;
	EGwJunkList list;
	ESourceRegistry *registry;
	ESource *account;
} JunkData;

static void
junk_data_free (JunkData *data)
{
	g_object_unref (data->shell_view);
	g_object_unref (data->folder);
	g_ptr_array_unref (data->uids);
	g_ptr_array_unref (data->matches);
	g_object_unref (data->registry);
	g_object_unref (data->account);
	g_free (data);
}

static void
junk_thread (GTask *task,
	     gpointer source_object,
	     gpointer task_data,
	     GCancellable *cancellable)
{
	JunkData *data = task_data;
	GPtrArray *entries = NULL;
	EGwConnection *cnc;
	GError *error = NULL;
	gboolean success;
	guint ii;

	cnc = e_groupwise_ui_connect_sync (data->registry, data->account, cancellable, &error);
	if (cnc)
		entries = e_gw_connection_get_junk_entries_sync (cnc, cancellable, &error);
	success = entries != NULL;
	for (ii = 0; success && ii < data->matches->len; ii++)
		success = e_gw_connection_put_junk_entry_sync (cnc, data->matches->pdata[ii], data->domains, data->list,
			entries, cancellable, &error);
	if (entries)
		g_ptr_array_unref (entries);
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
junk_done (GObject *source_object,
	   GAsyncResult *result,
	   gpointer user_data)
{
	JunkData *data = g_task_get_task_data (G_TASK (result));
	GError *error = NULL;
	guint ii;

	if (!g_task_propagate_boolean (G_TASK (result), &error)) {
		e_alert_submit (E_ALERT_SINK (e_shell_view_get_shell_content (data->shell_view)), "mail:async-error",
			_("updating the junk mail lists"), error ? error->message : "", NULL);
		g_clear_error (&error);
		return;
	}

	/* Junk and blocked into the Junk Mail folder, trusted out of it (the
	 * provider moves them; the lists are set, nothing to learn) */
	for (ii = 0; ii < data->uids->len; ii++) {
		if (data->list == E_GW_JUNK_LIST_TRUST)
			camel_folder_set_message_flags (data->folder, data->uids->pdata[ii],
				CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_NOTJUNK, CAMEL_MESSAGE_NOTJUNK);
		else
			camel_folder_set_message_flags (data->folder, data->uids->pdata[ii],
				CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_NOTJUNK, CAMEL_MESSAGE_JUNK);
	}
	camel_folder_synchronize (data->folder, FALSE, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
}

static void
junk_sender (EShellView *shell_view,
	     EGwJunkList list)
{
	static const struct {
		const gchar *title;
		const gchar *explanation;
		const gchar *button;
	} texts[] = {
		{ N_("Junk the Sender"), N_("Future mail goes into the Junk Mail folder; this message goes there now."), N_("_Junk") },
		{ N_("Block the Sender"), N_("Future mail is not delivered; this message goes into the Junk Mail folder."), N_("_Block") },
		{ N_("Trust the Sender"), N_("Mail from the sender is never junk; this message goes back into the Mailbox."), N_("_Trust") }
	};
	EMailReader *reader = ref_reader (shell_view);
	CamelFolder *folder = reader ? ref_groupwise_folder (reader) : NULL;
	GtkWidget *dialog, *box, *label, *by_address, *by_domain;
	GPtrArray *uids = NULL, *senders = NULL;
	ESource *account = NULL;
	gchar *text, *domain;

	if (folder) {
		uids = e_mail_reader_get_selected_uids (reader);
		senders = dup_senders (folder, uids);
		account = e_groupwise_ui_ref_account_source (shell_view_get_registry (shell_view),
			CAMEL_SERVICE (camel_folder_get_parent_store (folder)));
	}
	if (!senders || senders->len == 0 || !account) {
		g_clear_pointer (&uids, g_ptr_array_unref);
		g_clear_pointer (&senders, g_ptr_array_unref);
		g_clear_object (&account);
		g_clear_object (&folder);
		g_clear_object (&reader);
		return;
	}

	dialog = gtk_dialog_new_with_buttons (_(texts[list].title), GTK_WINDOW (e_shell_view_get_shell_window (shell_view)),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		_("_Cancel"), GTK_RESPONSE_CANCEL, _(texts[list].button), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
	box = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_box_set_spacing (GTK_BOX (box), 6);

	label = gtk_label_new (_(texts[list].explanation));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_max_width_chars (GTK_LABEL (label), 55);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

	domain = strchr (senders->pdata[0], '@') + 1;
	if (senders->len == 1)
		text = g_strdup_printf (_("Only the _address %s"), (const gchar *) senders->pdata[0]);
	else
		text = g_strdup_printf (_("Only the _addresses of the %u senders"), senders->len);
	by_address = gtk_radio_button_new_with_mnemonic (NULL, text);
	g_free (text);
	if (senders->len == 1)
		text = g_strdup_printf (_("The whole _domain %s (with its subdomains)"), domain);
	else
		text = g_strdup (_("Their whole _domains (with their subdomains)"));
	by_domain = gtk_radio_button_new_with_mnemonic_from_widget (GTK_RADIO_BUTTON (by_address), text);
	g_free (text);
	gtk_box_pack_start (GTK_BOX (box), by_address, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), by_domain, FALSE, FALSE, 0);

	label = gtk_label_new (_("This applies to Internet mail only; the lists are in the GroupWise Settings "
		"of the account."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_max_width_chars (GTK_LABEL (label), 55);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_widget_set_margin_top (label, 6);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
	gtk_widget_show_all (box);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		JunkData *data = g_new0 (JunkData, 1);
		GTask *task;
		guint ii;

		data->shell_view = g_object_ref (shell_view);
		data->folder = g_object_ref (folder);
		data->uids = g_ptr_array_ref (uids);
		data->list = list;
		data->registry = g_object_ref (shell_view_get_registry (shell_view));
		data->account = g_object_ref (account);
		data->domains = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (by_domain));
		data->matches = g_ptr_array_new_with_free_func (g_free);
		for (ii = 0; ii < senders->len; ii++) {
			const gchar *match = data->domains ? strchr (senders->pdata[ii], '@') + 1 : senders->pdata[ii];
			guint jj;
			gboolean known = FALSE;

			for (jj = 0; jj < data->matches->len && !known; jj++)
				known = g_ascii_strcasecmp (data->matches->pdata[jj], match) == 0;
			if (!known)
				g_ptr_array_add (data->matches, g_strdup (match));
		}

		task = g_task_new (NULL, NULL, junk_done, NULL);
		g_task_set_task_data (task, data, (GDestroyNotify) junk_data_free);
		g_task_run_in_thread (task, junk_thread);
		g_object_unref (task);
	}
	gtk_widget_destroy (dialog);

	g_ptr_array_unref (uids);
	g_ptr_array_unref (senders);
	g_object_unref (account);
	g_object_unref (folder);
	g_object_unref (reader);
}

static void
action_junk_cb (EUIAction *action,
		GVariant *parameter,
		gpointer user_data)
{
	junk_sender (user_data, E_GW_JUNK_LIST_JUNK);
}

static void
action_block_cb (EUIAction *action,
		 GVariant *parameter,
		 gpointer user_data)
{
	junk_sender (user_data, E_GW_JUNK_LIST_BLOCK);
}

static void
action_trust_cb (EUIAction *action,
		 GVariant *parameter,
		 gpointer user_data)
{
	junk_sender (user_data, E_GW_JUNK_LIST_TRUST);
}

static void
action_junk_settings_cb (EUIAction *action,
			 GVariant *parameter,
			 gpointer user_data)
{
	EShellView *shell_view = user_data;
	EMailReader *reader = ref_reader (shell_view);
	CamelFolder *folder = reader ? ref_groupwise_folder (reader) : NULL;
	ESource *account = folder ? e_groupwise_ui_ref_account_source (shell_view_get_registry (shell_view),
		CAMEL_SERVICE (camel_folder_get_parent_store (folder))) : NULL;

	if (account)
		e_groupwise_settings_window_show (GTK_WINDOW (e_shell_view_get_shell_window (shell_view)),
			shell_view_get_registry (shell_view), account, e_groupwise_junk_tab ());
	g_clear_object (&account);
	g_clear_object (&folder);
	g_clear_object (&reader);
}

/* ------------------------------------------------------------------ */
/* Sent Items: delivery status and retract */

/* The folder of the reader when it is the Sent Items of a GroupWise
 * account; @out_read_only: the account is only read */
static CamelFolder *
ref_groupwise_sent (EMailReader *reader,
		    gboolean *out_read_only)
{
	CamelFolder *folder = e_mail_reader_ref_folder (reader);
	CamelFolderInfoFlags flags = 0;
	CamelStore *store = folder ? camel_folder_get_parent_store (folder) : NULL;
	CamelProvider *provider = store ? camel_service_get_provider (CAMEL_SERVICE (store)) : NULL;
	CamelSettings *settings;
	EMailSession *session;
	gboolean read_only = FALSE;

	if (!provider || g_strcmp0 (provider->protocol, "groupwise") != 0) {
		g_clear_object (&folder);
		return NULL;
	}

	session = e_mail_backend_get_session (e_mail_reader_get_backend (reader));
	if (!mail_folder_cache_get_folder_info_flags (e_mail_session_get_folder_cache (session), store,
		camel_folder_get_full_name (folder), &flags) ||
	    (flags & CAMEL_FOLDER_TYPE_MASK) != CAMEL_FOLDER_TYPE_SENT) {
		g_object_unref (folder);
		return NULL;
	}

	settings = camel_service_ref_settings (CAMEL_SERVICE (store));
	if (settings && g_object_class_find_property (G_OBJECT_GET_CLASS (settings), "read-only"))
		g_object_get (settings, "read-only", &read_only, NULL);
	g_clear_object (&settings);
	if (out_read_only)
		*out_read_only = read_only;

	return folder;
}

static gchar *
dup_subject (CamelFolder *folder,
	     const gchar *uid)
{
	CamelMessageInfo *info = camel_folder_get_message_info (folder, uid);
	gchar *subject = info ? g_strdup (camel_message_info_get_subject (info)) : NULL;

	g_clear_object (&info);

	return subject;
}

typedef enum {
	SENT_STATUS,
	SENT_RETRACT,
	SENT_RESEND
} SentAction;

static void
sent_action (EShellView *shell_view,
	     SentAction what)
{
	EMailReader *reader = ref_reader (shell_view);
	CamelFolder *folder = reader ? ref_groupwise_sent (reader, NULL) : NULL;
	ESource *account = folder ? e_groupwise_ui_ref_account_source (shell_view_get_registry (shell_view),
		CAMEL_SERVICE (camel_folder_get_parent_store (folder))) : NULL;
	GPtrArray *uids = account ? e_mail_reader_get_selected_uids (reader) : NULL;

	if (uids && uids->len > 0) {
		GtkWindow *window = GTK_WINDOW (e_shell_view_get_shell_window (shell_view));
		gchar *subject = dup_subject (folder, uids->pdata[0]);

		if (what == SENT_RETRACT)
			e_groupwise_sent_retract (window, E_ALERT_SINK (e_shell_view_get_shell_content (shell_view)),
				shell_view_get_registry (shell_view), account, uids, subject);
		else if (what == SENT_RESEND)
			e_groupwise_sent_resend (e_shell_window_get_shell (e_shell_view_get_shell_window (shell_view)),
				E_ALERT_SINK (e_shell_view_get_shell_content (shell_view)),
				shell_view_get_registry (shell_view), account, folder, uids->pdata[0], subject);
		else
			e_groupwise_sent_status_show (window, shell_view_get_registry (shell_view), account,
				uids->pdata[0], subject);
		g_free (subject);
	}

	g_clear_pointer (&uids, g_ptr_array_unref);
	g_clear_object (&account);
	g_clear_object (&folder);
	g_clear_object (&reader);
}

static void
action_retract_cb (EUIAction *action,
		   GVariant *parameter,
		   gpointer user_data)
{
	sent_action (user_data, SENT_RETRACT);
}

static void
action_resend_cb (EUIAction *action,
		  GVariant *parameter,
		  gpointer user_data)
{
	sent_action (user_data, SENT_RESEND);
}

static void
action_status_cb (EUIAction *action,
		  GVariant *parameter,
		  gpointer user_data)
{
	sent_action (user_data, SENT_STATUS);
}

static void
set_visible (EUIActionGroup *action_group,
	     const gchar *name,
	     gboolean visible)
{
	EUIAction *action = action_group ? e_ui_action_group_get_action (action_group, name) : NULL;

	if (action)
		e_ui_action_set_visible (action, visible);
}

/* The folder the reader shows now: the rules of the folder left and of
 * the folder opened */
static void
folder_loaded_cb (EMailReader *reader,
		  EShellView *shell_view)
{
	CamelFolder *folder = e_mail_reader_ref_folder (reader);
	CamelFolder *left = g_object_get_data (G_OBJECT (reader), "groupwise-rule-folder");

	e_groupwise_rule_runner_folder_changed (shell_view_get_registry (shell_view), left, folder);
	g_object_set_data_full (G_OBJECT (reader), "groupwise-rule-folder", folder, folder ? g_object_unref : NULL);
}

static void
watch_reader (EShellView *shell_view,
	      EMailReader *reader)
{
	if (!reader || g_object_get_data (G_OBJECT (reader), "groupwise-rule-watch"))
		return;

	g_object_set_data (G_OBJECT (reader), "groupwise-rule-watch", GINT_TO_POINTER (1));
	g_signal_connect (reader, "folder-loaded", G_CALLBACK (folder_loaded_cb), shell_view);
}

static void
update_actions_cb (EShellView *shell_view,
		   gpointer user_data)
{
	EUIActionGroup *action_group;
	EMailReader *reader = ref_reader (shell_view);
	CamelFolder *trash = reader ? ref_groupwise_trash (reader) : NULL;
	CamelFolder *folder = reader ? ref_groupwise_folder (reader) : NULL;
	ESource *account = ref_selected_account (shell_view);
	GPtrArray *uids = folder ? e_mail_reader_get_selected_uids (reader) : NULL;
	gboolean sent_read_only = TRUE;
	CamelFolder *sent = reader ? ref_groupwise_sent (reader, &sent_read_only) : NULL;
	guint selected = 0;

	/* The reader exists by now */
	watch_reader (shell_view, reader);

	action_group = e_ui_manager_get_action_group (e_shell_view_get_ui_manager (shell_view), "mail");
	set_visible (action_group, RESTORE_ACTION, trash != NULL);
	set_visible (action_group, SETTINGS_ACTION, account != NULL);
	/* Messages of GroupWise, not in the Trash */
	set_visible (action_group, JUNK_MENU, folder && !trash && uids && uids->len > 0);
	set_visible (action_group, JUNK_ACTION, folder && !trash);
	set_visible (action_group, BLOCK_ACTION, folder && !trash);
	set_visible (action_group, TRUST_ACTION, folder && !trash);
	set_visible (action_group, JUNK_SETTINGS_ACTION, folder && !trash);
	/* Sent Items: what happened to a message, and taking messages back */
	if (sent) {
		GPtrArray *sent_uids = e_mail_reader_get_selected_uids (reader);

		selected = sent_uids ? sent_uids->len : 0;
		g_clear_pointer (&sent_uids, g_ptr_array_unref);
	}
	set_visible (action_group, STATUS_ACTION, sent && selected == 1);
	set_visible (action_group, RETRACT_ACTION, sent && !sent_read_only && selected > 0);
	set_visible (action_group, RESEND_ACTION, sent && !sent_read_only && selected == 1);
	g_clear_object (&sent);

	g_clear_pointer (&uids, g_ptr_array_unref);
	g_clear_object (&account);
	g_clear_object (&folder);
	g_clear_object (&trash);
	g_clear_object (&reader);
}

static EUIElement *
copy_element_deep (EUIElement *elem)
{
	EUIElement *copy = e_ui_element_copy (elem);
	guint ii;

	for (ii = 0; ii < e_ui_element_get_n_children (elem); ii++)
		e_ui_element_add_child (copy, copy_element_deep (e_ui_element_get_child (elem, ii)));

	return copy;
}

/* A merged item lands at the end of the menu; Restore belongs on top. The
 * parser has no way to insert elsewhere, so what was before it moves behind
 * it (as copies, children included). */
static void
move_restore_to_top (EUIManager *ui_manager)
{
	EUIElement *root = e_ui_parser_get_root (e_ui_manager_get_parser (ui_manager));
	EUIElement *popup = root ? e_ui_element_get_child_by_id (root, "mail-message-popup") : NULL;
	EUIElement *item;
	GPtrArray *before;
	guint n, ii;

	n = popup ? e_ui_element_get_n_children (popup) : 0;
	if (n < 3)
		return;

	/* The Restore item and its separator are the last two */
	item = e_ui_element_get_child (popup, n - 2);
	if (e_ui_element_get_kind (item) != E_UI_ELEMENT_KIND_ITEM ||
	    g_strcmp0 (e_ui_element_item_get_action (item), RESTORE_ACTION) != 0)
		return;

	before = g_ptr_array_new ();
	for (ii = 0; ii < n - 2; ii++)
		g_ptr_array_add (before, copy_element_deep (e_ui_element_get_child (popup, ii)));
	for (ii = 0; ii < n - 2; ii++)
		e_ui_element_remove_child (popup, e_ui_element_get_child (popup, 0));
	for (ii = 0; ii < before->len; ii++)
		e_ui_element_add_child (popup, before->pdata[ii]);
	g_ptr_array_unref (before);

	e_ui_manager_changed (ui_manager);
}

/* The Junk Mail submenu and Evolution's "Add Sender to Address Book" (the
 * GroupWise client offers it in the Junk Mail folder), merged at the end of
 * the menu, go right after Evolution's own junk items */
static void
move_junk_menu (EUIManager *ui_manager)
{
	EUIElement *root = e_ui_parser_get_root (e_ui_manager_get_parser (ui_manager));
	EUIElement *popup = root ? e_ui_element_get_child_by_id (root, "mail-message-popup") : NULL;
	GPtrArray *order;
	guint n, ii, anchor = G_MAXUINT;

	/* The submenu and "Add Sender to Address Book" are the last two */
	n = popup ? e_ui_element_get_n_children (popup) : 0;
	if (n < 3 || e_ui_element_get_kind (e_ui_element_get_child (popup, n - 2)) != E_UI_ELEMENT_KIND_SUBMENU ||
	    g_strcmp0 (e_ui_element_submenu_get_action (e_ui_element_get_child (popup, n - 2)), JUNK_MENU) != 0)
		return;

	for (ii = 0; ii + 2 < n; ii++) {
		EUIElement *child = e_ui_element_get_child (popup, ii);

		if (e_ui_element_get_kind (child) == E_UI_ELEMENT_KIND_ITEM &&
		    g_str_has_prefix (e_ui_element_item_get_action (child), "mail-mark-notjunk"))
			anchor = ii;
	}
	if (anchor == G_MAXUINT)
		return;

	order = g_ptr_array_new ();
	for (ii = 0; ii + 2 < n; ii++) {
		g_ptr_array_add (order, copy_element_deep (e_ui_element_get_child (popup, ii)));
		if (ii == anchor) {
			g_ptr_array_add (order, copy_element_deep (e_ui_element_get_child (popup, n - 2)));
			g_ptr_array_add (order, copy_element_deep (e_ui_element_get_child (popup, n - 1)));
		}
	}
	for (ii = 0; ii < n; ii++)
		e_ui_element_remove_child (popup, e_ui_element_get_child (popup, 0));
	for (ii = 0; ii < order->len; ii++)
		e_ui_element_add_child (popup, order->pdata[ii]);
	g_ptr_array_unref (order);

	e_ui_manager_changed (ui_manager);
}

/* ------------------------------------------------------------------ */
/* Proxy accounts in grey in the folder list */

static void
paint_proxy_row (GtkTreeModel *model,
		 GtkTreeIter *iter)
{
	static const GdkRGBA grey = { 0.5, 0.5, 0.5, 1.0 };
	CamelStore *store = NULL;
	GdkRGBA *rgba = NULL;

	gtk_tree_model_get (model, iter, COL_OBJECT_CAMEL_STORE, &store, COL_RGBA_FOREGROUND_RGBA, &rgba, -1);
	/* A colour the user gave a folder stays */
	if (store && !rgba && e_groupwise_ui_is_proxy_store (CAMEL_SERVICE (store)))
		gtk_tree_store_set (GTK_TREE_STORE (model), iter, COL_RGBA_FOREGROUND_RGBA, &grey, -1);
	g_clear_pointer (&rgba, gdk_rgba_free);
	g_clear_object (&store);
}

static void
folder_tree_row_changed_cb (GtkTreeModel *model,
			    GtkTreePath *path,
			    GtkTreeIter *iter,
			    gpointer user_data)
{
	paint_proxy_row (model, iter);
}

static gboolean
paint_existing_row (GtkTreeModel *model,
		    GtkTreePath *path,
		    GtkTreeIter *iter,
		    gpointer user_data)
{
	paint_proxy_row (model, iter);

	return FALSE;
}

/* ------------------------------------------------------------------ */
/* The system folders first, in the order of the GroupWise client */

/* 0 for folders of the user; the Mailbox is first anyway (Evolution) */
static guint
system_folder_rank (GtkTreeModel *model,
		    GtkTreeIter *iter)
{
	gchar *full_name = NULL;
	guint flags = 0, rank = 0;

	gtk_tree_model_get (model, iter, COL_STRING_FULL_NAME, &full_name, COL_UINT_FLAGS, &flags, -1);
	if (full_name && !strchr (full_name, '/')) {
		switch (flags & CAMEL_FOLDER_TYPE_MASK) {
		case CAMEL_FOLDER_TYPE_SENT:
			rank = 1;
			break;
		case CAMEL_FOLDER_TYPE_DRAFTS:
			rank = 3;
			break;
		case CAMEL_FOLDER_TYPE_JUNK:
			rank = 4;
			break;
		case CAMEL_FOLDER_TYPE_TRASH:
			rank = 5;
			break;
		default:
			/* No Camel type: by the name the server gives them */
			if (g_ascii_strcasecmp (full_name, "Tasklist") == 0 || g_ascii_strcasecmp (full_name, "Checklist") == 0)
				rank = 2;
			else if (g_ascii_strcasecmp (full_name, "Cabinet") == 0)
				rank = 6;
			break;
		}
	}
	g_free (full_name);

	return rank;
}

/* Mailbox, Sent Items, Tasklist, Work In Progress, Junk Mail, Trash, Cabinet,
 * then the user's folders by name; -2: Evolution's order (other accounts,
 * or an order the user set) */
static gint
compare_folders_cb (EMFolderTreeModel *model,
		    const gchar *store_uid,
		    GtkTreeIter *a,
		    GtkTreeIter *b,
		    gpointer user_data)
{
	CamelStore *store = NULL;
	CamelProvider *provider;
	guint rank_a, rank_b;

	gtk_tree_model_get (GTK_TREE_MODEL (model), a, COL_OBJECT_CAMEL_STORE, &store, -1);
	provider = store ? camel_service_get_provider (CAMEL_SERVICE (store)) : NULL;
	g_clear_object (&store);
	if (!provider || g_strcmp0 (provider->protocol, "groupwise") != 0)
		return -2;

	rank_a = system_folder_rank (GTK_TREE_MODEL (model), a);
	rank_b = system_folder_rank (GTK_TREE_MODEL (model), b);
	if (rank_a == rank_b)
		return -2;
	if (!rank_a || !rank_b)
		return rank_a ? -1 : 1;

	return rank_a < rank_b ? -1 : 1;
}

static void
setup_folder_tree (void)
{
	static gboolean connected;
	EMFolderTreeModel *model;

	if (connected)
		return;
	connected = TRUE;

	model = em_folder_tree_model_get_default ();
	gtk_tree_model_foreach (GTK_TREE_MODEL (model), paint_existing_row, NULL);
	g_signal_connect (model, "row-changed", G_CALLBACK (folder_tree_row_changed_cb), NULL);
	g_signal_connect (model, "compare-folders", G_CALLBACK (compare_folders_cb), NULL);
	/* Accounts already in the list: sorted again (setting the same sort
	 * column again would not) */
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (model),
		GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (model),
		GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
}

static void
e_groupwise_mail_ui_constructed (GObject *object)
{
	static const gchar *eui =
		"<eui>"
		  "<menu id='main-menu'>"
		    "<submenu action='edit-menu'>"
		      "<placeholder id='edit-actions'>"
			"<item action='" RESTORE_ACTION "'/>"
		      "</placeholder>"
		    "</submenu>"
		  "</menu>"
		  "<menu id='mail-message-popup' is-popup='true'>"
		    "<item action='" RESTORE_ACTION "'/>"
		    "<separator/>"
		  "</menu>"
		"</eui>";

	static const EUIActionEntry entries[] = {
		{ RESTORE_ACTION,
		  "edit-undo",
		  N_("_Restore"),
		  NULL,
		  N_("Put the selected messages back into the folders they were deleted from"),
		  action_restore_cb, NULL, NULL, NULL }
	};

	static const gchar *more_eui =
		"<eui>"
		  "<menu id='mail-folder-popup' is-popup='true'>"
		    "<placeholder id='mail-folder-popup-actions'>"
		      "<item action='" SETTINGS_ACTION "'/>"
		    "</placeholder>"
		  "</menu>"
		  "<menu id='mail-message-popup' is-popup='true'>"
		    "<submenu action='" JUNK_MENU "'>"
		      "<item action='" TRUST_ACTION "'/>"
		      "<item action='" JUNK_ACTION "'/>"
		      "<item action='" BLOCK_ACTION "'/>"
		      "<separator/>"
		      "<item action='" JUNK_SETTINGS_ACTION "'/>"
		    "</submenu>"
		    "<item action='mail-add-sender'/>"
		    "<separator/>"
		    "<item action='" STATUS_ACTION "'/>"
		    "<item action='" RESEND_ACTION "'/>"
		    "<item action='" RETRACT_ACTION "'/>"
		  "</menu>"
		  "<menu id='main-menu'>"
		    "<placeholder id='custom-menus'>"
		      "<submenu action='mail-message-menu'>"
			"<separator/>"
			"<item action='" STATUS_ACTION "'/>"
			"<item action='" RESEND_ACTION "'/>"
			"<item action='" RETRACT_ACTION "'/>"
		      "</submenu>"
		    "</placeholder>"
		  "</menu>"
		"</eui>";

	static const EUIActionEntry more_entries[] = {
		{ SETTINGS_ACTION,
		  "preferences-system",
		  N_("GroupWise _Settings…"),
		  NULL,
		  N_("Settings the GroupWise server keeps for the account: junk mail handling and more"),
		  action_settings_cb, NULL, NULL, NULL },
		{ JUNK_MENU, NULL, N_("GroupWise _Junk Mail"), NULL, NULL, NULL, NULL, NULL, NULL },
		{ JUNK_SETTINGS_ACTION,
		  NULL,
		  N_("Junk Mail _Settings…"),
		  NULL,
		  N_("The junk mail handling of GroupWise: settings and lists"),
		  action_junk_settings_cb, NULL, NULL, NULL },
		{ JUNK_ACTION,
		  NULL,
		  N_("_Junk the Sender…"),
		  NULL,
		  N_("Put the sender or the sender's domain onto the junk list of GroupWise"),
		  action_junk_cb, NULL, NULL, NULL },
		{ BLOCK_ACTION,
		  NULL,
		  N_("_Block the Sender…"),
		  NULL,
		  N_("Put the sender or the sender's domain onto the block list of GroupWise"),
		  action_block_cb, NULL, NULL, NULL },
		{ TRUST_ACTION,
		  NULL,
		  N_("_Trust the Sender…"),
		  NULL,
		  N_("Put the sender or the sender's domain onto the trust list of GroupWise"),
		  action_trust_cb, NULL, NULL, NULL },
		{ STATUS_ACTION,
		  "document-properties",
		  N_("Delivery _Status…"),
		  NULL,
		  N_("What happened to the message at each recipient: delivered, opened, deleted …"),
		  action_status_cb, NULL, NULL, NULL },
		{ RESEND_ACTION,
		  "mail-send",
		  N_("Resen_d…"),
		  NULL,
		  N_("Open the message to change it and send it anew; sending asks whether the original is retracted"),
		  action_resend_cb, NULL, NULL, NULL },
		{ RETRACT_ACTION,
		  NULL,
		  N_("Re_tract…"),
		  NULL,
		  N_("Take the selected messages back from the recipients' mailboxes"),
		  action_retract_cb, NULL, NULL, NULL }
	};

	EShellView *shell_view;
	EShellViewClass *shell_view_class;

	G_OBJECT_CLASS (e_groupwise_mail_ui_parent_class)->constructed (object);

	shell_view = E_SHELL_VIEW (e_extension_get_extensible (E_EXTENSION (object)));
	shell_view_class = E_SHELL_VIEW_GET_CLASS (shell_view);
	if (!shell_view_class || g_strcmp0 (shell_view_class->ui_manager_id, "org.gnome.evolution.mail") != 0)
		return;

	e_ui_manager_add_actions_with_eui_data (e_shell_view_get_ui_manager (shell_view), "mail", GETTEXT_PACKAGE,
		entries, G_N_ELEMENTS (entries), shell_view, eui);
	move_restore_to_top (e_shell_view_get_ui_manager (shell_view));

	e_ui_manager_add_actions_with_eui_data (e_shell_view_get_ui_manager (shell_view), "mail", GETTEXT_PACKAGE,
		more_entries, G_N_ELEMENTS (more_entries), shell_view, more_eui);
	move_junk_menu (e_shell_view_get_ui_manager (shell_view));

	g_signal_connect (shell_view, "update-actions", G_CALLBACK (update_actions_cb), NULL);
	setup_folder_tree ();
	/* Signatures edited in Evolution go to GroupWise */
	e_groupwise_signature_sync_start (shell_view_get_registry (shell_view));
	/* The proxy accounts work with the server as the main account does */
	e_groupwise_proxy_options_start (shell_view_get_registry (shell_view));
	/* The port the server tells the events of the mailbox at: the firewall */
	e_groupwise_events_port_start (shell_view);
	/* The rules of the client's events, for accounts that want it */
	e_groupwise_rule_runner_start (shell_view_get_registry (shell_view));
}

static void
e_groupwise_mail_ui_class_init (EGroupwiseMailUIClass *class)
{
	G_OBJECT_CLASS (class)->constructed = e_groupwise_mail_ui_constructed;
	E_EXTENSION_CLASS (class)->extensible_type = E_TYPE_SHELL_VIEW;
}

static void
e_groupwise_mail_ui_class_finalize (EGroupwiseMailUIClass *class)
{
}

static void
e_groupwise_mail_ui_init (EGroupwiseMailUI *extension)
{
}

void
e_groupwise_mail_ui_type_register (GTypeModule *type_module)
{
	e_groupwise_mail_ui_register_type (type_module);
}

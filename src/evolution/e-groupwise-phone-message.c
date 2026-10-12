/*
 * e-groupwise-phone-message.c: writing a GroupWise phone message
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
 * The phone message of the GroupWise client (File > New > Phone Message):
 * a note for someone who was called while away — who called, the company,
 * the number, what the caller did or wants, and a text. GroupWise keeps it
 * as an item of its own kind; the recipients see it as a phone message in
 * the GroupWise client and in Evolution. It has no subject of its own: the
 * caller and the number stand for it.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>
#include <shell/e-shell.h>
#include <shell/e-shell-window.h>

#include "e-gw-backend-utils.h"
#include "e-gw-mailbox.h"

#include "e-groupwise-phone-message.h"
#include "e-groupwise-ui-utils.h"

#define SECTION "to"

static const struct {
	EGwPhoneFlags flag;
	const gchar *label;
} flag_boxes[] = {
	{ E_GW_PHONE_CALLED, N_("_Telephoned") },
	{ E_GW_PHONE_PLEASE_CALL, N_("_Please call") },
	{ E_GW_PHONE_WILL_CALL, N_("Will call _again") },
	{ E_GW_PHONE_RETURNED_CALL, N_("_Returned your call") },
	{ E_GW_PHONE_WANTS_TO_SEE_YOU, N_("_Wants to see you") },
	{ E_GW_PHONE_CAME_TO_SEE_YOU, N_("Came to see _you") },
	{ E_GW_PHONE_URGENT, N_("_Urgent") }
};

typedef struct {
	GtkWidget *dialog;	/* NULL once it is gone */
	ESourceRegistry *registry;
	ENameSelector *name_selector;
	GPtrArray *accounts;	/* ESource, as in the combo box */
	GtkWidget *account, *caller, *company, *phone, *text, *message;
	GtkWidget *boxes[G_N_ELEMENTS (flag_boxes)];
} PhoneDialog;

typedef struct {
	PhoneDialog *pd;
	ESourceRegistry *registry;
	ESource *account;
	GPtrArray *names, *emails;	/* NULL-terminated */
	gchar *caller, *company, *phone, *text;
	EGwPhoneFlags flags;
} SendData;

static void
send_data_free (gpointer ptr)
{
	SendData *data = ptr;

	g_clear_object (&data->registry);
	g_clear_object (&data->account);
	g_ptr_array_unref (data->names);
	g_ptr_array_unref (data->emails);
	g_free (data->caller);
	g_free (data->company);
	g_free (data->phone);
	g_free (data->text);
	g_free (data);
}

static void
phone_dialog_free (gpointer ptr)
{
	PhoneDialog *pd = ptr;

	if (pd->name_selector)
		e_name_selector_cancel_loading (pd->name_selector);
	g_clear_object (&pd->name_selector);
	g_clear_object (&pd->registry);
	g_ptr_array_unref (pd->accounts);
	g_free (pd);
}

static void
set_message (PhoneDialog *pd,
	     const gchar *text)
{
	gtk_label_set_text (GTK_LABEL (pd->message), text ? text : "");
	gtk_widget_set_visible (pd->message, text && *text);
}

static void
send_thread (GTask *task,
	     gpointer source_object,
	     gpointer task_data,
	     GCancellable *cancellable)
{
	SendData *data = task_data;
	EGwConnection *cnc;
	GError *error = NULL;
	gboolean sent = FALSE;

	cnc = e_groupwise_ui_connect_items_sync (data->registry, data->account, cancellable, &error);
	if (cnc) {
		sent = e_gw_connection_send_phone_message_sync (cnc, (const gchar * const *) data->names->pdata,
			(const gchar * const *) data->emails->pdata, data->caller, data->company, data->phone,
			data->flags, data->text, cancellable, &error);
		e_gw_connection_logout_sync (cnc, NULL);
		g_object_unref (cnc);
	}

	if (sent)
		g_task_return_boolean (task, TRUE);
	else
		g_task_return_error (task, error);
}

static void
send_done_cb (GObject *source_object,
	      GAsyncResult *result,
	      gpointer user_data)
{
	/* The dialog lives until this: it cannot be closed while sending */
	PhoneDialog *pd = user_data;
	GError *error = NULL;

	if (g_task_propagate_boolean (G_TASK (result), &error)) {
		gtk_widget_destroy (pd->dialog);
	} else {
		gchar *text = g_strdup_printf (_("The phone message was not sent: %s"), error ? error->message : "");

		set_message (pd, text);
		gtk_widget_set_sensitive (pd->dialog, TRUE);
		g_free (text);
		g_clear_error (&error);
	}
}

static gchar *
dup_entry (GtkWidget *entry)
{
	return g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (entry))));
}

static void
response_cb (GtkDialog *dialog,
	     gint response,
	     PhoneDialog *pd)
{
	ENameSelectorModel *model = e_name_selector_peek_model (pd->name_selector);
	EDestinationStore *store = NULL;
	GList *destinations, *link;
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	SendData *data;
	GTask *task;
	gint active;
	guint ii;

	if (response != GTK_RESPONSE_OK) {
		gtk_widget_destroy (GTK_WIDGET (dialog));
		return;
	}

	data = g_new0 (SendData, 1);
	data->names = g_ptr_array_new_with_free_func (g_free);
	data->emails = g_ptr_array_new_with_free_func (g_free);
	e_name_selector_model_peek_section (model, SECTION, NULL, &store);
	destinations = store ? e_destination_store_list_destinations (store) : NULL;
	for (link = destinations; link; link = g_list_next (link)) {
		const gchar *email = e_destination_get_email (link->data);

		if (email && *email) {
			g_ptr_array_add (data->names, g_strdup (e_destination_get_name (link->data)));
			g_ptr_array_add (data->emails, g_strdup (email));
		}
	}
	g_list_free (destinations);

	data->caller = dup_entry (pd->caller);
	data->company = dup_entry (pd->company);
	data->phone = dup_entry (pd->phone);
	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (pd->text));
	gtk_text_buffer_get_bounds (buffer, &start, &end);
	data->text = g_strstrip (gtk_text_buffer_get_text (buffer, &start, &end, FALSE));
	for (ii = 0; ii < G_N_ELEMENTS (flag_boxes); ii++) {
		if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (pd->boxes[ii])))
			data->flags |= flag_boxes[ii].flag;
	}

	if (!data->emails->len) {
		set_message (pd, _("Name whom the phone message is for."));
		send_data_free (data);
		return;
	}
	if (!*data->caller && !*data->text) {
		set_message (pd, _("Say who called, or write a message."));
		send_data_free (data);
		return;
	}
	g_ptr_array_add (data->names, NULL);
	g_ptr_array_add (data->emails, NULL);

	active = gtk_combo_box_get_active (GTK_COMBO_BOX (pd->account));
	data->registry = g_object_ref (pd->registry);
	data->account = g_object_ref (pd->accounts->pdata[active >= 0 ? active : 0]);

	set_message (pd, NULL);
	gtk_widget_set_sensitive (GTK_WIDGET (dialog), FALSE);
	task = g_task_new (NULL, NULL, send_done_cb, pd);
	g_task_set_task_data (task, data, send_data_free);
	g_task_run_in_thread (task, send_thread);
	g_object_unref (task);
}

/* Not while it is being sent */
static gboolean
delete_event_cb (GtkWidget *dialog,
		 GdkEvent *event,
		 gpointer user_data)
{
	return !gtk_widget_get_sensitive (dialog);
}

static void
to_clicked_cb (GtkButton *button,
	       PhoneDialog *pd)
{
	e_name_selector_show_dialog (pd->name_selector, pd->dialog);
}

static void
selector_response_cb (GtkDialog *dialog,
		      gint response,
		      gpointer user_data)
{
	gtk_widget_hide (GTK_WIDGET (dialog));
}

static GtkWidget *
labelled_entry (GtkGrid *grid,
		gint row,
		const gchar *text)
{
	GtkWidget *label = gtk_label_new_with_mnemonic (text), *entry = gtk_entry_new ();

	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
	gtk_widget_set_hexpand (entry, TRUE);
	gtk_grid_attach (grid, label, 0, row, 1, 1);
	gtk_grid_attach (grid, entry, 1, row, 2, 1);

	return entry;
}

static void
phone_dialog_new (GtkWindow *parent,
		  ESourceRegistry *registry,
		  EClientCache *client_cache,
		  ESource *preferred_account)
{
	PhoneDialog *pd;
	GtkWidget *dialog, *box, *grid, *label, *button, *entry, *flags, *scrolled;
	GList *sources, *link;
	gint row = 0, preferred = -1, own = -1;
	guint ii;

	pd = g_new0 (PhoneDialog, 1);
	pd->registry = g_object_ref (registry);
	pd->accounts = g_ptr_array_new_with_free_func (g_object_unref);

	/* The GroupWise accounts: the user's own, and those opened as proxy */
	sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_MAIL_ACCOUNT);
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;
		ESourceBackend *extension = e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT);

		if (g_strcmp0 (e_source_backend_get_backend_name (extension), "groupwise") != 0)
			continue;
		if (preferred_account && e_source_equal (source, preferred_account))
			preferred = pd->accounts->len;
		if (own < 0) {
			CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (registry, source);
			gchar *proxy = settings ? camel_groupwise_settings_dup_proxy (settings) : NULL;

			if (!proxy)
				own = pd->accounts->len;
			g_clear_object (&settings);
			g_free (proxy);
		}
		g_ptr_array_add (pd->accounts, g_object_ref (source));
	}
	g_list_free_full (sources, g_object_unref);
	if (!pd->accounts->len) {
		phone_dialog_free (pd);
		return;
	}

	dialog = gtk_dialog_new_with_buttons (_("Phone Message"), parent,
		GTK_DIALOG_DESTROY_WITH_PARENT, _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Send"), GTK_RESPONSE_OK, NULL);
	pd->dialog = dialog;
	gtk_window_set_default_size (GTK_WINDOW (dialog), 520, -1);
	g_object_set_data_full (G_OBJECT (dialog), "groupwise-phone-dialog", pd, phone_dialog_free);

	box = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_box_set_spacing (GTK_BOX (box), 12);

	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_box_pack_start (GTK_BOX (box), grid, TRUE, TRUE, 0);

	/* From which mailbox it goes; shown when there is a choice */
	label = gtk_label_new_with_mnemonic (_("_From:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	pd->account = gtk_combo_box_text_new ();
	for (ii = 0; ii < pd->accounts->len; ii++)
		gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (pd->account), e_source_get_display_name (pd->accounts->pdata[ii]));
	gtk_combo_box_set_active (GTK_COMBO_BOX (pd->account), preferred >= 0 ? preferred : MAX (own, 0));
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), pd->account);
	gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), pd->account, 1, row++, 2, 1);
	if (pd->accounts->len < 2) {
		gtk_widget_set_no_show_all (label, TRUE);
		gtk_widget_set_no_show_all (pd->account, TRUE);
	}

	/* Whom it is for: typed (the address books complete), or picked */
	pd->name_selector = e_name_selector_new (client_cache);
	e_name_selector_model_add_section (e_name_selector_peek_model (pd->name_selector), SECTION, _("To"), NULL);
	g_signal_connect (e_name_selector_peek_dialog (pd->name_selector), "response", G_CALLBACK (selector_response_cb), NULL);
	entry = GTK_WIDGET (e_name_selector_peek_section_entry (pd->name_selector, SECTION));
	gtk_widget_set_hexpand (entry, TRUE);
	e_name_selector_load_books (pd->name_selector);
	label = gtk_label_new_with_mnemonic (_("T_o:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
	button = gtk_button_new_with_mnemonic (_("A_ddress Book…"));
	gtk_widget_set_tooltip_text (button, _("Pick the recipients from the address books"));
	g_signal_connect (button, "clicked", G_CALLBACK (to_clicked_cb), pd);
	gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), entry, 1, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), button, 2, row++, 1, 1);

	pd->caller = labelled_entry (GTK_GRID (grid), row++, _("C_aller:"));
	pd->company = labelled_entry (GTK_GRID (grid), row++, _("Compa_ny:"));
	pd->phone = labelled_entry (GTK_GRID (grid), row++, _("P_hone:"));

	/* What the caller did or wants, in two columns */
	flags = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (flags), 3);
	gtk_grid_set_column_spacing (GTK_GRID (flags), 24);
	for (ii = 0; ii < G_N_ELEMENTS (flag_boxes); ii++) {
		pd->boxes[ii] = gtk_check_button_new_with_mnemonic (_(flag_boxes[ii].label));
		gtk_grid_attach (GTK_GRID (flags), pd->boxes[ii], ii % 2, ii / 2, 1, 1);
	}
	gtk_grid_attach (GTK_GRID (grid), flags, 1, row++, 2, 1);

	label = gtk_label_new_with_mnemonic (_("_Message:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_widget_set_valign (label, GTK_ALIGN_START);
	pd->text = gtk_text_view_new ();
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (pd->text), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin (GTK_TEXT_VIEW (pd->text), 4);
	gtk_text_view_set_right_margin (GTK_TEXT_VIEW (pd->text), 4);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), pd->text);
	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request (scrolled, -1, 130);
	gtk_widget_set_vexpand (scrolled, TRUE);
	gtk_container_add (GTK_CONTAINER (scrolled), pd->text);
	gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), scrolled, 1, row++, 2, 1);

	pd->message = gtk_label_new (NULL);
	gtk_label_set_line_wrap (GTK_LABEL (pd->message), TRUE);
	gtk_label_set_xalign (GTK_LABEL (pd->message), 0.0);
	gtk_label_set_max_width_chars (GTK_LABEL (pd->message), 60);
	gtk_widget_set_no_show_all (pd->message, TRUE);
	gtk_box_pack_start (GTK_BOX (box), pd->message, FALSE, FALSE, 0);

	g_signal_connect (dialog, "response", G_CALLBACK (response_cb), pd);
	g_signal_connect (dialog, "delete-event", G_CALLBACK (delete_event_cb), NULL);
	gtk_widget_show_all (dialog);
	gtk_widget_grab_focus (entry);
}

void
e_groupwise_phone_message_new (EShellView *shell_view,
			       ESource *preferred_account)
{
	EShell *shell;

	g_return_if_fail (E_IS_SHELL_VIEW (shell_view));

	shell = e_shell_window_get_shell (E_SHELL_WINDOW (e_shell_view_get_shell_window (shell_view)));
	phone_dialog_new (GTK_WINDOW (e_shell_view_get_shell_window (shell_view)), e_shell_get_registry (shell),
		e_shell_get_client_cache (shell), preferred_account);
}

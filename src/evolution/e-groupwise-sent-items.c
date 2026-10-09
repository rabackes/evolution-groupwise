/*
 * e-groupwise-sent-items.c: the delivery status of a sent item, and taking
 * sent items back from the recipients
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
 * What the GroupWise client shows in the properties of a sent item: for
 * each recipient what happened to the item (delivered, opened, deleted,
 * retracted …) and when. The POA keeps it in <recipientStatus> of the sent
 * item. Retract takes a sent item back from the recipients' mailboxes as
 * the GroupWise client does; the sent item stays, its recipients show
 * "retracted". Both need a connection of their own to the POA (a proxy
 * login for a proxy account). Resend opens a sent message in the composer
 * to be changed and sent again; sending asks, as the GroupWise client
 * does, whether the original is retracted.
 */

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>
#include <composer/e-msg-composer.h>
#include <composer/e-composer-header-table.h>
#include <mail/em-composer-utils.h>

#include "e-gw-connection.h"
#include "e-gw-xml.h"
#include "e-groupwise-ui-utils.h"
#include "e-groupwise-sent-items.h"

#define STATUS_VIEW "id subject recipients recipientStatus"

/* The events of <recipientStatus>, as the GroupWise client names them */
static const struct {
	const gchar *element;
	const gchar *label;
} events[] = {
	{ "delivered", N_("Delivered") },
	{ "undeliverable", N_("Undeliverable") },
	{ "transferred", N_("Transferred") },
	{ "transferDelayed", N_("Transfer delayed") },
	{ "transferFailed", N_("Transfer failed") },
	{ "downloaded", N_("Downloaded") },
	{ "downloadedByThirdParty", N_("Downloaded by another program") },
	{ "retractRequested", N_("Retraction requested") },
	{ "retracted", N_("Retracted") },
	{ "opened", N_("Opened") },
	{ "deleted", N_("Deleted") },
	{ "undeleted", N_("Undeleted") },
	{ "purged", N_("Purged") },
	{ "accepted", N_("Accepted") },
	{ "declined", N_("Declined") },
	{ "replied", N_("Replied") },
	{ "forwarded", N_("Forwarded") },
	{ "shared", N_("Shared") },
	{ "started", N_("Started") },
	{ "completed", N_("Completed") },
	{ "incomplete", N_("Marked incomplete") },
	{ "delegated", N_("Delegated") }
};

enum {
	COL_TEXT,
	COL_TIME,
	N_COLUMNS
};

typedef struct {
	ESourceRegistry *registry;
	ESource *account;
	gchar *uid;
	gchar *subject;
	GtkWidget *dialog;	/* weak */
	GtkTreeStore *store;
	GtkWidget *info;	/* weak */
} StatusJob;

static void
status_job_free (StatusJob *job)
{
	g_object_unref (job->registry);
	g_object_unref (job->account);
	g_free (job->uid);
	g_free (job->subject);
	if (job->dialog)
		g_object_remove_weak_pointer (G_OBJECT (job->dialog), (gpointer *) &job->dialog);
	if (job->info)
		g_object_remove_weak_pointer (G_OBJECT (job->info), (gpointer *) &job->info);
	g_object_unref (job->store);
	g_free (job);
}

static const gchar *
event_label (const gchar *element)
{
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (events); ii++) {
		if (g_strcmp0 (events[ii].element, element) == 0)
			return _(events[ii].label);
	}

	return NULL;
}

/* A time of the POA ("2026-09-30T12:06:47Z") in the local zone */
static gchar *
format_time (const gchar *iso)
{
	GDateTime *utc, *local;
	gchar *text;

	utc = iso && *iso ? g_date_time_new_from_iso8601 (iso, NULL) : NULL;
	if (!utc)
		return g_strdup (iso ? iso : "");
	local = g_date_time_to_local (utc);
	text = g_date_time_format (local, "%x %X");
	g_date_time_unref (local);
	g_date_time_unref (utc);

	return text;
}

static const gchar *
dist_label (const gchar *dist)
{
	if (g_strcmp0 (dist, "CC") == 0)
		return _("CC");
	if (g_strcmp0 (dist, "BC") == 0)
		return _("BC");

	return _("To");
}

/* The rows of the dialog from the sent item (in the main thread) */
static void
fill_status (GtkTreeStore *store,
	     xmlNode *item)
{
	xmlNode *recipient;

	for (recipient = e_gw_xml_first_child (e_gw_xml_find (item, "distribution/recipients"), "recipient"); recipient;
	     recipient = e_gw_xml_next_sibling (recipient, "recipient")) {
		gchar *name = e_gw_xml_dup_text (recipient, "displayName");
		gchar *email = e_gw_xml_dup_text (recipient, "email");
		gchar *dist = e_gw_xml_dup_text (recipient, "distType");
		gchar *who;
		xmlNode *status = e_gw_xml_find (recipient, "recipientStatus"), *event;
		GtkTreeIter parent, child;
		guint count = 0;

		if (name && *name && email && *email && g_ascii_strcasecmp (name, email) != 0)
			who = g_strdup_printf ("%s <%s> (%s)", name, email, dist_label (dist));
		else
			who = g_strdup_printf ("%s (%s)", email && *email ? email : name ? name : "", dist_label (dist));
		gtk_tree_store_append (store, &parent, NULL);
		gtk_tree_store_set (store, &parent, COL_TEXT, who, COL_TIME, "", -1);

		for (event = status ? status->children : NULL; event; event = event->next) {
			const gchar *label;
			gchar *when, *comment, *text;

			if (event->type != XML_ELEMENT_NODE)
				continue;
			label = event_label ((const gchar *) event->name);
			if (!label)
				continue;
			/* The time, or the comment of an answer */
			comment = e_gw_xml_dup_text (event, "comment");
			if (comment) {
				when = g_strdup ("");
			} else {
				xmlChar *content = xmlNodeGetContent (event);

				when = format_time ((const gchar *) content);
				xmlFree (content);
			}
			text = comment && *comment ? g_strdup_printf ("%s: %s", label, comment) : g_strdup (label);
			gtk_tree_store_append (store, &child, &parent);
			gtk_tree_store_set (store, &child, COL_TEXT, text, COL_TIME, when, -1);
			count++;
			g_free (text);
			g_free (when);
			g_free (comment);
		}
		if (!count) {
			gtk_tree_store_append (store, &child, &parent);
			gtk_tree_store_set (store, &child, COL_TEXT, _("No status yet"), COL_TIME, "", -1);
		}

		g_free (who);
		g_free (name);
		g_free (email);
		g_free (dist);
	}
}

static void
status_thread (GTask *task,
	       gpointer source_object,
	       gpointer task_data,
	       GCancellable *cancellable)
{
	StatusJob *job = task_data;
	EGwConnection *cnc;
	EGwResponse *response = NULL;
	GError *error = NULL;

	cnc = e_groupwise_ui_connect_items_sync (job->registry, job->account, cancellable, &error);
	if (cnc) {
		response = e_gw_connection_get_item_sync (cnc, job->uid, STATUS_VIEW, cancellable, &error);
		e_gw_connection_logout_sync (cnc, NULL);
		g_object_unref (cnc);
	}

	if (response)
		g_task_return_pointer (task, response, (GDestroyNotify) e_gw_response_free);
	else
		g_task_return_error (task, error);
}

static void
status_done (GObject *source_object,
	     GAsyncResult *result,
	     gpointer user_data)
{
	StatusJob *job = g_task_get_task_data (G_TASK (result));
	EGwResponse *response;
	GError *error = NULL;

	response = g_task_propagate_pointer (G_TASK (result), &error);
	if (!job->dialog) {
		g_clear_error (&error);
		g_clear_pointer (&response, e_gw_response_free);
		return;
	}

	if (response) {
		xmlNode *item = e_gw_xml_find (e_gw_response_get_node (response), "item");

		if (item)
			fill_status (job->store, item);
		if (job->info)
			gtk_widget_hide (job->info);
		e_gw_response_free (response);
	} else if (job->info) {
		gchar *text = g_strdup_printf (_("The delivery status could not be read: %s"),
			error ? error->message : _("Unknown error"));

		gtk_label_set_text (GTK_LABEL (job->info), text);
		g_free (text);
	}
	g_clear_error (&error);
}

void
e_groupwise_sent_status_show (GtkWindow *parent,
			      ESourceRegistry *registry,
			      ESource *account,
			      const gchar *uid,
			      const gchar *subject)
{
	GtkWidget *dialog, *content, *label, *scrolled, *view;
	GtkCellRenderer *renderer;
	StatusJob *job;
	GTask *task;
	gchar *title;

	g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));
	g_return_if_fail (E_IS_SOURCE (account));
	g_return_if_fail (uid != NULL);

	dialog = gtk_dialog_new_with_buttons (_("Delivery Status"), parent, GTK_DIALOG_DESTROY_WITH_PARENT,
		_("_Close"), GTK_RESPONSE_CLOSE, NULL);
	gtk_window_set_default_size (GTK_WINDOW (dialog), 560, 400);
	g_signal_connect (dialog, "response", G_CALLBACK (gtk_widget_destroy), NULL);

	content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	gtk_container_set_border_width (GTK_CONTAINER (content), 12);
	gtk_box_set_spacing (GTK_BOX (content), 6);

	label = gtk_label_new (NULL);
	title = g_markup_printf_escaped ("<b>%s</b>", subject && *subject ? subject : _("(No subject)"));
	gtk_label_set_markup (GTK_LABEL (label), title);
	g_free (title);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start (GTK_BOX (content), label, FALSE, FALSE, 0);

	job = g_new0 (StatusJob, 1);
	job->registry = g_object_ref (registry);
	job->account = g_object_ref (account);
	job->uid = g_strdup (uid);
	job->subject = g_strdup (subject);
	job->store = gtk_tree_store_new (N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING);

	job->info = gtk_label_new (_("Reading the delivery status from the GroupWise server…"));
	gtk_label_set_xalign (GTK_LABEL (job->info), 0.0);
	gtk_label_set_line_wrap (GTK_LABEL (job->info), TRUE);
	gtk_box_pack_start (GTK_BOX (content), job->info, FALSE, FALSE, 0);
	g_object_add_weak_pointer (G_OBJECT (job->info), (gpointer *) &job->info);

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_box_pack_start (GTK_BOX (content), scrolled, TRUE, TRUE, 0);

	view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (job->store));
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_append_column (GTK_TREE_VIEW (view),
		gtk_tree_view_column_new_with_attributes (_("Recipient / Event"), renderer, "text", COL_TEXT, NULL));
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_append_column (GTK_TREE_VIEW (view),
		gtk_tree_view_column_new_with_attributes (_("Time"), renderer, "text", COL_TIME, NULL));
	gtk_container_add (GTK_CONTAINER (scrolled), view);
	/* The events show as soon as they come */
	g_signal_connect_object (job->store, "row-inserted", G_CALLBACK (gtk_tree_view_expand_all), view, G_CONNECT_SWAPPED);

	job->dialog = dialog;
	g_object_add_weak_pointer (G_OBJECT (dialog), (gpointer *) &job->dialog);

	gtk_widget_show_all (dialog);

	task = g_task_new (NULL, NULL, status_done, NULL);
	g_task_set_task_data (task, job, (GDestroyNotify) status_job_free);
	g_task_run_in_thread (task, status_thread);
	g_object_unref (task);
}

/* ------------------------------------------------------------------ */

typedef struct {
	ESourceRegistry *registry;
	ESource *account;
	GPtrArray *uids;
	EAlertSink *alert_sink;
} RetractJob;

static void
retract_job_free (RetractJob *job)
{
	g_object_unref (job->registry);
	g_object_unref (job->account);
	g_ptr_array_unref (job->uids);
	g_clear_object (&job->alert_sink);
	g_free (job);
}

static void
retract_thread (GTask *task,
		gpointer source_object,
		gpointer task_data,
		GCancellable *cancellable)
{
	RetractJob *job = task_data;
	EGwConnection *cnc;
	GError *error = NULL;
	gboolean success = FALSE;

	cnc = e_groupwise_ui_connect_items_sync (job->registry, job->account, cancellable, &error);
	if (cnc) {
		g_ptr_array_add (job->uids, NULL);
		success = e_gw_connection_retract_from_recipients_sync (cnc, (const gchar * const *) job->uids->pdata,
			cancellable, &error);
		g_ptr_array_remove_index (job->uids, job->uids->len - 1);
		e_gw_connection_logout_sync (cnc, NULL);
		g_object_unref (cnc);
	}

	if (success)
		g_task_return_boolean (task, TRUE);
	else
		g_task_return_error (task, error);
}

static void
retract_done (GObject *source_object,
	      GAsyncResult *result,
	      gpointer user_data)
{
	RetractJob *job = g_task_get_task_data (G_TASK (result));
	GError *error = NULL;

	if (!g_task_propagate_boolean (G_TASK (result), &error) && job->alert_sink)
		e_alert_submit (job->alert_sink, "mail:async-error", _("retracting messages"),
			error ? error->message : "", NULL);
	g_clear_error (&error);
}

static void
start_retract (EAlertSink *alert_sink,
	       ESourceRegistry *registry,
	       ESource *account,
	       GPtrArray *uids)
{
	RetractJob *job;
	GTask *task;
	guint ii;

	job = g_new0 (RetractJob, 1);
	job->registry = g_object_ref (registry);
	job->account = g_object_ref (account);
	job->uids = g_ptr_array_new_with_free_func (g_free);
	for (ii = 0; ii < uids->len; ii++)
		g_ptr_array_add (job->uids, g_strdup (uids->pdata[ii]));
	job->alert_sink = alert_sink ? g_object_ref (alert_sink) : NULL;

	task = g_task_new (NULL, NULL, retract_done, NULL);
	g_task_set_task_data (task, job, (GDestroyNotify) retract_job_free);
	g_task_run_in_thread (task, retract_thread);
	g_object_unref (task);
}

void
e_groupwise_sent_retract (GtkWindow *parent,
			  EAlertSink *alert_sink,
			  ESourceRegistry *registry,
			  ESource *account,
			  GPtrArray *uids,
			  const gchar *subject)
{
	GtkWidget *dialog;
	gint response;

	g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));
	g_return_if_fail (E_IS_SOURCE (account));
	g_return_if_fail (uids != NULL);

	if (!uids->len)
		return;

	if (uids->len == 1)
		dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, _("Retract “%s” from the recipients?"),
			subject && *subject ? subject : _("(No subject)"));
	else
		dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, _("Retract the %u selected messages from the recipients?"),
			uids->len);
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s",
		_("GroupWise takes the message out of the mailboxes of the recipients in GroupWise; the sent message "
		  "stays in the Sent Items, with the recipients shown as retracted. Recipients outside GroupWise "
		  "have got it already."));
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Cancel"), GTK_RESPONSE_CANCEL);
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Retract"), GTK_RESPONSE_ACCEPT);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_CANCEL);
	response = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
	if (response == GTK_RESPONSE_ACCEPT)
		start_retract (alert_sink, registry, account, uids);
}

/* ------------------------------------------------------------------ */

typedef struct {
	EShell *shell;
	EAlertSink *alert_sink;
	ESourceRegistry *registry;
	ESource *account;
	CamelFolder *folder;
	gchar *uid;
	gchar *subject;
	CamelMimeMessage *message;
	gboolean retract;	/* the user's answer when sending */
} ResendData;

static void
resend_data_free (ResendData *data)
{
	g_object_unref (data->shell);
	g_clear_object (&data->alert_sink);
	g_object_unref (data->registry);
	g_object_unref (data->account);
	g_object_unref (data->folder);
	g_free (data->uid);
	g_free (data->subject);
	g_clear_object (&data->message);
	g_free (data);
}

/* Sending the new message: the original retracted? */
static gboolean
resend_presend_cb (EMsgComposer *composer,
		   ResendData *data)
{
	GtkWidget *dialog;
	gint response;

	dialog = gtk_message_dialog_new (GTK_WINDOW (composer), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, _("Retract the original message “%s” from the recipients?"),
		data->subject && *data->subject ? data->subject : _("(No subject)"));
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s",
		_("The message is sent anew. GroupWise can take the original out of the mailboxes of the recipients "
		  "in GroupWise; recipients outside GroupWise have got it already."));
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Cancel"), GTK_RESPONSE_CANCEL);
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Do Not Retract"), GTK_RESPONSE_NO);
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Retract"), GTK_RESPONSE_YES);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_YES);
	response = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);

	if (response != GTK_RESPONSE_YES && response != GTK_RESPONSE_NO)
		return FALSE;

	data->retract = response == GTK_RESPONSE_YES;

	return TRUE;
}

/* The new message goes out: the original is taken back if the user wants */
static void
resend_send_cb (EMsgComposer *composer,
		CamelMimeMessage *message,
		EActivity *activity,
		ResendData *data)
{
	if (data->retract) {
		GPtrArray *uids = g_ptr_array_new ();

		g_ptr_array_add (uids, data->uid);
		start_retract (data->alert_sink, data->registry, data->account, uids);
		g_ptr_array_unref (uids);
		/* Once */
		data->retract = FALSE;
	}
}

/* The message has its signature already (GroupWise's is plain text in it,
 * Evolution does not see it as one): no second one under it */
static gboolean
resend_no_signature_cb (gpointer user_data)
{
	EMsgComposer *composer = g_weak_ref_get (user_data);

	if (composer) {
		e_composer_header_table_set_signature_uid (e_msg_composer_get_header_table (composer), "none");
		g_object_unref (composer);
	}

	return G_SOURCE_REMOVE;
}

static void
weak_ref_free (gpointer data)
{
	g_weak_ref_clear (data);
	g_free (data);
}

static void
resend_composer_cb (GObject *source_object,
		    GAsyncResult *result,
		    gpointer user_data)
{
	ResendData *data = user_data;
	EMsgComposer *composer;
	GError *error = NULL;

	composer = e_msg_composer_new_finish (result, &error);
	if (!composer) {
		if (data->alert_sink)
			e_alert_submit (data->alert_sink, "mail:async-error", _("opening the message to resend it"),
				error ? error->message : "", NULL);
		g_clear_error (&error);
		resend_data_free (data);
		return;
	}

	/* As "Edit as New Message", from the Sent Items */
	em_utils_edit_message (composer, data->folder, data->message, data->uid, TRUE, FALSE);
	/* The identity (and its signature) may be set a moment later */
	e_composer_header_table_set_signature_uid (e_msg_composer_get_header_table (composer), "none");
	{
		GWeakRef *ref = g_new0 (GWeakRef, 1);

		g_weak_ref_init (ref, composer);
		g_timeout_add_full (G_PRIORITY_DEFAULT, 500, resend_no_signature_cb, ref, weak_ref_free);
	}
	g_signal_connect (composer, "presend", G_CALLBACK (resend_presend_cb), data);
	g_signal_connect (composer, "send", G_CALLBACK (resend_send_cb), data);
	g_object_set_data_full (G_OBJECT (composer), "groupwise-resend", data, (GDestroyNotify) resend_data_free);
}

static void
resend_message_cb (GObject *source_object,
		   GAsyncResult *result,
		   gpointer user_data)
{
	ResendData *data = user_data;
	GError *error = NULL;

	data->message = camel_folder_get_message_finish (CAMEL_FOLDER (source_object), result, &error);
	if (!data->message) {
		if (data->alert_sink)
			e_alert_submit (data->alert_sink, "mail:async-error", _("opening the message to resend it"),
				error ? error->message : "", NULL);
		g_clear_error (&error);
		resend_data_free (data);
		return;
	}

	e_msg_composer_new (data->shell, resend_composer_cb, data);
}

void
e_groupwise_sent_resend (EShell *shell,
			 EAlertSink *alert_sink,
			 ESourceRegistry *registry,
			 ESource *account,
			 CamelFolder *folder,
			 const gchar *uid,
			 const gchar *subject)
{
	ResendData *data;

	g_return_if_fail (E_IS_SHELL (shell));
	g_return_if_fail (CAMEL_IS_FOLDER (folder));
	g_return_if_fail (uid != NULL);

	data = g_new0 (ResendData, 1);
	data->shell = g_object_ref (shell);
	data->alert_sink = alert_sink ? g_object_ref (alert_sink) : NULL;
	data->registry = g_object_ref (registry);
	data->account = g_object_ref (account);
	data->folder = g_object_ref (folder);
	data->uid = g_strdup (uid);
	data->subject = g_strdup (subject);

	camel_folder_get_message (folder, uid, G_PRIORITY_DEFAULT, NULL, resend_message_cb, data);
}

/*
 * e-groupwise-events-port.c: asks to open the port the server tells events at
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
 * With the account option for it, the GroupWise server tells at once when
 * something happens in the mailbox, connecting to a port of this computer.
 * A firewall on the computer usually does not let it in. The mail store
 * notices that (events found that the server was to tell, twice in a row)
 * and says so on the store object; here the user is asked whether the
 * port is opened in the firewall (firewalld: firewall-cmd, which lets the
 * system ask for the administrator's password).
 */

#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include <e-util/e-util.h>
#include <libemail-engine/libemail-engine.h>
#include <mail/e-mail-backend.h>
#include <shell/e-shell-view.h>
#include <shell/e-shell-window.h>

#include "e-groupwise-events-port.h"

/* As camel-groupwise-store.h (the provider's) */
#define STORE_PORT_UNREACHABLE "groupwise-events-port-unreachable"

#define CHECK_SECONDS 20
#define RESPONSE_NEVER 1

static GHashTable *asked;	/* ports asked about in this session */

static gchar *
dup_state_file (void)
{
	return g_build_filename (g_get_user_config_dir (), "evolution-groupwise", "events.ini", NULL);
}

/* "Do not ask again" for a port */
static gboolean
is_declined (guint port)
{
	GKeyFile *key_file = g_key_file_new ();
	gchar *path = dup_state_file (), *name = g_strdup_printf ("%u", port);
	gboolean declined;

	g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, NULL);
	declined = g_key_file_get_boolean (key_file, "Declined Ports", name, NULL);
	g_free (name);
	g_free (path);
	g_key_file_free (key_file);

	return declined;
}

static void
set_declined (guint port)
{
	GKeyFile *key_file = g_key_file_new ();
	gchar *path = dup_state_file (), *dir = g_path_get_dirname (path), *name = g_strdup_printf ("%u", port);

	g_key_file_load_from_file (key_file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	g_key_file_set_boolean (key_file, "Declined Ports", name, TRUE);
	g_mkdir_with_parents (dir, 0700);
	g_key_file_save_to_file (key_file, path, NULL);
	g_free (name);
	g_free (dir);
	g_free (path);
	g_key_file_free (key_file);
}

static void
show_message (GtkWindow *parent,
	      GtkMessageType type,
	      const gchar *primary,
	      const gchar *secondary)
{
	GtkWidget *dialog = gtk_message_dialog_new (parent, GTK_DIALOG_DESTROY_WITH_PARENT, type, GTK_BUTTONS_CLOSE, "%s", primary);

	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", secondary);
	g_signal_connect (dialog, "response", G_CALLBACK (gtk_widget_destroy), NULL);
	gtk_widget_show (dialog);
}

typedef struct {
	GtkWindow *parent;	/* weak */
	guint port;
} OpenData;

static void
open_data_free (OpenData *data)
{
	if (data->parent)
		g_object_remove_weak_pointer (G_OBJECT (data->parent), (gpointer *) &data->parent);
	g_free (data);
}

static void	open_port_step		(OpenData *data);

static void
open_port_done_cb (GObject *source_object,
		   GAsyncResult *result,
		   gpointer user_data)
{
	OpenData *data = user_data;
	GError *error = NULL;

	if (g_subprocess_wait_check_finish (G_SUBPROCESS (source_object), result, &error)) {
		g_debug ("events port: %u opened in the firewall", data->port);
	} else {
		gchar *primary = g_strdup_printf (_("TCP port %u could not be opened in the firewall"), data->port);

		show_message (data->parent, GTK_MESSAGE_WARNING, primary, error ? error->message : "");
		g_free (primary);
		g_clear_error (&error);
	}
	open_data_free (data);
}

static void
open_port_step (OpenData *data)
{
	/* For good and for the running firewall, as the administrator in one
	 * go: one question for the password (firewall-cmd by itself asks for
	 * each of its calls) */
	gchar *script = g_strdup_printf ("firewall-cmd --permanent --add-port=%u/tcp && firewall-cmd --add-port=%u/tcp",
		data->port, data->port);
	gchar *pkexec = g_find_program_in_path ("pkexec");
	GSubprocess *process;
	GError *error = NULL;

	process = pkexec ?
		g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error,
			pkexec, "/bin/sh", "-c", script, NULL) :
		g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error,
			"/bin/sh", "-c", script, NULL);
	g_free (pkexec);
	g_free (script);
	if (process) {
		g_subprocess_wait_check_async (process, NULL, open_port_done_cb, data);
		g_object_unref (process);
	} else {
		gchar *primary = g_strdup_printf (_("TCP port %u could not be opened in the firewall"), data->port);

		show_message (data->parent, GTK_MESSAGE_WARNING, primary, error ? error->message : "");
		g_free (primary);
		g_clear_error (&error);
		open_data_free (data);
	}
}

static void
ask_response_cb (GtkDialog *dialog,
		 gint response,
		 gpointer user_data)
{
	guint port = GPOINTER_TO_UINT (user_data);
	GtkWindow *parent = gtk_window_get_transient_for (GTK_WINDOW (dialog));

	if (response == RESPONSE_NEVER) {
		set_declined (port);
	} else if (response == GTK_RESPONSE_ACCEPT) {
		gchar *program = g_find_program_in_path ("firewall-cmd");

		if (program) {
			OpenData *data = g_new0 (OpenData, 1);

			data->parent = parent;
			if (parent)
				g_object_add_weak_pointer (G_OBJECT (parent), (gpointer *) &data->parent);
			data->port = port;
			open_port_step (data);
		} else {
			gchar *secondary = g_strdup_printf (_("This computer has no firewalld (firewall-cmd). Let TCP port %u in "
				"with the firewall it uses."), port);

			show_message (parent, GTK_MESSAGE_INFO, _("Evolution cannot open the port itself"), secondary);
			g_free (secondary);
		}
		g_free (program);
	}
	gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
ask (GtkWindow *parent,
     guint port)
{
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new (parent, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
		_("The GroupWise server does not reach this computer on port %u"), port);
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
		_("The server is to tell at once when something changes in your mailbox, but its messages do not arrive: "
		  "most likely the firewall of this computer does not let them in. The changes still show, after the "
		  "interval set for the account.\n\n"
		  "Evolution can open TCP port %u in the firewall; the system then asks for the administrator's password."),
		port);
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("Do Not Ask _Again"), RESPONSE_NEVER);
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Not Now"), GTK_RESPONSE_CANCEL);
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Open the Port"), GTK_RESPONSE_ACCEPT);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
	g_signal_connect (dialog, "response", G_CALLBACK (ask_response_cb), GUINT_TO_POINTER (port));
	gtk_widget_show (dialog);
}

static gboolean
check_cb (gpointer user_data)
{
	EShell *shell = g_weak_ref_get (user_data);
	EShellBackend *mail_backend;
	CamelSession *session;
	GList *services, *link;

	if (!shell)
		return G_SOURCE_REMOVE;

	/* The mail backend has the accounts, whichever view is shown */
	mail_backend = e_shell_get_backend_by_name (shell, "mail");
	if (!mail_backend || !E_IS_MAIL_BACKEND (mail_backend)) {
		g_object_unref (shell);
		return G_SOURCE_CONTINUE;
	}
	session = CAMEL_SESSION (e_mail_backend_get_session (E_MAIL_BACKEND (mail_backend)));
	services = camel_session_list_services (session);
	for (link = services; link; link = g_list_next (link)) {
		CamelProvider *provider = CAMEL_IS_STORE (link->data) ? camel_service_get_provider (link->data) : NULL;
		guint port;

		if (!provider || g_strcmp0 (provider->protocol, "groupwise") != 0)
			continue;
		port = GPOINTER_TO_UINT (g_object_get_data (link->data, STORE_PORT_UNREACHABLE));
		/* Once per port and session (the accounts of a login share it) */
		if (port && !g_hash_table_contains (asked, GUINT_TO_POINTER (port))) {
			g_hash_table_add (asked, GUINT_TO_POINTER (port));
			if (!is_declined (port))
				ask (e_shell_get_active_window (shell), port);
		}
	}
	g_list_free_full (services, g_object_unref);
	g_object_unref (shell);

	return G_SOURCE_CONTINUE;
}

static void
weak_ref_free (gpointer data)
{
	g_weak_ref_clear (data);
	g_free (data);
}

void
e_groupwise_events_port_start (EShell *shell)
{
	GWeakRef *ref;

	g_return_if_fail (E_IS_SHELL (shell));

	if (asked)
		return;
	asked = g_hash_table_new (g_direct_hash, g_direct_equal);

	ref = g_new0 (GWeakRef, 1);
	g_weak_ref_init (ref, shell);
	g_timeout_add_seconds_full (G_PRIORITY_DEFAULT_IDLE, CHECK_SECONDS, check_cb, ref, weak_ref_free);
}

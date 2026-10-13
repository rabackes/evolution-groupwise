/*
 * e-groupwise-proxy-calendar.c: setting up the calendar of another user as a proxy calendar
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

#include <string.h>

#include <glib/gi18n-lib.h>

#include <e-util/e-util.h>
#include <shell/e-shell-content.h>
#include <shell/e-shell-window.h>

#include "camel-groupwise-settings.h"
#include "e-gw-backend-utils.h"
#include "e-gw-proxy.h"
#include "e-source-groupwise-folder.h"

#include "e-groupwise-proxy-calendar.h"
#include "e-groupwise-ui-utils.h"

typedef struct {
	EShellView *shell_view;
	ESourceRegistry *registry;
	ESource *collection;
	GPtrArray *users;	/* EGwProxyUser */
	gchar *own_email;
} AddData;

static void
add_data_free (gpointer ptr)
{
	AddData *data = ptr;

	g_clear_object (&data->shell_view);
	g_clear_object (&data->registry);
	g_clear_object (&data->collection);
	g_clear_pointer (&data->users, g_ptr_array_unref);
	g_free (data->own_email);
	g_free (data);
}

static void
load_thread (GTask *task,
	     gpointer source_object,
	     gpointer task_data,
	     GCancellable *cancellable)
{
	AddData *data = task_data;
	GError *error = NULL;
	EGwConnection *cnc = e_groupwise_ui_connect_items_sync (data->registry, data->collection, cancellable, &error);

	if (cnc) {
		data->users = e_gw_connection_get_proxy_users_sync (cnc, cancellable, &error);
		data->own_email = g_strdup (e_gw_connection_get_user_email (cnc));
		e_gw_connection_logout_sync (cnc, cancellable);
		g_object_unref (cnc);
	}

	if (error)
		g_task_return_error (task, error);
	else
		g_task_return_boolean (task, TRUE);
}

/* Whether the setting has a calendar for @email already */
static gboolean
has_entry (const gchar *value,
	   const gchar *email)
{
	gchar **lines = value ? g_strsplit (value, "\n", -1) : NULL;
	gboolean found = FALSE;
	guint ii;

	for (ii = 0; lines && lines[ii] && !found; ii++) {
		const gchar *tab = strchr (lines[ii], '\t');

		found = tab && strlen (email) == (gsize) (tab - lines[ii]) &&
			g_ascii_strncasecmp (lines[ii], email, tab - lines[ii]) == 0;
	}
	g_strfreev (lines);

	return found;
}

/* The users whose calendars show already: proxy calendars of the account
 * (those of the GroupWise client as well) and proxy accounts; folded
 * e-mail address set */
static GHashTable *
list_shown (ESourceRegistry *registry,
	    ESource *collection)
{
	GHashTable *shown = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	GList *sources, *link;

	sources = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_CALENDAR);
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;
		gchar *proxy;

		if (g_strcmp0 (e_source_get_parent (source), e_source_get_uid (collection)) != 0 ||
		    !e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
			continue;
		proxy = e_source_groupwise_folder_dup_proxy (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
		if (proxy && *proxy)
			g_hash_table_add (shown, g_utf8_casefold (proxy, -1));
		g_free (proxy);
	}
	g_list_free_full (sources, g_object_unref);

	sources = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_COLLECTION);
	for (link = sources; link; link = g_list_next (link)) {
		gchar *proxy = e_groupwise_ui_dup_proxy (link->data);

		if (proxy && *proxy)
			g_hash_table_add (shown, g_utf8_casefold (proxy, -1));
		g_free (proxy);
	}
	g_list_free_full (sources, g_object_unref);

	return shown;
}

static void
user_changed_cb (GtkComboBox *combo,
		 GtkEntry *name)
{
	gchar *text = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (combo));

	gtk_entry_set_text (name, text ? text : "");
	g_free (text);
}

static void
loaded_cb (GObject *source_object,
	   GAsyncResult *result,
	   gpointer user_data)
{
	AddData *data = g_task_get_task_data (G_TASK (result));
	EAlertSink *sink = E_ALERT_SINK (e_shell_view_get_shell_content (data->shell_view));
	CamelGroupwiseSettings *settings;
	GtkWidget *dialog, *grid, *label, *combo, *name, *color;
	GdkRGBA rgba;
	GError *error = NULL;
	GHashTable *have;
	gchar *value;
	guint ii, shown = 0;

	if (!g_task_propagate_boolean (G_TASK (result), &error)) {
		e_alert_submit (sink, "mail:async-error", _("Reading the GroupWise proxy list"), error->message, NULL);
		g_clear_error (&error);
		return;
	}

	settings = e_gw_backend_ref_settings (data->registry, data->collection);
	if (!settings)
		return;
	value = camel_groupwise_settings_dup_proxy_calendars (settings);

	have = list_shown (data->registry, data->collection);
	combo = gtk_combo_box_text_new ();
	for (ii = 0; data->users && ii < data->users->len; ii++) {
		EGwProxyUser *user = g_ptr_array_index (data->users, ii);
		gchar *folded = user->email ? g_utf8_casefold (user->email, -1) : NULL;
		gboolean skip = !folded || !*folded || has_entry (value, user->email) || g_hash_table_contains (have, folded) ||
			(data->own_email && g_ascii_strcasecmp (user->email, data->own_email) == 0);

		g_free (folded);
		if (skip)
			continue;
		gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), user->email,
			user->display_name && *user->display_name ? user->display_name : user->email);
		shown++;
	}
	g_hash_table_destroy (have);
	if (!shown) {
		g_object_ref_sink (combo);
		g_object_unref (combo);
		e_alert_submit (sink, "mail:async-error", _("Adding a proxy calendar"),
			_("No other user has granted you proxy rights, or their calendars are set up already."), NULL);
		g_free (value);
		g_object_unref (settings);
		return;
	}

	dialog = gtk_dialog_new_with_buttons (_("Add Proxy Calendar"),
		GTK_WINDOW (e_shell_view_get_shell_window (data->shell_view)),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		_("_Cancel"), GTK_RESPONSE_CANCEL, _("_Add"), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_container_set_border_width (GTK_CONTAINER (grid), 12);
	gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))), grid, TRUE, TRUE, 0);

	label = gtk_label_new (_("The calendar of a user or resource that granted you proxy rights shows below "
		"your account. What you may do there is what they granted you for appointments."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_max_width_chars (GTK_LABEL (label), 50);
	gtk_label_set_xalign (GTK_LABEL (label), 0);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 2, 1);

	label = gtk_label_new_with_mnemonic (_("_User:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), combo);
	gtk_widget_set_hexpand (combo, TRUE);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), combo, 1, 1, 1, 1);

	name = gtk_entry_new ();
	gtk_entry_set_activates_default (GTK_ENTRY (name), TRUE);
	label = gtk_label_new_with_mnemonic (_("_Name:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), name);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 2, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), name, 1, 2, 1, 1);

	gdk_rgba_parse (&rgba, "#62a0ea");
	color = gtk_color_button_new_with_rgba (&rgba);
	gtk_widget_set_halign (color, GTK_ALIGN_START);
	label = gtk_label_new_with_mnemonic (_("C_olor:"));
	gtk_label_set_xalign (GTK_LABEL (label), 0);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), color);
	gtk_grid_attach (GTK_GRID (grid), label, 0, 3, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), color, 1, 3, 1, 1);

	g_signal_connect (combo, "changed", G_CALLBACK (user_changed_cb), name);
	gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
	gtk_widget_show_all (grid);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		const gchar *email = gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo));
		gchar *title = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (name))));
		gchar *new_value;

		gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (color), &rgba);
		if (!*title) {
			g_free (title);
			title = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (combo));
		}
		/* The fields of an entry are divided by tabs, the entries by lines */
		g_strdelimit (title, "\t\n", ' ');

		new_value = g_strdup_printf ("%s%s%s\t%s\t#%02x%02x%02x", value ? value : "", value && *value ? "\n" : "",
			email, title, (guint) (rgba.red * 255 + 0.5), (guint) (rgba.green * 255 + 0.5), (guint) (rgba.blue * 255 + 0.5));
		camel_groupwise_settings_set_proxy_calendars (settings, new_value);
		if (e_source_write_sync (data->collection, NULL, &error)) {
			/* The account lists its calendars anew */
			e_source_registry_refresh_backend (data->registry, e_source_get_uid (data->collection), NULL, NULL, NULL);
		} else {
			e_alert_submit (sink, "mail:async-error", _("Adding a proxy calendar"), error->message, NULL);
			g_clear_error (&error);
		}
		g_free (new_value);
		g_free (title);
	}
	gtk_widget_destroy (dialog);

	g_free (value);
	g_object_unref (settings);
}

void
e_groupwise_proxy_calendar_add (EShellView *shell_view,
				ESource *collection)
{
	EShell *shell;
	AddData *data;
	GTask *task;

	g_return_if_fail (E_IS_SHELL_VIEW (shell_view));
	g_return_if_fail (E_IS_SOURCE (collection));

	shell = e_shell_backend_get_shell (e_shell_view_get_shell_backend (shell_view));

	data = g_new0 (AddData, 1);
	data->shell_view = g_object_ref (shell_view);
	data->registry = g_object_ref (e_shell_get_registry (shell));
	data->collection = g_object_ref (collection);

	task = g_task_new (NULL, NULL, loaded_cb, NULL);
	g_task_set_task_data (task, data, add_data_free);
	g_task_run_in_thread (task, load_thread);
	g_object_unref (task);
}

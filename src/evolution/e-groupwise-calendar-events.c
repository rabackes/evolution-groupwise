/*
 * e-groupwise-calendar-events.c: calendars and lists follow the events of the mailbox
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

/* The mail store learns from the events of the POA what happens in the
 * mailbox, the calendars and lists included — but those are kept by
 * another process (the calendar factory). The store counts the events that
 * concern them in an object datum; this watches the number and has the
 * opened calendars, task and memo lists of that mailbox refreshed: those of
 * the store's account, and the proxy calendars of the same user that other
 * accounts show. */

#include <e-util/e-util.h>
#include <libemail-engine/libemail-engine.h>
#include <mail/e-mail-backend.h>
#include <shell/e-shell.h>
#include <shell/e-shell-view.h>

#include "e-source-groupwise-folder.h"
#include "e-groupwise-calendar-events.h"

#define CHECK_SECONDS 2
#define STORE_CALENDAR_EVENTS "groupwise-events-calendar"

static GHashTable *seen;	/* store UID -> number */

static const gchar *extensions[] = {
	E_SOURCE_EXTENSION_CALENDAR,
	E_SOURCE_EXTENSION_TASK_LIST,
	E_SOURCE_EXTENSION_MEMO_LIST
};

static void
refreshed_cb (GObject *source_object,
	      GAsyncResult *result,
	      gpointer user_data)
{
	GError *error = NULL;

	if (!e_client_refresh_finish (E_CLIENT (source_object), result, &error))
		g_debug ("events: refresh of %s: %s",
			e_source_get_display_name (e_client_get_source (E_CLIENT (source_object))), error ? error->message : "?");
	g_clear_error (&error);
}

static void
refresh_mailbox (EShell *shell,
		 CamelService *store)
{
	ESourceRegistry *registry = e_shell_get_registry (shell);
	EClientCache *client_cache = e_shell_get_client_cache (shell);
	ESource *account = e_source_registry_ref_source (registry, camel_service_get_uid (store));
	CamelSettings *settings = camel_service_ref_settings (store);
	gchar *collection = account ? e_source_dup_parent (account) : NULL, *proxy = NULL;
	guint ii, refreshed = 0;

	/* A proxy account: the mailbox of that user */
	if (settings && g_object_class_find_property (G_OBJECT_GET_CLASS (settings), "proxy"))
		g_object_get (settings, "proxy", &proxy, NULL);

	for (ii = 0; collection && ii < G_N_ELEMENTS (extensions); ii++) {
		GList *sources = e_source_registry_list_enabled (registry, extensions[ii]), *link;

		for (link = sources; link; link = g_list_next (link)) {
			ESource *source = link->data;
			gchar *parent = e_source_dup_parent (source), *role = NULL, *of = NULL;
			gboolean wanted;
			EClient *client;

			if (!e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER)) {
				g_free (parent);
				continue;
			}
			role = e_source_groupwise_folder_dup_role (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
			of = e_source_groupwise_folder_dup_proxy (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
			if (g_strcmp0 (role, E_GW_SOURCE_ROLE_PROXY) == 0)
				wanted = proxy && of && g_ascii_strcasecmp (proxy, of) == 0;
			else if (g_strcmp0 (role, E_GW_SOURCE_ROLE_SHARED) == 0)
				wanted = FALSE;
			else
				wanted = g_strcmp0 (parent, collection) == 0;

			/* Only what is open (the others refresh when opened) */
			client = wanted ? e_client_cache_ref_cached_client (client_cache, source, extensions[ii]) : NULL;
			if (client) {
				e_client_refresh (client, NULL, refreshed_cb, NULL);
				refreshed++;
				g_object_unref (client);
			}
			g_free (parent);
			g_free (role);
			g_free (of);
		}
		g_list_free_full (sources, g_object_unref);
	}
	g_debug ("events: %u calendars and lists of %s refreshed", refreshed, camel_service_get_display_name (store));

	g_free (collection);
	g_free (proxy);
	g_clear_object (&settings);
	g_clear_object (&account);
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
		const gchar *uid;
		guint number;

		if (!provider || g_strcmp0 (provider->protocol, "groupwise") != 0)
			continue;
		uid = camel_service_get_uid (link->data);
		number = GPOINTER_TO_UINT (g_object_get_data (link->data, STORE_CALENDAR_EVENTS));
		if (number != GPOINTER_TO_UINT (g_hash_table_lookup (seen, uid))) {
			g_hash_table_insert (seen, g_strdup (uid), GUINT_TO_POINTER (number));
			refresh_mailbox (shell, link->data);
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
e_groupwise_calendar_events_start (EShell *shell)
{
	GWeakRef *ref;

	g_return_if_fail (E_IS_SHELL (shell));

	if (seen)
		return;
	seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	ref = g_new0 (GWeakRef, 1);
	g_weak_ref_init (ref, shell);
	g_timeout_add_seconds_full (G_PRIORITY_DEFAULT_IDLE, CHECK_SECONDS, check_cb, ref, weak_ref_free);
}

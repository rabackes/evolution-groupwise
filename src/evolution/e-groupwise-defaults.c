/*
 * e-groupwise-defaults.c: the main account's calendar and lists as Evolution's defaults
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

/* "New → Meeting" outside the calendar view makes the meeting in
 * Evolution's default calendar. As long as that is the built-in one "On
 * This Computer", the meeting is no GroupWise meeting at all, and its
 * organizer is the first of all identities — with proxy accounts, one of
 * those. So the calendar, the task list and the memo list of the GroupWise
 * main account become the defaults: once per account, and only where the
 * default is still the built-in one (also when what was the default is
 * gone). Whatever the user makes the default afterwards stays. */

#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

#include "camel-groupwise-settings.h"
#include "e-source-groupwise-folder.h"
#include "e-groupwise-defaults.h"

#define STATE_GROUP "Defaults"

static gchar *
dup_state_file (void)
{
	return g_build_filename (g_get_user_config_dir (), "evolution-groupwise", "defaults.ini", NULL);
}

static gboolean
is_done (const gchar *account_uid)
{
	GKeyFile *key_file = g_key_file_new ();
	gchar *path = dup_state_file ();
	gboolean done;

	done = g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, NULL) &&
		g_key_file_get_boolean (key_file, STATE_GROUP, account_uid, NULL);
	g_key_file_free (key_file);
	g_free (path);

	return done;
}

static void
set_done (const gchar *account_uid)
{
	GKeyFile *key_file = g_key_file_new ();
	gchar *path = dup_state_file (), *dir = g_path_get_dirname (path);

	g_key_file_load_from_file (key_file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	g_key_file_set_boolean (key_file, STATE_GROUP, account_uid, TRUE);
	g_mkdir_with_parents (dir, 0700);
	g_key_file_save_to_file (key_file, path, NULL);
	g_key_file_free (key_file);
	g_free (dir);
	g_free (path);
}

/* The Calendar (or the one task or memo list) of the account: the folder
 * without a role */
static ESource *
ref_main_folder (ESourceRegistry *registry,
		 const gchar *account_uid,
		 const gchar *extension_name)
{
	GList *sources = e_source_registry_list_enabled (registry, extension_name), *link;
	ESource *found = NULL;

	for (link = sources; link && !found; link = g_list_next (link)) {
		ESource *source = link->data;
		gchar *role;

		if (g_strcmp0 (e_source_get_parent (source), account_uid) != 0 ||
		    !e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
			continue;
		role = e_source_groupwise_folder_dup_role (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
		if (!role || !*role)
			found = g_object_ref (source);
		g_free (role);
	}
	g_list_free_full (sources, g_object_unref);

	return found;
}

static void
check (ESourceRegistry *registry)
{
	const gchar *camel_extension = e_source_camel_get_extension_name ("groupwise");
	GList *accounts = e_source_registry_list_enabled (registry, camel_extension), *link;

	for (link = accounts; link; link = g_list_next (link)) {
		ESource *account = link->data;
		CamelSettings *settings = e_source_camel_get_settings (e_source_get_extension (account, camel_extension));
		ESource *calendar, *tasks, *memos, *current, *builtin;
		gchar *proxy;
		gboolean is_proxy;

		if (!CAMEL_IS_GROUPWISE_SETTINGS (settings))
			continue;
		proxy = camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (settings));
		is_proxy = proxy && *proxy;
		g_free (proxy);
		if (is_proxy || is_done (e_source_get_uid (account)))
			continue;

		/* Its folders come a moment after the account */
		calendar = ref_main_folder (registry, e_source_get_uid (account), E_SOURCE_EXTENSION_CALENDAR);
		if (!calendar)
			continue;
		tasks = ref_main_folder (registry, e_source_get_uid (account), E_SOURCE_EXTENSION_TASK_LIST);
		memos = ref_main_folder (registry, e_source_get_uid (account), E_SOURCE_EXTENSION_MEMO_LIST);

		current = e_source_registry_ref_default_calendar (registry);
		builtin = e_source_registry_ref_builtin_calendar (registry);
		if (current == builtin) {
			g_debug ("defaults: the default calendar is %s of %s now", e_source_get_display_name (calendar),
				e_source_get_display_name (account));
			e_source_registry_set_default_calendar (registry, calendar);
		}
		g_clear_object (&current);
		g_clear_object (&builtin);

		current = e_source_registry_ref_default_task_list (registry);
		builtin = e_source_registry_ref_builtin_task_list (registry);
		if (tasks && current == builtin)
			e_source_registry_set_default_task_list (registry, tasks);
		g_clear_object (&current);
		g_clear_object (&builtin);

		current = e_source_registry_ref_default_memo_list (registry);
		builtin = e_source_registry_ref_builtin_memo_list (registry);
		if (memos && current == builtin)
			e_source_registry_set_default_memo_list (registry, memos);
		g_clear_object (&current);
		g_clear_object (&builtin);

		set_done (e_source_get_uid (account));
		g_clear_object (&calendar);
		g_clear_object (&tasks);
		g_clear_object (&memos);
		/* The first main account has them */
		break;
	}
	g_list_free_full (accounts, g_object_unref);
}

static void
source_added_cb (ESourceRegistry *registry,
		 ESource *source,
		 gpointer user_data)
{
	if (e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR) &&
	    e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
		check (registry);
}

void
e_groupwise_defaults_start (ESourceRegistry *registry)
{
	static gboolean started;

	g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));

	if (started)
		return;
	started = TRUE;

	g_signal_connect (registry, "source-added", G_CALLBACK (source_added_cb), NULL);
	check (registry);
}

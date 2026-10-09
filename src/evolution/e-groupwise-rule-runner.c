/*
 * e-groupwise-rule-runner.c: Evolution runs the rules of the client's events
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
 * The POA runs the rules of new and filed items itself; those of the
 * client's events (Startup, Exit, FolderOpen, FolderClose) run only when a
 * client asks for them with executeRule. Evolution does that for accounts
 * whose settings want it: at startup, at quit (the quit waits), when a
 * folder is opened or left. One logged-in connection and the rules per
 * account, kept until the rules change.
 */

#include <string.h>

#include <glib/gi18n-lib.h>
#include <shell/e-shell.h>

#include "e-gw-backend-utils.h"
#include "e-gw-folder.h"
#include "e-gw-rule.h"

#include "e-groupwise-rule-runner.h"
#include "e-groupwise-ui-utils.h"

typedef struct {
	GMutex lock;		/* one request at a time per account */
	EGwConnection *cnc;
	GPtrArray *rules;	/* EGwRule, NULL: to be read */
	GHashTable *folder_ids;	/* Camel full name -> GroupWise folder ID */
} Account;

static GMutex accounts_lock;
static GHashTable *accounts;	/* account UID -> Account */

static Account *
ref_account (const gchar *uid)
{
	Account *account;

	g_mutex_lock (&accounts_lock);
	if (!accounts)
		accounts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	account = g_hash_table_lookup (accounts, uid);
	if (!account) {
		account = g_new0 (Account, 1);
		g_mutex_init (&account->lock);
		g_hash_table_insert (accounts, g_strdup (uid), account);
	}
	g_mutex_unlock (&accounts_lock);

	/* Accounts stay for the process */
	return account;
}

void
e_groupwise_rule_runner_forget (const gchar *account_uid)
{
	Account *account;

	g_return_if_fail (account_uid != NULL);

	account = ref_account (account_uid);
	g_mutex_lock (&account->lock);
	g_clear_pointer (&account->rules, g_ptr_array_unref);
	g_clear_pointer (&account->folder_ids, g_hash_table_destroy);
	g_mutex_unlock (&account->lock);
}

/* The Camel full names of the folders, as the store builds them */
static GHashTable *
folder_ids_of (GPtrArray *folders)
{
	GHashTable *by_id = g_hash_table_new (g_str_hash, g_str_equal);
	GHashTable *ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	guint ii;

	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];

		g_hash_table_insert (by_id, folder->id, folder);
	}
	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii], *up = folder;
		GString *path = g_string_new (NULL);
		guint depth = 0;

		while (up && up->type != E_GW_FOLDER_TYPE_ROOT && depth++ < 64) {
			gchar *name = g_strdup (up->name && *up->name ? up->name : up->id);

			g_strdelimit (name, "/", '_');
			if (path->len)
				g_string_prepend_c (path, '/');
			g_string_prepend (path, name);
			g_free (name);
			up = up->parent_id ? g_hash_table_lookup (by_id, up->parent_id) : NULL;
		}
		if (path->len)
			g_hash_table_insert (ids, g_string_free (path, FALSE), g_strdup (folder->id));
		else
			g_string_free (path, TRUE);
	}
	g_hash_table_destroy (by_id);

	return ids;
}

/* Logged in, with the rules; the account is locked */
static gboolean
prepare_account (Account *account,
		 ESourceRegistry *registry,
		 ESource *account_source,
		 GCancellable *cancellable,
		 GError **error)
{
	if (!account->cnc) {
		account->cnc = e_groupwise_ui_connect_sync (registry, account_source, cancellable, error);
		if (!account->cnc)
			return FALSE;
	}
	if (!account->rules) {
		GPtrArray *folders;

		account->rules = e_gw_connection_get_rules_sync (account->cnc, cancellable, error);
		if (!account->rules)
			return FALSE;
		folders = e_gw_connection_get_folder_list_sync (account->cnc, NULL, TRUE, cancellable, NULL);
		if (folders) {
			account->folder_ids = folder_ids_of (folders);
			g_ptr_array_unref (folders);
		}
	}

	return TRUE;
}

/* The enabled rules of the event (for the folder: of it or of any folder) */
static gboolean
run_event_sync (ESourceRegistry *registry,
		ESource *account_source,
		const gchar *execution,
		const gchar *folder_full_name,
		GCancellable *cancellable,
		GError **error)
{
	Account *account = ref_account (e_source_get_uid (account_source));
	const gchar *folder_id = NULL;
	gboolean success;
	guint ii;

	g_mutex_lock (&account->lock);
	success = prepare_account (account, registry, account_source, cancellable, error);
	if (success && folder_full_name)
		folder_id = account->folder_ids ? g_hash_table_lookup (account->folder_ids, folder_full_name) : NULL;
	for (ii = 0; success && ii < account->rules->len; ii++) {
		EGwRule *rule = account->rules->pdata[ii];

		if (!rule->enabled || g_strcmp0 (rule->execution, execution) != 0)
			continue;
		if (folder_full_name && rule->container && g_strcmp0 (rule->container, folder_id) != 0)
			continue;
		g_debug ("rule %s (%s) of %s: run", rule->name, execution, e_source_get_uid (account_source));
		success = e_gw_connection_execute_rule_sync (account->cnc, rule->id, cancellable, error);
	}
	g_mutex_unlock (&account->lock);

	return success;
}

/* ------------------------------------------------------------------ */
/* One rule */

typedef struct {
	ESourceRegistry *registry;
	ESource *account_source;
	gchar *rule_id;
	gchar *execution;
	gchar *folder;
	GObject *keep;		/* kept until done (the quit's activity) */
} Job;

static void
job_free (Job *job)
{
	g_object_unref (job->registry);
	g_object_unref (job->account_source);
	g_free (job->rule_id);
	g_free (job->execution);
	g_free (job->folder);
	g_clear_object (&job->keep);
	g_free (job);
}

static Job *
job_new (ESourceRegistry *registry,
	 ESource *account_source)
{
	Job *job = g_new0 (Job, 1);

	job->registry = g_object_ref (registry);
	job->account_source = g_object_ref (account_source);

	return job;
}

static void
run_rule_thread (GTask *task,
		 gpointer source_object,
		 gpointer task_data,
		 GCancellable *cancellable)
{
	Job *job = task_data;
	Account *account = ref_account (e_source_get_uid (job->account_source));
	GError *error = NULL;
	gboolean success;

	g_mutex_lock (&account->lock);
	if (!account->cnc)
		account->cnc = e_groupwise_ui_connect_sync (job->registry, job->account_source, cancellable, &error);
	success = account->cnc && e_gw_connection_execute_rule_sync (account->cnc, job->rule_id, cancellable, &error);
	g_mutex_unlock (&account->lock);

	if (success)
		g_task_return_boolean (task, TRUE);
	else
		g_task_return_error (task, error);
}

void
e_groupwise_rule_runner_run (ESourceRegistry *registry,
			     ESource *account_source,
			     const gchar *rule_id,
			     GCancellable *cancellable,
			     GAsyncReadyCallback callback,
			     gpointer user_data)
{
	Job *job = job_new (registry, account_source);
	GTask *task = g_task_new (NULL, cancellable, callback, user_data);

	job->rule_id = g_strdup (rule_id);
	g_task_set_task_data (task, job, (GDestroyNotify) job_free);
	g_task_run_in_thread (task, run_rule_thread);
	g_object_unref (task);
}

gboolean
e_groupwise_rule_runner_run_finish (GAsyncResult *result,
				    GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------ */
/* Events */

static void
event_thread (GTask *task,
	      gpointer source_object,
	      gpointer task_data,
	      GCancellable *cancellable)
{
	Job *job = task_data;
	GError *error = NULL;

	if (!run_event_sync (job->registry, job->account_source, job->execution, job->folder, cancellable, &error)) {
		g_warning ("GroupWise: the %s rules of %s: %s", job->execution, e_source_get_display_name (job->account_source),
			error ? error->message : "?");
		g_clear_error (&error);
	}
	g_task_return_boolean (task, TRUE);
}

static void
run_event (ESourceRegistry *registry,
	   ESource *account_source,
	   const gchar *execution,
	   const gchar *folder,
	   GObject *keep)
{
	Job *job = job_new (registry, account_source);
	GTask *task = g_task_new (NULL, NULL, NULL, NULL);

	job->execution = g_strdup (execution);
	job->folder = g_strdup (folder);
	job->keep = keep ? g_object_ref (keep) : NULL;
	g_task_set_task_data (task, job, (GDestroyNotify) job_free);
	g_task_run_in_thread (task, event_thread);
	g_object_unref (task);
}

typedef enum {
	WANT_STARTUP,
	WANT_FOLDERS
} Want;

static gboolean
account_wants (ESourceRegistry *registry,
	       ESource *account_source,
	       Want want)
{
	CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (registry, account_source);
	gboolean wants = FALSE;

	if (settings) {
		wants = want == WANT_STARTUP ? camel_groupwise_settings_get_run_startup_rules (settings) :
			camel_groupwise_settings_get_run_folder_rules (settings);
		g_object_unref (settings);
	}

	return wants;
}

/* The enabled GroupWise mail accounts that want the startup rules */
static GList *
startup_accounts (ESourceRegistry *registry)
{
	GList *sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_MAIL_ACCOUNT), *link, *wanted = NULL;

	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;

		if (g_strcmp0 (e_source_backend_get_backend_name (e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT)),
			       "groupwise") == 0 && account_wants (registry, source, WANT_STARTUP))
			wanted = g_list_prepend (wanted, g_object_ref (source));
	}
	g_list_free_full (sources, g_object_unref);

	return wanted;
}

static void
prepare_for_quit_cb (EShell *shell,
		     EActivity *activity,
		     ESourceRegistry *registry)
{
	GList *accounts_list = startup_accounts (registry), *link;

	/* The quit waits for the activity */
	for (link = accounts_list; link; link = g_list_next (link))
		run_event (registry, link->data, "Exit", NULL, G_OBJECT (activity));
	g_list_free_full (accounts_list, g_object_unref);
}

static void
unref_registry (gpointer data,
		GClosure *closure)
{
	g_object_unref (data);
}

void
e_groupwise_rule_runner_start (ESourceRegistry *registry)
{
	static gboolean started;
	GList *accounts_list, *link;
	EShell *shell;

	g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));

	if (started)
		return;
	started = TRUE;

	accounts_list = startup_accounts (registry);
	for (link = accounts_list; link; link = g_list_next (link))
		run_event (registry, link->data, "Startup", NULL, NULL);
	g_list_free_full (accounts_list, g_object_unref);

	shell = e_shell_get_default ();
	if (shell)
		g_signal_connect_data (shell, "prepare-for-quit", G_CALLBACK (prepare_for_quit_cb),
			g_object_ref (registry), unref_registry, 0);
}

static void
folder_event (ESourceRegistry *registry,
	      CamelFolder *folder,
	      const gchar *execution)
{
	CamelStore *store = folder ? camel_folder_get_parent_store (folder) : NULL;
	ESource *account_source = store ? e_groupwise_ui_ref_account_source (registry, CAMEL_SERVICE (store)) : NULL;

	if (account_source && account_wants (registry, account_source, WANT_FOLDERS))
		run_event (registry, account_source, execution, camel_folder_get_full_name (folder), NULL);
	g_clear_object (&account_source);
}

void
e_groupwise_rule_runner_folder_changed (ESourceRegistry *registry,
					CamelFolder *left,
					CamelFolder *opened)
{
	g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));

	if (left == opened)
		return;

	folder_event (registry, left, "FolderClose");
	folder_event (registry, opened, "FolderOpen");
}

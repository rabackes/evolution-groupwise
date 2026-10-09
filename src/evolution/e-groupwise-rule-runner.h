/*
 * e-groupwise-rule-runner.h: Evolution runs the rules of the client's events
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

#ifndef E_GROUPWISE_RULE_RUNNER_H
#define E_GROUPWISE_RULE_RUNNER_H

#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

/* Runs one rule now (executeRule) */
void		e_groupwise_rule_runner_run	(ESourceRegistry *registry,
						 ESource *account_source,
						 const gchar *rule_id,
						 GCancellable *cancellable,
						 GAsyncReadyCallback callback,
						 gpointer user_data);
gboolean	e_groupwise_rule_runner_run_finish
						(GAsyncResult *result,
						 GError **error);

/* Once per process: the Startup rules of the accounts that want it, and
 * their Exit rules when Evolution quits */
void		e_groupwise_rule_runner_start	(ESourceRegistry *registry);

/* A folder was left and another one opened (each may be NULL): their
 * Close Folder and Open Folder rules, for accounts that want it */
void		e_groupwise_rule_runner_folder_changed
						(ESourceRegistry *registry,
						 CamelFolder *left,
						 CamelFolder *opened);

/* The rules of the account changed: read them anew when needed */
void		e_groupwise_rule_runner_forget	(const gchar *account_uid);

G_END_DECLS

#endif /* E_GROUPWISE_RULE_RUNNER_H */

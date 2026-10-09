/*
 * camel-groupwise-settings.h: settings of a GroupWise mail account
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

#ifndef CAMEL_GROUPWISE_SETTINGS_H
#define CAMEL_GROUPWISE_SETTINGS_H

#include <camel/camel.h>

G_BEGIN_DECLS

#define CAMEL_TYPE_GROUPWISE_SETTINGS (camel_groupwise_settings_get_type ())
G_DECLARE_FINAL_TYPE (CamelGroupwiseSettings, camel_groupwise_settings, CAMEL, GROUPWISE_SETTINGS, CamelOfflineSettings)

/* Read-only: flag changes stay in Evolution, nothing is written to the
 * server. For archives, shared mailboxes and first tests. */
gboolean	camel_groupwise_settings_get_read_only
						(CamelGroupwiseSettings *settings);
void		camel_groupwise_settings_set_read_only
						(CamelGroupwiseSettings *settings,
						 gboolean read_only);

/* Whether new messages in the Mailbox go through Evolution's filters and
 * junk test (each one is downloaded for that). Off by default: GroupWise
 * applies rules and spam filtering on the server. */
gboolean	camel_groupwise_settings_get_filter_inbox
						(CamelGroupwiseSettings *settings);
void		camel_groupwise_settings_set_filter_inbox
						(CamelGroupwiseSettings *settings,
						 gboolean filter_inbox);

/* Rules of the GroupWise client's events, which the POA does not run
 * itself: Startup and Exit when Evolution starts and quits, Open Folder and
 * Close Folder when a folder is opened or left. Off by default. */
gboolean	camel_groupwise_settings_get_run_startup_rules
						(CamelGroupwiseSettings *settings);
void		camel_groupwise_settings_set_run_startup_rules
						(CamelGroupwiseSettings *settings,
						 gboolean run_startup_rules);
gboolean	camel_groupwise_settings_get_run_folder_rules
						(CamelGroupwiseSettings *settings);
void		camel_groupwise_settings_set_run_folder_rules
						(CamelGroupwiseSettings *settings,
						 gboolean run_folder_rules);

/* A proxy account: the e-mail address of the user whose mailbox it shows,
 * logged in as the account's user; NULL for the user's own mailbox */
gchar *		camel_groupwise_settings_dup_proxy
						(CamelGroupwiseSettings *settings);
void		camel_groupwise_settings_set_proxy
						(CamelGroupwiseSettings *settings,
						 const gchar *proxy);

/* Default port of the POA SOAP interface */
#define CAMEL_GROUPWISE_DEFAULT_PORT 7191

G_END_DECLS

#endif /* CAMEL_GROUPWISE_SETTINGS_H */

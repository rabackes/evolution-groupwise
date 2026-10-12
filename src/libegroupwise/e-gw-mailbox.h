/*
 * e-gw-mailbox.h: the mailbox itself: its size and its password
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

#ifndef E_GW_MAILBOX_H
#define E_GW_MAILBOX_H

#include "e-gw-connection.h"

G_BEGIN_DECLS

/* getDiskSpaceUsageRequest, in megabytes */
typedef struct {
	gint64 used;
	gint64 max_size;	/* 0: the mailbox has no limit */
	gint threshold;		/* percent of @max_size from which the user is warned */
} EGwDiskUsage;

gboolean	e_gw_connection_get_disk_usage_sync
						(EGwConnection *cnc,
						 EGwDiskUsage *out_usage,
						 GCancellable *cancellable,
						 GError **error);

/* modifyPasswordRequest: the password of the user logged in. A wrong old
 * password gives E_GW_ERROR_WRONG_PASSWORD. Where the system checks the
 * passwords in a directory (LDAP), the POA may refuse. */
#define E_GW_ERROR_WRONG_PASSWORD 53273

gboolean	e_gw_connection_modify_password_sync
						(EGwConnection *cnc,
						 const gchar *old_password,
						 const gchar *new_password,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_MAILBOX_H */

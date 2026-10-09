/*
 * e-gw-proxy.h: the proxy access to the user's mailbox
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

#ifndef E_GW_PROXY_H
#define E_GW_PROXY_H

#include <libxml/tree.h>

#include "e-gw-connection.h"

G_BEGIN_DECLS

/* The UUID of the entry for all users of the system */
#define E_GW_PROXY_ALL_USERS "<All User Access>"

/*
 * Who may log in as proxy of the user, with which rights (an
 * AccessRightEntry of getProxyAccessListRequest).
 */
typedef struct {
	gchar *id;		/* for modify and remove */
	gchar *uuid;		/* E_GW_PROXY_ALL_USERS: every user */
	gchar *email;
	gchar *display_name;
	EGwProxyRights rights;
} EGwProxyAccess;

void		e_gw_proxy_access_free		(EGwProxyAccess *access);
gboolean	e_gw_proxy_access_is_all_users	(const EGwProxyAccess *access);

/* The rights in an <entry> (proxy login, access list) */
EGwProxyRights	e_gw_proxy_rights_from_entry	(xmlNode *entry);

/* Returns: (transfer container) (element-type EGwProxyAccess) (nullable) */
GPtrArray *	e_gw_connection_get_proxy_access_list_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);
/* Grants the user with the e-mail address or name @who the rights;
 * returns the ID of the entry */
gchar *		e_gw_connection_create_proxy_access_sync
						(EGwConnection *cnc,
						 const gchar *who,
						 EGwProxyRights rights,
						 GCancellable *cancellable,
						 GError **error);
/* Changes the rights of an entry from @old_rights to @new_rights */
gboolean	e_gw_connection_modify_proxy_access_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 EGwProxyRights old_rights,
						 EGwProxyRights new_rights,
						 GCancellable *cancellable,
						 GError **error);
gboolean	e_gw_connection_remove_proxy_access_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_PROXY_H */

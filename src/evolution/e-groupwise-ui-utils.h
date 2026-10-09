/*
 * e-groupwise-ui-utils.h: helpers of the Evolution module
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

#ifndef E_GROUPWISE_UI_UTILS_H
#define E_GROUPWISE_UI_UTILS_H

#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

#include "e-gw-connection.h"

G_BEGIN_DECLS

/* The mail account source of a GroupWise store, NULL for other stores */
ESource *	e_groupwise_ui_ref_account_source
						(ESourceRegistry *registry,
						 CamelService *service);

/* A connection of its own to the POA of @account_source, logged in with the
 * password of the account from the keyring (for settings of the server);
 * for a proxy account a proxy login, which needs the right to change the
 * settings of the other mailbox */
EGwConnection *	e_groupwise_ui_connect_sync	(ESourceRegistry *registry,
						 ESource *account_source,
						 GCancellable *cancellable,
						 GError **error);

/* The same for work on the items of the mailbox (a proxy login with the
 * rights the other user granted for them) */
EGwConnection *	e_groupwise_ui_connect_items_sync
						(ESourceRegistry *registry,
						 ESource *account_source,
						 GCancellable *cancellable,
						 GError **error);

/* The e-mail address of the user whose mailbox a proxy account shows (the
 * setting Proxy of the source itself); NULL for other sources */
gchar *		e_groupwise_ui_dup_proxy	(ESource *source);
/* Whether the store is one of a GroupWise proxy account */
gboolean	e_groupwise_ui_is_proxy_store	(CamelService *service);

G_END_DECLS

#endif /* E_GROUPWISE_UI_UTILS_H */

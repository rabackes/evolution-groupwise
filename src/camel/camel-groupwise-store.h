/*
 * camel-groupwise-store.h: Camel store backed by the GroupWise SOAP interface
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

#ifndef CAMEL_GROUPWISE_STORE_H
#define CAMEL_GROUPWISE_STORE_H

#include <camel/camel.h>

#include "e-gw-connection.h"
#include "camel-groupwise-labels.h"
#include "camel-groupwise-store-summary.h"

G_BEGIN_DECLS

#define CAMEL_TYPE_GROUPWISE_STORE (camel_groupwise_store_get_type ())
G_DECLARE_FINAL_TYPE (CamelGroupwiseStore, camel_groupwise_store, CAMEL, GROUPWISE_STORE, CamelOfflineStore)

extern CamelServiceAuthType camel_groupwise_password_authtype;

/* The logged-in connection, NULL while offline or disconnected */
EGwConnection *	camel_groupwise_store_ref_connection
						(CamelGroupwiseStore *store);
/* Like ref_connection, but connects first when online and not connected */
EGwConnection *	camel_groupwise_store_ref_connection_sync
						(CamelGroupwiseStore *store,
						 GCancellable *cancellable,
						 GError **error);
CamelGroupwiseStoreSummary *
		camel_groupwise_store_get_summary
						(CamelGroupwiseStore *store);
/* The categories as Evolution's labels */
CamelGroupwiseLabels *
		camel_groupwise_store_get_labels
						(CamelGroupwiseStore *store);

/* Read-only by the settings, or a proxy login without the right to write mail */
gboolean	camel_groupwise_store_get_read_only
						(CamelGroupwiseStore *store);

/* Data of the store object: the port (as a pointer) the POA was to tell
 * the events of the mailbox at and, as it seems, does not reach; 0 else */
#define CAMEL_GROUPWISE_STORE_PORT_UNREACHABLE "groupwise-events-port-unreachable"

G_END_DECLS

#endif /* CAMEL_GROUPWISE_STORE_H */

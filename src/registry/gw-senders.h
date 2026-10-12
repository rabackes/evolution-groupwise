/*
 * gw-senders.h: the mailboxes the user may send from as senders of the account
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

#ifndef GW_SENDERS_H
#define GW_SENDERS_H

#include <libebackend/libebackend.h>

#include "e-gw-connection.h"

G_BEGIN_DECLS

/* The users and resources that granted the user proxy rights as further
 * senders (aliases) of the identity of the collection */
void		gw_collection_sync_senders	(ECollectionBackend *backend,
						 EGwConnection *cnc,
						 GCancellable *cancellable);

G_END_DECLS

#endif /* GW_SENDERS_H */

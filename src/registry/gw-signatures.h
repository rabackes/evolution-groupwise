/*
 * gw-signatures.h: the GroupWise signatures as signatures of Evolution
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

#ifndef GW_SIGNATURES_H
#define GW_SIGNATURES_H

#include <libebackend/libebackend.h>

#include "e-gw-connection.h"

G_BEGIN_DECLS

/* The signatures of the mailbox as children of the collection (@proxy:
 * the user of a proxy account, NULL for the own mailbox) */
void		gw_collection_sync_signatures	(ECollectionBackend *backend,
						 EGwConnection *cnc,
						 const gchar *proxy,
						 GCancellable *cancellable);

G_END_DECLS

#endif /* GW_SIGNATURES_H */

/*
 * camel-groupwise-transport.h: sends mail through the GroupWise POA
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

#ifndef CAMEL_GROUPWISE_TRANSPORT_H
#define CAMEL_GROUPWISE_TRANSPORT_H

#include <camel/camel.h>

G_BEGIN_DECLS

#define CAMEL_TYPE_GROUPWISE_TRANSPORT (camel_groupwise_transport_get_type ())
G_DECLARE_FINAL_TYPE (CamelGroupwiseTransport, camel_groupwise_transport, CAMEL, GROUPWISE_TRANSPORT, CamelTransport)

G_END_DECLS

#endif /* CAMEL_GROUPWISE_TRANSPORT_H */

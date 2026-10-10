/*
 * e-groupwise-proxy-options.h: options of the main account for its proxy accounts
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

#ifndef E_GROUPWISE_PROXY_OPTIONS_H
#define E_GROUPWISE_PROXY_OPTIONS_H

#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

/* From now on the proxy accounts follow the options of their main account
 * for the events of the mailbox */
void		e_groupwise_proxy_options_start	(ESourceRegistry *registry);

G_END_DECLS

#endif /* E_GROUPWISE_PROXY_OPTIONS_H */

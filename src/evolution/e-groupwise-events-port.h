/*
 * e-groupwise-events-port.h: asks to open the port the server tells events at
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

#ifndef E_GROUPWISE_EVENTS_PORT_H
#define E_GROUPWISE_EVENTS_PORT_H

#include <shell/e-shell.h>

G_BEGIN_DECLS

/* Watches the GroupWise mail stores for a port the server
 * does not reach, and asks the user about the firewall then */
void		e_groupwise_events_port_start	(EShell *shell);

G_END_DECLS

#endif /* E_GROUPWISE_EVENTS_PORT_H */

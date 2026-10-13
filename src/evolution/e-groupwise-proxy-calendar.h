/*
 * e-groupwise-proxy-calendar.h: setting up the calendar of another user as a proxy calendar
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

#ifndef E_GROUPWISE_PROXY_CALENDAR_H
#define E_GROUPWISE_PROXY_CALENDAR_H

#include <libedataserver/libedataserver.h>
#include <shell/e-shell-view.h>

G_BEGIN_DECLS

/* Asks for one of the users who granted the user of @collection (a
 * GroupWise account, not a proxy account) proxy rights, and shows that
 * user's calendar below the account. GroupWise keeps the proxy calendars
 * of its own client as folders that cannot be made from outside: this one
 * is kept in the settings of the account. */
void		e_groupwise_proxy_calendar_add	(EShellView *shell_view,
						 ESource *collection);

G_END_DECLS

#endif /* E_GROUPWISE_PROXY_CALENDAR_H */

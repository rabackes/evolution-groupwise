/*
 * e-groupwise-share-accept.h: accepting a folder another user shares
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

#ifndef E_GROUPWISE_SHARE_ACCEPT_H
#define E_GROUPWISE_SHARE_ACCEPT_H

#include <camel/camel.h>
#include <shell/e-shell-view.h>

G_BEGIN_DECLS

/* Whether the message of @uid is the notification of a shared folder */
gboolean	e_groupwise_share_accept_is_notification
						(const gchar *uid);

/* Shows who shares which folder with which rights and asks for the name,
 * the description and the place of the folder in the mailbox of
 * @account; then accepts. @folder holds the notification @uid. */
void		e_groupwise_share_accept_run	(EShellView *shell_view,
						 ESourceRegistry *registry,
						 ESource *account,
						 CamelFolder *folder,
						 const gchar *uid);

G_END_DECLS

#endif /* E_GROUPWISE_SHARE_ACCEPT_H */

/*
 * e-groupwise-phone-message.h: writing a GroupWise phone message
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

#ifndef E_GROUPWISE_PHONE_MESSAGE_H
#define E_GROUPWISE_PHONE_MESSAGE_H

#include <libedataserver/libedataserver.h>
#include <shell/e-shell-view.h>

G_BEGIN_DECLS

/* Opens the dialog for a new phone message. It is sent from the mailbox of
 * @preferred_account (a GroupWise mail account), or of the user's own
 * account when that is NULL. Does nothing without a GroupWise account. */
void		e_groupwise_phone_message_new	(EShellView *shell_view,
						 ESource *preferred_account);

G_END_DECLS

#endif /* E_GROUPWISE_PHONE_MESSAGE_H */

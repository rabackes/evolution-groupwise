/*
 * e-groupwise-sent-items.h: delivery status and retract of sent items
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

#ifndef E_GROUPWISE_SENT_ITEMS_H
#define E_GROUPWISE_SENT_ITEMS_H

#include <gtk/gtk.h>
#include <libedataserver/libedataserver.h>
#include <e-util/e-util.h>
#include <shell/e-shell.h>

G_BEGIN_DECLS

/* A window with what happened to the sent item @uid (its ID in the Sent
 * Items) at each recipient */
void		e_groupwise_sent_status_show	(GtkWindow *parent,
						 ESourceRegistry *registry,
						 ESource *account,
						 const gchar *uid,
						 const gchar *subject);
/* Asks, then takes the sent items back from the recipients' mailboxes;
 * errors go to @alert_sink */
void		e_groupwise_sent_retract	(GtkWindow *parent,
						 EAlertSink *alert_sink,
						 ESourceRegistry *registry,
						 ESource *account,
						 GPtrArray *uids,
						 const gchar *subject);

/* Opens the sent message @uid of @folder in a composer to be sent anew;
 * sending asks whether the original is retracted */
void		e_groupwise_sent_resend		(EShell *shell,
						 EAlertSink *alert_sink,
						 ESourceRegistry *registry,
						 ESource *account,
						 CamelFolder *folder,
						 const gchar *uid,
						 const gchar *subject);

G_END_DECLS

#endif /* E_GROUPWISE_SENT_ITEMS_H */

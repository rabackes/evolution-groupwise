/*
 * e-gw-events.h: what happened in a mailbox (GroupWise Web Services Events)
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

#ifndef E_GW_EVENTS_H
#define E_GW_EVENTS_H

#include "e-gw-connection.h"

G_BEGIN_DECLS

/*
 * The POA writes down what happens in a mailbox for the applications that
 * asked for it: an event configuration under a key of the application,
 * kept in the mailbox (of the other user in a proxy session). The records
 * say which item changed how, and mostly in which folder; reading them
 * with @remove takes them away. Other applications have configurations
 * there too (GroupWise Mobility): only the own key is ever touched.
 */

/* What the mail side wants to know about */
#define E_GW_EVENTS_MAIL \
	"FolderItemAdd", "FolderItemMove", "ItemDelete", "ItemUndelete", "ItemPurge", \
	"ItemMarkRead", "ItemMarkUnread", "ItemModify"

typedef struct {
	gchar *type;		/* "FolderItemAdd", "ItemMarkRead" … */
	gchar *item;		/* the item part of its ID, without type and container */
	gchar *container;	/* where it is now; NULL when the POA does not say */
	gchar *from;		/* where it was (moved, deleted); mostly NULL */
} EGwEvent;

void		e_gw_event_free			(EGwEvent *event);

/* Sets the configuration @key up (again): the event types (NULL-terminated),
 * records kept @persistence_days (0–20). With @address the POA also
 * connects to @address:@port for each first new record (after a
 * e_gw_connection_get_events_sync() with @notify), else it only records. */
gboolean	e_gw_connection_configure_events_sync
						(EGwConnection *cnc,
						 const gchar *key,
						 const gchar * const *events,
						 guint persistence_days,
						 const gchar *address,
						 guint port,
						 GCancellable *cancellable,
						 GError **error);

/* The records of @key since the last read with @remove.
 * Returns: (transfer container) (element-type EGwEvent) (nullable) */
GPtrArray *	e_gw_connection_get_events_sync	(EGwConnection *cnc,
						 const gchar *key,
						 gboolean remove,
						 gboolean notify,
						 GCancellable *cancellable,
						 GError **error);

/* Takes the configuration @key and its records out of the mailbox */
gboolean	e_gw_connection_remove_events_sync
						(EGwConnection *cnc,
						 const gchar *key,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_EVENTS_H */

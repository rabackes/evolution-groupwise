/*
 * e-gw-addressbook.h: GroupWise address books (getAddressBookListRequest)
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

#ifndef E_GW_ADDRESSBOOK_H
#define E_GW_ADDRESSBOOK_H

#include "e-gw-connection.h"

G_BEGIN_DECLS

#define E_GW_SYSTEM_ADDRESS_BOOK_ID "GroupWiseSystemAddressBook@52"

typedef struct {
	gchar *id;
	gchar *name;
	gchar *description;
	gboolean is_personal;		/* the user's own, writable */
	gboolean is_frequent_contacts;
	gboolean is_system;		/* the GroupWise Address Book of the system */
} EGwAddressBook;

void		e_gw_address_book_free		(EGwAddressBook *book);

/* Returns: (element-type EGwAddressBook) (transfer full) */
GPtrArray *	e_gw_connection_get_address_books_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_ADDRESSBOOK_H */

/*
 * e-gw-addressbook.c: GroupWise address books (getAddressBookListRequest)
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

#include "e-gw-addressbook.h"
#include "e-gw-xml.h"

void
e_gw_address_book_free (EGwAddressBook *book)
{
	if (!book)
		return;

	g_free (book->id);
	g_free (book->name);
	g_free (book->description);
	g_free (book);
}

GPtrArray *
e_gw_connection_get_address_books_sync (EGwConnection *cnc,
					GCancellable *cancellable,
					GError **error)
{
	EGwResponse *response;
	GPtrArray *books;
	xmlNode *node;

	response = e_gw_connection_call_sync (cnc, "getAddressBookList", NULL, cancellable, error);
	if (!response)
		return NULL;

	books = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_address_book_free);
	node = e_gw_xml_find (e_gw_response_get_node (response), "books");
	for (node = e_gw_xml_first_child (node, "book"); node; node = e_gw_xml_next_sibling (node, "book")) {
		EGwAddressBook *book = g_new0 (EGwAddressBook, 1);
		gchar *raw = e_gw_xml_dup_text (node, "id");

		book->id = raw && *raw ? e_gw_clean_id (raw) : NULL;
		g_free (raw);
		book->name = e_gw_xml_dup_text (node, "name");
		book->description = e_gw_xml_dup_text (node, "description");
		book->is_personal = e_gw_xml_get_bool (node, "isPersonal");
		book->is_frequent_contacts = e_gw_xml_get_bool (node, "isFrequentContacts");
		book->is_system = g_strcmp0 (book->id, E_GW_SYSTEM_ADDRESS_BOOK_ID) == 0;

		if (book->id)
			g_ptr_array_add (books, book);
		else
			e_gw_address_book_free (book);
	}

	e_gw_response_free (response);

	return books;
}

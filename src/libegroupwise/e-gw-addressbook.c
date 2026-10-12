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

#include "e-gw-share.h"
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
	g_free (book->owner_name);
	g_free (book);
}

gboolean
e_gw_address_book_is_id (const gchar *id)
{
	return id && g_str_has_suffix (id, "@53");
}

GPtrArray *
e_gw_connection_get_address_books_sync (EGwConnection *cnc,
					GCancellable *cancellable,
					GError **error)
{
	EGwResponse *response;
	GPtrArray *books;
	xmlNode *node;

	/* (without the view the list does not say which books are shared) */
	response = e_gw_connection_call_sync (cnc, "getAddressBookList",
		"<view>" E_GW_ADDRESS_BOOK_SHARE_VIEW "</view>", cancellable, error);
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
		book->is_shared_to_me = e_gw_xml_get_bool (node, "isSharedToMe");
		book->is_shared_by_me = e_gw_xml_get_bool (node, "isSharedByMe");
		if (book->is_shared_to_me) {
			book->share_rights = e_gw_share_rights_from_node (node) | E_GW_SHARE_READ;
			book->owner_name = e_gw_xml_dup_text (node, "owner/displayName");
		}

		if (book->id)
			g_ptr_array_add (books, book);
		else
			e_gw_address_book_free (book);
	}

	e_gw_response_free (response);

	return books;
}

gchar *
e_gw_connection_create_address_book_sync (EGwConnection *cnc,
					  const gchar *name,
					  GCancellable *cancellable,
					  GError **error)
{
	GString *xml;
	gchar *id;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (name != NULL, NULL);

	xml = g_string_new ("<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"AddressBook\">");
	e_gw_xml_add_leaf (xml, "name", name);
	g_string_append (xml, "</item>");
	id = e_gw_connection_create_item_sync (cnc, xml->str, cancellable, error);
	g_string_free (xml, TRUE);

	return id;
}

gboolean
e_gw_connection_rename_address_book_sync (EGwConnection *cnc,
					  const gchar *id,
					  const gchar *name,
					  GCancellable *cancellable,
					  GError **error)
{
	GString *updates;
	gboolean success;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (id != NULL, FALSE);
	g_return_val_if_fail (name != NULL, FALSE);

	updates = g_string_new ("<update>");
	e_gw_xml_add_leaf (updates, "name", name);
	g_string_append (updates, "</update>");
	success = e_gw_connection_modify_item_sync (cnc, id, updates->str, cancellable, error);
	g_string_free (updates, TRUE);

	return success;
}

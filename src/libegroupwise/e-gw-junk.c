/*
 * e-gw-junk.c: the junk mail handling of GroupWise
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

#include "e-gw-junk.h"
#include "e-gw-xml.h"

static const gchar *list_names[] = { "junk", "block", "trust" };

void
e_gw_junk_entry_free (EGwJunkEntry *entry)
{
	if (!entry)
		return;

	g_free (entry->id);
	g_free (entry->match);
	g_free (entry);
}

GPtrArray *
e_gw_connection_get_junk_entries_sync (EGwConnection *cnc,
				       GCancellable *cancellable,
				       GError **error)
{
	EGwResponse *response;
	GPtrArray *entries;
	guint ii;

	response = e_gw_connection_call_sync (cnc, "getJunkEntries", NULL, cancellable, error);
	if (!response)
		return NULL;

	entries = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_junk_entry_free);
	/* <junk>, <block> and <trust>, each with its <entry> elements */
	for (ii = 0; ii < G_N_ELEMENTS (list_names); ii++) {
		xmlNode *node;

		for (node = e_gw_xml_first_child (e_gw_xml_find (e_gw_response_get_node (response), list_names[ii]), "entry");
		     node; node = e_gw_xml_next_sibling (node, "entry")) {
			EGwJunkEntry *entry = g_new0 (EGwJunkEntry, 1);
			gchar *type = e_gw_xml_dup_text (node, "matchType");
			gchar *raw = e_gw_xml_dup_text (node, "id");

			entry->id = raw && *raw ? e_gw_clean_id (raw) : NULL;
			entry->match = e_gw_xml_dup_text (node, "match");
			entry->is_domain = g_strcmp0 (type, "domain") == 0;
			entry->list = ii;
			g_free (type);
			g_free (raw);
			if (entry->id && entry->match && *entry->match)
				g_ptr_array_add (entries, entry);
			else
				e_gw_junk_entry_free (entry);
		}
	}
	e_gw_response_free (response);

	return entries;
}

gchar *
e_gw_connection_create_junk_entry_sync (EGwConnection *cnc,
					const gchar *match,
					gboolean is_domain,
					EGwJunkList list,
					GCancellable *cancellable,
					GError **error)
{
	GString *inner = g_string_new ("<entry>");
	EGwResponse *response;
	gchar *raw, *id;

	g_return_val_if_fail (match != NULL, NULL);
	g_return_val_if_fail (list <= E_GW_JUNK_LIST_TRUST, NULL);

	e_gw_xml_add_leaf (inner, "match", match);
	e_gw_xml_add_leaf (inner, "matchType", is_domain ? "domain" : "email");
	e_gw_xml_add_leaf (inner, "listType", list_names[list]);
	g_string_append (inner, "</entry>");

	response = e_gw_connection_call_sync (cnc, "createJunkEntry", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return NULL;

	raw = e_gw_xml_dup_text (e_gw_response_get_node (response), "id");
	id = raw && *raw ? e_gw_clean_id (raw) : g_strdup ("");
	g_free (raw);
	e_gw_response_free (response);

	return id;
}

gboolean
e_gw_connection_remove_junk_entry_sync (EGwConnection *cnc,
					const gchar *id,
					GCancellable *cancellable,
					GError **error)
{
	GString *inner = g_string_new (NULL);
	EGwResponse *response;

	g_return_val_if_fail (id != NULL, FALSE);

	e_gw_xml_add_leaf (inner, "id", id);
	response = e_gw_connection_call_sync (cnc, "removeJunkEntry", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	e_gw_response_free (response);

	return response != NULL;
}

GHashTable *
e_gw_connection_get_junk_settings_sync (EGwConnection *cnc,
					GCancellable *cancellable,
					GError **error)
{
	EGwResponse *response;
	GHashTable *settings;
	xmlNode *node;

	response = e_gw_connection_call_sync (cnc, "getJunkMailSettings", NULL, cancellable, error);
	if (!response)
		return NULL;

	settings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	for (node = e_gw_xml_first_child (e_gw_xml_find (e_gw_response_get_node (response), "settings"), "setting");
	     node; node = e_gw_xml_next_sibling (node, "setting")) {
		gchar *field = e_gw_xml_dup_text (node, "field");

		if (field && *field)
			g_hash_table_replace (settings, field, e_gw_xml_dup_text (node, "value"));
		else
			g_free (field);
	}
	e_gw_response_free (response);

	return settings;
}

gboolean
e_gw_connection_modify_junk_settings_sync (EGwConnection *cnc,
					   GHashTable *settings,
					   GCancellable *cancellable,
					   GError **error)
{
	GString *inner = g_string_new ("<settings>");
	EGwResponse *response;
	GHashTableIter iter;
	gpointer key, value;

	g_return_val_if_fail (settings != NULL, FALSE);

	g_hash_table_iter_init (&iter, settings);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		g_string_append (inner, "<setting>");
		e_gw_xml_add_leaf (inner, "field", key);
		e_gw_xml_add_leaf (inner, "value", value ? value : "");
		g_string_append (inner, "</setting>");
	}
	g_string_append (inner, "</settings>");

	response = e_gw_connection_call_sync (cnc, "modifyJunkMailSettings", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	e_gw_response_free (response);

	return response != NULL;
}

gboolean
e_gw_connection_put_junk_entry_sync (EGwConnection *cnc,
				     const gchar *match,
				     gboolean is_domain,
				     EGwJunkList list,
				     GPtrArray *entries,
				     GCancellable *cancellable,
				     GError **error)
{
	GPtrArray *own = NULL;
	gboolean success = TRUE, present = FALSE;
	guint ii;

	g_return_val_if_fail (match != NULL && *match, FALSE);
	g_return_val_if_fail (list <= E_GW_JUNK_LIST_TRUST, FALSE);

	if (!entries) {
		own = e_gw_connection_get_junk_entries_sync (cnc, cancellable, error);
		if (!own)
			return FALSE;
		entries = own;
	}

	for (ii = 0; success && ii < entries->len; ii++) {
		EGwJunkEntry *entry = entries->pdata[ii];
		gboolean contradicts;

		if (entry->is_domain != is_domain || g_ascii_strcasecmp (entry->match, match) != 0)
			continue;

		switch (list) {
		case E_GW_JUNK_LIST_TRUST:
			contradicts = entry->list != E_GW_JUNK_LIST_TRUST;
			break;
		case E_GW_JUNK_LIST_BLOCK:
			contradicts = entry->list != E_GW_JUNK_LIST_BLOCK;
			break;
		default:
			/* Junk: a blocked one stays blocked */
			contradicts = entry->list == E_GW_JUNK_LIST_TRUST;
			break;
		}

		if (entry->list == list) {
			present = TRUE;
		} else if (contradicts) {
			g_debug ("junk: %s off the %s list", match, list_names[entry->list]);
			success = e_gw_connection_remove_junk_entry_sync (cnc, entry->id, cancellable, error);
		} else if (entry->list == E_GW_JUNK_LIST_BLOCK) {
			/* On the block list, junk adds nothing */
			present = TRUE;
		}
	}

	if (success && !present) {
		gchar *id;

		g_debug ("junk: %s onto the %s list", match, list_names[list]);
		id = e_gw_connection_create_junk_entry_sync (cnc, match, is_domain, list, cancellable, error);
		success = id != NULL;
		g_free (id);
	}

	if (own)
		g_ptr_array_unref (own);

	return success;
}

gboolean
e_gw_connection_learn_junk_sender_sync (EGwConnection *cnc,
					const gchar *address,
					gboolean junk,
					GPtrArray *entries,
					GCancellable *cancellable,
					GError **error)
{
	g_return_val_if_fail (address != NULL && *address, FALSE);

	return e_gw_connection_put_junk_entry_sync (cnc, address, FALSE,
		junk ? E_GW_JUNK_LIST_JUNK : E_GW_JUNK_LIST_TRUST, entries, cancellable, error);
}

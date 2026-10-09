/*
 * e-gw-category.c: GroupWise categories
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

#include <string.h>

#include "e-gw-category.h"
#include "e-gw-xml.h"

void
e_gw_category_free (EGwCategory *category)
{
	if (!category)
		return;

	g_free (category->id);
	g_free (category->name);
	g_free (category);
}

gchar *
e_gw_category_dup_display_name (const EGwCategory *category)
{
	const gchar *raw = NULL;
	GString *name;
	const gchar *pp;

	g_return_val_if_fail (category != NULL, NULL);

	/* The names of Evolution's labels, translated as Evolution shows them */
	switch (category->type) {
	case E_GW_CATEGORY_URGENT:
		raw = "I_mportant";
		break;
	case E_GW_CATEGORY_PERSONAL:
		raw = "_Personal";
		break;
	case E_GW_CATEGORY_FOLLOW_UP:
		raw = "_To Do";
		break;
	case E_GW_CATEGORY_LOW_PRIORITY:
		raw = "_Later";
		break;
	default:
		return g_strdup (category->name);
	}

	name = g_string_new (NULL);
	for (pp = g_dgettext ("evolution", raw); *pp; pp++) {
		if (*pp != '_')
			g_string_append_c (name, *pp);
	}

	return g_string_free (name, FALSE);
}

gchar *
e_gw_category_ref (const gchar *id)
{
	const gchar *at;

	if (!id || !*id)
		return NULL;

	at = strrchr (id, '@');

	return g_strdup_printf ("%.*s@12", (gint) (at ? at - id : (gssize) strlen (id)), id);
}

static EGwCategoryType
type_from_string (const gchar *type)
{
	if (g_strcmp0 (type, "Personal") == 0)
		return E_GW_CATEGORY_PERSONAL;
	if (g_strcmp0 (type, "FollowUp") == 0)
		return E_GW_CATEGORY_FOLLOW_UP;
	if (g_strcmp0 (type, "Urgent") == 0)
		return E_GW_CATEGORY_URGENT;
	if (g_strcmp0 (type, "LowPriority") == 0)
		return E_GW_CATEGORY_LOW_PRIORITY;

	return E_GW_CATEGORY_NORMAL;
}

GPtrArray *
e_gw_connection_get_categories_sync (EGwConnection *cnc,
				     GCancellable *cancellable,
				     GError **error)
{
	EGwResponse *response;
	GPtrArray *categories;
	xmlNode *node;

	response = e_gw_connection_call_sync (cnc, "getCategoryList", NULL, cancellable, error);
	if (!response)
		return NULL;

	categories = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_category_free);
	node = e_gw_xml_find (e_gw_response_get_node (response), "categories");
	for (node = e_gw_xml_first_child (node, "category"); node; node = e_gw_xml_next_sibling (node, "category")) {
		gchar *raw = e_gw_xml_dup_text (node, "id");
		gchar *id = raw && *raw ? e_gw_clean_id (raw) : NULL;
		gchar *type = e_gw_xml_dup_text (node, "type");
		EGwCategory *category;

		g_free (raw);
		if (!id) {
			g_free (type);
			continue;
		}

		category = g_new0 (EGwCategory, 1);
		category->id = e_gw_category_ref (id);
		category->name = e_gw_xml_dup_text (node, "name");
		category->type = type_from_string (type);
		category->color = e_gw_xml_find (node, "color") ? e_gw_xml_get_int (node, "color", -1) : -1;
		category->hidden = e_gw_xml_get_bool (node, "flags/notInMasterList");
		g_ptr_array_add (categories, category);
		g_free (type);
		g_free (id);
	}
	e_gw_response_free (response);

	return categories;
}

gchar *
e_gw_connection_create_category_sync (EGwConnection *cnc,
				      const gchar *name,
				      gint64 color,
				      GCancellable *cancellable,
				      GError **error)
{
	GString *xml = g_string_new ("<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Category\">");
	gchar *id, *ref;

	g_return_val_if_fail (name != NULL, NULL);

	e_gw_xml_add_leaf (xml, "name", name);
	if (color >= 0)
		e_gw_xml_add_int (xml, "color", color);
	g_string_append (xml, "</item>");

	id = e_gw_connection_create_item_sync (cnc, xml->str, cancellable, error);
	g_string_free (xml, TRUE);

	ref = e_gw_category_ref (id);
	g_free (id);

	return ref;
}

gchar **
e_gw_item_dup_categories (xmlNode *item)
{
	GPtrArray *ids = g_ptr_array_new ();
	xmlNode *node;

	for (node = e_gw_xml_first_child (e_gw_xml_find (item, "categories"), "category"); node;
	     node = e_gw_xml_next_sibling (node, "category")) {
		gchar *raw = e_gw_xml_dup_text (node, NULL);
		gchar *id = raw && *raw ? e_gw_clean_id (raw) : NULL;

		if (id)
			g_ptr_array_add (ids, e_gw_category_ref (id));
		g_free (raw);
		g_free (id);
	}
	g_ptr_array_add (ids, NULL);

	return (gchar **) g_ptr_array_free (ids, FALSE);
}

static void
append_list (GString *xml,
	     const gchar *part,
	     GPtrArray *ids)
{
	guint ii;

	if (ids->len == 0)
		return;

	g_string_append_printf (xml, "<%s><categories>", part);
	for (ii = 0; ii < ids->len; ii++)
		e_gw_xml_add_leaf (xml, "category", ids->pdata[ii]);
	g_string_append_printf (xml, "</categories></%s>", part);
}

gchar *
e_gw_categories_updates_xml (const gchar * const *before,
			     const gchar * const *after)
{
	GPtrArray *added = g_ptr_array_new (), *removed = g_ptr_array_new ();
	GString *xml = g_string_new (NULL);
	guint ii;

	/* As GroupWise Web: categories are added and deleted, not replaced */
	for (ii = 0; after && after[ii]; ii++) {
		if (!before || !g_strv_contains (before, after[ii]))
			g_ptr_array_add (added, (gpointer) after[ii]);
	}
	for (ii = 0; before && before[ii]; ii++) {
		if (!after || !g_strv_contains (after, before[ii]))
			g_ptr_array_add (removed, (gpointer) before[ii]);
	}
	append_list (xml, "add", added);
	append_list (xml, "delete", removed);
	g_ptr_array_unref (added);
	g_ptr_array_unref (removed);

	return g_string_free (xml, FALSE);
}

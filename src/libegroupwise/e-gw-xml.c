/*
 * e-gw-xml.c: helpers to build GroupWise SOAP requests and read responses
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

#include "e-gw-xml.h"

void
e_gw_xml_add_leaf (GString *xml,
		   const gchar *tag,
		   const gchar *value)
{
	gchar *escaped = g_markup_escape_text (value ? value : "", -1);

	g_string_append_printf (xml, "<%s>%s</%s>", tag, escaped, tag);
	g_free (escaped);
}

void
e_gw_xml_add_int (GString *xml,
		  const gchar *tag,
		  gint64 value)
{
	g_string_append_printf (xml, "<%s>%" G_GINT64_FORMAT "</%s>", tag, value, tag);
}

void
e_gw_xml_add_bool (GString *xml,
		   const gchar *tag,
		   gboolean value)
{
	g_string_append_printf (xml, "<%s>%s</%s>", tag, value ? "true" : "false", tag);
}

static gboolean
node_is (xmlNode *node,
	 const gchar *name,
	 gsize name_len)
{
	return node->type == XML_ELEMENT_NODE
		&& strlen ((const gchar *) node->name) == name_len
		&& strncmp ((const gchar *) node->name, name, name_len) == 0;
}

xmlNode *
e_gw_xml_first_child (xmlNode *parent,
		      const gchar *name)
{
	xmlNode *child;

	if (!parent)
		return NULL;

	for (child = parent->children; child; child = child->next) {
		if (child->type == XML_ELEMENT_NODE && (!name || node_is (child, name, strlen (name))))
			return child;
	}

	return NULL;
}

xmlNode *
e_gw_xml_next_sibling (xmlNode *node,
		       const gchar *name)
{
	for (node = node ? node->next : NULL; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE && (!name || node_is (node, name, strlen (name))))
			return node;
	}

	return NULL;
}

xmlNode *
e_gw_xml_find (xmlNode *parent,
	       const gchar *path)
{
	const gchar *segment = path;

	while (parent && segment && *segment) {
		const gchar *slash = strchr (segment, '/');
		gsize len = slash ? (gsize) (slash - segment) : strlen (segment);
		xmlNode *child;

		for (child = parent->children; child; child = child->next) {
			if (node_is (child, segment, len))
				break;
		}

		parent = child;
		segment = slash ? slash + 1 : NULL;
	}

	return parent;
}

gchar *
e_gw_xml_dup_text (xmlNode *parent,
		   const gchar *path)
{
	xmlNode *node = path && *path ? e_gw_xml_find (parent, path) : parent;
	xmlChar *content;
	gchar *text;

	if (!node)
		return NULL;

	content = xmlNodeGetContent (node);
	text = g_strstrip (g_strdup ((const gchar *) content));
	xmlFree (content);

	return text;
}

gint64
e_gw_xml_get_int (xmlNode *parent,
		  const gchar *path,
		  gint64 default_value)
{
	gchar *text = e_gw_xml_dup_text (parent, path);
	gint64 value = default_value;

	if (text && *text && !g_ascii_string_to_signed (text, 10, G_MININT64, G_MAXINT64, &value, NULL))
		value = default_value;
	g_free (text);

	return value;
}

gboolean
e_gw_xml_get_bool (xmlNode *parent,
		   const gchar *path)
{
	gchar *text = e_gw_xml_dup_text (parent, path);
	gboolean value = text && (g_strcmp0 (text, "1") == 0 || g_ascii_strcasecmp (text, "true") == 0);

	g_free (text);

	return value;
}

gchar *
e_gw_xml_dup_attr (xmlNode *node,
		   const gchar *name)
{
	xmlChar *value;
	gchar *copy;

	if (!node)
		return NULL;

	value = xmlGetProp (node, (const xmlChar *) name);
	copy = g_strdup ((const gchar *) value);
	xmlFree (value);

	return copy;
}

gchar *
e_gw_clean_id (const gchar *id)
{
	GString *clean;

	if (!id)
		return NULL;

	clean = g_string_sized_new (strlen (id));
	for (; *id; id++) {
		if (!g_ascii_isspace (*id))
			g_string_append_c (clean, *id);
	}

	return g_string_free (clean, FALSE);
}

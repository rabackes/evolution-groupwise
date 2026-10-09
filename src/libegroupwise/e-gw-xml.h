/*
 * e-gw-xml.h: helpers to build GroupWise SOAP requests and read responses
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

#ifndef E_GW_XML_H
#define E_GW_XML_H

#include <glib.h>
#include <libxml/tree.h>

G_BEGIN_DECLS

/* Request building: appends <tag>escaped value</tag> to @xml */
void		e_gw_xml_add_leaf		(GString *xml,
						 const gchar *tag,
						 const gchar *value);
void		e_gw_xml_add_int		(GString *xml,
						 const gchar *tag,
						 gint64 value);
void		e_gw_xml_add_bool		(GString *xml,
						 const gchar *tag,
						 gboolean value);

/* Response reading. Element names are matched by local name, namespaces are
 * ignored. A path is a '/'-separated list of child names ("status/code"). */
xmlNode *	e_gw_xml_find			(xmlNode *parent,
						 const gchar *path);
xmlNode *	e_gw_xml_first_child		(xmlNode *parent,
						 const gchar *name);
xmlNode *	e_gw_xml_next_sibling		(xmlNode *node,
						 const gchar *name);
gchar *		e_gw_xml_dup_text		(xmlNode *parent,
						 const gchar *path);
gint64		e_gw_xml_get_int		(xmlNode *parent,
						 const gchar *path,
						 gint64 default_value);
gboolean	e_gw_xml_get_bool		(xmlNode *parent,
						 const gchar *path);
gchar *		e_gw_xml_dup_attr		(xmlNode *node,
						 const gchar *name);

/* Item and folder IDs may come wrapped over several lines; they never contain whitespace. */
gchar *		e_gw_clean_id			(const gchar *id);

G_END_DECLS

#endif /* E_GW_XML_H */

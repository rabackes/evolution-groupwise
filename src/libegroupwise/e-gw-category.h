/*
 * e-gw-category.h: GroupWise categories
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

#ifndef E_GW_CATEGORY_H
#define E_GW_CATEGORY_H

#include "e-gw-connection.h"

G_BEGIN_DECLS

/* The categories GroupWise brings along; the client shows them translated */
typedef enum {
	E_GW_CATEGORY_NORMAL,
	E_GW_CATEGORY_PERSONAL,
	E_GW_CATEGORY_FOLLOW_UP,
	E_GW_CATEGORY_URGENT,
	E_GW_CATEGORY_LOW_PRIORITY
} EGwCategoryType;

typedef struct {
	gchar *id;		/* as items name it (see e_gw_category_ref) */
	gchar *name;
	EGwCategoryType type;
	gint64 color;		/* COLORREF (0x00BBGGRR), -1 if none */
	gboolean hidden;	/* notInMasterList: not offered to the user */
} EGwCategory;

void		e_gw_category_free		(EGwCategory *category);

/* The name the user sees: the built-in categories by the names of
 * Evolution's built-in labels (Important, Personal, To Do, Later, in the
 * user's language), the others by their own */
gchar *		e_gw_category_dup_display_name	(const EGwCategory *category);

/* The ID of a category as items refer to it: the list names the built-in
 * ones "1.po.domain...@61", items "1.po.domain...@12" */
gchar *		e_gw_category_ref		(const gchar *id);

/* getCategoryListRequest
 * Returns: (element-type EGwCategory) (transfer full) */
GPtrArray *	e_gw_connection_get_categories_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);

/* createItemRequest of a Category; returns its ID. @color < 0: none. */
gchar *		e_gw_connection_create_category_sync
						(EGwConnection *cnc,
						 const gchar *name,
						 gint64 color,
						 GCancellable *cancellable,
						 GError **error);

/* The category IDs of an item (<categories>, "category" in a view), in
 * the form of e_gw_category_ref(); NULL-terminated, never NULL */
gchar **	e_gw_item_dup_categories	(xmlNode *item);

/* modifyItemRequest parts that make an item with the categories @before
 * have @after (NULL-terminated, IDs as items name them); empty when equal */
gchar *		e_gw_categories_updates_xml	(const gchar * const *before,
						 const gchar * const *after);

G_END_DECLS

#endif /* E_GW_CATEGORY_H */

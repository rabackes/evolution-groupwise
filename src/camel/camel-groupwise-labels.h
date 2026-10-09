/*
 * camel-groupwise-labels.h: GroupWise categories as Evolution's labels
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

#ifndef CAMEL_GROUPWISE_LABELS_H
#define CAMEL_GROUPWISE_LABELS_H

#include <camel/camel.h>

#include "e-gw-category.h"

G_BEGIN_DECLS

/*
 * Evolution labels a message with a user flag, its "tag"; the labels (name,
 * color, tag) are in the GSettings key org.gnome.evolution.mail labels.
 * The built-in categories of GroupWise are Evolution's built-in labels
 * (Urgent = Important, Personal, Follow-up = To Do, Low priority = Later),
 * every other category the label of the same name, added to Evolution's
 * list when missing. Thread-safe; the categories are kept in a file for
 * offline use.
 */
typedef struct _CamelGroupwiseLabels CamelGroupwiseLabels;

CamelGroupwiseLabels *
		camel_groupwise_labels_new	(const gchar *filename);
void		camel_groupwise_labels_free	(CamelGroupwiseLabels *labels);

/* Takes the category list of the server (EGwCategory) and adds the labels
 * missing in Evolution */
void		camel_groupwise_labels_set_categories
						(CamelGroupwiseLabels *labels,
						 GPtrArray *categories);

/* The tag of the category @id (as items name it), NULL if unknown */
gchar *		camel_groupwise_labels_dup_tag	(CamelGroupwiseLabels *labels,
						 const gchar *id);

/* Whether the user flag @tag is a label (of Evolution or a category) */
gboolean	camel_groupwise_labels_is_label	(CamelGroupwiseLabels *labels,
						 const gchar *tag);

/* The category of the label @tag, created on the server when there is none
 * (named and colored as the label); NULL on failure */
gchar *		camel_groupwise_labels_dup_category_sync
						(CamelGroupwiseLabels *labels,
						 const gchar *tag,
						 EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);

/* The label flags of @info that are categories now: @ids (as items name
 * them) become user flags, other label flags go */
gboolean	camel_groupwise_labels_apply	(CamelGroupwiseLabels *labels,
						 CamelMessageInfo *info,
						 const gchar * const *ids);

/* The label tags among the user flags of @info (NULL-terminated) */
gchar **	camel_groupwise_labels_dup_info_tags
						(CamelGroupwiseLabels *labels,
						 CamelMessageInfo *info);

G_END_DECLS

#endif /* CAMEL_GROUPWISE_LABELS_H */

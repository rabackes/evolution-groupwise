/*
 * e-groupwise-rule-editor.h: the editor of a GroupWise rule
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

#ifndef E_GROUPWISE_RULE_EDITOR_H
#define E_GROUPWISE_RULE_EDITOR_H

#include <gtk/gtk.h>

#include "e-gw-rule.h"

G_BEGIN_DECLS

/* What the editor offers besides the rule: the folders of the mailbox and
 * its categories */
typedef struct {
	GPtrArray *folders;	/* EGroupwiseRuleFolder, by path */
	GPtrArray *categories;	/* EGwCategory */
} EGroupwiseRuleContext;

typedef struct {
	gchar *id;
	gchar *path;		/* "Cabinet/Projekte" */
} EGroupwiseRuleFolder;

EGroupwiseRuleContext *
		e_groupwise_rule_context_new	(GPtrArray *folders,
						 GPtrArray *categories);
void		e_groupwise_rule_context_free	(EGroupwiseRuleContext *context);
const gchar *	e_groupwise_rule_context_folder_path
						(const EGroupwiseRuleContext *context,
						 const gchar *id);

/* Whether the editor can show all of the rule; the others are only shown */
gboolean	e_groupwise_rule_is_editable	(const EGwRule *rule);

/* The event of a rule as the list shows it */
gchar *		e_groupwise_rule_dup_event_label
						(const EGwRule *rule,
						 const EGroupwiseRuleContext *context);

/* Edits a copy of @rule (NULL: a new one); returns the rule as the user
 * saved it, or NULL when cancelled */
EGwRule *	e_groupwise_rule_editor_run	(GtkWindow *parent,
						 const EGwRule *rule,
						 const EGroupwiseRuleContext *context);

G_END_DECLS

#endif /* E_GROUPWISE_RULE_EDITOR_H */

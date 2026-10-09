/*
 * e-gw-rule.h: the rules of the mailbox
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

#ifndef E_GW_RULE_H
#define E_GW_RULE_H

#include "e-gw-connection.h"

G_BEGIN_DECLS

/*
 * A rule (item type "Rule"): when it runs (execution, for folder events
 * with the folder), on which items (types, source), under which conditions
 * (a filter: groups joined by and/or/not of entries field-op-value) and
 * what it does (actions). The POA runs the rules of new and filed items
 * itself; those of the client's events (startup, exit, folder open and
 * close, user activated) run when a client asks (executeRule).
 *
 * The out of office rule (VacationRule) has e-gw-vacation.h.
 */

/* A node of the filter: a group (op and, or, not; children) or an entry */
typedef struct _EGwFilterNode EGwFilterNode;
struct _EGwFilterNode {
	gchar *op;		/* and, or, not; contains, eq, fieldLT, ... */
	GPtrArray *children;	/* EGwFilterNode; NULL for an entry */
	gchar *field;
	gchar *value;
	gchar *date;		/* a relative date (Today, ...); @value is the offset in days */
	gchar *mask;		/* for bitCare */
};

EGwFilterNode *	e_gw_filter_node_new_group	(const gchar *op);
EGwFilterNode *	e_gw_filter_node_new_entry	(const gchar *field,
						 const gchar *op,
						 const gchar *value);
void		e_gw_filter_node_free		(EGwFilterNode *node);
EGwFilterNode *	e_gw_filter_node_copy		(const EGwFilterNode *node);

/* The mail an action sends (reply, forward, send, delegate) */
typedef struct {
	gchar *email;
	gchar *display_name;
	gchar *dist_type;	/* TO, CC, BC */
} EGwRuleRecipient;

typedef struct {
	gchar *type;		/* RuleActionType: Move, Reply, ... */
	gchar *container;	/* Move, Link */
	gchar *accept_level;	/* Accept: Free, Tentative, Busy, OutOfOffice */
	gchar *comment;		/* Accept, Delete (decline), Delegate */
	GPtrArray *categories;	/* Category: category IDs */
	/* The item of Reply, ReplyWithText, Forward, SimpleForward,
	 * FlatForward, Send, Delegate */
	gboolean has_item;
	gchar *subject;
	gchar *text;		/* plain text of the message */
	GPtrArray *recipients;	/* EGwRuleRecipient */
	gchar *item_xml;	/* the item as read, for what is not above */
	/* Parts of the item written back as read: subjectPrefix, security,
	 * options; distribution/sendoptions */
	gchar *item_extra_xml;
	gchar *send_options_xml;
} EGwRuleAction;

EGwRuleAction *	e_gw_rule_action_new		(const gchar *type);
void		e_gw_rule_action_free		(EGwRuleAction *action);
EGwRuleAction *	e_gw_rule_action_copy		(const EGwRuleAction *action);
void		e_gw_rule_action_add_recipient	(EGwRuleAction *action,
						 const gchar *email,
						 const gchar *display_name,
						 const gchar *dist_type);

typedef struct {
	gchar *id;
	gchar *name;
	gboolean enabled;
	gchar *execution;	/* Execution: New, FolderNew, Startup, ... */
	gint sequence;
	gchar *types;		/* Mail Appointment Task Note PhoneMessage, space separated; NULL: all */
	gchar *source;		/* received sent personal draft, space separated */
	gchar *container;	/* the folder of folder events, NULL: any */
	gchar *conflict;	/* AppointmentConflict of Accept, NULL: none */
	EGwFilterNode *filter;	/* NULL: every item */
	GPtrArray *actions;	/* EGwRuleAction */
} EGwRule;

EGwRule *	e_gw_rule_new			(void);
void		e_gw_rule_free			(EGwRule *rule);
EGwRule *	e_gw_rule_copy			(const EGwRule *rule);

/* The rules in their order (VacationRule left out).
 * Returns: (transfer container) (element-type EGwRule) (nullable) */
GPtrArray *	e_gw_connection_get_rules_sync	(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);
/* Returns the ID of the new rule */
gchar *		e_gw_connection_create_rule_sync
						(EGwConnection *cnc,
						 const EGwRule *rule,
						 GCancellable *cancellable,
						 GError **error);
/* Writes what differs between @old_rule and @new_rule (the ID of @old_rule);
 * filter and actions as a whole */
gboolean	e_gw_connection_modify_rule_sync
						(EGwConnection *cnc,
						 const EGwRule *old_rule,
						 const EGwRule *new_rule,
						 GCancellable *cancellable,
						 GError **error);
gboolean	e_gw_connection_remove_rule_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 GCancellable *cancellable,
						 GError **error);
/* Runs the rule now, on the server */
gboolean	e_gw_connection_execute_rule_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 GCancellable *cancellable,
						 GError **error);

/* The XML of a filter and of actions as sent (for tests) */
gchar *		e_gw_filter_node_to_xml		(const EGwFilterNode *node);
gchar *		e_gw_rule_actions_to_xml	(GPtrArray *actions);

G_END_DECLS

#endif /* E_GW_RULE_H */

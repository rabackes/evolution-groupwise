/*
 * e-gw-rule.c: the rules of the mailbox
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

/*
 * Found out on a GroupWise 26 POA: createItem of type Rule adds a rule;
 * modifyItem <update> replaces name, enabled, execution, sequence, types,
 * source, container and filter, but appends to <actions> — they are
 * replaced by <delete><actions/></delete> and <add><actions/></add>. A new
 * filter clears types and source (they go along in the same update). The
 * POA does not renumber the sequence. A missing <enabled> is off.
 */

#include <string.h>

#include "e-gw-rule.h"
#include "e-gw-xml.h"

#define XSI_NS "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""

/* An ID, without the line breaks the POA may put in */
static gchar *
dup_id (xmlNode *parent,
	const gchar *path)
{
	gchar *text = e_gw_xml_dup_text (parent, path);
	gchar *id = text ? e_gw_clean_id (text) : NULL;

	g_free (text);

	return id;
}

/* ------------------------------------------------------------------ */
/* Filter */

EGwFilterNode *
e_gw_filter_node_new_group (const gchar *op)
{
	EGwFilterNode *node = g_new0 (EGwFilterNode, 1);

	node->op = g_strdup (op);
	node->children = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_filter_node_free);

	return node;
}

EGwFilterNode *
e_gw_filter_node_new_entry (const gchar *field,
			    const gchar *op,
			    const gchar *value)
{
	EGwFilterNode *node = g_new0 (EGwFilterNode, 1);

	node->field = g_strdup (field);
	node->op = g_strdup (op);
	node->value = g_strdup (value);

	return node;
}

void
e_gw_filter_node_free (EGwFilterNode *node)
{
	if (!node)
		return;

	g_free (node->op);
	g_clear_pointer (&node->children, g_ptr_array_unref);
	g_free (node->field);
	g_free (node->value);
	g_free (node->date);
	g_free (node->mask);
	g_free (node);
}

EGwFilterNode *
e_gw_filter_node_copy (const EGwFilterNode *node)
{
	EGwFilterNode *copy;
	guint ii;

	if (!node)
		return NULL;

	if (node->children) {
		copy = e_gw_filter_node_new_group (node->op);
		for (ii = 0; ii < node->children->len; ii++)
			g_ptr_array_add (copy->children, e_gw_filter_node_copy (node->children->pdata[ii]));
	} else {
		copy = e_gw_filter_node_new_entry (node->field, node->op, node->value);
	}
	copy->date = g_strdup (node->date);
	copy->mask = g_strdup (node->mask);

	return copy;
}

static EGwFilterNode *
filter_node_from_xml (xmlNode *element)
{
	gchar *type = e_gw_xml_dup_attr (element, "type");
	EGwFilterNode *node;

	if (type && g_str_has_suffix (type, "FilterGroup")) {
		xmlNode *child;

		node = e_gw_filter_node_new_group (NULL);
		node->op = e_gw_xml_dup_text (element, "op");
		for (child = e_gw_xml_first_child (element, "element"); child; child = e_gw_xml_next_sibling (child, "element"))
			g_ptr_array_add (node->children, filter_node_from_xml (child));
	} else {
		node = e_gw_filter_node_new_entry (NULL, NULL, NULL);
		node->op = e_gw_xml_dup_text (element, "op");
		node->field = e_gw_xml_dup_text (element, "field");
		node->value = e_gw_xml_dup_text (element, "value");
		node->date = e_gw_xml_dup_text (element, "date");
		node->mask = e_gw_xml_dup_text (element, "mask");
	}
	g_free (type);

	return node;
}

static void
add_filter_node_xml (GString *xml,
		     const EGwFilterNode *node,
		     gboolean top)
{
	g_string_append_printf (xml, "<element %sxsi:type=\"%s\">", top ? XSI_NS " " : "",
		node->children ? "FilterGroup" : "FilterEntry");
	e_gw_xml_add_leaf (xml, "op", node->op);
	if (node->children) {
		guint ii;

		for (ii = 0; ii < node->children->len; ii++)
			add_filter_node_xml (xml, node->children->pdata[ii], FALSE);
	} else {
		if (node->field)
			e_gw_xml_add_leaf (xml, "field", node->field);
		if (node->value)
			e_gw_xml_add_leaf (xml, "value", node->value);
		if (node->date)
			e_gw_xml_add_leaf (xml, "date", node->date);
		if (node->mask)
			e_gw_xml_add_leaf (xml, "mask", node->mask);
	}
	g_string_append (xml, "</element>");
}

gchar *
e_gw_filter_node_to_xml (const EGwFilterNode *node)
{
	GString *xml = g_string_new ("<filter>");

	if (node)
		add_filter_node_xml (xml, node, TRUE);
	g_string_append (xml, "</filter>");

	return g_string_free (xml, FALSE);
}

/* ------------------------------------------------------------------ */
/* Actions */

static void
rule_recipient_free (EGwRuleRecipient *recipient)
{
	g_free (recipient->email);
	g_free (recipient->display_name);
	g_free (recipient->dist_type);
	g_free (recipient);
}

EGwRuleAction *
e_gw_rule_action_new (const gchar *type)
{
	EGwRuleAction *action = g_new0 (EGwRuleAction, 1);

	action->type = g_strdup (type);
	action->categories = g_ptr_array_new_with_free_func (g_free);
	action->recipients = g_ptr_array_new_with_free_func ((GDestroyNotify) rule_recipient_free);

	return action;
}

void
e_gw_rule_action_free (EGwRuleAction *action)
{
	if (!action)
		return;

	g_free (action->type);
	g_free (action->container);
	g_free (action->accept_level);
	g_free (action->comment);
	g_ptr_array_unref (action->categories);
	g_free (action->subject);
	g_free (action->text);
	g_ptr_array_unref (action->recipients);
	g_free (action->item_xml);
	g_free (action->item_extra_xml);
	g_free (action->send_options_xml);
	g_free (action);
}

void
e_gw_rule_action_add_recipient (EGwRuleAction *action,
				const gchar *email,
				const gchar *display_name,
				const gchar *dist_type)
{
	EGwRuleRecipient *recipient = g_new0 (EGwRuleRecipient, 1);

	recipient->email = g_strdup (email);
	recipient->display_name = g_strdup (display_name);
	recipient->dist_type = g_strdup (dist_type ? dist_type : "TO");
	g_ptr_array_add (action->recipients, recipient);
}

EGwRuleAction *
e_gw_rule_action_copy (const EGwRuleAction *action)
{
	EGwRuleAction *copy = e_gw_rule_action_new (action->type);
	guint ii;

	copy->container = g_strdup (action->container);
	copy->accept_level = g_strdup (action->accept_level);
	copy->comment = g_strdup (action->comment);
	for (ii = 0; ii < action->categories->len; ii++)
		g_ptr_array_add (copy->categories, g_strdup (action->categories->pdata[ii]));
	copy->has_item = action->has_item;
	copy->subject = g_strdup (action->subject);
	copy->text = g_strdup (action->text);
	for (ii = 0; ii < action->recipients->len; ii++) {
		EGwRuleRecipient *recipient = action->recipients->pdata[ii];

		e_gw_rule_action_add_recipient (copy, recipient->email, recipient->display_name, recipient->dist_type);
	}
	copy->item_xml = g_strdup (action->item_xml);
	copy->item_extra_xml = g_strdup (action->item_extra_xml);
	copy->send_options_xml = g_strdup (action->send_options_xml);

	return copy;
}

static gchar *
dump_node (xmlNode *node)
{
	xmlBuffer *buffer = xmlBufferCreate ();
	gchar *xml;

	xmlNodeDump (buffer, node->doc, node, 0, 0);
	xml = g_strndup ((const gchar *) xmlBufferContent (buffer), xmlBufferLength (buffer));
	xmlBufferFree (buffer);

	return xml;
}

/* The text of the message of an item, base64 in its first text part */
static gchar *
item_text (xmlNode *item)
{
	xmlNode *part;

	for (part = e_gw_xml_first_child (e_gw_xml_find (item, "message"), "part"); part; part = e_gw_xml_next_sibling (part, "part")) {
		gchar *content_type = e_gw_xml_dup_attr (part, "contentType");
		gboolean plain = !content_type || g_ascii_strcasecmp (content_type, "text/plain") == 0;
		xmlChar *content;
		gchar *text = NULL;

		g_free (content_type);
		if (!plain)
			continue;
		content = xmlNodeGetContent (part);
		if (content) {
			gsize len = 0;
			guchar *decoded = g_base64_decode ((const gchar *) content, &len);

			text = g_utf8_make_valid ((const gchar *) decoded, len);
			g_free (decoded);
			xmlFree (content);
		}
		return text;
	}

	return NULL;
}

static EGwRuleAction *
action_from_xml (xmlNode *node)
{
	EGwRuleAction *action = e_gw_rule_action_new (NULL);
	xmlNode *item, *child;

	g_free (action->type);
	action->type = e_gw_xml_dup_text (node, "type");
	action->container = dup_id (node, "container");
	action->accept_level = e_gw_xml_dup_text (node, "acceptLevel");
	action->comment = e_gw_xml_dup_text (node, "message");
	for (child = e_gw_xml_first_child (e_gw_xml_find (node, "categories"), "category"); child;
	     child = e_gw_xml_next_sibling (child, "category")) {
		xmlChar *id = xmlNodeGetContent (child);

		if (id && *id)
			g_ptr_array_add (action->categories, g_strdup ((const gchar *) id));
		xmlFree (id);
	}

	item = e_gw_xml_find (node, "item");
	if (item) {
		action->has_item = TRUE;
		action->subject = e_gw_xml_dup_text (item, "subject");
		action->text = item_text (item);
		for (child = e_gw_xml_first_child (e_gw_xml_find (item, "distribution/recipients"), "recipient"); child;
		     child = e_gw_xml_next_sibling (child, "recipient")) {
			gchar *email = e_gw_xml_dup_text (child, "email");
			gchar *name = e_gw_xml_dup_text (child, "displayName");
			gchar *dist_type = e_gw_xml_dup_text (child, "distType");

			e_gw_rule_action_add_recipient (action, email, name, dist_type);
			g_free (email);
			g_free (name);
			g_free (dist_type);
		}
		action->item_xml = dump_node (item);

		/* What the client set besides: kept as it is */
		{
			static const gchar *kept[] = { "subjectPrefix", "security", "options" };
			GString *extra = g_string_new (NULL);
			xmlNode *sendoptions = e_gw_xml_find (item, "distribution/sendoptions");
			guint ii;

			for (ii = 0; ii < G_N_ELEMENTS (kept); ii++) {
				xmlNode *element = e_gw_xml_find (item, kept[ii]);

				if (element) {
					gchar *xml = dump_node (element);

					g_string_append (extra, xml);
					g_free (xml);
				}
			}
			action->item_extra_xml = extra->len ? g_string_free (extra, FALSE) : (g_string_free (extra, TRUE), NULL);
			action->send_options_xml = sendoptions ? dump_node (sendoptions) : NULL;
		}
	}

	return action;
}

static void
add_action_xml (GString *xml,
		const EGwRuleAction *action)
{
	guint ii;

	g_string_append (xml, "<action>");
	e_gw_xml_add_leaf (xml, "type", action->type);
	if (action->container)
		e_gw_xml_add_leaf (xml, "container", action->container);
	if (action->has_item) {
		g_string_append (xml, "<item " XSI_NS " xsi:type=\"Mail\">");
		if (action->subject)
			e_gw_xml_add_leaf (xml, "subject", action->subject);
		if (action->recipients->len || action->send_options_xml) {
			g_string_append (xml, "<distribution><recipients>");
			for (ii = 0; ii < action->recipients->len; ii++) {
				EGwRuleRecipient *recipient = action->recipients->pdata[ii];

				g_string_append (xml, "<recipient>");
				if (recipient->display_name)
					e_gw_xml_add_leaf (xml, "displayName", recipient->display_name);
				e_gw_xml_add_leaf (xml, "email", recipient->email);
				e_gw_xml_add_leaf (xml, "distType", recipient->dist_type);
				g_string_append (xml, "</recipient>");
			}
			g_string_append (xml, "</recipients>");
			if (action->send_options_xml)
				g_string_append (xml, action->send_options_xml);
			g_string_append (xml, "</distribution>");
		}
		if (action->text) {
			gchar *encoded = g_base64_encode ((const guchar *) action->text, strlen (action->text));

			g_string_append_printf (xml, "<message><part contentType=\"text/plain\">%s</part></message>", encoded);
			g_free (encoded);
		}
		if (action->item_extra_xml)
			g_string_append (xml, action->item_extra_xml);
		g_string_append (xml, "</item>");
	}
	if (action->comment)
		e_gw_xml_add_leaf (xml, "message", action->comment);
	if (action->accept_level)
		e_gw_xml_add_leaf (xml, "acceptLevel", action->accept_level);
	if (action->categories->len) {
		g_string_append (xml, "<categories>");
		for (ii = 0; ii < action->categories->len; ii++)
			e_gw_xml_add_leaf (xml, "category", action->categories->pdata[ii]);
		g_string_append (xml, "</categories>");
	}
	g_string_append (xml, "</action>");
}

gchar *
e_gw_rule_actions_to_xml (GPtrArray *actions)
{
	GString *xml = g_string_new ("<actions>");
	guint ii;

	for (ii = 0; actions && ii < actions->len; ii++)
		add_action_xml (xml, actions->pdata[ii]);
	g_string_append (xml, "</actions>");

	return g_string_free (xml, FALSE);
}

/* ------------------------------------------------------------------ */
/* Rules */

EGwRule *
e_gw_rule_new (void)
{
	EGwRule *rule = g_new0 (EGwRule, 1);

	rule->actions = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_rule_action_free);

	return rule;
}

void
e_gw_rule_free (EGwRule *rule)
{
	if (!rule)
		return;

	g_free (rule->id);
	g_free (rule->name);
	g_free (rule->execution);
	g_free (rule->types);
	g_free (rule->source);
	g_free (rule->container);
	g_free (rule->conflict);
	e_gw_filter_node_free (rule->filter);
	g_ptr_array_unref (rule->actions);
	g_free (rule);
}

EGwRule *
e_gw_rule_copy (const EGwRule *rule)
{
	EGwRule *copy = e_gw_rule_new ();
	guint ii;

	copy->id = g_strdup (rule->id);
	copy->name = g_strdup (rule->name);
	copy->enabled = rule->enabled;
	copy->execution = g_strdup (rule->execution);
	copy->sequence = rule->sequence;
	copy->types = g_strdup (rule->types);
	copy->source = g_strdup (rule->source);
	copy->container = g_strdup (rule->container);
	copy->conflict = g_strdup (rule->conflict);
	copy->filter = e_gw_filter_node_copy (rule->filter);
	for (ii = 0; ii < rule->actions->len; ii++)
		g_ptr_array_add (copy->actions, e_gw_rule_action_copy (rule->actions->pdata[ii]));

	return copy;
}

static gint
compare_sequence (gconstpointer a,
		  gconstpointer b)
{
	const EGwRule *rule_a = *(EGwRule * const *) a;
	const EGwRule *rule_b = *(EGwRule * const *) b;

	return rule_a->sequence - rule_b->sequence;
}

GPtrArray *
e_gw_connection_get_rules_sync (EGwConnection *cnc,
				GCancellable *cancellable,
				GError **error)
{
	EGwResponse *response;
	GPtrArray *rules;
	xmlNode *node;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	response = e_gw_connection_call_sync (cnc, "getRuleList", NULL, cancellable, error);
	if (!response)
		return NULL;

	rules = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_rule_free);
	node = e_gw_xml_find (e_gw_response_get_node (response), "rules");
	for (node = e_gw_xml_first_child (node, "rule"); node; node = e_gw_xml_next_sibling (node, "rule")) {
		gchar *type = e_gw_xml_dup_attr (node, "type");
		EGwRule *rule;
		xmlNode *child;

		/* The out of office rule is e-gw-vacation's */
		if (type && g_str_has_suffix (type, "VacationRule")) {
			g_free (type);
			continue;
		}
		g_free (type);

		rule = e_gw_rule_new ();
		rule->id = dup_id (node, "id");
		rule->name = e_gw_xml_dup_text (node, "name");
		rule->enabled = e_gw_xml_get_bool (node, "enabled");
		rule->execution = e_gw_xml_dup_text (node, "execution");
		rule->sequence = (gint) e_gw_xml_get_int (node, "sequence", 0);
		rule->types = e_gw_xml_dup_text (node, "types");
		rule->source = e_gw_xml_dup_text (node, "source");
		rule->container = dup_id (node, "container");
		rule->conflict = e_gw_xml_dup_text (node, "conflict");
		child = e_gw_xml_first_child (e_gw_xml_find (node, "filter"), "element");
		if (child)
			rule->filter = filter_node_from_xml (child);
		for (child = e_gw_xml_first_child (e_gw_xml_find (node, "actions"), "action"); child;
		     child = e_gw_xml_next_sibling (child, "action"))
			g_ptr_array_add (rule->actions, action_from_xml (child));

		if (rule->id && *rule->id)
			g_ptr_array_add (rules, rule);
		else
			e_gw_rule_free (rule);
	}
	e_gw_response_free (response);

	g_ptr_array_sort (rules, compare_sequence);

	return rules;
}

/* The fields of a rule besides filter and actions, when they differ from
 * @old_rule (all of them without) */
static void
add_rule_fields (GString *xml,
		 const EGwRule *old_rule,
		 const EGwRule *rule,
		 gboolean filter_changed)
{
#define CHANGED(field) (!old_rule || g_strcmp0 (old_rule->field, rule->field) != 0)
	/* A new filter clears types and source on the POA: they go along */
#define CHANGED_WITH_FILTER(field) (filter_changed || CHANGED (field))
	if (CHANGED (name))
		e_gw_xml_add_leaf (xml, "name", rule->name ? rule->name : "");
	if (!old_rule || old_rule->enabled != rule->enabled)
		e_gw_xml_add_bool (xml, "enabled", rule->enabled);
	if (CHANGED (execution) && rule->execution)
		e_gw_xml_add_leaf (xml, "execution", rule->execution);
	if (!old_rule || old_rule->sequence != rule->sequence)
		e_gw_xml_add_int (xml, "sequence", rule->sequence);
	if (CHANGED_WITH_FILTER (types))
		e_gw_xml_add_leaf (xml, "types", rule->types ? rule->types : "");
	if (CHANGED_WITH_FILTER (source))
		e_gw_xml_add_leaf (xml, "source", rule->source ? rule->source : "");
	if (CHANGED (container) && rule->container)
		e_gw_xml_add_leaf (xml, "container", rule->container);
	if (CHANGED (conflict) && rule->conflict)
		e_gw_xml_add_leaf (xml, "conflict", rule->conflict);
#undef CHANGED_WITH_FILTER
#undef CHANGED
}

gchar *
e_gw_connection_create_rule_sync (EGwConnection *cnc,
				  const EGwRule *rule,
				  GCancellable *cancellable,
				  GError **error)
{
	EGwResponse *response;
	GString *inner;
	gchar *part, *id;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (rule != NULL, NULL);

	inner = g_string_new ("<item " XSI_NS " xsi:type=\"Rule\">");
	add_rule_fields (inner, NULL, rule, FALSE);
	if (rule->filter) {
		part = e_gw_filter_node_to_xml (rule->filter);
		g_string_append (inner, part);
		g_free (part);
	}
	part = e_gw_rule_actions_to_xml (rule->actions);
	g_string_append (inner, part);
	g_free (part);
	g_string_append (inner, "</item>");

	response = e_gw_connection_call_sync (cnc, "createItem", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return NULL;

	id = dup_id (e_gw_response_get_node (response), "id");
	e_gw_response_free (response);
	if (!id || !*id) {
		g_free (id);
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, "createItem Rule: no ID");
		return NULL;
	}

	return id;
}

gboolean
e_gw_connection_modify_rule_sync (EGwConnection *cnc,
				  const EGwRule *old_rule,
				  const EGwRule *new_rule,
				  GCancellable *cancellable,
				  GError **error)
{
	EGwResponse *response;
	GString *update, *inner;
	gchar *old_part, *new_part;
	gboolean actions_changed, filter_changed;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (old_rule != NULL && old_rule->id != NULL, FALSE);
	g_return_val_if_fail (new_rule != NULL, FALSE);

	old_part = old_rule->filter ? e_gw_filter_node_to_xml (old_rule->filter) : NULL;
	new_part = new_rule->filter ? e_gw_filter_node_to_xml (new_rule->filter) : NULL;
	filter_changed = g_strcmp0 (old_part, new_part) != 0;
	update = g_string_new (NULL);
	add_rule_fields (update, old_rule, new_rule, filter_changed);
	if (filter_changed)
		g_string_append (update, new_part ? new_part : "<filter/>");
	g_free (old_part);
	g_free (new_part);

	old_part = e_gw_rule_actions_to_xml (old_rule->actions);
	new_part = e_gw_rule_actions_to_xml (new_rule->actions);
	actions_changed = g_strcmp0 (old_part, new_part) != 0;
	g_free (old_part);

	if (!update->len && !actions_changed) {
		g_string_free (update, TRUE);
		g_free (new_part);
		return TRUE;
	}

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "id", old_rule->id);
	g_string_append (inner, "<updates>");
	/* Actions are appended by <update>: the old ones go first */
	if (actions_changed)
		g_string_append_printf (inner, "<delete><actions/></delete><add>%s</add>", new_part);
	if (update->len)
		g_string_append_printf (inner, "<update>%s</update>", update->str);
	g_string_append (inner, "</updates>");
	g_string_free (update, TRUE);
	g_free (new_part);

	response = e_gw_connection_call_sync (cnc, "modifyItem", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	e_gw_response_free (response);

	return TRUE;
}

static gboolean
id_call (EGwConnection *cnc,
	 const gchar *action,
	 const gchar *id,
	 GCancellable *cancellable,
	 GError **error)
{
	EGwResponse *response;
	GString *inner;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (id != NULL, FALSE);

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "id", id);
	response = e_gw_connection_call_sync (cnc, action, inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	e_gw_response_free (response);

	return TRUE;
}

gboolean
e_gw_connection_remove_rule_sync (EGwConnection *cnc,
				  const gchar *id,
				  GCancellable *cancellable,
				  GError **error)
{
	return id_call (cnc, "removeItem", id, cancellable, error);
}

gboolean
e_gw_connection_execute_rule_sync (EGwConnection *cnc,
				   const gchar *id,
				   GCancellable *cancellable,
				   GError **error)
{
	return id_call (cnc, "executeRule", id, cancellable, error);
}

/*
 * e-groupwise-rule-editor.c: the editor of a GroupWise rule
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
 * As the rule dialog of the GroupWise client: name, event (with the
 * sources of new items or the folder of folder events), item types,
 * conditions joined by "all" or "any" (one level, as normal users use
 * them), actions in their order. Rules with
 * what it cannot show (nested groups, "not", other fields or operators,
 * other actions) are shown without saving: saving would lose parts.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-category.h"
#include "e-gw-folder.h"

#include "e-groupwise-rule-editor.h"

/* ------------------------------------------------------------------ */
/* What the editor knows */

static const struct {
	const gchar *execution;
	const gchar *label;
	gboolean has_sources;
	gboolean has_folder;
} events[] = {
	{ "New", N_("New item"), TRUE, FALSE },
	{ "FolderNew", N_("Filed item"), FALSE, TRUE },
	{ "Completed", N_("Completed item"), FALSE, FALSE },
	{ "FolderOpen", N_("Open folder"), FALSE, TRUE },
	{ "FolderClose", N_("Close folder"), FALSE, TRUE },
	{ "Startup", N_("Startup"), TRUE, FALSE },
	{ "Exit", N_("Exit"), TRUE, FALSE },
	{ "User", N_("User activated"), TRUE, FALSE }
};

static const struct {
	const gchar *name;
	const gchar *label;
} sources[] = {
	{ "received", N_("_Received") },
	{ "sent", N_("_Sent") },
	{ "personal", N_("_Personal") },
	{ "draft", N_("_Draft") }
};

static const struct {
	const gchar *name;
	const gchar *label;
} item_types[] = {
	{ "Mail", N_("_Mail") },
	{ "PhoneMessage", N_("P_hone message") },
	{ "Appointment", N_("_Appointment") },
	{ "Task", N_("_Task") },
	{ "Note", N_("Reminder _note") }
};

typedef enum {
	FIELD_TEXT,
	FIELD_DATE,	/* compared with today, plus days */
	FIELD_NUMBER,
	FIELD_CHAR,	/* one letter, kept as its character code */
	FIELD_COUNTER,	/* a number, also compared with another counter */
	FIELD_ENUM	/* a value of a list */
} FieldKind;

typedef struct {
	const gchar *value;
	const gchar *label;
} EnumValue;

static const EnumValue priority_values[] = {
	{ "4", N_("High") }, { "2", N_("Standard") }, { "1", N_("Low") }, { NULL, NULL }
};
static const EnumValue copy_type_values[] = {
	{ "1", N_("To") }, { "2", N_("CC") }, { "4", N_("BC") }, { NULL, NULL }
};
static const EnumValue status_values[] = {
	{ "accepted", N_("Accepted") }, { "completed", N_("Completed") }, { "opened", N_("Opened") },
	{ "read", N_("Has been read") }, { "private", N_("Private") }, { NULL, NULL }
};
/* Of items a third-party archive stubbed */
static const EnumValue extended_status_values[] = {
	{ "32", N_("Third-party item downloaded") }, { "256", N_("Archived by a third party") },
	{ "512", N_("Third-party program code replaced") }, { "1024", N_("Third-party item restored") }, { NULL, NULL }
};
static const EnumValue thread_values[] = {
	{ "1", N_("Collapsed") }, { "2", N_("Watched") }, { "4", N_("Ignored") }, { NULL, NULL }
};
static const EnumValue attachment_values[] = {
	{ "File", N_("File") }, { "Sound", N_("Audio") }, { "MultiMedia", N_("Movie") }, { "Object", N_("Object (OLE)") },
	{ "DocumentReference", N_("Document reference") }, { "Mail", N_("Mail") }, { "Appointment", N_("Appointment") },
	{ "Task", N_("Task") }, { "Note", N_("Note") }, { "PhoneMessage", N_("Phone message") }, { NULL, NULL }
};
static const EnumValue mention_values[] = {
	{ "mentioned", N_("Me") }, { NULL, NULL }
};
static const EnumValue send_option_values[] = {
	{ "512", N_("Reply requested") }, { NULL, NULL }
};

/* The fields of the GroupWise client's condition list, without those of
 * documents and libraries and the internal ones; numbers are GroupWise
 * field IDs without a schema name. Found out with rules made in the
 * GroupWise client. */
static const struct {
	const gchar *field;
	const gchar *label;
	FieldKind kind;
	const EnumValue *values;	/* FIELD_ENUM */
	const gchar *enum_op;		/* FIELD_ENUM: bitCare or eq */
	gboolean with_mask;		/* the value as the mask as well */
	gboolean twice;			/* the client writes the entry twice */
} fields[] = {
	{ "from", N_("From"), FIELD_TEXT },
	{ "to", N_("To"), FIELD_TEXT },
	{ "cc", N_("CC"), FIELD_TEXT },
	{ "subject", N_("Subject"), FIELD_TEXT },
	{ "originalSubject", N_("My subject"), FIELD_TEXT },
	{ "message", N_("Message"), FIELD_TEXT },
	{ "place", N_("Place"), FIELD_TEXT },
	{ "attachments", N_("Attachments"), FIELD_TEXT },
	{ "annotation", N_("Annotation"), FIELD_TEXT },
	{ "sharer", N_("Posted by"), FIELD_TEXT },
	{ "account", N_("Account"), FIELD_TEXT },
	{ "caller", N_("Caller's name"), FIELD_TEXT },
	{ "company", N_("Caller's company"), FIELD_TEXT },
	{ "phone", N_("Caller's phone number"), FIELD_TEXT },
	{ "created", N_("Created"), FIELD_DATE },
	{ "delivered", N_("Delivered"), FIELD_DATE },
	{ "startDate", N_("Started"), FIELD_DATE },
	{ "endDate", N_("End"), FIELD_DATE },
	{ "1436", N_("Due by"), FIELD_DATE },
	{ "119", N_("Assigned date"), FIELD_DATE },
	{ "922", N_("Completed date"), FIELD_DATE },
	{ "size", N_("Size"), FIELD_NUMBER },
	/* xgettext:no-c-format */
	{ "percentComplete", N_("% complete"), FIELD_NUMBER },
	{ "122", N_("Task priority"), FIELD_NUMBER },
	{ "120", N_("Task category"), FIELD_CHAR },
	{ "acceptedTotals", N_("Number accepted"), FIELD_COUNTER },
	{ "repliedTotals", N_("Number replied"), FIELD_COUNTER },
	{ "completedTotals", N_("Number completed"), FIELD_COUNTER },
	{ "deletedTotals", N_("Number deleted"), FIELD_COUNTER },
	{ "openedTotals", N_("Number opened"), FIELD_COUNTER },
	{ "totalUsers", N_("Total recipients"), FIELD_COUNTER },
	{ "priority", N_("Priority"), FIELD_ENUM, priority_values, "bitCare", FALSE, TRUE },
	{ "distType", N_("Copy type"), FIELD_ENUM, copy_type_values, "eq", FALSE, FALSE },
	{ "status", N_("Item status"), FIELD_ENUM, status_values, "bitCare", TRUE, FALSE },
	{ "532", N_("Extended item status"), FIELD_ENUM, extended_status_values, "bitCare", FALSE, TRUE },
	{ "collapsedState", N_("Thread state"), FIELD_ENUM, thread_values, "bitCare", FALSE, TRUE },
	{ "sharedAttachmentTypes", N_("Attachment list"), FIELD_ENUM, attachment_values, "bitCare", TRUE, FALSE },
	{ "personalAttachmentTypes", N_("Personal attachments"), FIELD_ENUM, attachment_values, "bitCare", TRUE, FALSE },
	{ "messageFlags", N_("Mentioned"), FIELD_ENUM, mention_values, "bitCare", TRUE, FALSE },
	{ "sendoptions", N_("Send options"), FIELD_ENUM, send_option_values, "bitCare", FALSE, TRUE }
};

/* The operators by kind; FIELD_CHAR takes those of numbers, the counters
 * those of numbers and those comparing with another counter */
typedef enum {
	OPS_TEXT,
	OPS_DATE,
	OPS_NUMBER,
	OPS_COMPARE	/* with another counter: <date> names it, <value> is added */
} OpsKind;

static const struct {
	const gchar *op;
	const gchar *label;
	OpsKind kind;
} operators[] = {
	{ "contains", N_("contains"), OPS_TEXT },
	{ "notContains", N_("does not contain"), OPS_TEXT },
	{ "begins", N_("begins with"), OPS_TEXT },
	{ "eq", N_("matches"), OPS_TEXT },
	{ "ne", N_("is not"), OPS_TEXT },
	/* "created fieldLT Today -3": created before today minus 3 days */
	{ "fieldEqual", N_("on today plus (days)"), OPS_DATE },
	{ "fieldNE", N_("not on today plus (days)"), OPS_DATE },
	{ "fieldLT", N_("before today plus (days)"), OPS_DATE },
	{ "fieldLTE", N_("on or before today plus (days)"), OPS_DATE },
	{ "fieldGT", N_("after today plus (days)"), OPS_DATE },
	{ "fieldGTE", N_("on or after today plus (days)"), OPS_DATE },
	{ "eq", N_("="), OPS_NUMBER },
	{ "ne", N_("≠"), OPS_NUMBER },
	{ "gt", N_(">"), OPS_NUMBER },
	{ "gte", N_("≥"), OPS_NUMBER },
	{ "lt", N_("<"), OPS_NUMBER },
	{ "lte", N_("≤"), OPS_NUMBER },
	{ "fieldEqual", N_("= field"), OPS_COMPARE },
	{ "fieldNE", N_("≠ field"), OPS_COMPARE },
	{ "fieldGT", N_("> field"), OPS_COMPARE },
	{ "fieldGTE", N_("≥ field"), OPS_COMPARE },
	{ "fieldLT", N_("< field"), OPS_COMPARE },
	{ "fieldLTE", N_("≤ field"), OPS_COMPARE }
};

typedef enum {
	PARAM_NONE = 0,
	PARAM_FOLDER = 1 << 0,
	PARAM_RECIPIENTS = 1 << 1,
	PARAM_SUBJECT = 1 << 2,
	PARAM_TEXT = 1 << 3,
	PARAM_ACCEPT = 1 << 4,
	PARAM_COMMENT = 1 << 5,
	PARAM_CATEGORY = 1 << 6
} ActionParams;

/* In the order of the GroupWise client; the unconditional reply last,
 * its name is its warning */
static const struct {
	const gchar *type;
	const gchar *label;
	ActionParams params;
} actions[] = {
	{ "Send", N_("Send mail"), PARAM_RECIPIENTS | PARAM_SUBJECT | PARAM_TEXT },
	{ "SimpleForward", N_("Forward"), PARAM_RECIPIENTS | PARAM_SUBJECT | PARAM_TEXT },
	{ "Forward", N_("Forward as attachment"), PARAM_RECIPIENTS | PARAM_SUBJECT | PARAM_TEXT },
	{ "Delegate", N_("Delegate"), PARAM_RECIPIENTS | PARAM_COMMENT },
	{ "Reply", N_("Reply to sender"), PARAM_SUBJECT | PARAM_TEXT },
	{ "Accept", N_("Accept"), PARAM_ACCEPT | PARAM_COMMENT },
	{ "Category", N_("Category"), PARAM_CATEGORY },
	{ "Delete", N_("Delete / decline"), PARAM_COMMENT },
	{ "Purge", N_("Prune message"), PARAM_NONE },
	{ "Move", N_("Move to folder"), PARAM_FOLDER },
	{ "Link", N_("Link to folder"), PARAM_FOLDER },
	{ "MarkPrivate", N_("Mark private"), PARAM_NONE },
	{ "MarkRead", N_("Mark read"), PARAM_NONE },
	{ "MarkUnread", N_("Mark unread"), PARAM_NONE },
	{ "StopRules", N_("Stop rule processing"), PARAM_NONE },
	{ "ReplyWithText", N_("Reply unconditionally (dangerous)"), PARAM_SUBJECT | PARAM_TEXT }
};

static const struct {
	const gchar *level;
	const gchar *label;
} accept_levels[] = {
	{ "Busy", N_("Busy") },
	{ "Tentative", N_("Tentative") },
	{ "Free", N_("Free") },
	{ "OutOfOffice", N_("Out of office") }
};

static gint
find_event (const gchar *execution)
{
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (events); ii++) {
		if (g_strcmp0 (events[ii].execution, execution) == 0)
			return ii;
	}

	return -1;
}

static gint
find_field (const gchar *field)
{
	guint ii;

	for (ii = 0; field && ii < G_N_ELEMENTS (fields); ii++) {
		if (g_strcmp0 (fields[ii].field, field) == 0)
			return ii;
	}

	return -1;
}

/* Which operators a field takes (one or two kinds) */
static gboolean
field_takes (gint field,
	     OpsKind kind)
{
	switch (fields[field].kind) {
	case FIELD_TEXT:
		return kind == OPS_TEXT;
	case FIELD_DATE:
		return kind == OPS_DATE;
	case FIELD_NUMBER:
	case FIELD_CHAR:
		return kind == OPS_NUMBER;
	case FIELD_COUNTER:
		return kind == OPS_NUMBER || kind == OPS_COMPARE;
	default:
		return FALSE;
	}
}

static gint
find_operator (const gchar *op,
	       gint field,
	       gboolean compare)
{
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (operators); ii++) {
		if (g_strcmp0 (operators[ii].op, op) == 0 && field_takes (field, operators[ii].kind) &&
		    (operators[ii].kind == OPS_COMPARE) == compare)
			return ii;
	}

	return -1;
}

static gint
find_enum_value (gint field,
		 const gchar *value)
{
	guint ii;

	for (ii = 0; fields[field].values && fields[field].values[ii].value; ii++) {
		if (g_strcmp0 (fields[field].values[ii].value, value) == 0)
			return ii;
	}

	return -1;
}

static gint
find_action (const gchar *type)
{
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (actions); ii++) {
		if (g_strcmp0 (actions[ii].type, type) == 0)
			return ii;
	}

	return -1;
}

/* ------------------------------------------------------------------ */
/* Context */

static void
rule_folder_free (EGroupwiseRuleFolder *folder)
{
	g_free (folder->id);
	g_free (folder->path);
	g_free (folder);
}

static gint
compare_folder_path (gconstpointer a,
		     gconstpointer b)
{
	const EGroupwiseRuleFolder *fa = *(EGroupwiseRuleFolder * const *) a;
	const EGroupwiseRuleFolder *fb = *(EGroupwiseRuleFolder * const *) b;

	return g_utf8_collate (fa->path, fb->path);
}

EGroupwiseRuleContext *
e_groupwise_rule_context_new (GPtrArray *folders,
			      GPtrArray *categories)
{
	EGroupwiseRuleContext *context = g_new0 (EGroupwiseRuleContext, 1);
	GHashTable *by_id = g_hash_table_new (g_str_hash, g_str_equal);
	guint ii;

	context->folders = g_ptr_array_new_with_free_func ((GDestroyNotify) rule_folder_free);
	context->categories = categories ? g_ptr_array_ref (categories) : g_ptr_array_new ();

	for (ii = 0; folders && ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];

		g_hash_table_insert (by_id, folder->id, folder);
	}
	for (ii = 0; folders && ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii], *up;
		GString *path;
		guint depth = 0;

		if (folder->type == E_GW_FOLDER_TYPE_ROOT)
			continue;
		path = g_string_new (folder->name ? folder->name : folder->id);
		for (up = folder->parent_id ? g_hash_table_lookup (by_id, folder->parent_id) : NULL;
		     up && up->type != E_GW_FOLDER_TYPE_ROOT && depth < 32;
		     up = up->parent_id ? g_hash_table_lookup (by_id, up->parent_id) : NULL, depth++) {
			g_string_prepend (path, "/");
			g_string_prepend (path, up->name ? up->name : up->id);
		}
		{
			EGroupwiseRuleFolder *entry = g_new0 (EGroupwiseRuleFolder, 1);

			entry->id = g_strdup (folder->id);
			entry->path = g_string_free (path, FALSE);
			g_ptr_array_add (context->folders, entry);
		}
	}
	g_ptr_array_sort (context->folders, compare_folder_path);
	g_hash_table_destroy (by_id);

	return context;
}

void
e_groupwise_rule_context_free (EGroupwiseRuleContext *context)
{
	if (!context)
		return;

	g_ptr_array_unref (context->folders);
	g_ptr_array_unref (context->categories);
	g_free (context);
}

const gchar *
e_groupwise_rule_context_folder_path (const EGroupwiseRuleContext *context,
				      const gchar *id)
{
	guint ii;

	for (ii = 0; context && id && ii < context->folders->len; ii++) {
		EGroupwiseRuleFolder *folder = context->folders->pdata[ii];

		if (g_strcmp0 (folder->id, id) == 0)
			return folder->path;
	}

	return NULL;
}

/* ------------------------------------------------------------------ */
/* What can be edited */

static gboolean
entry_is_editable (const EGwFilterNode *node)
{
	gint field;

	if (node->children)
		return FALSE;
	field = find_field (node->field);
	if (field < 0)
		return FALSE;

	switch (fields[field].kind) {
	case FIELD_TEXT:
	case FIELD_NUMBER:
	case FIELD_CHAR:
		return !node->date && !node->mask && find_operator (node->op, field, FALSE) >= 0;
	case FIELD_DATE:
		return !node->mask && g_strcmp0 (node->date, "Today") == 0 && find_operator (node->op, field, FALSE) >= 0;
	case FIELD_COUNTER:
		if (node->mask)
			return FALSE;
		if (node->date) {
			gint other = find_field (node->date);

			return other >= 0 && fields[other].kind == FIELD_COUNTER && find_operator (node->op, field, TRUE) >= 0;
		}
		return find_operator (node->op, field, FALSE) >= 0;
	case FIELD_ENUM:
		return !node->date && g_strcmp0 (node->op, fields[field].enum_op) == 0 &&
			find_enum_value (field, node->value) >= 0 &&
			(fields[field].with_mask ? g_strcmp0 (node->mask, node->value) == 0 : !node->mask || !*node->mask);
	}

	return FALSE;
}

gboolean
e_groupwise_rule_is_editable (const EGwRule *rule)
{
	guint ii;

	if (find_event (rule->execution) < 0)
		return FALSE;
	if (rule->conflict && g_strcmp0 (rule->conflict, "Yes") != 0 && g_strcmp0 (rule->conflict, "No") != 0 &&
	    g_strcmp0 (rule->conflict, "Ignore") != 0)
		return FALSE;

	if (rule->filter) {
		if (rule->filter->children) {
			if (g_strcmp0 (rule->filter->op, "and") != 0 && g_strcmp0 (rule->filter->op, "or") != 0)
				return FALSE;
			for (ii = 0; ii < rule->filter->children->len; ii++) {
				if (!entry_is_editable (rule->filter->children->pdata[ii]))
					return FALSE;
			}
		} else if (!entry_is_editable (rule->filter)) {
			return FALSE;
		}
	}

	for (ii = 0; ii < rule->actions->len; ii++) {
		EGwRuleAction *action = rule->actions->pdata[ii];

		if (find_action (action->type) < 0 || action->categories->len > 1)
			return FALSE;
	}

	return TRUE;
}

gchar *
e_groupwise_rule_dup_event_label (const EGwRule *rule,
				  const EGroupwiseRuleContext *context)
{
	gint event = find_event (rule->execution);
	const gchar *label = event >= 0 ? _(events[event].label) : rule->execution;
	const gchar *folder = rule->container ? e_groupwise_rule_context_folder_path (context, rule->container) : NULL;

	if (folder)
		return g_strdup_printf ("%s: %s", label, folder);

	return g_strdup (label ? label : "");
}

static gboolean
same_entry (const EGwFilterNode *a,
	    const EGwFilterNode *b)
{
	return !a->children && !b->children && g_strcmp0 (a->field, b->field) == 0 && g_strcmp0 (a->op, b->op) == 0 &&
		g_strcmp0 (a->value, b->value) == 0 && g_strcmp0 (a->date, b->date) == 0 && g_strcmp0 (a->mask, b->mask) == 0;
}

/* ------------------------------------------------------------------ */
/* The dialog */

typedef struct _Editor Editor;

typedef struct {
	Editor *editor;
	GtkWidget *row;
	GtkWidget *field, *op, *value, *days, *other, *offset, *choice;
	gint current;		/* the field the operators are for */
} ConditionRow;

typedef struct {
	Editor *editor;
	GtkWidget *row;
	gint action;
	GtkWidget *folder, *recipients, *subject, *text, *accept, *comment, *category;
	EGwRuleAction *original;	/* what the widgets do not show */
} ActionRow;

struct _Editor {
	const EGroupwiseRuleContext *context;
	gboolean editable;
	GtkWidget *dialog;
	GtkWidget *name, *event, *sources_box, *folder, *folder_label, *conflict;
	GtkWidget *sources[G_N_ELEMENTS (sources)];
	GtkWidget *types[G_N_ELEMENTS (item_types)];
	GtkWidget *match, *conditions_box, *actions_box, *add_action;
	GPtrArray *conditions;	/* ConditionRow */
	GPtrArray *actions;	/* ActionRow */
};

static GtkWidget *
folder_combo (const EGroupwiseRuleContext *context,
	      const gchar *any_label,
	      const gchar *selected)
{
	GtkWidget *combo = gtk_combo_box_text_new ();
	guint ii;

	if (any_label)
		gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), "", any_label);
	for (ii = 0; ii < context->folders->len; ii++) {
		EGroupwiseRuleFolder *folder = context->folders->pdata[ii];

		gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), folder->id, folder->path);
	}
	if (selected && !gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), selected)) {
		/* A folder not in the list (gone, or of another kind): kept */
		gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), selected, selected);
		gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), selected);
	} else if (!selected) {
		gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
	}

	return combo;
}

static void
update_event (Editor *editor)
{
	gint event = gtk_combo_box_get_active (GTK_COMBO_BOX (editor->event));

	if (event < 0)
		return;
	gtk_widget_set_visible (editor->sources_box, events[event].has_sources);
	gtk_widget_set_visible (editor->folder, events[event].has_folder);
	gtk_widget_set_visible (editor->folder_label, events[event].has_folder);
}

static void
event_changed_cb (GtkComboBox *combo,
		  Editor *editor)
{
	update_event (editor);
}

static void
update_conflict (Editor *editor)
{
	/* Appointment conflicts for appointments only */
	gtk_widget_set_sensitive (editor->conflict,
		gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (editor->types[2])));
}

static void
type_toggled_cb (GtkToggleButton *button,
		 Editor *editor)
{
	update_conflict (editor);
}

/* Conditions */

/* The widgets of a row after its field and operator */
static void
update_condition_widgets (ConditionRow *row)
{
	gint field = row->current;
	const gchar *op = gtk_combo_box_get_active_id (GTK_COMBO_BOX (row->op));
	gboolean compare = fields[field].kind == FIELD_COUNTER && op && g_str_has_prefix (op, "field");

	gtk_widget_set_visible (row->value, fields[field].kind == FIELD_TEXT || fields[field].kind == FIELD_NUMBER ||
		fields[field].kind == FIELD_CHAR || (fields[field].kind == FIELD_COUNTER && !compare));
	gtk_widget_set_visible (row->days, fields[field].kind == FIELD_DATE);
	gtk_widget_set_visible (row->other, compare);
	gtk_widget_set_visible (row->offset, compare);
	gtk_widget_set_visible (row->choice, fields[field].kind == FIELD_ENUM);
}

static void
fill_operators (ConditionRow *row,
		const gchar *selected,
		gboolean compare)
{
	gint field = row->current;
	guint ii;

	gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (row->op));
	if (fields[field].kind == FIELD_ENUM) {
		/* One operator: "includes" for flags, "is" for the copy type */
		gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (row->op), fields[field].enum_op,
			g_strcmp0 (fields[field].enum_op, "eq") == 0 ? _("is") : _("includes"));
	} else {
		for (ii = 0; ii < G_N_ELEMENTS (operators); ii++) {
			if (field_takes (field, operators[ii].kind)) {
				/* The IDs of an operator used twice ("eq", "fieldLT") tell the kinds apart */
				gchar *id = operators[ii].kind == OPS_COMPARE ? g_strconcat ("field-compare:", operators[ii].op, NULL) :
					g_strdup (operators[ii].op);

				gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (row->op), id, _(operators[ii].label));
				g_free (id);
			}
		}
	}
	if (selected) {
		gchar *id = compare ? g_strconcat ("field-compare:", selected, NULL) : g_strdup (selected);

		if (!gtk_combo_box_set_active_id (GTK_COMBO_BOX (row->op), id))
			gtk_combo_box_set_active (GTK_COMBO_BOX (row->op), 0);
		g_free (id);
	} else {
		gtk_combo_box_set_active (GTK_COMBO_BOX (row->op), 0);
	}

	gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (row->choice));
	for (ii = 0; fields[field].values && fields[field].values[ii].value; ii++)
		gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (row->choice), fields[field].values[ii].value,
			_(fields[field].values[ii].label));
	gtk_combo_box_set_active (GTK_COMBO_BOX (row->choice), 0);
}

/* Operator IDs: "op", or "field-compare:op" for a comparison with a counter */
static const gchar *
row_op (ConditionRow *row,
	gboolean *out_compare)
{
	const gchar *id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (row->op));

	*out_compare = id && g_str_has_prefix (id, "field-compare:");

	return *out_compare ? id + strlen ("field-compare:") : id;
}

static void
condition_field_changed_cb (GtkComboBox *combo,
			    ConditionRow *row)
{
	gint field = find_field (gtk_combo_box_get_active_id (combo));

	if (field >= 0 && field != row->current) {
		row->current = field;
		fill_operators (row, NULL, FALSE);
		update_condition_widgets (row);
	}
}

static void
condition_op_changed_cb (GtkComboBox *combo,
			 ConditionRow *row)
{
	update_condition_widgets (row);
}

static void
remove_condition_cb (GtkButton *button,
		     ConditionRow *row)
{
	Editor *editor = row->editor;

	gtk_widget_destroy (row->row);
	g_ptr_array_remove (editor->conditions, row);
}

static gint
compare_field_labels (gconstpointer a,
		      gconstpointer b)
{
	return g_utf8_collate (_(fields[GPOINTER_TO_INT (*(gpointer const *) a)].label),
		_(fields[GPOINTER_TO_INT (*(gpointer const *) b)].label));
}

/* The fields by their names in the user's language, as the client lists them */
static GPtrArray *
sorted_fields (void)
{
	static GPtrArray *sorted;
	guint ii;

	if (!sorted) {
		sorted = g_ptr_array_new ();
		for (ii = 0; ii < G_N_ELEMENTS (fields); ii++)
			g_ptr_array_add (sorted, GINT_TO_POINTER (ii));
		g_ptr_array_sort (sorted, compare_field_labels);
	}

	return sorted;
}

static void
add_condition (Editor *editor,
	       const EGwFilterNode *node)
{
	ConditionRow *row = g_new0 (ConditionRow, 1);
	GPtrArray *order = sorted_fields ();
	GtkWidget *button;
	gint field = node ? find_field (node->field) : find_field ("subject");
	gboolean compare = node && node->date && field >= 0 && fields[field].kind == FIELD_COUNTER;
	guint ii;

	if (field < 0)
		field = find_field ("subject");
	row->editor = editor;
	row->current = field;
	row->row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

	row->field = gtk_combo_box_text_new ();
	for (ii = 0; ii < order->len; ii++) {
		gint index = GPOINTER_TO_INT (order->pdata[ii]);

		gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (row->field), fields[index].field, _(fields[index].label));
	}
	gtk_combo_box_set_active_id (GTK_COMBO_BOX (row->field), fields[field].field);
	gtk_box_pack_start (GTK_BOX (row->row), row->field, FALSE, FALSE, 0);

	row->op = gtk_combo_box_text_new ();
	gtk_box_pack_start (GTK_BOX (row->row), row->op, FALSE, FALSE, 0);

	row->value = gtk_entry_new ();
	gtk_entry_set_width_chars (GTK_ENTRY (row->value), 20);
	gtk_box_pack_start (GTK_BOX (row->row), row->value, TRUE, TRUE, 0);
	row->days = gtk_spin_button_new_with_range (-3650, 3650, 1);
	gtk_box_pack_start (GTK_BOX (row->row), row->days, FALSE, FALSE, 0);
	row->other = gtk_combo_box_text_new ();
	for (ii = 0; ii < order->len; ii++) {
		gint index = GPOINTER_TO_INT (order->pdata[ii]);

		if (fields[index].kind == FIELD_COUNTER)
			gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (row->other), fields[index].field, _(fields[index].label));
	}
	gtk_combo_box_set_active (GTK_COMBO_BOX (row->other), 0);
	gtk_box_pack_start (GTK_BOX (row->row), row->other, FALSE, FALSE, 0);
	row->offset = gtk_spin_button_new_with_range (-100000, 100000, 1);
	gtk_widget_set_tooltip_text (row->offset, _("Added to the other field"));
	gtk_box_pack_start (GTK_BOX (row->row), row->offset, FALSE, FALSE, 0);
	row->choice = gtk_combo_box_text_new ();
	gtk_box_pack_start (GTK_BOX (row->row), row->choice, TRUE, TRUE, 0);

	button = gtk_button_new_from_icon_name ("list-remove", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text (button, _("Remove the condition"));
	g_signal_connect (button, "clicked", G_CALLBACK (remove_condition_cb), row);
	gtk_box_pack_end (GTK_BOX (row->row), button, FALSE, FALSE, 0);

	/* Which of them shows depends on the field and operator: not shown by
	 * any show_all of the dialog around them */
	gtk_widget_set_no_show_all (row->value, TRUE);
	gtk_widget_set_no_show_all (row->days, TRUE);
	gtk_widget_set_no_show_all (row->other, TRUE);
	gtk_widget_set_no_show_all (row->offset, TRUE);
	gtk_widget_set_no_show_all (row->choice, TRUE);
	gtk_widget_show_all (row->row);
	fill_operators (row, node ? node->op : NULL, compare);
	if (node && node->value) {
		switch (fields[field].kind) {
		case FIELD_DATE:
			gtk_spin_button_set_value (GTK_SPIN_BUTTON (row->days), g_ascii_strtod (node->value, NULL));
			break;
		case FIELD_CHAR: {
			/* The letter's character code */
			gchar letter[8] = { 0 };

			g_unichar_to_utf8 ((gunichar) g_ascii_strtoull (node->value, NULL, 10), letter);
			gtk_entry_set_text (GTK_ENTRY (row->value), letter);
			break;
		}
		case FIELD_ENUM:
			gtk_combo_box_set_active_id (GTK_COMBO_BOX (row->choice), node->value);
			break;
		case FIELD_COUNTER:
			if (compare) {
				gtk_combo_box_set_active_id (GTK_COMBO_BOX (row->other), node->date);
				gtk_spin_button_set_value (GTK_SPIN_BUTTON (row->offset), g_ascii_strtod (node->value, NULL));
				break;
			}
			/* fall through */
		default:
			gtk_entry_set_text (GTK_ENTRY (row->value), node->value);
			break;
		}
	}
	update_condition_widgets (row);
	g_signal_connect (row->field, "changed", G_CALLBACK (condition_field_changed_cb), row);
	g_signal_connect (row->op, "changed", G_CALLBACK (condition_op_changed_cb), row);

	gtk_box_pack_start (GTK_BOX (editor->conditions_box), row->row, FALSE, FALSE, 0);
	g_ptr_array_add (editor->conditions, row);
}

static void
add_condition_cb (GtkButton *button,
		  Editor *editor)
{
	add_condition (editor, NULL);
}

/* Actions */

static void
remove_action_cb (GtkButton *button,
		  ActionRow *row)
{
	Editor *editor = row->editor;

	gtk_widget_destroy (row->row);
	g_ptr_array_remove (editor->actions, row);
}

static void
move_action (ActionRow *row,
	     gint delta)
{
	Editor *editor = row->editor;
	guint index;

	if (!g_ptr_array_find (editor->actions, row, &index) ||
	    (delta < 0 && index == 0) || (delta > 0 && index + 1 >= editor->actions->len))
		return;

	g_ptr_array_steal_index (editor->actions, index);
	g_ptr_array_insert (editor->actions, index + delta, row);
	gtk_box_reorder_child (GTK_BOX (editor->actions_box), row->row, index + delta);
}

static void
action_up_cb (GtkButton *button,
	      ActionRow *row)
{
	move_action (row, -1);
}

static void
action_down_cb (GtkButton *button,
		ActionRow *row)
{
	move_action (row, 1);
}

static void
action_row_free (ActionRow *row)
{
	e_gw_rule_action_free (row->original);
	g_free (row);
}

static gchar *
recipients_text (const EGwRuleAction *action)
{
	GString *text = g_string_new (NULL);
	guint ii;

	for (ii = 0; action && ii < action->recipients->len; ii++) {
		EGwRuleRecipient *recipient = action->recipients->pdata[ii];

		if (text->len)
			g_string_append (text, ", ");
		g_string_append (text, recipient->email ? recipient->email : "");
	}

	return g_string_free (text, FALSE);
}

static GtkWidget *
param_label (const gchar *text)
{
	GtkWidget *label = gtk_label_new_with_mnemonic (text);

	gtk_label_set_xalign (GTK_LABEL (label), 1.0);

	return label;
}

static void
add_action (Editor *editor,
	    gint index,
	    const EGwRuleAction *action)
{
	ActionRow *row = g_new0 (ActionRow, 1);
	GtkWidget *frame, *grid, *header, *label, *button, *scrolled;
	ActionParams params = actions[index].params;
	gint line = 0;
	guint ii;

	row->editor = editor;
	row->action = index;
	row->original = action ? e_gw_rule_action_copy (action) : e_gw_rule_action_new (actions[index].type);

	frame = gtk_frame_new (NULL);
	row->row = frame;
	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 4);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
	gtk_container_set_border_width (GTK_CONTAINER (grid), 6);
	gtk_container_add (GTK_CONTAINER (frame), grid);

	header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	label = gtk_label_new (NULL);
	{
		gchar *markup = g_markup_printf_escaped ("<b>%s</b>", _(actions[index].label));

		gtk_label_set_markup (GTK_LABEL (label), markup);
		g_free (markup);
	}
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_box_pack_start (GTK_BOX (header), label, TRUE, TRUE, 0);
	button = gtk_button_new_from_icon_name ("go-up", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text (button, _("Earlier"));
	g_signal_connect (button, "clicked", G_CALLBACK (action_up_cb), row);
	gtk_box_pack_start (GTK_BOX (header), button, FALSE, FALSE, 0);
	button = gtk_button_new_from_icon_name ("go-down", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text (button, _("Later"));
	g_signal_connect (button, "clicked", G_CALLBACK (action_down_cb), row);
	gtk_box_pack_start (GTK_BOX (header), button, FALSE, FALSE, 0);
	button = gtk_button_new_from_icon_name ("list-remove", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text (button, _("Remove the action"));
	g_signal_connect (button, "clicked", G_CALLBACK (remove_action_cb), row);
	gtk_box_pack_start (GTK_BOX (header), button, FALSE, FALSE, 0);
	gtk_grid_attach (GTK_GRID (grid), header, 0, line++, 2, 1);

	if (params & PARAM_FOLDER) {
		row->folder = folder_combo (editor->context, NULL, action ? action->container : NULL);
		gtk_widget_set_hexpand (row->folder, TRUE);
		gtk_grid_attach (GTK_GRID (grid), param_label (_("Folder:")), 0, line, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), row->folder, 1, line++, 1, 1);
	}
	if (params & PARAM_RECIPIENTS) {
		gchar *text = recipients_text (action);

		row->recipients = gtk_entry_new ();
		gtk_entry_set_text (GTK_ENTRY (row->recipients), text);
		gtk_entry_set_placeholder_text (GTK_ENTRY (row->recipients), _("addresses, separated by commas"));
		gtk_widget_set_hexpand (row->recipients, TRUE);
		gtk_grid_attach (GTK_GRID (grid), param_label (_("To:")), 0, line, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), row->recipients, 1, line++, 1, 1);
		g_free (text);
	}
	if (params & PARAM_SUBJECT) {
		row->subject = gtk_entry_new ();
		if (action && action->subject)
			gtk_entry_set_text (GTK_ENTRY (row->subject), action->subject);
		gtk_widget_set_hexpand (row->subject, TRUE);
		gtk_grid_attach (GTK_GRID (grid), param_label (_("Subject:")), 0, line, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), row->subject, 1, line++, 1, 1);
	}
	if (params & PARAM_TEXT) {
		row->text = gtk_text_view_new ();
		gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (row->text), GTK_WRAP_WORD_CHAR);
		if (action && action->text)
			gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (row->text)), action->text, -1);
		scrolled = gtk_scrolled_window_new (NULL, NULL);
		gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
		gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scrolled), 60);
		gtk_container_add (GTK_CONTAINER (scrolled), row->text);
		gtk_widget_set_hexpand (scrolled, TRUE);
		gtk_grid_attach (GTK_GRID (grid), param_label (_("Message:")), 0, line, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), scrolled, 1, line++, 1, 1);
	}
	if (params & PARAM_ACCEPT) {
		row->accept = gtk_combo_box_text_new ();
		for (ii = 0; ii < G_N_ELEMENTS (accept_levels); ii++)
			gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (row->accept), accept_levels[ii].level, _(accept_levels[ii].label));
		if (!action || !gtk_combo_box_set_active_id (GTK_COMBO_BOX (row->accept), action->accept_level))
			gtk_combo_box_set_active (GTK_COMBO_BOX (row->accept), 0);
		gtk_grid_attach (GTK_GRID (grid), param_label (_("Show as:")), 0, line, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), row->accept, 1, line++, 1, 1);
	}
	if (params & PARAM_COMMENT) {
		row->comment = gtk_entry_new ();
		if (action && action->comment)
			gtk_entry_set_text (GTK_ENTRY (row->comment), action->comment);
		gtk_widget_set_hexpand (row->comment, TRUE);
		gtk_grid_attach (GTK_GRID (grid), param_label (_("Comment:")), 0, line, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), row->comment, 1, line++, 1, 1);
	}
	if (params & PARAM_CATEGORY) {
		row->category = gtk_combo_box_text_new ();
		for (ii = 0; ii < editor->context->categories->len; ii++) {
			EGwCategory *category = editor->context->categories->pdata[ii];
			gchar *name = e_gw_category_dup_display_name (category);

			if (!category->hidden)
				gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (row->category), category->id, name);
			g_free (name);
		}
		if (!action || !action->categories->len ||
		    !gtk_combo_box_set_active_id (GTK_COMBO_BOX (row->category), action->categories->pdata[0]))
			gtk_combo_box_set_active (GTK_COMBO_BOX (row->category), 0);
		gtk_grid_attach (GTK_GRID (grid), param_label (_("Category:")), 0, line, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), row->category, 1, line++, 1, 1);
	}

	gtk_widget_show_all (frame);
	gtk_box_pack_start (GTK_BOX (editor->actions_box), frame, FALSE, FALSE, 0);
	g_ptr_array_add (editor->actions, row);
}

static void
add_action_cb (GtkButton *button,
	       Editor *editor)
{
	gint index = gtk_combo_box_get_active (GTK_COMBO_BOX (editor->add_action));

	if (index >= 0)
		add_action (editor, index, NULL);
}

/* ------------------------------------------------------------------ */
/* Collecting */

static gchar *
joined_toggles (GtkWidget **buttons,
		guint n_buttons,
		const gchar * const *names)
{
	GString *text = g_string_new (NULL);
	guint ii, n_active = 0;

	for (ii = 0; ii < n_buttons; ii++) {
		if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (buttons[ii]))) {
			if (text->len)
				g_string_append_c (text, ' ');
			g_string_append (text, names[ii]);
			n_active++;
		}
	}

	/* All of them: no restriction */
	if (n_active == n_buttons) {
		g_string_free (text, TRUE);
		return NULL;
	}

	return g_string_free (text, FALSE);
}

static void
set_toggles (GtkWidget **buttons,
	     guint n_buttons,
	     const gchar * const *names,
	     const gchar *value)
{
	gchar **words = value ? g_strsplit (value, " ", -1) : NULL;
	guint ii;

	for (ii = 0; ii < n_buttons; ii++)
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (buttons[ii]),
			!value || g_strv_contains ((const gchar * const *) words, names[ii]));
	g_strfreev (words);
}

static EGwRuleAction *
collect_action (ActionRow *row)
{
	EGwRuleAction *action = e_gw_rule_action_copy (row->original);
	ActionParams params = actions[row->action].params;

	g_free (action->type);
	action->type = g_strdup (actions[row->action].type);
	if (params & PARAM_FOLDER) {
		g_free (action->container);
		action->container = g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (row->folder)));
	}
	if (params & (PARAM_RECIPIENTS | PARAM_SUBJECT | PARAM_TEXT))
		action->has_item = TRUE;
	if (params & PARAM_RECIPIENTS) {
		gchar **addresses = g_strsplit_set (gtk_entry_get_text (GTK_ENTRY (row->recipients)), ",;", -1);
		guint ii;

		g_ptr_array_set_size (action->recipients, 0);
		for (ii = 0; addresses[ii]; ii++) {
			gchar *address = g_strstrip (addresses[ii]);

			if (*address)
				e_gw_rule_action_add_recipient (action, address, NULL, "TO");
		}
		g_strfreev (addresses);
	}
	if (params & PARAM_SUBJECT) {
		g_free (action->subject);
		action->subject = g_strdup (gtk_entry_get_text (GTK_ENTRY (row->subject)));
	}
	if (params & PARAM_TEXT) {
		GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (row->text));
		GtkTextIter start, end;

		gtk_text_buffer_get_bounds (buffer, &start, &end);
		g_free (action->text);
		action->text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
	}
	if (params & PARAM_ACCEPT) {
		g_free (action->accept_level);
		action->accept_level = g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (row->accept)));
	}
	if (params & PARAM_COMMENT) {
		const gchar *comment = gtk_entry_get_text (GTK_ENTRY (row->comment));

		g_free (action->comment);
		action->comment = *comment ? g_strdup (comment) : NULL;
	}
	if (params & PARAM_CATEGORY) {
		const gchar *id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (row->category));

		g_ptr_array_set_size (action->categories, 0);
		if (id)
			g_ptr_array_add (action->categories, g_strdup (id));
	}

	return action;
}

/* The entries of a row: the client writes some twice, so do we */
static void
collect_condition (ConditionRow *row,
		   GPtrArray *nodes)
{
	gint field = row->current;
	gboolean compare;
	const gchar *op = row_op (row, &compare);
	EGwFilterNode *node;
	gchar number[32];

	switch (fields[field].kind) {
	case FIELD_DATE:
		g_snprintf (number, sizeof (number), "%d", gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (row->days)));
		node = e_gw_filter_node_new_entry (fields[field].field, op, number);
		node->date = g_strdup ("Today");
		break;
	case FIELD_CHAR: {
		const gchar *text = gtk_entry_get_text (GTK_ENTRY (row->value));

		g_snprintf (number, sizeof (number), "%u", *text ? (guint) g_utf8_get_char (text) : 0);
		node = e_gw_filter_node_new_entry (fields[field].field, op, number);
		break;
	}
	case FIELD_ENUM: {
		const gchar *value = gtk_combo_box_get_active_id (GTK_COMBO_BOX (row->choice));

		node = e_gw_filter_node_new_entry (fields[field].field, fields[field].enum_op, value);
		/* As the client writes them: the value as the mask, or an empty mask */
		if (fields[field].with_mask)
			node->mask = g_strdup (value);
		else if (fields[field].twice)
			node->mask = g_strdup ("");
		break;
	}
	case FIELD_COUNTER:
		if (compare) {
			g_snprintf (number, sizeof (number), "%d", gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (row->offset)));
			node = e_gw_filter_node_new_entry (fields[field].field, op, number);
			node->date = g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (row->other)));
			break;
		}
		/* fall through */
	default:
		node = e_gw_filter_node_new_entry (fields[field].field, op, gtk_entry_get_text (GTK_ENTRY (row->value)));
		break;
	}

	g_ptr_array_add (nodes, node);
	if (fields[field].twice)
		g_ptr_array_add (nodes, e_gw_filter_node_copy (node));
}

static EGwRule *
collect (Editor *editor,
	 const EGwRule *original)
{
	EGwRule *rule = original ? e_gw_rule_copy (original) : e_gw_rule_new ();
	const gchar *source_names[G_N_ELEMENTS (sources)], *type_names[G_N_ELEMENTS (item_types)];
	gint event = gtk_combo_box_get_active (GTK_COMBO_BOX (editor->event));
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (sources); ii++)
		source_names[ii] = sources[ii].name;
	for (ii = 0; ii < G_N_ELEMENTS (item_types); ii++)
		type_names[ii] = item_types[ii].name;

	g_free (rule->name);
	rule->name = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (editor->name))));
	g_free (rule->execution);
	rule->execution = g_strdup (events[event].execution);
	g_free (rule->source);
	rule->source = events[event].has_sources ?
		joined_toggles (editor->sources, G_N_ELEMENTS (sources), source_names) : NULL;
	g_free (rule->container);
	rule->container = events[event].has_folder && gtk_combo_box_get_active_id (GTK_COMBO_BOX (editor->folder)) &&
		*gtk_combo_box_get_active_id (GTK_COMBO_BOX (editor->folder)) ?
		g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (editor->folder))) : NULL;
	g_free (rule->types);
	rule->types = joined_toggles (editor->types, G_N_ELEMENTS (item_types), type_names);
	g_free (rule->conflict);
	rule->conflict = gtk_widget_get_sensitive (editor->conflict) &&
		g_strcmp0 (gtk_combo_box_get_active_id (GTK_COMBO_BOX (editor->conflict)), "Ignore") != 0 ?
		g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (editor->conflict))) :
		(original && original->conflict ? g_strdup ("Ignore") : NULL);

	e_gw_filter_node_free (rule->filter);
	rule->filter = NULL;
	{
		GPtrArray *nodes = g_ptr_array_new ();

		for (ii = 0; ii < editor->conditions->len; ii++)
			collect_condition (editor->conditions->pdata[ii], nodes);
		if (nodes->len == 1) {
			rule->filter = nodes->pdata[0];
		} else if (nodes->len > 1) {
			rule->filter = e_gw_filter_node_new_group (gtk_combo_box_get_active_id (GTK_COMBO_BOX (editor->match)));
			for (ii = 0; ii < nodes->len; ii++)
				g_ptr_array_add (rule->filter->children, nodes->pdata[ii]);
		}
		g_ptr_array_unref (nodes);
	}

	g_ptr_array_set_size (rule->actions, 0);
	for (ii = 0; ii < editor->actions->len; ii++)
		g_ptr_array_add (rule->actions, collect_action (editor->actions->pdata[ii]));

	return rule;
}

/* ------------------------------------------------------------------ */

static GtkWidget *
section_label (const gchar *text)
{
	GtkWidget *label = gtk_label_new (NULL);
	gchar *markup = g_markup_printf_escaped ("<b>%s</b>", text);

	gtk_label_set_markup (GTK_LABEL (label), markup);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_widget_set_margin_top (label, 6);
	g_free (markup);

	return label;
}

static GtkWidget *
row_label (const gchar *text)
{
	GtkWidget *label = gtk_label_new_with_mnemonic (text);

	gtk_label_set_xalign (GTK_LABEL (label), 1.0);

	return label;
}

EGwRule *
e_groupwise_rule_editor_run (GtkWindow *parent,
			     const EGwRule *rule,
			     const EGroupwiseRuleContext *context)
{
	Editor editor = { NULL };
	GtkWidget *content, *scrolled, *box, *grid, *hbox, *button, *label;
	const gchar *source_names[G_N_ELEMENTS (sources)], *type_names[G_N_ELEMENTS (item_types)];
	EGwRule *result = NULL;
	gint event, line = 0;
	guint ii;

	editor.context = context;
	editor.editable = !rule || e_groupwise_rule_is_editable (rule);
	editor.conditions = g_ptr_array_new_with_free_func (g_free);
	editor.actions = g_ptr_array_new_with_free_func ((GDestroyNotify) action_row_free);
	for (ii = 0; ii < G_N_ELEMENTS (sources); ii++)
		source_names[ii] = sources[ii].name;
	for (ii = 0; ii < G_N_ELEMENTS (item_types); ii++)
		type_names[ii] = item_types[ii].name;

	editor.dialog = gtk_dialog_new_with_buttons (rule ? _("Edit Rule") : _("New Rule"), parent,
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, _("_Cancel"), GTK_RESPONSE_CANCEL, NULL);
	if (editor.editable)
		gtk_dialog_add_button (GTK_DIALOG (editor.dialog), _("_Save"), GTK_RESPONSE_OK);
	gtk_window_set_default_size (GTK_WINDOW (editor.dialog), 720, 680);
	content = gtk_dialog_get_content_area (GTK_DIALOG (editor.dialog));

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_box_pack_start (GTK_BOX (content), scrolled, TRUE, TRUE, 0);
	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_container_add (GTK_CONTAINER (scrolled), box);

	if (!editor.editable) {
		label = gtk_label_new (_("This rule has conditions or actions Evolution cannot show; it is shown as far as "
			"possible and cannot be saved here. Edit it in the GroupWise client."));
		gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
		gtk_label_set_xalign (GTK_LABEL (label), 0.0);
		gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
	}

	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);

	editor.name = gtk_entry_new ();
	gtk_widget_set_hexpand (editor.name, TRUE);
	if (rule && rule->name)
		gtk_entry_set_text (GTK_ENTRY (editor.name), rule->name);
	label = row_label (_("Rule _name:"));
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), editor.name);
	gtk_grid_attach (GTK_GRID (grid), label, 0, line, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), editor.name, 1, line++, 1, 1);

	editor.event = gtk_combo_box_text_new ();
	for (ii = 0; ii < G_N_ELEMENTS (events); ii++)
		gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (editor.event), events[ii].execution, _(events[ii].label));
	event = rule ? find_event (rule->execution) : 0;
	gtk_combo_box_set_active (GTK_COMBO_BOX (editor.event), event >= 0 ? event : 0);
	label = row_label (_("When _event is:"));
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), editor.event);
	gtk_grid_attach (GTK_GRID (grid), label, 0, line, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), editor.event, 1, line++, 1, 1);

	editor.sources_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
	for (ii = 0; ii < G_N_ELEMENTS (sources); ii++) {
		editor.sources[ii] = gtk_check_button_new_with_mnemonic (_(sources[ii].label));
		gtk_box_pack_start (GTK_BOX (editor.sources_box), editor.sources[ii], FALSE, FALSE, 0);
	}
	set_toggles (editor.sources, G_N_ELEMENTS (sources), source_names, rule ? rule->source : "received");
	gtk_grid_attach (GTK_GRID (grid), editor.sources_box, 1, line++, 1, 1);

	editor.folder = folder_combo (context, _("(any folder)"), rule ? rule->container : NULL);
	editor.folder_label = row_label (_("_Folder:"));
	gtk_label_set_mnemonic_widget (GTK_LABEL (editor.folder_label), editor.folder);
	gtk_grid_attach (GTK_GRID (grid), editor.folder_label, 0, line, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), editor.folder, 1, line++, 1, 1);

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
	for (ii = 0; ii < G_N_ELEMENTS (item_types); ii++) {
		editor.types[ii] = gtk_check_button_new_with_mnemonic (_(item_types[ii].label));
		gtk_box_pack_start (GTK_BOX (hbox), editor.types[ii], FALSE, FALSE, 0);
	}
	set_toggles (editor.types, G_N_ELEMENTS (item_types), type_names, rule ? rule->types : "Mail");
	gtk_grid_attach (GTK_GRID (grid), row_label (_("Item types:")), 0, line, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), hbox, 1, line++, 1, 1);

	editor.conflict = gtk_combo_box_text_new ();
	gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (editor.conflict), "Ignore", _("Regardless of conflicts"));
	gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (editor.conflict), "No", _("Only without a conflict"));
	gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (editor.conflict), "Yes", _("Only with a conflict"));
	if (!rule || !rule->conflict || !gtk_combo_box_set_active_id (GTK_COMBO_BOX (editor.conflict), rule->conflict))
		gtk_combo_box_set_active (GTK_COMBO_BOX (editor.conflict), 0);
	gtk_grid_attach (GTK_GRID (grid), row_label (_("Appointment con_flicts:")), 0, line, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), editor.conflict, 1, line++, 1, 1);

	/* Conditions */
	gtk_box_pack_start (GTK_BOX (box), section_label (_("Conditions")), FALSE, FALSE, 0);
	editor.match = gtk_combo_box_text_new ();
	gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (editor.match), "and", _("All conditions must be met (and)"));
	gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (editor.match), "or", _("Any condition must be met (or)"));
	if (!rule || !rule->filter || !rule->filter->children ||
	    !gtk_combo_box_set_active_id (GTK_COMBO_BOX (editor.match), rule->filter->op))
		gtk_combo_box_set_active (GTK_COMBO_BOX (editor.match), 0);
	gtk_widget_set_halign (editor.match, GTK_ALIGN_START);
	gtk_box_pack_start (GTK_BOX (box), editor.match, FALSE, FALSE, 0);
	editor.conditions_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_pack_start (GTK_BOX (box), editor.conditions_box, FALSE, FALSE, 0);
	if (rule && rule->filter && editor.editable) {
		if (rule->filter->children) {
			for (ii = 0; ii < rule->filter->children->len; ii++) {
				EGwFilterNode *node = rule->filter->children->pdata[ii];
				gint field = find_field (node->field);

				/* An entry the client wrote twice is one condition */
				if (ii > 0 && field >= 0 && fields[field].twice && same_entry (node, rule->filter->children->pdata[ii - 1]))
					continue;
				add_condition (&editor, node);
			}
		} else {
			add_condition (&editor, rule->filter);
		}
	} else if (rule && rule->filter) {
		gchar *xml = e_gw_filter_node_to_xml (rule->filter);

		label = gtk_label_new (xml);
		gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
		gtk_label_set_line_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
		gtk_label_set_selectable (GTK_LABEL (label), TRUE);
		gtk_label_set_xalign (GTK_LABEL (label), 0.0);
		gtk_box_pack_start (GTK_BOX (editor.conditions_box), label, FALSE, FALSE, 0);
		g_free (xml);
	}
	button = gtk_button_new_with_mnemonic (_("Add _Condition"));
	gtk_widget_set_halign (button, GTK_ALIGN_START);
	g_signal_connect (button, "clicked", G_CALLBACK (add_condition_cb), &editor);
	gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);

	/* Actions */
	gtk_box_pack_start (GTK_BOX (box), section_label (_("Actions")), FALSE, FALSE, 0);
	editor.actions_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (box), editor.actions_box, FALSE, FALSE, 0);
	for (ii = 0; rule && ii < rule->actions->len; ii++) {
		EGwRuleAction *action = rule->actions->pdata[ii];
		gint index = find_action (action->type);

		if (index >= 0) {
			add_action (&editor, index, action);
		} else {
			gchar *text = g_strdup_printf (_("Action “%s”"), action->type ? action->type : "?");

			label = gtk_label_new (text);
			gtk_label_set_xalign (GTK_LABEL (label), 0.0);
			gtk_box_pack_start (GTK_BOX (editor.actions_box), label, FALSE, FALSE, 0);
			g_free (text);
		}
	}
	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	editor.add_action = gtk_combo_box_text_new ();
	for (ii = 0; ii < G_N_ELEMENTS (actions); ii++)
		gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (editor.add_action), actions[ii].type, _(actions[ii].label));
	gtk_combo_box_set_active (GTK_COMBO_BOX (editor.add_action), 9);
	gtk_box_pack_start (GTK_BOX (hbox), editor.add_action, FALSE, FALSE, 0);
	button = gtk_button_new_with_mnemonic (_("Add _Action"));
	g_signal_connect (button, "clicked", G_CALLBACK (add_action_cb), &editor);
	gtk_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), hbox, FALSE, FALSE, 0);

	g_signal_connect (editor.event, "changed", G_CALLBACK (event_changed_cb), &editor);
	for (ii = 0; ii < G_N_ELEMENTS (item_types); ii++)
		g_signal_connect (editor.types[ii], "toggled", G_CALLBACK (type_toggled_cb), &editor);

	gtk_widget_show_all (content);
	update_event (&editor);
	update_conflict (&editor);
	gtk_widget_set_sensitive (box, TRUE);
	if (!editor.editable) {
		gtk_widget_set_sensitive (grid, FALSE);
		gtk_widget_set_sensitive (editor.conditions_box, FALSE);
		gtk_widget_set_sensitive (editor.actions_box, FALSE);
		gtk_widget_set_sensitive (editor.match, FALSE);
		gtk_widget_set_sensitive (button, FALSE);
		gtk_widget_set_sensitive (editor.add_action, FALSE);
	}

	while (gtk_dialog_run (GTK_DIALOG (editor.dialog)) == GTK_RESPONSE_OK) {
		gchar *name = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (editor.name))));
		gboolean no_name = !*name;

		g_free (name);
		if (no_name) {
			GtkWidget *message = gtk_message_dialog_new (GTK_WINDOW (editor.dialog), GTK_DIALOG_MODAL,
				GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, _("The rule needs a name."));

			gtk_dialog_run (GTK_DIALOG (message));
			gtk_widget_destroy (message);
			continue;
		}
		if (!editor.actions->len) {
			GtkWidget *message = gtk_message_dialog_new (GTK_WINDOW (editor.dialog), GTK_DIALOG_MODAL,
				GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, _("The rule needs at least one action."));

			gtk_dialog_run (GTK_DIALOG (message));
			gtk_widget_destroy (message);
			continue;
		}
		result = collect (&editor, rule);
		break;
	}

	gtk_widget_destroy (editor.dialog);
	g_ptr_array_unref (editor.conditions);
	g_ptr_array_unref (editor.actions);

	return result;
}

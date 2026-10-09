/*
 * camel-groupwise-labels.c: GroupWise categories as Evolution's labels
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

#include <stdio.h>
#include <string.h>

#include <glib/gi18n-lib.h>

#include "camel-groupwise-labels.h"

#define LABELS_SCHEMA "org.gnome.evolution.mail"
#define LABELS_KEY "labels"
#define DEFAULT_COLOR "#808080"

/* Evolution's built-in labels (e-mail-label-list-store.c): the untranslated
 * name as kept in GSettings, the tag, and the GroupWise category */
static const struct {
	const gchar *name;
	const gchar *tag;
	EGwCategoryType type;
} builtin[] = {
	{ "I_mportant", "$Labelimportant", E_GW_CATEGORY_URGENT },
	{ "_Work", "$Labelwork", E_GW_CATEGORY_NORMAL },
	{ "_Personal", "$Labelpersonal", E_GW_CATEGORY_PERSONAL },
	{ "_To Do", "$Labeltodo", E_GW_CATEGORY_FOLLOW_UP },
	{ "_Later", "$Labellater", E_GW_CATEGORY_LOW_PRIORITY }
};

struct _CamelGroupwiseLabels {
	GMutex lock;
	gchar *filename;
	GSettings *settings;		/* NULL without Evolution */
	GHashTable *tag_by_id;		/* category ID -> tag */
	GHashTable *id_by_tag;		/* tag -> category ID */
};

typedef struct {
	gchar *name;	/* as Evolution shows it */
	gchar *color;
	gchar *tag;
} Label;

static void
label_free (Label *label)
{
	g_free (label->name);
	g_free (label->color);
	g_free (label->tag);
	g_free (label);
}

/* The tag Evolution makes of a label name (Thunderbird compatible) */
static gchar *
tag_from_name (const gchar *name)
{
	gchar *temp = g_ascii_strdown (name, -1), *tag;

	g_strdelimit (temp, " ()/{%*<>\\\"", '_');
	tag = camel_utf8_utf7 (temp);
	g_free (temp);

	return tag;
}

/* A label name as Evolution shows it: translated, without the mnemonic */
static gchar *
display_name (const gchar *raw)
{
	GString *name = g_string_new (NULL);
	const gchar *translated = g_dgettext ("evolution", raw), *pp;

	for (pp = translated; *pp; pp++) {
		if (*pp != '_')
			g_string_append_c (name, *pp);
	}

	return g_string_free (name, FALSE);
}

/* Evolution's labels ("name:color[|tag]") */
static GPtrArray *
read_labels (CamelGroupwiseLabels *labels)
{
	GPtrArray *result = g_ptr_array_new_with_free_func ((GDestroyNotify) label_free);
	gchar **entries;
	guint ii, jj;

	if (!labels->settings)
		return result;

	entries = g_settings_get_strv (labels->settings, LABELS_KEY);
	for (ii = 0; entries && entries[ii]; ii++) {
		gchar **parts = g_strsplit_set (entries[ii], ":|", 3);
		Label *label;

		if (g_strv_length (parts) < 2) {
			g_strfreev (parts);
			continue;
		}
		label = g_new0 (Label, 1);
		label->name = display_name (parts[0]);
		label->color = g_strdup (parts[1]);
		if (parts[2] && *parts[2])
			label->tag = g_strdup (parts[2]);
		for (jj = 0; !label->tag && jj < G_N_ELEMENTS (builtin); jj++) {
			if (g_strcmp0 (parts[0], builtin[jj].name) == 0)
				label->tag = g_strdup (builtin[jj].tag);
		}
		if (!label->tag)
			label->tag = tag_from_name (parts[0]);
		g_ptr_array_add (result, label);
		g_strfreev (parts);
	}
	g_strfreev (entries);

	return result;
}

static Label *
find_label (GPtrArray *list,
	    const gchar *name,
	    const gchar *tag)
{
	guint ii;

	for (ii = 0; ii < list->len; ii++) {
		Label *label = list->pdata[ii];

		if (tag && g_strcmp0 (label->tag, tag) == 0)
			return label;
		if (name && g_utf8_collate (label->name, name) == 0)
			return label;
	}

	return NULL;
}

static gchar *
color_text (gint64 colorref)
{
	if (colorref < 0)
		return g_strdup (DEFAULT_COLOR);

	return g_strdup_printf ("#%02x%02x%02x", (guint) (colorref & 0xff),
		(guint) ((colorref >> 8) & 0xff), (guint) ((colorref >> 16) & 0xff));
}

static gint64
colorref_from_text (const gchar *text)
{
	guint r, g, b;

	if (text && sscanf (text, "#%02x%02x%02x", &r, &g, &b) == 3)
		return r | (g << 8) | (b << 16);

	return -1;
}

static const gchar *
builtin_tag (EGwCategoryType type)
{
	guint ii;

	for (ii = 0; type != E_GW_CATEGORY_NORMAL && ii < G_N_ELEMENTS (builtin); ii++) {
		if (builtin[ii].type == type)
			return builtin[ii].tag;
	}

	return NULL;
}

static void
map_locked (CamelGroupwiseLabels *labels,
	    const gchar *id,
	    const gchar *tag)
{
	g_hash_table_replace (labels->tag_by_id, g_strdup (id), g_strdup (tag));
	g_hash_table_replace (labels->id_by_tag, g_strdup (tag), g_strdup (id));
}

static void
save_locked (CamelGroupwiseLabels *labels)
{
	GKeyFile *key_file = g_key_file_new ();
	GHashTableIter iter;
	gpointer key, value;
	GError *error = NULL;

	g_hash_table_iter_init (&iter, labels->tag_by_id);
	while (g_hash_table_iter_next (&iter, &key, &value))
		g_key_file_set_string (key_file, "Categories", key, value);
	if (!g_key_file_save_to_file (key_file, labels->filename, &error)) {
		g_debug ("labels: cannot save %s: %s", labels->filename, error->message);
		g_clear_error (&error);
	}
	g_key_file_free (key_file);
}

CamelGroupwiseLabels *
camel_groupwise_labels_new (const gchar *filename)
{
	CamelGroupwiseLabels *labels = g_new0 (CamelGroupwiseLabels, 1);
	GSettingsSchema *schema;
	GKeyFile *key_file = g_key_file_new ();
	gchar **ids;
	guint ii;

	g_mutex_init (&labels->lock);
	labels->filename = g_strdup (filename);
	labels->tag_by_id = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	labels->id_by_tag = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	/* Only where Evolution is installed */
	schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (), LABELS_SCHEMA, TRUE);
	if (schema && g_settings_schema_has_key (schema, LABELS_KEY))
		labels->settings = g_settings_new_full (schema, NULL, NULL);
	g_clear_pointer (&schema, g_settings_schema_unref);

	/* What the last login found, for offline use */
	if (filename && g_key_file_load_from_file (key_file, filename, G_KEY_FILE_NONE, NULL)) {
		ids = g_key_file_get_keys (key_file, "Categories", NULL, NULL);
		for (ii = 0; ids && ids[ii]; ii++) {
			gchar *tag = g_key_file_get_string (key_file, "Categories", ids[ii], NULL);

			if (tag && *tag)
				map_locked (labels, ids[ii], tag);
			g_free (tag);
		}
		g_strfreev (ids);
	}
	g_key_file_free (key_file);

	return labels;
}

void
camel_groupwise_labels_free (CamelGroupwiseLabels *labels)
{
	if (!labels)
		return;

	g_clear_object (&labels->settings);
	g_hash_table_destroy (labels->tag_by_id);
	g_hash_table_destroy (labels->id_by_tag);
	g_free (labels->filename);
	g_mutex_clear (&labels->lock);
	g_free (labels);
}

void
camel_groupwise_labels_set_categories (CamelGroupwiseLabels *labels,
				       GPtrArray *categories)
{
	GPtrArray *evolution, *added = g_ptr_array_new_with_free_func (g_free);
	guint ii;

	g_return_if_fail (labels != NULL);
	g_return_if_fail (categories != NULL);

	g_mutex_lock (&labels->lock);
	evolution = read_labels (labels);
	g_hash_table_remove_all (labels->tag_by_id);
	g_hash_table_remove_all (labels->id_by_tag);

	for (ii = 0; ii < categories->len; ii++) {
		EGwCategory *category = categories->pdata[ii];
		const gchar *tag = builtin_tag (category->type);
		Label *label;

		if (tag || !category->name || !*category->name) {
			if (tag)
				map_locked (labels, category->id, tag);
			continue;
		}

		label = find_label (evolution, category->name, NULL);
		if (label) {
			map_locked (labels, category->id, label->tag);
		} else {
			gchar *new_tag = tag_from_name (category->name);

			/* A tag another label has already (another name, same tag) */
			if (!find_label (evolution, NULL, new_tag) && !g_hash_table_contains (labels->id_by_tag, new_tag)) {
				map_locked (labels, category->id, new_tag);
				/* What the GroupWise client does not offer is not added */
				if (!category->hidden) {
					gchar *color = color_text (category->color);

					g_ptr_array_add (added, g_strdup_printf ("%s:%s|%s", category->name, color, new_tag));
					g_free (color);
				}
			}
			g_free (new_tag);
		}
	}

	/* Evolution follows the key: the new labels show at once */
	if (added->len > 0 && labels->settings) {
		gchar **entries = g_settings_get_strv (labels->settings, LABELS_KEY);
		GPtrArray *all = g_ptr_array_new ();

		for (ii = 0; entries && entries[ii]; ii++)
			g_ptr_array_add (all, entries[ii]);
		for (ii = 0; ii < added->len; ii++) {
			g_debug ("labels: adding %s", (const gchar *) added->pdata[ii]);
			g_ptr_array_add (all, added->pdata[ii]);
		}
		g_ptr_array_add (all, NULL);
		g_settings_set_strv (labels->settings, LABELS_KEY, (const gchar * const *) all->pdata);
		g_ptr_array_unref (all);
		g_strfreev (entries);
	}

	save_locked (labels);
	g_mutex_unlock (&labels->lock);

	g_ptr_array_unref (evolution);
	g_ptr_array_unref (added);
}

gchar *
camel_groupwise_labels_dup_tag (CamelGroupwiseLabels *labels,
				const gchar *id)
{
	gchar *tag;

	g_return_val_if_fail (labels != NULL, NULL);

	g_mutex_lock (&labels->lock);
	tag = id ? g_strdup (g_hash_table_lookup (labels->tag_by_id, id)) : NULL;
	g_mutex_unlock (&labels->lock);

	return tag;
}

gboolean
camel_groupwise_labels_is_label (CamelGroupwiseLabels *labels,
				 const gchar *tag)
{
	GPtrArray *evolution;
	gboolean is_label;

	g_return_val_if_fail (labels != NULL, FALSE);

	if (!tag || !*tag)
		return FALSE;

	g_mutex_lock (&labels->lock);
	is_label = g_hash_table_contains (labels->id_by_tag, tag);
	if (!is_label) {
		evolution = read_labels (labels);
		is_label = find_label (evolution, NULL, tag) != NULL;
		g_ptr_array_unref (evolution);
	}
	g_mutex_unlock (&labels->lock);

	return is_label;
}

gchar *
camel_groupwise_labels_dup_category_sync (CamelGroupwiseLabels *labels,
					  const gchar *tag,
					  EGwConnection *cnc,
					  GCancellable *cancellable,
					  GError **error)
{
	GPtrArray *evolution;
	Label *label;
	gchar *id, *name, *color;

	g_return_val_if_fail (labels != NULL, NULL);
	g_return_val_if_fail (tag != NULL, NULL);

	g_mutex_lock (&labels->lock);
	id = g_strdup (g_hash_table_lookup (labels->id_by_tag, tag));
	evolution = id ? NULL : read_labels (labels);
	label = evolution ? find_label (evolution, NULL, tag) : NULL;
	name = label ? g_strdup (label->name) : NULL;
	color = label ? g_strdup (label->color) : NULL;
	g_clear_pointer (&evolution, g_ptr_array_unref);
	g_mutex_unlock (&labels->lock);

	if (id || !name) {
		g_free (name);
		g_free (color);
		return id;
	}

	/* A label GroupWise does not know yet: a category of its name */
	g_debug ("labels: creating the category %s for %s", name, tag);
	id = e_gw_connection_create_category_sync (cnc, name, colorref_from_text (color), cancellable, error);
	if (id) {
		g_mutex_lock (&labels->lock);
		map_locked (labels, id, tag);
		save_locked (labels);
		g_mutex_unlock (&labels->lock);
	}
	g_free (name);
	g_free (color);

	return id;
}

gchar **
camel_groupwise_labels_dup_info_tags (CamelGroupwiseLabels *labels,
				      CamelMessageInfo *info)
{
	const CamelNamedFlags *flags;
	GPtrArray *tags = g_ptr_array_new ();
	guint ii, count;

	camel_message_info_property_lock (info);
	flags = camel_message_info_get_user_flags (info);
	count = flags ? camel_named_flags_get_length (flags) : 0;
	for (ii = 0; ii < count; ii++) {
		const gchar *tag = camel_named_flags_get (flags, ii);

		if (camel_groupwise_labels_is_label (labels, tag))
			g_ptr_array_add (tags, g_strdup (tag));
	}
	camel_message_info_property_unlock (info);
	g_ptr_array_add (tags, NULL);

	return (gchar **) g_ptr_array_free (tags, FALSE);
}

gboolean
camel_groupwise_labels_apply (CamelGroupwiseLabels *labels,
			      CamelMessageInfo *info,
			      const gchar * const *ids)
{
	GPtrArray *wanted = g_ptr_array_new_with_free_func (g_free);
	gchar **current;
	gboolean changed = FALSE;
	guint ii;

	for (ii = 0; ids && ids[ii]; ii++) {
		gchar *tag = camel_groupwise_labels_dup_tag (labels, ids[ii]);

		if (tag)
			g_ptr_array_add (wanted, tag);
	}
	g_ptr_array_add (wanted, NULL);

	current = camel_groupwise_labels_dup_info_tags (labels, info);
	for (ii = 0; current[ii]; ii++) {
		if (!g_strv_contains ((const gchar * const *) wanted->pdata, current[ii]))
			changed = camel_message_info_set_user_flag (info, current[ii], FALSE) || changed;
	}
	for (ii = 0; wanted->pdata[ii]; ii++) {
		if (!g_strv_contains ((const gchar * const *) current, wanted->pdata[ii]))
			changed = camel_message_info_set_user_flag (info, wanted->pdata[ii], TRUE) || changed;
	}
	g_strfreev (current);
	g_ptr_array_unref (wanted);

	return changed;
}

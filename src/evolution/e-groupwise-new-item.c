/*
 * e-groupwise-new-item.c: new items made outside their view go to the main account
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

/* "New → Meeting" (appointment, task, memo) puts the new item into the
 * calendar or list that is selected in the calendar, task or memo view —
 * also when another view is shown and the user does not see that
 * selection. With proxy accounts it is then a matter of chance whose
 * mailbox the item goes to and who the organizer of a meeting is. An item
 * made outside its view, that would go to a mailbox of another user (a
 * proxy account, a proxy or shared calendar), goes to the calendar or
 * list of the GroupWise main account instead; the editor still lets the
 * user choose another. Made in its view, the item goes where the user has
 * selected, as before. */

#include <camel/camel.h>
#include <e-util/e-util.h>
#include <calendar/gui/e-comp-editor.h>
#include <calendar/gui/e-comp-editor-event.h>
#include <calendar/gui/e-comp-editor-memo.h>
#include <calendar/gui/e-comp-editor-page-general.h>
#include <calendar/gui/e-comp-editor-task.h>
#include <shell/e-shell.h>
#include <shell/e-shell-window.h>

#include "camel-groupwise-settings.h"
#include "e-source-groupwise-folder.h"
#include "e-groupwise-new-item.h"

typedef struct _EGroupwiseNewItem {
	EExtension parent;

	gboolean checked;
	guint idle;
} EGroupwiseNewItem;

typedef struct _EGroupwiseNewItemClass {
	EExtensionClass parent_class;
} EGroupwiseNewItemClass;

GType e_groupwise_new_item_get_type (void);

G_DEFINE_DYNAMIC_TYPE (EGroupwiseNewItem, e_groupwise_new_item, E_TYPE_EXTENSION)

/* The GroupWise settings of the account @source belongs to; not referenced */
static CamelGroupwiseSettings *
get_account_settings (ESourceRegistry *registry,
		      ESource *source,
		      ESource **out_account)
{
	const gchar *extension_name = e_source_camel_get_extension_name ("groupwise");
	ESource *account = e_source_registry_find_extension (registry, source, extension_name);
	CamelSettings *settings = account ? e_source_camel_get_settings (e_source_get_extension (account, extension_name)) : NULL;

	if (!settings || !CAMEL_IS_GROUPWISE_SETTINGS (settings)) {
		g_clear_object (&account);
		return NULL;
	}
	if (out_account)
		*out_account = account;
	else
		g_object_unref (account);

	return CAMEL_GROUPWISE_SETTINGS (settings);
}

static gboolean
is_proxy_account (CamelGroupwiseSettings *settings)
{
	gchar *proxy = camel_groupwise_settings_dup_proxy (settings);
	gboolean set = proxy && *proxy;

	g_free (proxy);

	return set;
}

static gboolean
same_login (CamelGroupwiseSettings *a,
	    CamelGroupwiseSettings *b)
{
	gchar *ha = camel_network_settings_dup_host (CAMEL_NETWORK_SETTINGS (a));
	gchar *hb = camel_network_settings_dup_host (CAMEL_NETWORK_SETTINGS (b));
	gchar *ua = camel_network_settings_dup_user (CAMEL_NETWORK_SETTINGS (a));
	gchar *ub = camel_network_settings_dup_user (CAMEL_NETWORK_SETTINGS (b));
	gboolean same = ha && hb && ua && ub && *ha && *ua && !g_ascii_strcasecmp (ha, hb) && !g_ascii_strcasecmp (ua, ub);

	g_free (ha);
	g_free (hb);
	g_free (ua);
	g_free (ub);

	return same;
}

/* The Calendar (the task list, the memo list) of the main account that
 * the account of @settings is a proxy account of, or that is that account */
static ESource *
ref_main_folder (ESourceRegistry *registry,
		 CamelGroupwiseSettings *settings,
		 const gchar *extension_name)
{
	GList *sources = e_source_registry_list_enabled (registry, extension_name), *link;
	ESource *found = NULL;

	for (link = sources; link && !found; link = g_list_next (link)) {
		ESource *source = link->data;
		CamelGroupwiseSettings *other;
		gchar *role;

		if (!e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
			continue;
		role = e_source_groupwise_folder_dup_role (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
		other = !role || !*role ? get_account_settings (registry, source, NULL) : NULL;
		if (other && !is_proxy_account (other) && same_login (settings, other))
			found = g_object_ref (source);
		g_free (role);
	}
	g_list_free_full (sources, g_object_unref);

	return found;
}

/* The view the user looks at */
static const gchar *
get_active_view (EShell *shell)
{
	GList *link;

	for (link = gtk_application_get_windows (GTK_APPLICATION (shell)); link; link = g_list_next (link)) {
		if (E_IS_SHELL_WINDOW (link->data))
			return e_shell_window_get_active_view (link->data);
	}

	return NULL;
}

static gboolean
check_idle_cb (gpointer user_data)
{
	EGroupwiseNewItem *self = user_data;
	ECompEditor *comp_editor = E_COMP_EDITOR (e_extension_get_extensible (E_EXTENSION (self)));
	ECalClient *client = e_comp_editor_get_target_client (comp_editor);
	ECompEditorPage *page;
	ESourceRegistry *registry;
	ESource *source, *account = NULL, *main_folder = NULL;
	CamelGroupwiseSettings *settings;
	const gchar *view_name, *extension_name;
	gchar *role = NULL;

	self->idle = 0;
	/* Once, when the new item knows where it would go and its window is
	 * there (building it, Evolution selects the calendar it started with
	 * once more) */
	if (self->checked || !client || !gtk_widget_get_realized (GTK_WIDGET (comp_editor)))
		return G_SOURCE_REMOVE;
	self->checked = TRUE;

	if (!(e_comp_editor_get_flags (comp_editor) & E_COMP_EDITOR_FLAG_IS_NEW))
		return G_SOURCE_REMOVE;

	if (E_IS_COMP_EDITOR_EVENT (comp_editor)) {
		view_name = "calendar";
		extension_name = E_SOURCE_EXTENSION_CALENDAR;
	} else if (E_IS_COMP_EDITOR_TASK (comp_editor)) {
		view_name = "tasks";
		extension_name = E_SOURCE_EXTENSION_TASK_LIST;
	} else if (E_IS_COMP_EDITOR_MEMO (comp_editor)) {
		view_name = "memos";
		extension_name = E_SOURCE_EXTENSION_MEMO_LIST;
	} else {
		return G_SOURCE_REMOVE;
	}
	/* In its view the user sees what is selected */
	if (g_strcmp0 (get_active_view (e_comp_editor_get_shell (comp_editor)), view_name) == 0)
		return G_SOURCE_REMOVE;

	registry = e_shell_get_registry (e_comp_editor_get_shell (comp_editor));
	source = e_client_get_source (E_CLIENT (client));
	settings = get_account_settings (registry, source, &account);
	if (!settings)
		return G_SOURCE_REMOVE;

	if (e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
		role = e_source_groupwise_folder_dup_role (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
	/* Another user's mailbox? */
	if (is_proxy_account (settings) || g_strcmp0 (role, E_GW_SOURCE_ROLE_PROXY) == 0 ||
	    g_strcmp0 (role, E_GW_SOURCE_ROLE_SHARED) == 0)
		main_folder = ref_main_folder (registry, settings, extension_name);
	page = main_folder ? e_comp_editor_get_page (comp_editor, E_TYPE_COMP_EDITOR_PAGE_GENERAL) : NULL;
	if (page && main_folder != source) {
		g_debug ("new item: the main account's %s instead of %s of %s", e_source_get_display_name (main_folder),
			e_source_get_display_name (source), e_source_get_display_name (account));
		e_comp_editor_page_general_set_selected_source (E_COMP_EDITOR_PAGE_GENERAL (page), main_folder);
	}

	g_clear_object (&main_folder);
	g_clear_object (&account);
	g_free (role);

	return G_SOURCE_REMOVE;
}

static void
schedule_check_cb (EGroupwiseNewItem *self)
{
	if (!self->checked && !self->idle)
		self->idle = g_idle_add (check_idle_cb, self);
}

static void
e_groupwise_new_item_dispose (GObject *object)
{
	EGroupwiseNewItem *self = (EGroupwiseNewItem *) object;

	if (self->idle) {
		g_source_remove (self->idle);
		self->idle = 0;
	}

	G_OBJECT_CLASS (e_groupwise_new_item_parent_class)->dispose (object);
}

static void
e_groupwise_new_item_constructed (GObject *object)
{
	G_OBJECT_CLASS (e_groupwise_new_item_parent_class)->constructed (object);

	g_signal_connect_object (e_extension_get_extensible (E_EXTENSION (object)), "notify::target-client",
		G_CALLBACK (schedule_check_cb), object, G_CONNECT_AFTER | G_CONNECT_SWAPPED);
	g_signal_connect_object (e_extension_get_extensible (E_EXTENSION (object)), "realize",
		G_CALLBACK (schedule_check_cb), object, G_CONNECT_AFTER | G_CONNECT_SWAPPED);
}

static void
e_groupwise_new_item_class_init (EGroupwiseNewItemClass *class)
{
	G_OBJECT_CLASS (class)->constructed = e_groupwise_new_item_constructed;
	G_OBJECT_CLASS (class)->dispose = e_groupwise_new_item_dispose;
	E_EXTENSION_CLASS (class)->extensible_type = E_TYPE_COMP_EDITOR;
}

static void
e_groupwise_new_item_class_finalize (EGroupwiseNewItemClass *class)
{
}

static void
e_groupwise_new_item_init (EGroupwiseNewItem *self)
{
}

void
e_groupwise_new_item_type_register (GTypeModule *type_module)
{
	e_groupwise_new_item_register_type (type_module);
}

/*
 * e-gw-folder.c: GroupWise folders (getFolderListRequest)
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

#include "e-gw-folder.h"
#include "e-gw-xml.h"

static const struct {
	const gchar *name;
	EGwFolderType type;
} folder_types[] = {
	{ "Normal", E_GW_FOLDER_TYPE_NORMAL },
	{ "Root", E_GW_FOLDER_TYPE_ROOT },
	{ "Mailbox", E_GW_FOLDER_TYPE_MAILBOX },
	{ "SentItems", E_GW_FOLDER_TYPE_SENT_ITEMS },
	{ "Draft", E_GW_FOLDER_TYPE_DRAFT },
	{ "Trash", E_GW_FOLDER_TYPE_TRASH },
	{ "JunkMail", E_GW_FOLDER_TYPE_JUNK },
	{ "Calendar", E_GW_FOLDER_TYPE_CALENDAR },
	{ "Contacts", E_GW_FOLDER_TYPE_CONTACTS },
	{ "Cabinet", E_GW_FOLDER_TYPE_CABINET },
	{ "Checklist", E_GW_FOLDER_TYPE_CHECKLIST },
	{ "Documents", E_GW_FOLDER_TYPE_DOCUMENTS },
	{ "Query", E_GW_FOLDER_TYPE_QUERY },
	{ "Notes", E_GW_FOLDER_TYPE_NOTES }
};

EGwFolderType
e_gw_folder_type_from_string (const gchar *folder_type)
{
	guint ii;

	if (!folder_type || !*folder_type)
		return E_GW_FOLDER_TYPE_NORMAL;

	for (ii = 0; ii < G_N_ELEMENTS (folder_types); ii++) {
		if (g_strcmp0 (folder_types[ii].name, folder_type) == 0)
			return folder_types[ii].type;
	}

	return E_GW_FOLDER_TYPE_OTHER;
}

static gchar *
dup_id (xmlNode *node,
	const gchar *path)
{
	gchar *text = e_gw_xml_dup_text (node, path);
	gchar *id = text && *text ? e_gw_clean_id (text) : NULL;

	g_free (text);

	return id;
}

EGwFolder *
e_gw_folder_new_from_node (xmlNode *node)
{
	EGwFolder *folder;
	gchar *kind;

	g_return_val_if_fail (node != NULL, NULL);

	folder = g_new0 (EGwFolder, 1);
	folder->id = dup_id (node, "id");
	folder->name = e_gw_xml_dup_text (node, "name");
	folder->parent_id = dup_id (node, "parent");
	folder->folder_type = e_gw_xml_dup_text (node, "folderType");
	folder->type = e_gw_folder_type_from_string (folder->folder_type);
	folder->count = e_gw_xml_get_int (node, "count", -1);
	folder->unread_count = e_gw_xml_get_int (node, "unreadCount", -1);
	folder->is_shared_to_me = e_gw_xml_get_bool (node, "isSharedToMe");
	folder->is_shared_by_me = e_gw_xml_get_bool (node, "isSharedByMe");
	/* GroupWise 26 names the owner with <displayName>, <email>, <uuid> */
	if (e_gw_xml_find (node, "owner/email")) {
		folder->owner = e_gw_xml_dup_text (node, "owner/email");
		folder->owner_name = e_gw_xml_dup_text (node, "owner/displayName");
	} else {
		folder->owner = e_gw_xml_dup_text (node, "owner");
	}
	folder->proxy_email = e_gw_xml_dup_text (node, "proxy/email");
	folder->proxy_uuid = e_gw_xml_dup_text (node, "proxy/uuid");
	folder->color = e_gw_xml_find (node, "calendarAttribute/color") ?
		e_gw_xml_get_int (node, "calendarAttribute/color", -1) : -1;
	folder->description = e_gw_xml_dup_text (node, "description");
	folder->is_calendar = folder->type == E_GW_FOLDER_TYPE_CALENDAR || e_gw_xml_find (node, "calendarAttribute") != NULL;
	folder->includes_content = TRUE;
	{
		xmlNode *flag;

		for (flag = e_gw_xml_first_child (e_gw_xml_find (node, "calendarAttribute"), "flags"); flag;
		     flag = e_gw_xml_next_sibling (flag, "flags")) {
			gchar *text = e_gw_xml_dup_text (flag, NULL);

			if (g_strcmp0 (text, "DontIncludeContent") == 0)
				folder->includes_content = FALSE;
			g_free (text);
		}
	}

	kind = e_gw_xml_dup_attr (node, "type");
	if (g_strcmp0 (kind, "SystemFolder") == 0 || e_gw_xml_get_bool (node, "isSystemFolder"))
		folder->kind = E_GW_FOLDER_KIND_SYSTEM;
	else if (g_strcmp0 (kind, "SharedFolder") == 0)
		folder->kind = E_GW_FOLDER_KIND_SHARED;
	else
		folder->kind = E_GW_FOLDER_KIND_FOLDER;
	g_free (kind);

	return folder;
}

void
e_gw_folder_free (EGwFolder *folder)
{
	if (!folder)
		return;

	g_free (folder->id);
	g_free (folder->name);
	g_free (folder->parent_id);
	g_free (folder->folder_type);
	g_free (folder->owner);
	g_free (folder->owner_name);
	g_free (folder->proxy_email);
	g_free (folder->proxy_uuid);
	g_free (folder->description);
	g_free (folder);
}

/* The calendar of another user: a proxy calendar ("Muster, Erika-Proxy-Kalender.")
 * or one shared to the user */
static gboolean
is_other_users_calendar (const EGwFolder *folder)
{
	gchar *description = folder->description ? g_utf8_strdown (folder->description, -1) : NULL;
	/* A shared folder is the user's own when the user shares it */
	gboolean other = folder->is_shared_to_me ||
		(folder->kind == E_GW_FOLDER_KIND_SHARED && !folder->is_shared_by_me) ||
		folder->proxy_email || g_strcmp0 (folder->folder_type, "Proxy") == 0 ||
		(description && (strstr (description, "proxy-kalender") || strstr (description, "proxy calendar")));

	g_free (description);

	return other;
}

gboolean
e_gw_folder_is_own_subcalendar (const EGwFolder *folder,
				GPtrArray *folders)
{
	const EGwFolder *parent = NULL;
	guint ii;
	gboolean holds_other = FALSE, holds_own = FALSE;

	g_return_val_if_fail (folder != NULL, FALSE);

	if (!folder->is_calendar || folder->type == E_GW_FOLDER_TYPE_CALENDAR || is_other_users_calendar (folder) ||
	    g_strcmp0 (folder->folder_type, "Subscribe") == 0)
		return FALSE;

	for (ii = 0; folders && ii < folders->len; ii++) {
		const EGwFolder *other = folders->pdata[ii];

		if (g_strcmp0 (other->id, folder->parent_id) == 0)
			parent = other;
		if (g_strcmp0 (other->parent_id, folder->id) == 0 && other->is_calendar) {
			if (is_other_users_calendar (other))
				holds_other = TRUE;
			else
				holds_own = TRUE;
		}
	}

	/* Below the Calendar (or one of its own subcalendars) */
	if (!parent || !parent->is_calendar || is_other_users_calendar (parent))
		return FALSE;

	/* A folder for the columns of other users' calendars */
	return !holds_other || holds_own;
}

/* Below the Calendar, at any depth */
static gboolean
below_calendar (const EGwFolder *folder,
		GPtrArray *folders)
{
	const gchar *parent_id = folder->parent_id;
	guint depth, ii;

	for (depth = 0; parent_id && depth < 32; depth++) {
		const EGwFolder *parent = NULL;

		for (ii = 0; folders && ii < folders->len && !parent; ii++) {
			if (g_strcmp0 (((EGwFolder *) folders->pdata[ii])->id, parent_id) == 0)
				parent = folders->pdata[ii];
		}
		if (!parent)
			return FALSE;
		if (parent->type == E_GW_FOLDER_TYPE_CALENDAR)
			return TRUE;
		parent_id = parent->parent_id;
	}

	return FALSE;
}

EGwCalendarRole
e_gw_folder_get_calendar_role (const EGwFolder *folder,
			       GPtrArray *folders)
{
	g_return_val_if_fail (folder != NULL, E_GW_CALENDAR_ROLE_NONE);

	if (folder->type == E_GW_FOLDER_TYPE_CALENDAR && folder->kind == E_GW_FOLDER_KIND_SYSTEM)
		return E_GW_CALENDAR_ROLE_MAIN;
	if (!folder->is_calendar || g_strcmp0 (folder->folder_type, "Subscribe") == 0)
		return E_GW_CALENDAR_ROLE_NONE;
	if (e_gw_folder_is_own_subcalendar (folder, folders))
		return E_GW_CALENDAR_ROLE_OWN;
	if (!below_calendar (folder, folders))
		return E_GW_CALENDAR_ROLE_NONE;
	/* Only a proxy calendar naming its user can be opened */
	if ((folder->proxy_email && *folder->proxy_email) || (folder->proxy_uuid && *folder->proxy_uuid))
		return E_GW_CALENDAR_ROLE_PROXY;
	if (folder->is_shared_to_me)
		return E_GW_CALENDAR_ROLE_SHARED;

	return E_GW_CALENDAR_ROLE_NONE;
}

gchar *
e_gw_folder_dup_color (const EGwFolder *folder)
{
	g_return_val_if_fail (folder != NULL, NULL);

	if (folder->color < 0)
		return NULL;

	/* COLORREF: red in the lowest byte (as GroupWise Web reads it) */
	return g_strdup_printf ("#%02x%02x%02x", (guint) (folder->color & 0xff),
		(guint) ((folder->color >> 8) & 0xff), (guint) ((folder->color >> 16) & 0xff));
}

GPtrArray *
e_gw_connection_get_folder_list_sync (EGwConnection *cnc,
				      const gchar *parent,
				      gboolean recurse,
				      GCancellable *cancellable,
				      GError **error)
{
	GString *inner = g_string_new (NULL);
	EGwResponse *response;
	GPtrArray *folders;
	xmlNode *node;

	e_gw_xml_add_leaf (inner, "parent", parent ? parent : "folders");
	g_string_append (inner, "<view/>");
	e_gw_xml_add_bool (inner, "recurse", recurse);
	e_gw_xml_add_bool (inner, "imap", FALSE);
	e_gw_xml_add_bool (inner, "nntp", FALSE);

	response = e_gw_connection_call_sync (cnc, "getFolderList", inner->str, cancellable, error);
	g_string_free (inner, TRUE);

	if (!response)
		return NULL;

	folders = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_folder_free);
	node = e_gw_xml_find (e_gw_response_get_node (response), "folders");
	for (node = e_gw_xml_first_child (node, "folder"); node; node = e_gw_xml_next_sibling (node, "folder")) {
		EGwFolder *folder = e_gw_folder_new_from_node (node);

		if (folder->id)
			g_ptr_array_add (folders, folder);
		else
			e_gw_folder_free (folder);
	}

	e_gw_response_free (response);

	return folders;
}

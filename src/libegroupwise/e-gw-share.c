/*
 * e-gw-share.c: sharing folders with other users
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
 * A folder is shared by changing it (modifyItemRequest) with the users in
 * its <acl>; the POA then makes it a SharedFolder, which changes the end of
 * its ID, and sends the new users a SharedNotification. Who accepts that
 * (acceptShareRequest) gets a folder of the own mailbox standing for the
 * shared one; who the share is taken from loses that folder again.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-share.h"
#include "e-gw-xml.h"

static const struct {
	const gchar *name;
	EGwShareRights right;
} rights_map[] = {
	{ "read", E_GW_SHARE_READ },
	{ "add", E_GW_SHARE_ADD },
	{ "edit", E_GW_SHARE_EDIT },
	{ "delete", E_GW_SHARE_DELETE }
};

static const struct {
	const gchar *name;
	EGwShareStatus status;
} status_map[] = {
	{ "pending", E_GW_SHARE_STATUS_PENDING },
	{ "opened", E_GW_SHARE_STATUS_OPENED },
	{ "accepted", E_GW_SHARE_STATUS_ACCEPTED },
	{ "deleted", E_GW_SHARE_STATUS_DELETED },
	{ "owner", E_GW_SHARE_STATUS_OWNER }
};

EGwShareRights
e_gw_share_rights_from_node (xmlNode *parent)
{
	xmlNode *rights = e_gw_xml_find (parent, "rights");
	EGwShareRights out = 0;
	guint ii;

	for (ii = 0; rights && ii < G_N_ELEMENTS (rights_map); ii++) {
		if (e_gw_xml_get_bool (rights, rights_map[ii].name))
			out |= rights_map[ii].right;
	}

	return out;
}

EGwShareEntry *
e_gw_share_entry_new (const gchar *display_name,
		      const gchar *email,
		      EGwShareRights rights)
{
	EGwShareEntry *entry = g_new0 (EGwShareEntry, 1);

	entry->display_name = g_strdup (display_name);
	entry->email = g_strdup (email);
	entry->rights = rights | E_GW_SHARE_READ;
	entry->status = E_GW_SHARE_STATUS_PENDING;

	return entry;
}

EGwShareEntry *
e_gw_share_entry_copy (const EGwShareEntry *entry)
{
	EGwShareEntry *copy;

	if (!entry)
		return NULL;

	copy = e_gw_share_entry_new (entry->display_name, entry->email, entry->rights);
	copy->uuid = g_strdup (entry->uuid);
	copy->status = entry->status;

	return copy;
}

void
e_gw_share_entry_free (EGwShareEntry *entry)
{
	if (!entry)
		return;

	g_free (entry->display_name);
	g_free (entry->email);
	g_free (entry->uuid);
	g_free (entry);
}

void
e_gw_share_free (EGwShare *share)
{
	if (!share)
		return;

	g_free (share->id);
	g_free (share->name);
	g_free (share->description);
	g_free (share->owner_name);
	g_free (share->owner_email);
	g_clear_pointer (&share->entries, g_ptr_array_unref);
	g_free (share);
}

EGwShareRights
e_gw_share_get_my_rights (const EGwShare *share)
{
	g_return_val_if_fail (share != NULL, 0);

	if (!share->shared_to_me)
		return E_GW_SHARE_ALL;
	if (!share->entries->len)
		return E_GW_SHARE_READ;

	return ((EGwShareEntry *) share->entries->pdata[0])->rights | E_GW_SHARE_READ;
}

EGwShare *
e_gw_connection_get_share_sync (EGwConnection *cnc,
				const gchar *folder_id,
				GCancellable *cancellable,
				GError **error)
{
	EGwResponse *response;
	EGwShare *share;
	GString *inner;
	xmlNode *folder, *node;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (folder_id != NULL, NULL);

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "id", folder_id);
	response = e_gw_connection_call_sync (cnc, "getFolder", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return NULL;

	folder = e_gw_xml_find (e_gw_response_get_node (response), "folder");
	if (!folder) {
		e_gw_response_free (response);
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_ITEM_NOT_FOUND, _("The folder does not exist"));
		return NULL;
	}

	share = g_new0 (EGwShare, 1);
	share->id = e_gw_xml_dup_text (folder, "id");
	if (share->id) {
		gchar *id = e_gw_clean_id (share->id);

		g_free (share->id);
		share->id = id;
	} else {
		share->id = g_strdup (folder_id);
	}
	share->name = e_gw_xml_dup_text (folder, "name");
	share->description = e_gw_xml_dup_text (folder, "description");
	share->shared_by_me = e_gw_xml_get_bool (folder, "isSharedByMe");
	share->shared_to_me = e_gw_xml_get_bool (folder, "isSharedToMe");
	share->share_tree = e_gw_xml_get_bool (folder, "flags/shareTree");
	share->owner_name = e_gw_xml_dup_text (folder, "owner/displayName");
	share->owner_email = e_gw_xml_dup_text (folder, "owner/email");
	share->entries = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_share_entry_free);

	node = e_gw_xml_find (folder, "acl");
	for (node = e_gw_xml_first_child (node, "entry"); node; node = e_gw_xml_next_sibling (node, "entry")) {
		EGwShareEntry *entry = g_new0 (EGwShareEntry, 1);
		gchar *status = e_gw_xml_dup_text (node, "status");
		guint ii;

		entry->display_name = e_gw_xml_dup_text (node, "displayName");
		entry->email = e_gw_xml_dup_text (node, "email");
		entry->uuid = e_gw_xml_dup_text (node, "uuid");
		entry->rights = e_gw_share_rights_from_node (node);
		/* An accepted folder lists the user alone, without status */
		entry->status = share->shared_to_me ? E_GW_SHARE_STATUS_ACCEPTED : E_GW_SHARE_STATUS_PENDING;
		for (ii = 0; status && ii < G_N_ELEMENTS (status_map); ii++) {
			if (g_ascii_strcasecmp (status, status_map[ii].name) == 0)
				entry->status = status_map[ii].status;
		}
		g_free (status);
		g_ptr_array_add (share->entries, entry);
	}
	e_gw_response_free (response);

	return share;
}

/* <acl><entry>..</entry>..</acl> of the users in @entries */
static void
add_acl_xml (GString *xml,
	     GPtrArray *entries)
{
	guint ii, jj;

	g_string_append (xml, "<acl>");
	for (ii = 0; ii < entries->len; ii++) {
		EGwShareEntry *entry = entries->pdata[ii];

		g_string_append (xml, "<entry>");
		e_gw_xml_add_leaf (xml, "displayName", entry->display_name ? entry->display_name : entry->email);
		e_gw_xml_add_leaf (xml, "email", entry->email);
		if (entry->uuid && *entry->uuid)
			e_gw_xml_add_leaf (xml, "uuid", entry->uuid);
		g_string_append (xml, "<rights>");
		for (jj = 0; jj < G_N_ELEMENTS (rights_map); jj++) {
			if ((entry->rights | E_GW_SHARE_READ) & rights_map[jj].right)
				e_gw_xml_add_int (xml, rights_map[jj].name, 1);
		}
		g_string_append (xml, "</rights></entry>");
	}
	g_string_append (xml, "</acl>");
}

gboolean
e_gw_connection_modify_share_sync (EGwConnection *cnc,
				   const gchar *folder_id,
				   GPtrArray *added,
				   GPtrArray *changed,
				   GPtrArray *removed,
				   gboolean share_tree,
				   const gchar *subject,
				   const gchar *message,
				   const gchar *description,
				   GCancellable *cancellable,
				   GError **error)
{
	EGwResponse *response;
	GString *inner;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (folder_id != NULL, FALSE);

	if (added && !added->len)
		added = NULL;
	if (changed && !changed->len)
		changed = NULL;
	if (removed && !removed->len)
		removed = NULL;
	if (!added && !changed && !removed)
		return TRUE;

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "id", folder_id);
	if (added) {
		g_string_append (inner, "<notification>");
		if (subject && *subject)
			e_gw_xml_add_leaf (inner, "subject", subject);
		if (message && *message)
			e_gw_xml_add_leaf (inner, "message", message);
		if (description && *description)
			e_gw_xml_add_leaf (inner, "description", description);
		g_string_append (inner, "</notification>");
	}
	g_string_append (inner, "<updates>");
	if (added) {
		g_string_append (inner, "<add>");
		/* The POA takes the flag only here, with the new users */
		if (share_tree)
			g_string_append (inner, "<flags><shareTree>1</shareTree></flags>");
		add_acl_xml (inner, added);
		g_string_append (inner, "</add>");
	}
	if (removed) {
		g_string_append (inner, "<delete>");
		add_acl_xml (inner, removed);
		g_string_append (inner, "</delete>");
	}
	if (changed) {
		g_string_append (inner, "<update>");
		add_acl_xml (inner, changed);
		g_string_append (inner, "</update>");
	}
	g_string_append (inner, "</updates>");

	response = e_gw_connection_call_sync (cnc, "modifyItem", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	e_gw_response_free (response);

	return TRUE;
}

void
e_gw_share_notification_free (EGwShareNotification *notification)
{
	if (!notification)
		return;

	g_free (notification->id);
	g_free (notification->folder_name);
	g_free (notification->subject);
	g_free (notification->sender_name);
	g_free (notification->sender_email);
	g_free (notification->message);
	g_free (notification->description);
	g_free (notification);
}

gboolean
e_gw_share_is_notification_id (const gchar *id)
{
	const gchar *at = id ? strchr (id, '@') : NULL;

	/* BASE@37 or BASE@37:CONTAINER */
	return at && g_str_has_prefix (at, "@37") && (at[3] == '\0' || at[3] == ':');
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

EGwShareNotification *
e_gw_connection_get_share_notification_sync (EGwConnection *cnc,
					     const gchar *id,
					     GCancellable *cancellable,
					     GError **error)
{
	EGwShareNotification *notification;
	EGwResponse *response;
	xmlNode *item;
	gchar *kind;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (id != NULL, NULL);

	response = e_gw_connection_get_item_sync (cnc, id, "default peek message", cancellable, error);
	if (!response)
		return NULL;

	item = e_gw_xml_find (e_gw_response_get_node (response), "item");
	kind = item ? e_gw_xml_dup_text (item, "notification") : NULL;
	if (g_strcmp0 (kind, "SharedFolder") != 0) {
		g_free (kind);
		e_gw_response_free (response);
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_ITEM_NOT_FOUND,
			_("The message does not offer a shared folder"));
		return NULL;
	}
	g_free (kind);

	notification = g_new0 (EGwShareNotification, 1);
	notification->id = g_strdup (id);
	notification->folder_name = e_gw_xml_dup_text (item, "name");
	notification->subject = e_gw_xml_dup_text (item, "subject");
	notification->sender_name = e_gw_xml_dup_text (item, "distribution/from/displayName");
	notification->sender_email = e_gw_xml_dup_text (item, "distribution/from/email");
	notification->message = item_text (item);
	notification->description = e_gw_xml_dup_text (item, "description");
	notification->rights = e_gw_share_rights_from_node (item) | E_GW_SHARE_READ;
	e_gw_response_free (response);

	return notification;
}

gchar *
e_gw_connection_accept_share_sync (EGwConnection *cnc,
				   const gchar *notification_id,
				   const gchar *name,
				   const gchar *description,
				   const gchar *container,
				   GCancellable *cancellable,
				   GError **error)
{
	EGwResponse *response;
	GString *inner;
	gchar *id, *clean;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (notification_id != NULL, NULL);

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "id", notification_id);
	if (name && *name)
		e_gw_xml_add_leaf (inner, "name", name);
	if (container && *container)
		e_gw_xml_add_leaf (inner, "container", container);
	if (description && *description)
		e_gw_xml_add_leaf (inner, "description", description);
	response = e_gw_connection_call_sync (cnc, "acceptShare", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return NULL;

	id = e_gw_xml_dup_text (e_gw_response_get_node (response), "id");
	e_gw_response_free (response);
	if (!id || !*id) {
		g_free (id);
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, _("The server did not name the new folder"));
		return NULL;
	}
	clean = e_gw_clean_id (id);
	g_free (id);

	return clean;
}

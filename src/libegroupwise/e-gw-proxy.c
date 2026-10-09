/*
 * e-gw-proxy.c: the proxy access to the user's mailbox
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

#include <glib/gi18n-lib.h>

#include "e-gw-proxy.h"
#include "e-gw-xml.h"

/* The rights as elements of an AccessRightEntry, in the order of the schema */
static const struct {
	const gchar *group;
	const gchar *name;
	EGwProxyRights right;
} rights_map[] = {
	{ "appointment", "read", E_GW_PROXY_APPOINTMENT_READ },
	{ "appointment", "write", E_GW_PROXY_APPOINTMENT_WRITE },
	{ "mail", "read", E_GW_PROXY_MAIL_READ },
	{ "mail", "write", E_GW_PROXY_MAIL_WRITE },
	{ "misc", "alarms", E_GW_PROXY_ALARMS },
	{ "misc", "notify", E_GW_PROXY_NOTIFY },
	{ "misc", "readHidden", E_GW_PROXY_READ_HIDDEN },
	{ "misc", "setup", E_GW_PROXY_SETUP },
	{ "misc", "setupSecurity", E_GW_PROXY_SETUP_SECURITY },
	{ "note", "read", E_GW_PROXY_NOTE_READ },
	{ "note", "write", E_GW_PROXY_NOTE_WRITE },
	{ "task", "read", E_GW_PROXY_TASK_READ },
	{ "task", "write", E_GW_PROXY_TASK_WRITE }
};

EGwProxyRights
e_gw_proxy_rights_from_entry (xmlNode *entry)
{
	EGwProxyRights rights = 0;
	guint ii;

	for (ii = 0; entry && ii < G_N_ELEMENTS (rights_map); ii++) {
		gchar *path = g_strconcat (rights_map[ii].group, "/", rights_map[ii].name, NULL);

		if (e_gw_xml_get_bool (entry, path))
			rights |= rights_map[ii].right;
		g_free (path);
	}

	return rights;
}

/* <appointment><read>1</read>...</appointment>... for the rights set */
static void
add_rights_xml (GString *xml,
		EGwProxyRights rights)
{
	const gchar *open = NULL;
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (rights_map); ii++) {
		if (!(rights & rights_map[ii].right))
			continue;
		if (g_strcmp0 (open, rights_map[ii].group) != 0) {
			if (open)
				g_string_append_printf (xml, "</%s>", open);
			open = rights_map[ii].group;
			g_string_append_printf (xml, "<%s>", open);
		}
		g_string_append_printf (xml, "<%s>1</%s>", rights_map[ii].name, rights_map[ii].name);
	}
	if (open)
		g_string_append_printf (xml, "</%s>", open);
}

void
e_gw_proxy_access_free (EGwProxyAccess *access)
{
	if (!access)
		return;

	g_free (access->id);
	g_free (access->uuid);
	g_free (access->email);
	g_free (access->display_name);
	g_free (access);
}

gboolean
e_gw_proxy_access_is_all_users (const EGwProxyAccess *access)
{
	return access && g_strcmp0 (access->uuid, E_GW_PROXY_ALL_USERS) == 0;
}

GPtrArray *
e_gw_connection_get_proxy_access_list_sync (EGwConnection *cnc,
					    GCancellable *cancellable,
					    GError **error)
{
	EGwResponse *response;
	GPtrArray *list;
	xmlNode *node;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	response = e_gw_connection_call_sync (cnc, "getProxyAccessList", NULL, cancellable, error);
	if (!response)
		return NULL;

	list = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_proxy_access_free);
	node = e_gw_xml_find (e_gw_response_get_node (response), "accessRights");
	for (node = e_gw_xml_first_child (node, "entry"); node; node = e_gw_xml_next_sibling (node, "entry")) {
		EGwProxyAccess *access = g_new0 (EGwProxyAccess, 1);

		access->id = e_gw_xml_dup_text (node, "id");
		access->uuid = e_gw_xml_dup_text (node, "uuid");
		access->email = e_gw_xml_dup_text (node, "email");
		access->display_name = e_gw_xml_dup_text (node, "displayName");
		access->rights = e_gw_proxy_rights_from_entry (node);
		if (access->id && *access->id)
			g_ptr_array_add (list, access);
		else
			e_gw_proxy_access_free (access);
	}
	e_gw_response_free (response);

	return list;
}

/* The GroupWise user of an e-mail address or a name (resolveRequest):
 * the POA grants rights only with the UUID */
static gboolean
resolve_user (EGwConnection *cnc,
	      const gchar *who,
	      gchar **out_uuid,
	      gchar **out_email,
	      gchar **out_display_name,
	      GCancellable *cancellable,
	      GError **error)
{
	EGwResponse *response;
	GString *inner;
	xmlNode *recipient;

	inner = g_string_new ("<recipients><recipient>");
	e_gw_xml_add_leaf (inner, strchr (who, '@') ? "email" : "displayName", who);
	g_string_append (inner, "</recipient></recipients>");
	response = e_gw_connection_call_sync (cnc, "resolve", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	recipient = e_gw_xml_find (e_gw_response_get_node (response), "recipients/recipient");
	*out_uuid = recipient ? e_gw_xml_dup_text (recipient, "uuid") : NULL;
	*out_email = recipient ? e_gw_xml_dup_text (recipient, "email") : NULL;
	*out_display_name = recipient ? e_gw_xml_dup_text (recipient, "displayName") : NULL;
	e_gw_response_free (response);

	if (!*out_uuid || !**out_uuid) {
		g_clear_pointer (out_uuid, g_free);
		g_clear_pointer (out_email, g_free);
		g_clear_pointer (out_display_name, g_free);
		g_set_error (error, E_GW_ERROR, E_GW_ERROR_UNKNOWN_USER, _("No GroupWise user “%s”"), who);
		return FALSE;
	}

	return TRUE;
}

gchar *
e_gw_connection_create_proxy_access_sync (EGwConnection *cnc,
					  const gchar *who,
					  EGwProxyRights rights,
					  GCancellable *cancellable,
					  GError **error)
{
	EGwResponse *response;
	GString *inner;
	gchar *uuid = NULL, *email = NULL, *display_name = NULL, *id;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (who != NULL, NULL);

	if (!resolve_user (cnc, who, &uuid, &email, &display_name, cancellable, error))
		return NULL;

	inner = g_string_new ("<entry>");
	e_gw_xml_add_leaf (inner, "displayName", display_name);
	e_gw_xml_add_leaf (inner, "email", email);
	e_gw_xml_add_leaf (inner, "uuid", uuid);
	add_rights_xml (inner, rights);
	g_string_append (inner, "</entry>");
	response = e_gw_connection_call_sync (cnc, "createProxyAccess", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	g_free (display_name);
	g_free (email);
	if (!response) {
		g_free (uuid);
		return NULL;
	}

	id = e_gw_xml_dup_text (e_gw_response_get_node (response), "id");
	e_gw_response_free (response);
	if (!id || !*id) {
		g_free (id);
		id = g_strconcat (uuid, "@60", NULL);
	}
	g_free (uuid);

	return id;
}

gboolean
e_gw_connection_modify_proxy_access_sync (EGwConnection *cnc,
					  const gchar *id,
					  EGwProxyRights old_rights,
					  EGwProxyRights new_rights,
					  GCancellable *cancellable,
					  GError **error)
{
	EGwProxyRights added = new_rights & ~old_rights, removed = old_rights & ~new_rights;
	EGwResponse *response;
	GString *inner;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (id != NULL, FALSE);

	if (!added && !removed)
		return TRUE;

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "id", id);
	g_string_append (inner, "<updates>");
	if (added) {
		g_string_append (inner, "<add>");
		add_rights_xml (inner, added);
		g_string_append (inner, "</add>");
	}
	if (removed) {
		g_string_append (inner, "<delete>");
		add_rights_xml (inner, removed);
		g_string_append (inner, "</delete>");
	}
	g_string_append (inner, "</updates>");
	response = e_gw_connection_call_sync (cnc, "modifyProxyAccess", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	e_gw_response_free (response);

	return TRUE;
}

gboolean
e_gw_connection_remove_proxy_access_sync (EGwConnection *cnc,
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
	response = e_gw_connection_call_sync (cnc, "removeProxyAccess", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	e_gw_response_free (response);

	return TRUE;
}

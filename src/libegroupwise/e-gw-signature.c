/*
 * e-gw-signature.c: the signatures of the user
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

#include "e-gw-signature.h"
#include "e-gw-xml.h"

void
e_gw_signature_free (EGwSignature *signature)
{
	if (!signature)
		return;

	g_free (signature->id);
	g_free (signature->name);
	g_clear_pointer (&signature->mime, g_bytes_unref);
	g_free (signature);
}

void
e_gw_signatures_free (EGwSignatures *signatures)
{
	if (!signatures)
		return;

	g_clear_pointer (&signatures->signatures, g_ptr_array_unref);
	g_free (signatures);
}

EGwSignatures *
e_gw_connection_get_signatures_sync (EGwConnection *cnc,
				     GCancellable *cancellable,
				     GError **error)
{
	EGwResponse *response;
	EGwSignatures *signatures;
	xmlNode *list, *node;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);

	response = e_gw_connection_call_sync (cnc, "getSignatures", NULL, cancellable, error);
	if (!response)
		return NULL;

	signatures = g_new0 (EGwSignatures, 1);
	signatures->signatures = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_signature_free);
	list = e_gw_xml_find (e_gw_response_get_node (response), "signatures");
	for (node = e_gw_xml_first_child (list, "signature"); node; node = e_gw_xml_next_sibling (node, "signature")) {
		EGwSignature *signature = g_new0 (EGwSignature, 1);
		gchar *data = e_gw_xml_dup_text (node, "part/data");

		signature->id = e_gw_xml_dup_text (node, "id");
		signature->name = e_gw_xml_dup_text (node, "name");
		signature->is_default = e_gw_xml_get_bool (node, "default");
		if (data) {
			gsize len = 0;
			guchar *decoded;

			/* The data comes in lines */
			g_strdelimit (data, "\r\n\t ", ' ');
			decoded = g_base64_decode (g_strstrip (data), &len);
			signature->mime = g_bytes_new_take (decoded, len);
			g_free (data);
		}
		if (signature->id && *signature->id)
			g_ptr_array_add (signatures->signatures, signature);
		else
			e_gw_signature_free (signature);
	}
	signatures->enabled = TRUE;
	node = e_gw_xml_find (list, "setting");
	if (node) {
		gchar *add = e_gw_xml_dup_text (node, "add");

		signatures->enabled = e_gw_xml_get_bool (node, "enabled");
		signatures->automatic = g_strcmp0 (add, "Automatic") == 0;
		g_free (add);
	}
	e_gw_response_free (response);

	return signatures;
}

static void
add_signature_xml (GString *xml,
		   const gchar *id,
		   const gchar *name,
		   GBytes *mime,
		   gint is_default)
{
	g_string_append (xml, "<signature>");
	if (id)
		e_gw_xml_add_leaf (xml, "id", id);
	if (name)
		e_gw_xml_add_leaf (xml, "name", name);
	if (is_default >= 0)
		e_gw_xml_add_bool (xml, "default", is_default > 0);
	if (mime) {
		gsize len;
		gconstpointer data = g_bytes_get_data (mime, &len);
		gchar *encoded = g_base64_encode (data, len);

		g_string_append_printf (xml, "<part><data>%s</data></part>", encoded);
		g_free (encoded);
	}
	g_string_append (xml, "</signature>");
}

gchar *
e_gw_connection_create_signature_sync (EGwConnection *cnc,
				       const gchar *name,
				       GBytes *mime,
				       gboolean is_default,
				       GCancellable *cancellable,
				       GError **error)
{
	EGwResponse *response;
	GString *inner;
	gchar *id;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (name != NULL, NULL);
	g_return_val_if_fail (mime != NULL, NULL);

	inner = g_string_new (NULL);
	add_signature_xml (inner, NULL, name, mime, is_default ? 1 : 0);
	response = e_gw_connection_call_sync (cnc, "createSignature", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return NULL;

	id = e_gw_xml_dup_text (e_gw_response_get_node (response), "id");
	e_gw_response_free (response);
	if (!id || !*id) {
		g_free (id);
		g_set_error_literal (error, E_GW_ERROR, E_GW_ERROR_XML, "createSignature: no ID");
		return NULL;
	}

	return id;
}

gboolean
e_gw_connection_modify_signature_sync (EGwConnection *cnc,
				       const gchar *id,
				       const gchar *name,
				       GBytes *mime,
				       gint is_default,
				       GCancellable *cancellable,
				       GError **error)
{
	EGwResponse *response;
	GString *inner;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (id != NULL, FALSE);

	inner = g_string_new ("<updates>");
	add_signature_xml (inner, id, name, mime, is_default);
	g_string_append (inner, "</updates>");
	response = e_gw_connection_call_sync (cnc, "modifySignatures", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	e_gw_response_free (response);

	return TRUE;
}

gboolean
e_gw_connection_remove_signature_sync (EGwConnection *cnc,
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
	e_gw_xml_add_bool (inner, "all", FALSE);
	e_gw_xml_add_bool (inner, "global", FALSE);
	response = e_gw_connection_call_sync (cnc, "removeSignature", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	e_gw_response_free (response);

	return TRUE;
}

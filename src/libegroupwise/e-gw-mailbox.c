/*
 * e-gw-mailbox.c: the mailbox itself: its size and its password
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

#include "e-gw-mailbox.h"
#include "e-gw-xml.h"

gboolean
e_gw_connection_get_disk_usage_sync (EGwConnection *cnc,
				     EGwDiskUsage *out_usage,
				     GCancellable *cancellable,
				     GError **error)
{
	EGwResponse *response;
	xmlNode *usage;
	gchar *in_mb;
	gint64 divisor;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (out_usage != NULL, FALSE);

	response = e_gw_connection_call_sync (cnc, "getDiskSpaceUsage", NULL, cancellable, error);
	if (!response)
		return FALSE;

	usage = e_gw_xml_find (e_gw_response_get_node (response), "diskUsage");
	/* Bytes from servers before GroupWise 8 SP1 */
	in_mb = usage ? e_gw_xml_dup_attr (usage, "inMB") : NULL;
	divisor = g_strcmp0 (in_mb, "1") == 0 || g_strcmp0 (in_mb, "true") == 0 ? 1 : 1024 * 1024;
	g_free (in_mb);

	out_usage->used = usage ? e_gw_xml_get_int (usage, "used", 0) / divisor : 0;
	out_usage->max_size = usage ? e_gw_xml_get_int (usage, "maxSize", 0) / divisor : 0;
	out_usage->threshold = usage ? (gint) e_gw_xml_get_int (usage, "threshold", 0) : 0;
	e_gw_response_free (response);

	return TRUE;
}

gboolean
e_gw_connection_modify_password_sync (EGwConnection *cnc,
				      const gchar *old_password,
				      const gchar *new_password,
				      GCancellable *cancellable,
				      GError **error)
{
	EGwResponse *response;
	GString *inner;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (old_password != NULL, FALSE);
	g_return_val_if_fail (new_password != NULL, FALSE);

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "old", old_password);
	e_gw_xml_add_leaf (inner, "new", new_password);
	response = e_gw_connection_call_sync (cnc, "modifyPassword", inner->str, cancellable, error);
	/* Not left lying in memory */
	memset (inner->str, 0, inner->len);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	e_gw_response_free (response);

	return TRUE;
}

gchar *
e_gw_connection_send_phone_message_sync (EGwConnection *cnc,
					 const gchar * const *names,
					 const gchar * const *emails,
					 const gchar *caller,
					 const gchar *company,
					 const gchar *phone,
					 EGwPhoneFlags flags,
					 const gchar *text,
					 GCancellable *cancellable,
					 GError **error)
{
	static const struct {
		EGwPhoneFlags flag;
		const gchar *name;
	} flag_names[] = {
		{ E_GW_PHONE_CALLED, "called" },
		{ E_GW_PHONE_PLEASE_CALL, "pleaseCall" },
		{ E_GW_PHONE_WILL_CALL, "willCall" },
		{ E_GW_PHONE_RETURNED_CALL, "returnedYourCall" },
		{ E_GW_PHONE_WANTS_TO_SEE_YOU, "wantsToSeeYou" },
		{ E_GW_PHONE_CAME_TO_SEE_YOU, "cameToSeeYou" },
		{ E_GW_PHONE_URGENT, "urgent" }
	};
	GString *xml;
	gchar *subject, *id;
	guint ii;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (emails != NULL && emails[0] != NULL, NULL);

	xml = g_string_new ("<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"PhoneMessage\">");
	subject = g_strstrip (g_strconcat (caller ? caller : "", " ", phone ? phone : "", NULL));
	e_gw_xml_add_leaf (xml, "subject", subject);
	g_free (subject);

	g_string_append (xml, "<distribution><recipients>");
	for (ii = 0; emails[ii]; ii++) {
		g_string_append (xml, "<recipient>");
		e_gw_xml_add_leaf (xml, "displayName", names && names[ii] && *names[ii] ? names[ii] : emails[ii]);
		e_gw_xml_add_leaf (xml, "email", emails[ii]);
		e_gw_xml_add_leaf (xml, "distType", "TO");
		g_string_append (xml, "</recipient>");
	}
	g_string_append (xml, "</recipients></distribution>");

	if (text && *text) {
		gchar *encoded = g_base64_encode ((const guchar *) text, strlen (text));

		g_string_append_printf (xml, "<message><part contentType=\"text/plain\">%s</part></message>", encoded);
		g_free (encoded);
	}
	if (caller && *caller)
		e_gw_xml_add_leaf (xml, "caller", caller);
	if (company && *company)
		e_gw_xml_add_leaf (xml, "company", company);
	if (phone && *phone)
		e_gw_xml_add_leaf (xml, "phone", phone);
	g_string_append (xml, "<flags>");
	for (ii = 0; ii < G_N_ELEMENTS (flag_names); ii++) {
		if (flags & flag_names[ii].flag)
			e_gw_xml_add_int (xml, flag_names[ii].name, 1);
	}
	g_string_append (xml, "</flags></item>");

	id = e_gw_connection_send_item_sync (cnc, xml->str, cancellable, error);
	g_string_free (xml, TRUE);

	return id;
}

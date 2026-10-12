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

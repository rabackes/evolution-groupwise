/*
 * e-gw-events.c: what happened in a mailbox (GroupWise Web Services Events)
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

#include "e-gw-events.h"
#include "e-gw-xml.h"

void
e_gw_event_free (EGwEvent *event)
{
	if (!event)
		return;

	g_free (event->type);
	g_free (event->item);
	g_free (event->container);
	g_free (event->from);
	g_free (event);
}

static gboolean
call (EGwConnection *cnc,
      const gchar *action,
      GString *inner,
      GCancellable *cancellable,
      GError **error)
{
	EGwResponse *response = e_gw_connection_call_sync (cnc, action, inner->str, cancellable, error);

	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;
	e_gw_response_free (response);

	return TRUE;
}

gboolean
e_gw_connection_configure_events_sync (EGwConnection *cnc,
				       const gchar *key,
				       const gchar * const *events,
				       guint persistence_days,
				       const gchar *address,
				       guint port,
				       GCancellable *cancellable,
				       GError **error)
{
	GString *inner;
	guint ii;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (key != NULL && events != NULL, FALSE);

	inner = g_string_new ("<events enabled=\"1\">");
	e_gw_xml_add_leaf (inner, "key", key);
	e_gw_xml_add_int (inner, "persistence", MIN (persistence_days, 20));
	if (address && *address && port) {
		e_gw_xml_add_leaf (inner, "ipAddress", address);
		e_gw_xml_add_int (inner, "port", port);
	}
	/* Each type an element of its own: a plain list is taken, and
	 * nothing recorded */
	g_string_append (inner, "<definition><events>");
	for (ii = 0; events[ii]; ii++)
		e_gw_xml_add_leaf (inner, "event", events[ii]);
	g_string_append (inner, "</events></definition></events>");

	return call (cnc, "configureEvents", inner, cancellable, error);
}

gboolean
e_gw_connection_has_events_sync (EGwConnection *cnc,
				 const gchar *key,
				 const gchar * const *events,
				 const gchar *address,
				 guint port,
				 GCancellable *cancellable)
{
	EGwResponse *response;
	GString *inner;
	xmlNode *config, *node;
	gboolean has = FALSE;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (key != NULL && events != NULL, FALSE);

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "key", key);
	response = e_gw_connection_call_sync (cnc, "getEventConfiguration", inner->str, cancellable, NULL);
	g_string_free (inner, TRUE);
	if (!response)
		return FALSE;

	/* <events><event enabled="1"><key/><ipAddress/><port/>
	 *   <definition><events><event>Type</event>…</events></definition></event></events> */
	for (config = e_gw_xml_first_child (e_gw_xml_find (e_gw_response_get_node (response), "events"), "event");
	     config && !has; config = e_gw_xml_next_sibling (config, "event")) {
		gchar *config_key = e_gw_xml_dup_text (config, "key");
		gchar *enabled = e_gw_xml_dup_attr (config, "enabled");

		if (g_strcmp0 (config_key, key) == 0 && g_strcmp0 (enabled, "0") != 0) {
			GHashTable *types = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
			guint ii;

			for (node = e_gw_xml_first_child (e_gw_xml_find (config, "definition/events"), "event"); node;
			     node = e_gw_xml_next_sibling (node, "event")) {
				gchar *type = e_gw_xml_dup_text (node, NULL);

				if (type)
					g_hash_table_add (types, type);
			}
			has = g_hash_table_size (types) > 0;
			for (ii = 0; events[ii] && has; ii++)
				has = g_hash_table_contains (types, events[ii]);
			g_hash_table_destroy (types);
			/* ... and tells there, or nowhere */
			if (has) {
				gchar *config_address = e_gw_xml_dup_text (config, "ipAddress");
				guint config_port = (guint) e_gw_xml_get_int (config, "port", 0);
				gboolean wanted = address && *address && port;
				gboolean tells = config_address && *config_address && config_port;

				has = wanted ? tells && g_strcmp0 (config_address, address) == 0 && config_port == port : !tells;
				g_free (config_address);
			}
		}
		g_free (config_key);
		g_free (enabled);
	}
	e_gw_response_free (response);

	return has;
}

GPtrArray *
e_gw_connection_get_events_sync (EGwConnection *cnc,
				 const gchar *key,
				 gboolean remove,
				 gboolean notify,
				 GCancellable *cancellable,
				 GError **error)
{
	EGwResponse *response;
	GPtrArray *events;
	GString *inner;
	xmlNode *node;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), NULL);
	g_return_val_if_fail (key != NULL, NULL);

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "key", key);
	if (remove)
		e_gw_xml_add_leaf (inner, "remove", "1");
	if (notify)
		e_gw_xml_add_leaf (inner, "notify", "1");
	response = e_gw_connection_call_sync (cnc, "getEvents", inner->str, cancellable, error);
	g_string_free (inner, TRUE);
	if (!response)
		return NULL;

	events = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_event_free);
	/* <events><event><event>Type</event><id/><container/>…</event>…</events> */
	for (node = e_gw_xml_first_child (e_gw_xml_find (e_gw_response_get_node (response), "events"), "event"); node;
	     node = e_gw_xml_next_sibling (node, "event")) {
		EGwEvent *event = g_new0 (EGwEvent, 1);
		gchar *text;

		event->type = e_gw_xml_dup_text (node, "event");
		text = e_gw_xml_dup_text (node, "id");
		event->item = text && *text ? e_gw_clean_id (text) : NULL;
		g_free (text);
		text = e_gw_xml_dup_text (node, "container");
		event->container = text && *text ? e_gw_clean_id (text) : NULL;
		g_free (text);
		text = e_gw_xml_dup_text (node, "from");
		event->from = text && *text ? e_gw_clean_id (text) : NULL;
		g_free (text);

		if (event->type && event->item)
			g_ptr_array_add (events, event);
		else
			e_gw_event_free (event);
	}
	e_gw_response_free (response);

	return events;
}

gboolean
e_gw_connection_remove_events_sync (EGwConnection *cnc,
				    const gchar *key,
				    GCancellable *cancellable,
				    GError **error)
{
	GString *inner;

	g_return_val_if_fail (E_IS_GW_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (key != NULL, FALSE);

	/* The records first: they would stay without their configuration */
	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "key", key);
	call (cnc, "removeEvents", inner, cancellable, NULL);

	inner = g_string_new (NULL);
	e_gw_xml_add_leaf (inner, "key", key);

	return call (cnc, "removeEventConfiguration", inner, cancellable, error);
}

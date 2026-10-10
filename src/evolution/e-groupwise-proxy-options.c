/*
 * e-groupwise-proxy-options.c: options of the main account for its proxy accounts
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
 * A proxy account is the user's own login into another mailbox. Options
 * that say how Evolution works with the server, not with that mailbox, are
 * set once: what the main account has for the events of the mailbox
 * (asked for or not, how often) goes to its proxy accounts, when the main
 * account changes and when a proxy account is made.
 */

#include <libedataserver/libedataserver.h>

#include "camel-groupwise-settings.h"
#include "e-groupwise-proxy-options.h"

/* The GroupWise settings of a source that has them (its collection keeps
 * them), not referenced */
static CamelGroupwiseSettings *
get_settings (ESource *source)
{
	const gchar *extension_name = e_source_camel_get_extension_name ("groupwise");
	CamelSettings *settings;

	if (!e_source_has_extension (source, extension_name))
		return NULL;
	settings = e_source_camel_get_settings (e_source_get_extension (source, extension_name));

	return CAMEL_IS_GROUPWISE_SETTINGS (settings) ? CAMEL_GROUPWISE_SETTINGS (settings) : NULL;
}

static gboolean
is_proxy (CamelGroupwiseSettings *settings)
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

/* The options of @main for the proxy account @proxy; written when they differ */
static void
copy_options (ESource *main_source,
	      ESource *proxy_source)
{
	CamelGroupwiseSettings *from = get_settings (main_source), *to = get_settings (proxy_source);

	if (camel_groupwise_settings_get_use_events_interval (from) == camel_groupwise_settings_get_use_events_interval (to) &&
	    camel_groupwise_settings_get_events_interval (from) == camel_groupwise_settings_get_events_interval (to))
		return;

	g_debug ("proxy options: %s takes the events options of %s", e_source_get_display_name (proxy_source),
		e_source_get_display_name (main_source));
	camel_groupwise_settings_set_use_events_interval (to, camel_groupwise_settings_get_use_events_interval (from));
	camel_groupwise_settings_set_events_interval (to, camel_groupwise_settings_get_events_interval (from));
	if (e_source_get_writable (proxy_source))
		e_source_write (proxy_source, NULL, NULL, NULL);
}

/* @source changed or came: a main account gives to its proxy accounts, a
 * proxy account takes from its main account */
static void
source_changed_cb (ESourceRegistry *registry,
		   ESource *source,
		   gpointer user_data)
{
	CamelGroupwiseSettings *settings = get_settings (source), *other;
	gboolean proxy;
	GList *sources, *link;

	if (!settings)
		return;

	proxy = is_proxy (settings);
	sources = e_source_registry_list_sources (registry, e_source_camel_get_extension_name ("groupwise"));
	for (link = sources; link; link = g_list_next (link)) {
		other = link->data != (gpointer) source ? get_settings (link->data) : NULL;
		if (!other || is_proxy (other) == proxy || !same_login (settings, other))
			continue;
		if (proxy) {
			copy_options (link->data, source);
			break;
		}
		copy_options (source, link->data);
	}
	g_list_free_full (sources, g_object_unref);
}

void
e_groupwise_proxy_options_start (ESourceRegistry *registry)
{
	static gboolean started;
	GList *sources, *link;

	g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));

	if (started)
		return;
	started = TRUE;

	/* What changed while Evolution was not running */
	sources = e_source_registry_list_sources (registry, e_source_camel_get_extension_name ("groupwise"));
	for (link = sources; link; link = g_list_next (link)) {
		CamelGroupwiseSettings *settings = get_settings (link->data);

		if (settings && !is_proxy (settings))
			source_changed_cb (registry, link->data, NULL);
	}
	g_list_free_full (sources, g_object_unref);

	g_signal_connect (registry, "source-added", G_CALLBACK (source_changed_cb), NULL);
	g_signal_connect (registry, "source-changed", G_CALLBACK (source_changed_cb), NULL);
}

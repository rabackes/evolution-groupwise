/*
 * camel-groupwise-provider.c: registers the "groupwise" Camel provider
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

#include <glib/gi18n-lib.h>

#include "camel-groupwise-settings.h"
#include "camel-groupwise-store.h"
#include "camel-groupwise-transport.h"

static CamelProviderConfEntry groupwise_conf_entries[] = {
	{ CAMEL_PROVIDER_CONF_SECTION_START, "mailcheck", NULL, N_("Checking for New Mail") },
	{ CAMEL_PROVIDER_CONF_CHECKBOX, "filter-inbox", NULL,
	  N_("_Apply filters and the junk test to new messages in the Mailbox"), "0" },
	{ CAMEL_PROVIDER_CONF_SECTION_END },
	{ CAMEL_PROVIDER_CONF_SECTION_START, "general", NULL, N_("Options") },
	{ CAMEL_PROVIDER_CONF_CHECKBOX, "stay-synchronized", NULL,
	  N_("Copy folder content locally for _offline operation"), "0" },
	{ CAMEL_PROVIDER_CONF_CHECKBOX, "read-only", NULL,
	  N_("_Read-only: never change anything on the server"), "0" },
	{ CAMEL_PROVIDER_CONF_SECTION_END },
	{ CAMEL_PROVIDER_CONF_SECTION_START, "rules", NULL, N_("GroupWise Rules") },
	{ CAMEL_PROVIDER_CONF_CHECKBOX, "run-startup-rules", NULL,
	  N_("Run the rules for _startup and exit when Evolution starts and quits"), "0" },
	{ CAMEL_PROVIDER_CONF_CHECKBOX, "run-folder-rules", NULL,
	  N_("Run the rules for opening and closing _folders"), "0" },
	{ CAMEL_PROVIDER_CONF_SECTION_END },
	{ CAMEL_PROVIDER_CONF_END }
};

static CamelProviderPortEntry groupwise_port_entries[] = {
	{ CAMEL_GROUPWISE_DEFAULT_PORT, N_("Default GroupWise SOAP port"), FALSE },
	{ 0, NULL, 0 }
};

static CamelProvider groupwise_provider = {
	"groupwise",
	N_("GroupWise"),

	N_("For accessing GroupWise mailboxes through the SOAP interface of the Post Office Agent."),

	"mail",

	/* GroupWise keeps sent items itself */
	CAMEL_PROVIDER_IS_REMOTE | CAMEL_PROVIDER_IS_SOURCE |
	CAMEL_PROVIDER_IS_STORAGE | CAMEL_PROVIDER_SUPPORTS_SSL |
	CAMEL_PROVIDER_DISABLE_SENT_FOLDER,

	CAMEL_URL_NEED_USER | CAMEL_URL_NEED_HOST | CAMEL_URL_ALLOW_PORT | CAMEL_URL_ALLOW_AUTH,

	groupwise_conf_entries,

	groupwise_port_entries,

	/* ... */
};

static void
add_hash (guint *hash,
	  const gchar *s)
{
	if (s && *s)
		*hash ^= g_str_hash (s);
}

static guint
groupwise_url_hash (gconstpointer key)
{
	const CamelURL *u = key;
	guint hash = 0;

	add_hash (&hash, u->user);
	add_hash (&hash, u->host);
	hash ^= u->port;

	return hash;
}

static gboolean
groupwise_url_equal (gconstpointer a,
		     gconstpointer b)
{
	const CamelURL *u1 = a, *u2 = b;

	return g_strcmp0 (u1->protocol, u2->protocol) == 0
		&& g_strcmp0 (u1->user, u2->user) == 0
		&& g_strcmp0 (u1->host, u2->host) == 0
		&& u1->port == u2->port;
}

void
camel_provider_module_init (void)
{
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

	groupwise_provider.object_types[CAMEL_PROVIDER_STORE] = CAMEL_TYPE_GROUPWISE_STORE;
	/* Only together with the store of the same account */
	groupwise_provider.object_types[CAMEL_PROVIDER_TRANSPORT] = CAMEL_TYPE_GROUPWISE_TRANSPORT;
	groupwise_provider.url_hash = groupwise_url_hash;
	groupwise_provider.url_equal = groupwise_url_equal;
	groupwise_provider.authtypes = g_list_prepend (NULL, &camel_groupwise_password_authtype);
	groupwise_provider.translation_domain = GETTEXT_PACKAGE;

	camel_provider_register (&groupwise_provider);
}

/*
 * module-groupwise-backend.c: the GroupWise account in the source registry
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
 * A GroupWise account is a collection: one login, one password. The
 * collection backend reads the address books of the account after each
 * login and keeps an address book source per GroupWise address book, and
 * for the calendar folder a calendar, a task list and a memo list (GroupWise
 * keeps appointments, tasks and notes together). The children get the
 * password of the collection. The signatures of the mailbox become
 * signatures of Evolution (gw-signatures.c).
 *
 * A proxy account is a collection of its own whose settings name the other
 * user (Proxy): it logs in as proxy with the user's password and shows the
 * other mailbox — its mail, Calendar and own subcalendars, task and memo
 * lists, personal address books. Its calendars start unselected. The
 * user's collection then leaves out the proxy calendars of that user.
 */

#include <string.h>

#include <glib/gi18n-lib.h>
#include <libebackend/libebackend.h>

#include "e-gw-addressbook.h"
#include "e-gw-folder.h"
#include "e-gw-backend-utils.h"
#include "e-source-groupwise-folder.h"
#include "gw-senders.h"
#include "gw-signatures.h"

typedef ECollectionBackend EGwCollectionBackend;
typedef ECollectionBackendClass EGwCollectionBackendClass;
typedef ECollectionBackendFactory EGwCollectionBackendFactory;
typedef ECollectionBackendFactoryClass EGwCollectionBackendFactoryClass;

GType e_gw_collection_backend_get_type (void);
GType e_gw_collection_backend_factory_get_type (void);
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

G_DEFINE_DYNAMIC_TYPE (EGwCollectionBackend, e_gw_collection_backend, E_TYPE_COLLECTION_BACKEND)
G_DEFINE_DYNAMIC_TYPE (EGwCollectionBackendFactory, e_gw_collection_backend_factory, E_TYPE_COLLECTION_BACKEND_FACTORY)

static void ensure_trust_extension (ESource *source);

/* The user whose mailbox a collection shows as proxy, NULL for the own one */
static gchar *
dup_collection_proxy (ESource *collection)
{
	CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (NULL, collection);
	gchar *proxy = settings ? camel_groupwise_settings_dup_proxy (settings) : NULL;

	g_clear_object (&settings);

	return proxy;
}

static gboolean
backend_is_proxy (ECollectionBackend *backend)
{
	gchar *proxy = dup_collection_proxy (e_backend_get_source (E_BACKEND (backend)));
	gboolean is_proxy = proxy != NULL;

	g_free (proxy);

	return is_proxy;
}

static gchar *
child_folder_id (ESource *child)
{
	if (!e_source_has_extension (child, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
		return NULL;

	return e_source_groupwise_folder_dup_id (e_source_get_extension (child, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
}

/* The user's password from the last login, for what the user does to the
 * calendars and address books later (make, rename, delete one) */
#define DATA_CREDENTIALS "groupwise-credentials"
/* UID -> the name the source got from the server: another name is the user's */
#define DATA_NAMES "groupwise-server-names"
/* On a child: it can be renamed and deleted on the server */
#define DATA_MANAGED "groupwise-managed"
/* On a child: " (Owner)" the name of a shared calendar ends with here */
#define DATA_SUFFIX "groupwise-name-suffix"

static GMutex names_lock;

static GHashTable *
backend_names (ECollectionBackend *backend)
{
	GHashTable *names = g_object_get_data (G_OBJECT (backend), DATA_NAMES);

	if (!names) {
		names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
		g_object_set_data_full (G_OBJECT (backend), DATA_NAMES, names, (GDestroyNotify) g_hash_table_destroy);
	}

	return names;
}

/* The name a source has on the server; noted first, so that the change is
 * not taken for the user's */
static void
set_server_name (ECollectionBackend *backend,
		 ESource *child,
		 const gchar *name)
{
	g_mutex_lock (&names_lock);
	g_hash_table_insert (backend_names (backend), e_source_dup_uid (child), g_strdup (name));
	g_mutex_unlock (&names_lock);

	if (g_strcmp0 (e_source_get_display_name (child), name) != 0)
		e_source_set_display_name (child, name);
}

/* A calendar or address book the user can rename and delete in Evolution:
 * not those of another mailbox (a proxy account) */
static void
set_managed (ECollectionBackend *backend,
	     ESource *child,
	     gboolean managed,
	     const gchar *suffix)
{
	gchar *proxy = dup_collection_proxy (e_backend_get_source (E_BACKEND (backend)));

	if (proxy)
		managed = FALSE;
	g_free (proxy);

	g_object_set_data (G_OBJECT (child), DATA_MANAGED, GINT_TO_POINTER (managed ? 1 : 0));
	g_object_set_data_full (G_OBJECT (child), DATA_SUFFIX, g_strdup (suffix), g_free);
	e_server_side_source_set_remote_deletable (E_SERVER_SIDE_SOURCE (child), managed);
}

/* How often calendars, task lists and memo lists are compared with the
 * server (a full listing): changes made in the GroupWise client show up */
#define CALENDAR_REFRESH_MINUTES 15

/* Returns whether it changed the source */
static gboolean
ensure_calendar_refresh (ESource *child)
{
	ESourceRefresh *refresh;

	/* The user's own setting stays */
	if (e_source_has_extension (child, E_SOURCE_EXTENSION_REFRESH))
		return FALSE;

	refresh = e_source_get_extension (child, E_SOURCE_EXTENSION_REFRESH);
	e_source_refresh_set_enabled (refresh, TRUE);
	e_source_refresh_set_interval_minutes (refresh, CALENDAR_REFRESH_MINUTES);

	return TRUE;
}

/* The calendar folder gives three sources */
static const struct {
	const gchar *extension;
	const gchar *prefix;
} calendar_kinds[] = {
	{ E_SOURCE_EXTENSION_CALENDAR, "calendar:" },
	{ E_SOURCE_EXTENSION_TASK_LIST, "tasks:" },
	{ E_SOURCE_EXTENSION_MEMO_LIST, "memos:" }
};

/* The ID a folder keeps whether the user shares it or not: sharing makes
 * BASE@13 a BASE@35, and the source of a calendar is to stay the same */
static gchar *
stable_folder_id (const gchar *id)
{
	gchar *stable = g_strdup (id);

	if (stable && g_str_has_suffix (stable, "@35"))
		memcpy (stable + strlen (stable) - 2, "13", 2);

	return stable;
}

/* The resource ID: the folder ID, with the kind for calendar folders */
static gchar *
child_resource_id (ESource *child)
{
	gchar *folder_id = child_folder_id (child), *id = stable_folder_id (folder_id), *resource_id;
	guint ii;

	g_free (folder_id);

	if (id && e_source_has_extension (child, E_SOURCE_EXTENSION_MAIL_SIGNATURE)) {
		resource_id = g_strconcat ("signature:", id, NULL);
		g_free (id);
		return resource_id;
	}

	for (ii = 0; id && ii < G_N_ELEMENTS (calendar_kinds); ii++) {
		if (e_source_has_extension (child, calendar_kinds[ii].extension)) {
			resource_id = g_strconcat (calendar_kinds[ii].prefix, id, NULL);
			g_free (id);
			return resource_id;
		}
	}

	return id;
}

/* The children follow host, user and method of the collection */
static void
copy_authentication (ESource *collection,
		     ESource *child)
{
	ESourceAuthentication *from = e_source_get_extension (collection, E_SOURCE_EXTENSION_AUTHENTICATION);
	ESourceAuthentication *to = e_source_get_extension (child, E_SOURCE_EXTENSION_AUTHENTICATION);

	e_source_authentication_set_host (to, e_source_authentication_get_host (from));
	e_source_authentication_set_port (to, e_source_authentication_get_port (from));
	e_source_authentication_set_user (to, e_source_authentication_get_user (from));
	e_source_authentication_set_method (to, e_source_authentication_get_method (from));
}

static ESource *
new_address_book (ECollectionBackend *backend,
		  EGwAddressBook *book)
{
	ESource *collection = e_backend_get_source (E_BACKEND (backend));
	ESource *child = e_collection_backend_new_child (backend, book->id);
	ESourceBackend *extension;

	extension = e_source_get_extension (child, E_SOURCE_EXTENSION_ADDRESS_BOOK);
	e_source_backend_set_backend_name (extension, "groupwise");
	e_source_groupwise_folder_set_id (e_source_get_extension (child, E_SOURCE_EXTENSION_GROUPWISE_FOLDER), book->id);
	/* Kept locally: autocompletion and offline work need the whole book */
	e_source_offline_set_stay_synchronized (e_source_get_extension (child, E_SOURCE_EXTENSION_OFFLINE), TRUE);
	/* Names complete from GroupWise when writing; the user can switch it off.
	 * Not those of another user's mailbox: they would come twice. */
	e_source_autocomplete_set_include_me (e_source_get_extension (child, E_SOURCE_EXTENSION_AUTOCOMPLETE),
		!backend_is_proxy (backend));
	/* Where EDS keeps the trust in the server certificate; Evolution passes a
	 * confirmation on to the collection and all its children having it */
	e_source_webdav_set_ssl_trust (e_source_get_extension (child, E_SOURCE_EXTENSION_WEBDAV_BACKEND), "");
	copy_authentication (collection, child);

	return child;
}

/* Adds, renames and removes the address book sources after the server */
static void
sync_address_books (ECollectionBackend *backend,
		    GPtrArray *books)
{
	ESourceRegistryServer *server = e_collection_backend_ref_server (backend);
	GHashTable *by_id = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
	GList *children, *link;
	guint ii;

	children = e_collection_backend_list_contacts_sources (backend);
	for (link = children; link; link = g_list_next (link)) {
		gchar *id = child_folder_id (link->data);

		if (id)
			g_hash_table_insert (by_id, id, g_object_ref (link->data));
	}
	g_list_free_full (children, g_object_unref);

	for (ii = 0; ii < books->len; ii++) {
		EGwAddressBook *book = books->pdata[ii];
		ESource *child = g_hash_table_lookup (by_id, book->id);
		/* The system address book by the name GroupWise gives it in the user's language */
		const gchar *name = book->name && *book->name ? book->name : book->id;

		/* The user's books and those shared to the user; not the system
		 * address book and the Frequent Contacts */
		gboolean managed = book->is_personal && !book->is_system && !book->is_frequent_contacts;

		if (child) {
			set_server_name (backend, child, name);
			set_managed (backend, child, managed, NULL);
			/* Address books from before the autocompletion setting */
			if (!e_source_has_extension (child, E_SOURCE_EXTENSION_AUTOCOMPLETE))
				e_source_autocomplete_set_include_me (e_source_get_extension (child, E_SOURCE_EXTENSION_AUTOCOMPLETE),
					!backend_is_proxy (backend));
			/* ... and from before the trust setting */
			ensure_trust_extension (child);
			g_hash_table_remove (by_id, book->id);
		} else {
			child = new_address_book (backend, book);
			set_server_name (backend, child, name);
			set_managed (backend, child, managed, NULL);
			e_source_registry_server_add_source (server, child);
			g_object_unref (child);
		}
	}

	/* What is left is gone from the server */
	children = g_hash_table_get_values (by_id);
	for (link = children; link; link = g_list_next (link))
		e_source_registry_server_remove_source (server, link->data);
	g_list_free (children);

	g_hash_table_destroy (by_id);
	g_object_unref (server);
}

static const gchar *
role_name (EGwCalendarRole role)
{
	switch (role) {
	case E_GW_CALENDAR_ROLE_OWN:
		return E_GW_SOURCE_ROLE_OWN;
	case E_GW_CALENDAR_ROLE_PROXY:
		return E_GW_SOURCE_ROLE_PROXY;
	case E_GW_CALENDAR_ROLE_SHARED:
		return E_GW_SOURCE_ROLE_SHARED;
	default:
		return NULL;
	}
}

/* What the source of a calendar folder is called */
static gchar *
calendar_display_name (const EGwFolder *folder,
		       EGwCalendarRole role,
		       guint kind)
{
	/* GroupWise keeps the English name of the system calendar, its client
	 * translates it: so do we */
	const gchar *names[] = { _("Calendar"), _("Tasks"), _("Notes") };

	if (role == E_GW_CALENDAR_ROLE_MAIN)
		return g_strdup (names[kind]);
	if (role == E_GW_CALENDAR_ROLE_SHARED && folder->owner_name && *folder->owner_name)
		return g_strdup_printf ("%s (%s)", folder->name, folder->owner_name);

	return g_strdup (folder->name && *folder->name ? folder->name : folder->id);
}

static guint
calendar_order (EGwCalendarRole role)
{
	switch (role) {
	case E_GW_CALENDAR_ROLE_OWN:
		return 1;
	case E_GW_CALENDAR_ROLE_PROXY:
		return 2;
	case E_GW_CALENDAR_ROLE_SHARED:
		return 3;
	default:
		return 0;
	}
}

/* Sets the GroupWise folder extension; returns whether it changed */
static gboolean
set_folder_extension (ESource *child,
		      const EGwFolder *folder,
		      EGwCalendarRole role,
		      const gchar *proxy)
{
	ESourceGroupwiseFolder *extension = e_source_get_extension (child, E_SOURCE_EXTENSION_GROUPWISE_FOLDER);
	gchar *old_role = e_source_groupwise_folder_dup_role (extension);
	gchar *old_proxy = e_source_groupwise_folder_dup_proxy (extension);
	gchar *old_id = e_source_groupwise_folder_dup_id (extension);
	gboolean changed = g_strcmp0 (old_role, role_name (role)) != 0 || g_strcmp0 (old_proxy, proxy) != 0 ||
		g_strcmp0 (old_id, folder->id) != 0;

	g_free (old_id);

	e_source_groupwise_folder_set_id (extension, folder->id);
	e_source_groupwise_folder_set_role (extension, role_name (role));
	e_source_groupwise_folder_set_proxy (extension, proxy);
	g_free (old_role);
	g_free (old_proxy);

	return changed;
}

/* A proxy calendar set up in Evolution: no folder of the mailbox, an entry
 * of the setting ProxyCalendars. Its ID is this prefix and the address. */
#define LOCAL_PROXY_PREFIX "evolution-proxy:"

static gboolean
is_local_proxy_id (const gchar *id)
{
	return id && g_str_has_prefix (id, LOCAL_PROXY_PREFIX);
}

/* The entries of the setting: string arrays { e-mail, name, color } */
static GPtrArray *
dup_local_proxy_calendars (ESource *collection)
{
	GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
	CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (NULL, collection);
	gchar *value = settings ? camel_groupwise_settings_dup_proxy_calendars (settings) : NULL;
	gchar **lines = value ? g_strsplit (value, "\n", -1) : NULL;
	guint ii;

	for (ii = 0; lines && lines[ii]; ii++) {
		gchar **fields = g_strsplit (lines[ii], "\t", 3);

		if (fields[0] && *fields[0] && fields[1])
			g_ptr_array_add (entries, fields);
		else
			g_strfreev (fields);
	}
	g_strfreev (lines);
	g_free (value);
	g_clear_object (&settings);

	return entries;
}

/* Writes the entries back: without the one of @email, or with @name for it */
static gboolean
change_local_proxy_calendar (ECollectionBackend *backend,
			     const gchar *email,
			     const gchar *name,
			     GError **error)
{
	ESource *collection = e_backend_get_source (E_BACKEND (backend));
	CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (NULL, collection);
	GPtrArray *entries = dup_local_proxy_calendars (collection);
	GString *value = g_string_new (NULL);
	gboolean success;
	guint ii;

	for (ii = 0; ii < entries->len; ii++) {
		gchar **fields = entries->pdata[ii];
		gboolean it = g_ascii_strcasecmp (fields[0], email) == 0;

		if (it && !name)
			continue;
		if (value->len)
			g_string_append_c (value, '\n');
		g_string_append_printf (value, "%s\t%s\t%s", fields[0], it ? name : fields[1], fields[2] ? fields[2] : "");
	}
	g_ptr_array_unref (entries);

	if (settings)
		camel_groupwise_settings_set_proxy_calendars (settings, value->str);
	success = settings && e_source_write_sync (collection, NULL, error);
	g_string_free (value, TRUE);
	g_clear_object (&settings);

	return success;
}

static ESource *
new_calendar (ECollectionBackend *backend,
	      guint kind,
	      const EGwFolder *folder,
	      EGwCalendarRole role,
	      const gchar *proxy)
{
	ESource *collection = e_backend_get_source (E_BACKEND (backend));
	gchar *stable_id = stable_folder_id (folder->id);
	gchar *resource_id = g_strconcat (calendar_kinds[kind].prefix, stable_id, NULL);
	ESource *child = e_collection_backend_new_child (backend, resource_id);
	ESourceBackend *extension;
	gchar *color;

	extension = e_source_get_extension (child, calendar_kinds[kind].extension);
	e_source_backend_set_backend_name (extension, "groupwise");
	set_folder_extension (child, folder, role, proxy);
	/* The Calendar first, then the own subcalendars, then those of other
	 * users (each group by name) */
	e_source_selectable_set_order (E_SOURCE_SELECTABLE (extension), calendar_order (role));
	/* The color the calendar has in GroupWise, to start with */
	color = role != E_GW_CALENDAR_ROLE_MAIN ? e_gw_folder_dup_color (folder) : NULL;
	if (color)
		e_source_selectable_set_color (E_SOURCE_SELECTABLE (extension), color);
	g_free (color);
	/* Kept locally: reminders and offline work need the whole calendar */
	e_source_offline_set_stay_synchronized (e_source_get_extension (child, E_SOURCE_EXTENSION_OFFLINE), TRUE);
	/* Another user's mailbox: shown when the user ticks it */
	if (backend_is_proxy (backend))
		e_source_selectable_set_selected (E_SOURCE_SELECTABLE (extension), FALSE);
	/* Reminders for the user's own appointments, not for other users' */
	if (kind == 0 && (role == E_GW_CALENDAR_ROLE_MAIN || role == E_GW_CALENDAR_ROLE_OWN) &&
	    !backend_is_proxy (backend))
		e_source_alarms_set_include_me (e_source_get_extension (child, E_SOURCE_EXTENSION_ALARMS), TRUE);
	else if (kind == 0)
		e_source_alarms_set_include_me (e_source_get_extension (child, E_SOURCE_EXTENSION_ALARMS), FALSE);
	ensure_calendar_refresh (child);
	e_source_webdav_set_ssl_trust (e_source_get_extension (child, E_SOURCE_EXTENSION_WEBDAV_BACKEND), "");
	copy_authentication (collection, child);
	g_free (resource_id);
	g_free (stable_id);

	return child;
}

static void
sync_calendar_source (ECollectionBackend *backend,
		      ESourceRegistryServer *server,
		      GHashTable *by_resource,
		      guint kind,
		      const EGwFolder *folder,
		      EGwCalendarRole role,
		      GHashTable *proxies)
{
	/* The user of a proxy calendar by the UUID: the address in the folder is
	 * the one typed when it was added, and users can be renamed */
	const gchar *proxy = role != E_GW_CALENDAR_ROLE_PROXY ? NULL :
		proxies && folder->proxy_uuid && g_hash_table_contains (proxies, folder->proxy_uuid) ?
		g_hash_table_lookup (proxies, folder->proxy_uuid) : folder->proxy_email;
	gchar *stable_id = stable_folder_id (folder->id);
	gchar *resource_id = g_strconcat (calendar_kinds[kind].prefix, stable_id, NULL);
	gchar *name = calendar_display_name (folder, role, kind);
	ESource *child = g_hash_table_lookup (by_resource, resource_id);
	/* A calendar the user made, or accepted from another user */
	gboolean managed = kind == 0 && (role == E_GW_CALENDAR_ROLE_OWN || role == E_GW_CALENDAR_ROLE_SHARED ||
		is_local_proxy_id (folder->id));
	gchar *suffix = role == E_GW_CALENDAR_ROLE_SHARED && folder->owner_name && *folder->owner_name ?
		g_strdup_printf (" (%s)", folder->owner_name) : NULL;

	if (child) {
		gboolean changed;

		set_server_name (backend, child, name);
		set_managed (backend, child, managed, suffix);
		ensure_trust_extension (child);
		/* Sources from before the refresh setting or the roles */
		changed = ensure_calendar_refresh (child);
		changed = set_folder_extension (child, folder, role, proxy) || changed;
		/* ... or the order (one the user set stays) */
		{
			ESourceSelectable *selectable = e_source_get_extension (child, calendar_kinds[kind].extension);

			if (e_source_selectable_get_order (selectable) == 0 && calendar_order (role) != 0) {
				e_source_selectable_set_order (selectable, calendar_order (role));
				changed = TRUE;
			}
		}
		if (changed) {
			GError *error = NULL;

			if (!e_source_write_sync (child, NULL, &error)) {
				g_warning ("GroupWise: cannot store %s: %s", e_source_get_uid (child), error->message);
				g_clear_error (&error);
			}
		}
		g_hash_table_remove (by_resource, resource_id);
	} else {
		child = new_calendar (backend, kind, folder, role, proxy);
		set_server_name (backend, child, name);
		set_managed (backend, child, managed, suffix);
		e_source_registry_server_add_source (server, child);
		g_object_unref (child);
	}

	g_free (suffix);
	g_free (name);
	g_free (resource_id);
	g_free (stable_id);
}

/* A calendar, a task list and a memo list on the Calendar; a calendar for
 * each own subcalendar, proxy calendar and calendar shared to the user */
static void
sync_calendars (ECollectionBackend *backend,
		GPtrArray *folders,
		GHashTable *proxies,
		GHashTable *proxy_accounts)
{
	gboolean is_proxy = backend_is_proxy (backend);
	ESourceRegistryServer *server = e_collection_backend_ref_server (backend);
	GHashTable *by_resource = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
	GList *children, *link;
	guint ii, kind;

	children = e_collection_backend_list_calendar_sources (backend);
	for (link = children; link; link = g_list_next (link)) {
		gchar *resource_id = child_resource_id (link->data);

		if (resource_id)
			g_hash_table_insert (by_resource, resource_id, g_object_ref (link->data));
	}
	g_list_free_full (children, g_object_unref);

	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];
		EGwCalendarRole role = e_gw_folder_get_calendar_role (folder, folders);

		/* A proxy account shows the other user's own calendars; the user's
		 * collection leaves out proxy calendars that a proxy account shows */
		if (role == E_GW_CALENDAR_ROLE_PROXY || role == E_GW_CALENDAR_ROLE_SHARED) {
			const gchar *proxy = proxies && folder->proxy_uuid && g_hash_table_contains (proxies, folder->proxy_uuid) ?
				g_hash_table_lookup (proxies, folder->proxy_uuid) : folder->proxy_email;

			gchar *folded = proxy ? g_utf8_casefold (proxy, -1) : NULL;
			gboolean skip = is_proxy || (role == E_GW_CALENDAR_ROLE_PROXY && folded && proxy_accounts &&
				g_hash_table_contains (proxy_accounts, folded));

			g_free (folded);
			if (skip)
				continue;
		}

		if (role == E_GW_CALENDAR_ROLE_MAIN) {
			for (kind = 0; kind < G_N_ELEMENTS (calendar_kinds); kind++)
				sync_calendar_source (backend, server, by_resource, kind, folder, role, proxies);
		} else if (role != E_GW_CALENDAR_ROLE_NONE) {
			/* Appointments only: tasks and notes stay in the lists of the Calendar */
			sync_calendar_source (backend, server, by_resource, 0, folder, role, proxies);
		}
	}

	/* The proxy calendars set up in Evolution, unless the user has the same
	 * from GroupWise or as a proxy account */
	if (!is_proxy) {
		GPtrArray *entries = dup_local_proxy_calendars (e_backend_get_source (E_BACKEND (backend)));
		GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

		if (proxy_accounts) {
			GHashTableIter iter;
			gpointer key;

			g_hash_table_iter_init (&iter, proxy_accounts);
			while (g_hash_table_iter_next (&iter, &key, NULL))
				g_hash_table_add (seen, g_strdup (key));
		}
		for (ii = 0; ii < folders->len; ii++) {
			EGwFolder *folder = folders->pdata[ii];

			if (e_gw_folder_get_calendar_role (folder, folders) == E_GW_CALENDAR_ROLE_PROXY) {
				const gchar *proxy = proxies && folder->proxy_uuid && g_hash_table_contains (proxies, folder->proxy_uuid) ?
					g_hash_table_lookup (proxies, folder->proxy_uuid) : folder->proxy_email;

				if (proxy)
					g_hash_table_add (seen, g_utf8_casefold (proxy, -1));
			}
		}

		for (ii = 0; ii < entries->len; ii++) {
			gchar **fields = entries->pdata[ii];
			gchar *folded = g_utf8_casefold (fields[0], -1);
			EGwFolder folder = { 0 };
			guint red, green, blue;

			if (g_hash_table_contains (seen, folded)) {
				g_free (folded);
				continue;
			}
			folder.id = g_strconcat (LOCAL_PROXY_PREFIX, folded, NULL);
			folder.name = fields[1];
			folder.proxy_email = fields[0];
			folder.is_calendar = TRUE;
			folder.includes_content = TRUE;
			folder.count = folder.unread_count = -1;
			folder.color = fields[2] && sscanf (fields[2], "#%2x%2x%2x", &red, &green, &blue) == 3 ?
				(gint64) (red | (green << 8) | (blue << 16)) : -1;
			sync_calendar_source (backend, server, by_resource, 0, &folder, E_GW_CALENDAR_ROLE_PROXY, NULL);
			g_hash_table_add (seen, folded);
			g_free (folder.id);
		}
		g_hash_table_destroy (seen);
		g_ptr_array_unref (entries);
	}

	/* What is left is gone from the server */
	children = g_hash_table_get_values (by_resource);
	for (link = children; link; link = g_list_next (link))
		e_source_registry_server_remove_source (server, link->data);
	g_list_free (children);

	g_hash_table_destroy (by_resource);
	g_object_unref (server);
}

/* The users whose mailboxes proxy accounts of the same login show
 * (collections with the setting Proxy): folded e-mail address set */
static GHashTable *
list_proxy_accounts (ECollectionBackend *backend)
{
	ESourceRegistryServer *server = e_collection_backend_ref_server (backend);
	ESource *own = e_backend_get_source (E_BACKEND (backend));
	ESourceAuthentication *own_auth = e_source_get_extension (own, E_SOURCE_EXTENSION_AUTHENTICATION);
	GHashTable *accounts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	GList *collections, *link;

	collections = e_source_registry_server_list_sources (server, E_SOURCE_EXTENSION_COLLECTION);
	for (link = collections; link; link = g_list_next (link)) {
		ESource *collection = link->data;
		ESourceAuthentication *auth;
		gchar *proxy;

		if (collection == own || !e_source_has_extension (collection, E_SOURCE_EXTENSION_AUTHENTICATION))
			continue;
		auth = e_source_get_extension (collection, E_SOURCE_EXTENSION_AUTHENTICATION);
		if (g_ascii_strcasecmp (e_source_authentication_get_host (auth) ? e_source_authentication_get_host (auth) : "",
					e_source_authentication_get_host (own_auth) ? e_source_authentication_get_host (own_auth) : "") != 0 ||
		    g_ascii_strcasecmp (e_source_authentication_get_user (auth) ? e_source_authentication_get_user (auth) : "",
					e_source_authentication_get_user (own_auth) ? e_source_authentication_get_user (own_auth) : "") != 0)
			continue;
		proxy = dup_collection_proxy (collection);
		if (proxy)
			g_hash_table_add (accounts, g_utf8_casefold (proxy, -1));
		g_free (proxy);
	}
	g_list_free_full (collections, g_object_unref);
	g_object_unref (server);

	return accounts;
}

/* The address books and calendars of the mailbox as sources */
static void
sync_resources (ECollectionBackend *collection_backend,
		EGwConnection *cnc,
		const gchar *proxy,
		GCancellable *cancellable)
{
	ESource *source = e_backend_get_source (E_BACKEND (collection_backend));
	GPtrArray *books;

	if (e_collection_backend_get_part_enabled (collection_backend, E_COLLECTION_BACKEND_PART_CONTACTS)) {
		GError *local_error = NULL;

		books = e_gw_connection_get_address_books_sync (cnc, cancellable, &local_error);
		/* Of another user's mailbox only the personal address books: the
		 * system address book is the user's own anyway */
		for (guint ii = 0; proxy && books && ii < books->len; ) {
			if (((EGwAddressBook *) books->pdata[ii])->is_system)
				g_ptr_array_remove_index (books, ii);
			else
				ii++;
		}
		if (books) {
			sync_address_books (collection_backend, books);
			g_ptr_array_unref (books);
		} else {
			g_warning ("GroupWise address books of %s: %s", e_source_get_uid (source), local_error->message);
			g_clear_error (&local_error);
		}
	}

	if (e_collection_backend_get_part_enabled (collection_backend, E_COLLECTION_BACKEND_PART_CALENDAR)) {
		GError *local_error = NULL;
		GPtrArray *folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, cancellable, &local_error);

		if (folders) {
			/* The proxy calendars of the user; a proxy session has none */
			GHashTable *proxies = proxy ? NULL : e_gw_connection_get_proxy_list_sync (cnc, cancellable, NULL);
			GHashTable *proxy_accounts = proxy ? NULL : list_proxy_accounts (collection_backend);

			sync_calendars (collection_backend, folders, proxies, proxy_accounts);
			g_clear_pointer (&proxies, g_hash_table_destroy);
			g_clear_pointer (&proxy_accounts, g_hash_table_destroy);
			g_ptr_array_unref (folders);
		} else {
			g_warning ("GroupWise folders of %s: %s", e_source_get_uid (source), local_error->message);
			g_clear_error (&local_error);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Making, renaming and deleting calendars and address books */

/* A login of its own with the password of the last one */
static EGwConnection *
connect_again (ECollectionBackend *backend,
	       GCancellable *cancellable,
	       GError **error)
{
	ESource *source = e_backend_get_source (E_BACKEND (backend));
	ENamedParameters *credentials = g_object_get_data (G_OBJECT (backend), DATA_CREDENTIALS);
	CamelGroupwiseSettings *settings = e_gw_backend_ref_settings (NULL, source);
	ESourceAuthenticationResult result;
	EGwConnection *cnc = NULL;

	if (!credentials || !settings || backend_is_proxy (backend)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
			_("The GroupWise account is not connected"));
	} else {
		cnc = e_gw_backend_connect_sync (NULL, source, settings, NULL, credentials, &result, NULL, NULL, cancellable, error);
	}
	g_clear_object (&settings);

	return cnc;
}

/* "Name (Owner)" of a calendar another user shares is "Name" on the server */
static gchar *
dup_name_for_server (ESource *child,
		     const gchar *name)
{
	const gchar *suffix = g_object_get_data (G_OBJECT (child), DATA_SUFFIX);
	gchar *plain = g_strdup (name);

	if (suffix && g_str_has_suffix (plain, suffix) && strlen (plain) > strlen (suffix))
		plain[strlen (plain) - strlen (suffix)] = '\0';

	return g_strstrip (plain);
}

static gboolean
gw_collection_create_resource_sync (ECollectionBackend *backend,
				    ESource *source,
				    GCancellable *cancellable,
				    GError **error)
{
	EGwConnection *cnc;
	gchar *name = g_strstrip (e_source_dup_display_name (source)), *id = NULL;

	if (!*name || (!e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR) &&
	    !e_source_has_extension (source, E_SOURCE_EXTENSION_ADDRESS_BOOK))) {
		g_free (name);
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("GroupWise can make calendars and address books, with a name"));
		return FALSE;
	}

	cnc = connect_again (backend, cancellable, error);
	if (!cnc) {
		g_free (name);
		return FALSE;
	}

	if (e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR)) {
		GPtrArray *folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, cancellable, error);
		gchar *color = e_source_selectable_dup_color (e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR));
		const gchar *calendar_id = NULL;
		guint ii;

		for (ii = 0; folders && ii < folders->len && !calendar_id; ii++) {
			EGwFolder *folder = folders->pdata[ii];

			if (e_gw_folder_get_calendar_role (folder, folders) == E_GW_CALENDAR_ROLE_MAIN)
				calendar_id = folder->id;
		}
		if (calendar_id)
			id = e_gw_connection_create_calendar_sync (cnc, calendar_id, name, color, cancellable, error);
		else if (folders)
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("The mailbox has no Calendar"));
		g_clear_pointer (&folders, g_ptr_array_unref);
		g_free (color);
	} else {
		id = e_gw_connection_create_address_book_sync (cnc, name, cancellable, error);
	}

	/* The new one shows as the others do */
	if (id)
		sync_resources (backend, cnc, NULL, cancellable);
	e_gw_connection_logout_sync (cnc, cancellable);
	g_object_unref (cnc);
	g_free (name);
	g_free (id);

	return id != NULL;
}

static gboolean
gw_collection_delete_resource_sync (ECollectionBackend *backend,
				    ESource *source,
				    GCancellable *cancellable,
				    GError **error)
{
	EGwConnection *cnc;
	gchar *id = child_folder_id (source);
	gboolean success;

	if (!id || !g_object_get_data (G_OBJECT (source), DATA_MANAGED)) {
		g_free (id);
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("This calendar or address book cannot be deleted in GroupWise"));
		return FALSE;
	}

	/* A proxy calendar set up in Evolution: its entry */
	if (is_local_proxy_id (id)) {
		gchar *proxy = e_source_groupwise_folder_dup_proxy (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));

		success = proxy && change_local_proxy_calendar (backend, proxy, NULL, error);
		if (success) {
			ESourceRegistryServer *server = e_collection_backend_ref_server (backend);

			e_source_registry_server_remove_source (server, source);
			g_object_unref (server);
		}
		g_free (proxy);
		g_free (id);
		return success;
	}

	cnc = connect_again (backend, cancellable, error);
	if (!cnc) {
		g_free (id);
		return FALSE;
	}

	/* With all that is in it; one another user shared only leaves the
	 * user's mailbox */
	success = e_gw_connection_remove_folder_sync (cnc, id, cancellable, error);
	if (success) {
		ESourceRegistryServer *server = e_collection_backend_ref_server (backend);

		e_source_registry_server_remove_source (server, source);
		g_object_unref (server);
	}
	e_gw_connection_logout_sync (cnc, cancellable);
	g_object_unref (cnc);
	g_free (id);

	return success;
}

typedef struct {
	ECollectionBackend *backend;
	ESource *child;
	gchar *name;		/* as the user wrote it */
	gchar *old_name;
} RenameData;

static void
rename_data_free (gpointer ptr)
{
	RenameData *data = ptr;

	g_object_unref (data->backend);
	g_object_unref (data->child);
	g_free (data->name);
	g_free (data->old_name);
	g_free (data);
}

static void
rename_thread (GTask *task,
	       gpointer source_object,
	       gpointer task_data,
	       GCancellable *cancellable)
{
	RenameData *data = task_data;
	EGwConnection *cnc;
	GError *error = NULL;
	gchar *id = child_folder_id (data->child), *plain = dup_name_for_server (data->child, data->name);
	const gchar *suffix = g_object_get_data (G_OBJECT (data->child), DATA_SUFFIX);
	gboolean success = FALSE;

	if (is_local_proxy_id (id) && *plain) {
		gchar *proxy = e_source_groupwise_folder_dup_proxy (e_source_get_extension (data->child, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));

		success = proxy && change_local_proxy_calendar (data->backend, proxy, plain, &error);
		g_free (proxy);
		cnc = NULL;
	} else {
		cnc = id && *plain ? connect_again (data->backend, cancellable, &error) : NULL;
	}
	if (cnc) {
		if (e_source_has_extension (data->child, E_SOURCE_EXTENSION_ADDRESS_BOOK))
			success = e_gw_connection_rename_address_book_sync (cnc, id, plain, cancellable, &error);
		else
			success = e_gw_connection_modify_folder_sync (cnc, id, plain, NULL, cancellable, &error);
		e_gw_connection_logout_sync (cnc, cancellable);
		g_object_unref (cnc);
	}

	if (success) {
		/* A shared calendar keeps the owner behind its name */
		gchar *shown = g_strconcat (plain, suffix, NULL);

		g_debug ("%s renamed to %s", id, plain);
		set_server_name (data->backend, data->child, shown);
		g_free (shown);
	} else {
		g_warning ("GroupWise: cannot rename %s: %s", id ? id : "?", error ? error->message : "no name");
		/* As it is on the server */
		if (data->old_name)
			set_server_name (data->backend, data->child, data->old_name);
	}
	g_clear_error (&error);
	g_free (plain);
	g_free (id);
	g_task_return_boolean (task, success);
}

/* A name other than the one from the server is the user's: the calendar or
 * address book gets it in GroupWise */
static void
child_name_changed_cb (ESource *child,
		       GParamSpec *param,
		       ECollectionBackend *backend)
{
	const gchar *known;
	gchar *name = e_source_dup_display_name (child), *old_name;
	RenameData *data;
	GTask *task;

	g_mutex_lock (&names_lock);
	known = g_hash_table_lookup (backend_names (backend), e_source_get_uid (child));
	old_name = g_strdup (known);
	g_mutex_unlock (&names_lock);

	if (!old_name || !name || g_strcmp0 (old_name, name) == 0 || !g_object_get_data (G_OBJECT (child), DATA_MANAGED)) {
		g_free (old_name);
		g_free (name);
		return;
	}

	/* Noted at once: the write that follows tells the same name again */
	g_mutex_lock (&names_lock);
	g_hash_table_insert (backend_names (backend), e_source_dup_uid (child), g_strdup (name));
	g_mutex_unlock (&names_lock);

	data = g_new0 (RenameData, 1);
	data->backend = g_object_ref (backend);
	data->child = g_object_ref (child);
	data->name = name;
	data->old_name = old_name;
	task = g_task_new (NULL, NULL, NULL, NULL);
	g_task_set_task_data (task, data, rename_data_free);
	g_task_run_in_thread (task, rename_thread);
	g_object_unref (task);
}

static ESourceAuthenticationResult
gw_collection_authenticate_sync (EBackend *backend,
				 const ENamedParameters *credentials,
				 gchar **out_certificate_pem,
				 GTlsCertificateFlags *out_certificate_errors,
				 GCancellable *cancellable,
				 GError **error)
{
	ECollectionBackend *collection_backend = E_COLLECTION_BACKEND (backend);
	ESource *source = e_backend_get_source (backend);
	ESourceAuthenticationResult result;
	CamelGroupwiseSettings *settings;
	EGwConnection *cnc;
	gchar *proxy;

	settings = e_gw_backend_ref_settings (NULL, source);
	if (!settings) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The account has no GroupWise settings"));
		return E_SOURCE_AUTHENTICATION_ERROR;
	}

	proxy = camel_groupwise_settings_dup_proxy (settings);
	cnc = e_gw_backend_connect_sync (NULL, source, settings, proxy, credentials, &result,
		out_certificate_pem, out_certificate_errors, cancellable, error);
	g_object_unref (settings);
	if (!cnc) {
		g_free (proxy);
		return result;
	}

	sync_resources (collection_backend, cnc, proxy, cancellable);

	/* The signatures with the mail */
	if (e_collection_backend_get_part_enabled (collection_backend, E_COLLECTION_BACKEND_PART_MAIL))
		gw_collection_sync_signatures (collection_backend, cnc, proxy, cancellable);

	/* The mailboxes the user is proxy of as senders of the own account */
	if (!proxy && e_collection_backend_get_part_enabled (collection_backend, E_COLLECTION_BACKEND_PART_MAIL))
		gw_collection_sync_senders (collection_backend, cnc, cancellable);

	e_gw_connection_logout_sync (cnc, cancellable);
	g_object_unref (cnc);
	g_free (proxy);

	/* One password for the whole account */
	e_collection_backend_authenticate_children (collection_backend, credentials);

	/* ... kept for making, renaming and deleting calendars and address
	 * books, which Evolution offers for the user's own mailbox */
	g_object_set_data_full (G_OBJECT (backend), DATA_CREDENTIALS, e_named_parameters_new_clone (credentials),
		(GDestroyNotify) e_named_parameters_free);
	e_server_side_source_set_remote_creatable (E_SERVER_SIDE_SOURCE (source), !backend_is_proxy (collection_backend));
	/* Evolution then lets the name be changed in the properties of a
	 * calendar or address book as well */
	{
		ESourceCollection *extension = e_source_get_extension (source, E_SOURCE_EXTENSION_COLLECTION);
		gboolean rename = !backend_is_proxy (collection_backend);

		if (e_source_collection_get_allow_sources_rename (extension) != rename) {
			GError *local_error = NULL;

			e_source_collection_set_allow_sources_rename (extension, rename);
			if (!e_source_write_sync (source, NULL, &local_error)) {
				g_warning ("GroupWise: cannot store %s: %s", e_source_get_uid (source), local_error->message);
				g_clear_error (&local_error);
			}
		}
	}

	return E_SOURCE_AUTHENTICATION_ACCEPTED;
}

/* Evolution keeps a confirmed server certificate in the WebDAV extension
 * ("SslTrust") and passes it on only to sources having it: the collection
 * and every child need it, stored (an empty extension is not written). */
static void
ensure_trust_extension (ESource *source)
{
	GError *error = NULL;

	if (e_source_has_extension (source, E_SOURCE_EXTENSION_WEBDAV_BACKEND))
		return;

	e_source_webdav_set_ssl_trust (e_source_get_extension (source, E_SOURCE_EXTENSION_WEBDAV_BACKEND), "");
	if (!e_source_write_sync (source, NULL, &error)) {
		g_warning ("GroupWise: cannot store %s: %s", e_source_get_uid (source), error->message);
		g_clear_error (&error);
	}
}

/* The address books of the last session, from the cache of the registry:
 * they stay usable offline and before the login (or a certificate
 * confirmation) succeeded; the next login updates them */
static void
claim_old_resources (ECollectionBackend *backend)
{
	ESourceRegistryServer *server = e_collection_backend_ref_server (backend);
	GList *old, *link;

	old = e_collection_backend_claim_all_resources (backend);
	for (link = old; link; link = g_list_next (link)) {
		e_source_registry_server_add_source (server, link->data);
		ensure_trust_extension (link->data);
	}

	g_list_free_full (old, g_object_unref);
	g_object_unref (server);
}

static void
gw_collection_populate (ECollectionBackend *backend)
{
	ESource *collection = e_backend_get_source (E_BACKEND (backend));

	/* The collection keeps the trust in the certificate for the account */
	ensure_trust_extension (collection);

	claim_old_resources (backend);

	if (!e_collection_backend_get_part_enabled (backend, E_COLLECTION_BACKEND_PART_ANY))
		return;

	/* Asks for the password (or finds it); authenticate_sync does the rest */
	if (e_backend_get_online (E_BACKEND (backend)))
		e_backend_credentials_required_sync (E_BACKEND (backend), E_SOURCE_CREDENTIALS_REASON_REQUIRED,
			NULL, 0, NULL, NULL, NULL);
}

static gchar *
gw_collection_dup_resource_id (ECollectionBackend *backend,
			       ESource *child)
{
	return child_resource_id (child);
}

static gboolean
child_is_proxy_account (ESource *child)
{
	const gchar *extension_name = e_source_camel_get_extension_name ("groupwise");
	gchar *proxy;
	gboolean is_proxy;

	if (!e_source_has_extension (child, extension_name))
		return FALSE;

	proxy = camel_groupwise_settings_dup_proxy (CAMEL_GROUPWISE_SETTINGS (
		e_source_camel_get_settings (e_source_get_extension (child, extension_name))));
	is_proxy = proxy != NULL;
	g_free (proxy);

	return is_proxy;
}

static void
gw_collection_child_added (ECollectionBackend *backend,
			   ESource *child)
{
	ESource *collection = e_backend_get_source (E_BACKEND (backend));

	if (e_source_has_extension (child, E_SOURCE_EXTENSION_AUTHENTICATION)) {
		ESourceAuthentication *from = e_source_get_extension (collection, E_SOURCE_EXTENSION_AUTHENTICATION);
		ESourceAuthentication *to = e_source_get_extension (child, E_SOURCE_EXTENSION_AUTHENTICATION);

		e_binding_bind_property (from, "host", to, "host", G_BINDING_SYNC_CREATE);
		e_binding_bind_property (from, "user", to, "user", G_BINDING_SYNC_CREATE);
		e_binding_bind_property (from, "method", to, "method", G_BINDING_SYNC_CREATE);
	}

	E_COLLECTION_BACKEND_CLASS (e_gw_collection_backend_parent_class)->child_added (backend, child);

	/* Renamed in Evolution, a calendar or address book is renamed in GroupWise */
	if (e_source_has_extension (child, E_SOURCE_EXTENSION_CALENDAR) ||
	    e_source_has_extension (child, E_SOURCE_EXTENSION_ADDRESS_BOOK))
		g_signal_connect_object (child, "notify::display-name", G_CALLBACK (child_name_changed_cb), backend, 0);

	/* A proxy account made by version 0.3 below the collection: the
	 * settings window removes it and makes it anew as an account of its own */
	if (child_is_proxy_account (child))
		e_server_side_source_set_removable (E_SERVER_SIDE_SOURCE (child), TRUE);
	/* A signature can be removed in Evolution; Evolution removes it in GroupWise */
	if (e_source_has_extension (child, E_SOURCE_EXTENSION_MAIL_SIGNATURE))
		e_server_side_source_set_removable (E_SERVER_SIDE_SOURCE (child), TRUE);
}

static void
e_gw_collection_backend_class_init (EGwCollectionBackendClass *class)
{
	ECollectionBackendClass *collection_class = E_COLLECTION_BACKEND_CLASS (class);

	collection_class->populate = gw_collection_populate;
	collection_class->dup_resource_id = gw_collection_dup_resource_id;
	collection_class->child_added = gw_collection_child_added;
	collection_class->create_resource_sync = gw_collection_create_resource_sync;
	collection_class->delete_resource_sync = gw_collection_delete_resource_sync;

	E_BACKEND_CLASS (class)->authenticate_sync = gw_collection_authenticate_sync;
}

static void
e_gw_collection_backend_class_finalize (EGwCollectionBackendClass *class)
{
}

static void
e_gw_collection_backend_init (EGwCollectionBackend *backend)
{
}

/* An account made through the collection gets GroupWise mail as well */
static void
gw_factory_prepare_mail (ECollectionBackendFactory *factory,
			 ESource *mail_account_source,
			 ESource *mail_identity_source,
			 ESource *mail_transport_source)
{
	E_COLLECTION_BACKEND_FACTORY_CLASS (e_gw_collection_backend_factory_parent_class)->prepare_mail (
		factory, mail_account_source, mail_identity_source, mail_transport_source);

	e_source_backend_set_backend_name (e_source_get_extension (mail_account_source, E_SOURCE_EXTENSION_MAIL_ACCOUNT), "groupwise");
	e_source_backend_set_backend_name (e_source_get_extension (mail_transport_source, E_SOURCE_EXTENSION_MAIL_TRANSPORT), "groupwise");
}

static void
e_gw_collection_backend_factory_class_init (EGwCollectionBackendFactoryClass *class)
{
	class->factory_name = "groupwise";
	class->backend_type = e_gw_collection_backend_get_type ();
	class->prepare_mail = gw_factory_prepare_mail;
}

static void
e_gw_collection_backend_factory_class_finalize (EGwCollectionBackendFactoryClass *class)
{
}

static void
e_gw_collection_backend_factory_init (EGwCollectionBackendFactory *factory)
{
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

	e_gw_backend_ensure_types ();
	e_gw_collection_backend_register_type (type_module);
	e_gw_collection_backend_factory_register_type (type_module);
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
}

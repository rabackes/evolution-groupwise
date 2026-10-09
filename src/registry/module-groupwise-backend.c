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

#include <glib/gi18n-lib.h>
#include <libebackend/libebackend.h>

#include "e-gw-addressbook.h"
#include "e-gw-folder.h"
#include "e-gw-backend-utils.h"
#include "e-source-groupwise-folder.h"
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

/* The resource ID: the folder ID, with the kind for calendar folders */
static gchar *
child_resource_id (ESource *child)
{
	gchar *id = child_folder_id (child), *resource_id;
	guint ii;

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

		if (child) {
			if (g_strcmp0 (e_source_get_display_name (child), name) != 0)
				e_source_set_display_name (child, name);
			/* Address books from before the autocompletion setting */
			if (!e_source_has_extension (child, E_SOURCE_EXTENSION_AUTOCOMPLETE))
				e_source_autocomplete_set_include_me (e_source_get_extension (child, E_SOURCE_EXTENSION_AUTOCOMPLETE),
					!backend_is_proxy (backend));
			/* ... and from before the trust setting */
			ensure_trust_extension (child);
			g_hash_table_remove (by_id, book->id);
		} else {
			child = new_address_book (backend, book);
			e_source_set_display_name (child, name);
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
	gboolean changed = g_strcmp0 (old_role, role_name (role)) != 0 || g_strcmp0 (old_proxy, proxy) != 0;

	e_source_groupwise_folder_set_id (extension, folder->id);
	e_source_groupwise_folder_set_role (extension, role_name (role));
	e_source_groupwise_folder_set_proxy (extension, proxy);
	g_free (old_role);
	g_free (old_proxy);

	return changed;
}

static ESource *
new_calendar (ECollectionBackend *backend,
	      guint kind,
	      const EGwFolder *folder,
	      EGwCalendarRole role,
	      const gchar *proxy)
{
	ESource *collection = e_backend_get_source (E_BACKEND (backend));
	gchar *resource_id = g_strconcat (calendar_kinds[kind].prefix, folder->id, NULL);
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
	gchar *resource_id = g_strconcat (calendar_kinds[kind].prefix, folder->id, NULL);
	gchar *name = calendar_display_name (folder, role, kind);
	ESource *child = g_hash_table_lookup (by_resource, resource_id);

	if (child) {
		gboolean changed;

		if (g_strcmp0 (e_source_get_display_name (child), name) != 0)
			e_source_set_display_name (child, name);
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
		e_source_set_display_name (child, name);
		e_source_registry_server_add_source (server, child);
		g_object_unref (child);
	}

	g_free (name);
	g_free (resource_id);
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
	GPtrArray *books;
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

	/* The signatures with the mail */
	if (e_collection_backend_get_part_enabled (collection_backend, E_COLLECTION_BACKEND_PART_MAIL))
		gw_collection_sync_signatures (collection_backend, cnc, proxy, cancellable);

	e_gw_connection_logout_sync (cnc, cancellable);
	g_object_unref (cnc);
	g_free (proxy);

	/* One password for the whole account */
	e_collection_backend_authenticate_children (collection_backend, credentials);

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

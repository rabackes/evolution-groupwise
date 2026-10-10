/*
 * e-cal-backend-groupwise.c: GroupWise calendars, task lists and memo lists
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
 * An ECalMetaBackend on the GroupWise calendar folder: it holds the
 * appointments, the tasks and the notes, each source shows one of them.
 * What the user sent to others is not in it but in the Sent Items: those
 * items of the type are listed as well.
 * The whole folder is kept in the local cache; a sync lists every item with
 * its revision. The item ID is the "extra" of a component.
 *
 * A meeting the user organized is shown as the sent item: it has the
 * answers, and a change of it reaches the attendees. When the user invited
 * themselves (the GroupWise client does so), the alarm is on the received
 * copy: it is shown on the meeting, and the extra is "SENT-ID\nOWN-COPY-ID".
 *
 * Each own subcalendar is a calendar of its own (role "own" of the source):
 * an appointment in "Privat" is linked into the Calendar and "Privat", the
 * GroupWise Calendar shows it; here it shows in "Privat" only, the Calendar
 * lists the appointments of no subcalendar. New appointments of a
 * subcalendar are created in it (sendItem with its container).
 * Calendars of other users are read only: a proxy calendar (role "proxy")
 * through a proxy login into the other mailbox, with its Calendar and Sent
 * Items; a calendar shared to the user (role "shared") as it is.
 * The calendars of a proxy account (a collection with the setting Proxy)
 * are those of the other mailbox, in a proxy login, writable as far as the
 * other user granted it.
 *
 * GroupWise sends and answers meetings itself: the backend creates the
 * messages (sendItem with the attendees, accept, decline, retract), so
 * Evolution sends no iTIP mail for this calendar. An answer to an invitation
 * arrives as a changed PARTSTAT of the user's attendee.
 */

#include <string.h>

#include <glib/gi18n-lib.h>
#include <libedata-cal/libedata-cal.h>

#include "e-gw-backend-utils.h"
#include "e-gw-category.h"
#include "e-gw-calendar.h"
#include "e-gw-folder.h"
#include "e-gw-xml.h"
#include "e-source-groupwise-folder.h"

/* recipientStatus: the answers of the attendees to what the user sent */
#define ITEM_VIEW "default message recipients recipientStatus peek"
#define PAGE_SIZE 200

/* Part of every revision: a new version of the conversion refreshes the cache */
#define FORMAT_VERSION "9:"

/* EDS declares no cleanup function for its backends */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ECalMetaBackend, g_object_unref)

#define E_TYPE_CAL_BACKEND_GROUPWISE (e_cal_backend_groupwise_get_type ())
G_DECLARE_FINAL_TYPE (ECalBackendGroupwise, e_cal_backend_groupwise, E, CAL_BACKEND_GROUPWISE, ECalMetaBackend)

/* What the source shows */
typedef enum {
	ROLE_MAIN,	/* the Calendar (and the task and memo lists) */
	ROLE_OWN,	/* an own subcalendar */
	ROLE_PROXY,	/* the Calendar of another user */
	ROLE_SHARED	/* a calendar shared to the user */
} Role;

struct _ECalBackendGroupwise {
	ECalMetaBackend parent;

	GRecMutex lock;
	EGwConnection *cnc;
	Role role;
	gchar *folder_id;	/* the folder the source shows */
	gchar *calendar_id;	/* the Calendar of the mailbox */
	gchar *sent_folder_id;	/* NULL: its sent items are not listed */
	gchar *user_email;

	GPtrArray *subcalendars;	/* IDs of the own subcalendars (ROLE_MAIN) */
	GHashTable *item_subcalendars;	/* item ID without container -> GPtrArray of
					 * subcalendar IDs, as the last listing found */

	/* The categories of the mailbox: CATEGORIES of the components */
	GHashTable *category_names;	/* ID (as items name it) -> name */
	GHashTable *category_ids;	/* casefolded name -> ID */

	/* The login of the account (user@host), shared by its proxy sessions */
	gchar *account_key;
	gboolean proxy_session;
	gchar *user_name;	/* the owner of the mailbox (of a proxy session) */

	/* Travel time of new meetings, for the own copy the next listing
	 * finds: UID -> ICalComponent with the X-GW-TRAVEL-* wanted */
	GHashTable *pending_travel;
};

G_DEFINE_TYPE (ECalBackendGroupwise, e_cal_backend_groupwise, E_TYPE_CAL_META_BACKEND)

/* The meetings in the user's own calendars, per account (below) */
static void	own_meetings_publish	(ECalBackendGroupwise *cbgw,
					 GHashTable *uids);
static gboolean	own_meetings_published	(ECalBackendGroupwise *cbgw);
static void	own_meetings_forget	(ECalBackendGroupwise *cbgw);
static void	account_proxy_add	(const gchar *account_key,
					 const gchar *email);
static gboolean	account_proxy_contains	(const gchar *account_key,
					 const gchar *email);
static void	decorate_for_tooltip	(ECalBackendGroupwise *cbgw,
					 ICalComponent *comp);
static guint	account_proxies_stamp	(const gchar *account_key);
static void	account_proxies_scan	(ESourceRegistry *registry);

static EGwConnection *
ref_connection (ECalBackendGroupwise *cbgw,
		GError **error)
{
	EGwConnection *cnc;

	g_rec_mutex_lock (&cbgw->lock);
	cnc = cbgw->cnc ? g_object_ref (cbgw->cnc) : NULL;
	g_rec_mutex_unlock (&cbgw->lock);

	if (!cnc)
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_REPOSITORY_OFFLINE, _("Not connected"));

	return cnc;
}

/* The category list of the mailbox; its names go into Evolution's
 * categories (those GroupWise does not offer excepted) */
static void
load_categories (ECalBackendGroupwise *cbgw,
		 EGwConnection *cnc,
		 GCancellable *cancellable)
{
	GError *error = NULL;
	GPtrArray *categories = e_gw_connection_get_categories_sync (cnc, cancellable, &error);
	guint ii;

	if (!categories) {
		g_debug ("categories: %s", error ? error->message : "?");
		g_clear_error (&error);
		return;
	}

	g_rec_mutex_lock (&cbgw->lock);
	g_hash_table_remove_all (cbgw->category_names);
	g_hash_table_remove_all (cbgw->category_ids);
	for (ii = 0; ii < categories->len; ii++) {
		EGwCategory *category = categories->pdata[ii];
		gchar *name = e_gw_category_dup_display_name (category);

		if (!name || !*name) {
			g_free (name);
			continue;
		}
		g_hash_table_replace (cbgw->category_ids, g_utf8_casefold (name, -1), g_strdup (category->id));
		if (!category->hidden && !e_categories_exist (name))
			e_categories_add (name, NULL, NULL, TRUE);
		g_hash_table_replace (cbgw->category_names, g_strdup (category->id), name);
	}
	g_rec_mutex_unlock (&cbgw->lock);
	g_ptr_array_unref (categories);
}

/* The names of the categories of @item as CATEGORIES of @comp */
static void
take_item_categories (ECalBackendGroupwise *cbgw,
		      ICalComponent *comp,
		      xmlNode *item)
{
	gchar **ids = e_gw_item_dup_categories (item);
	GPtrArray *names = g_ptr_array_new ();
	guint ii;

	g_rec_mutex_lock (&cbgw->lock);
	for (ii = 0; ids[ii]; ii++) {
		const gchar *name = g_hash_table_lookup (cbgw->category_names, ids[ii]);

		if (name)
			g_ptr_array_add (names, (gpointer) name);
	}
	g_ptr_array_add (names, NULL);
	e_gw_calendar_set_categories (comp, (const gchar * const *) names->pdata);
	g_rec_mutex_unlock (&cbgw->lock);

	g_ptr_array_unref (names);
	g_strfreev (ids);
}

/* The categories of the item @id after the CATEGORIES of @comp; a name
 * GroupWise does not know becomes a category, ones the names do not stand
 * for stay */
static gboolean
save_categories (ECalBackendGroupwise *cbgw,
		 EGwConnection *cnc,
		 const gchar *id,
		 ICalComponent *comp,
		 GCancellable *cancellable,
		 GError **error)
{
	EGwResponse *response;
	GPtrArray *names = e_gw_calendar_dup_categories (comp), *wanted;
	xmlNode *item;
	gchar **current, *updates;
	gboolean success = TRUE;
	guint ii;

	response = e_gw_connection_get_item_sync (cnc, id, "id category", cancellable, error);
	item = response ? e_gw_xml_find (e_gw_response_get_node (response), "item") : NULL;
	if (!item) {
		e_gw_response_free (response);
		g_ptr_array_unref (names);
		return response != NULL;
	}
	current = e_gw_item_dup_categories (item);
	e_gw_response_free (response);

	wanted = g_ptr_array_new_with_free_func (g_free);
	g_rec_mutex_lock (&cbgw->lock);
	for (ii = 0; current[ii]; ii++) {
		if (!g_hash_table_contains (cbgw->category_names, current[ii]))
			g_ptr_array_add (wanted, g_strdup (current[ii]));
	}
	g_rec_mutex_unlock (&cbgw->lock);

	for (ii = 0; success && ii < names->len; ii++) {
		gchar *key = g_utf8_casefold (names->pdata[ii], -1), *cat_id;

		g_rec_mutex_lock (&cbgw->lock);
		cat_id = g_strdup (g_hash_table_lookup (cbgw->category_ids, key));
		g_rec_mutex_unlock (&cbgw->lock);

		if (!cat_id) {
			g_debug ("creating the category %s", (const gchar *) names->pdata[ii]);
			cat_id = e_gw_connection_create_category_sync (cnc, names->pdata[ii], -1, cancellable, error);
			if (cat_id) {
				g_rec_mutex_lock (&cbgw->lock);
				g_hash_table_replace (cbgw->category_ids, g_strdup (key), g_strdup (cat_id));
				g_hash_table_replace (cbgw->category_names, g_strdup (cat_id), g_strdup (names->pdata[ii]));
				g_rec_mutex_unlock (&cbgw->lock);
			} else {
				success = FALSE;
			}
		}
		if (cat_id && !g_ptr_array_find_with_equal_func (wanted, cat_id, g_str_equal, NULL))
			g_ptr_array_add (wanted, cat_id);
		else
			g_free (cat_id);
		g_free (key);
	}
	g_ptr_array_add (wanted, NULL);

	if (success) {
		updates = e_gw_categories_updates_xml ((const gchar * const *) current, (const gchar * const *) wanted->pdata);
		if (*updates) {
			g_debug ("categories of %s: %s", id, updates);
			success = e_gw_connection_modify_item_sync (cnc, id, updates, cancellable, error);
		}
		g_free (updates);
	}

	g_ptr_array_unref (wanted);
	g_ptr_array_unref (names);
	g_strfreev (current);

	return success;
}

#define CACHE_KEY_USER_EMAIL "gw-user-email"

/* The user's address: from the login, before it from the cache (Evolution
 * asks for it when it opens the calendar, to find the user among the
 * attendees) */
static gchar *
dup_user_email (ECalBackendGroupwise *cbgw)
{
	gchar *email;

	g_rec_mutex_lock (&cbgw->lock);
	email = g_strdup (cbgw->user_email);
	g_rec_mutex_unlock (&cbgw->lock);

	if (!email) {
		ECalCache *cache = e_cal_meta_backend_ref_cache (E_CAL_META_BACKEND (cbgw));

		if (cache) {
			email = e_cache_dup_key (E_CACHE (cache), CACHE_KEY_USER_EMAIL, NULL);
			g_object_unref (cache);
		}
		if (email && !*email)
			g_clear_pointer (&email, g_free);
	}

	return email;
}

/* The user's zone: all-day events and the days of tasks are local */
static ICalTimezone *
user_zone (void)
{
	ICalTimezone *zone = e_cal_util_get_system_timezone ();

	return zone ? zone : i_cal_timezone_get_utc_timezone ();
}

static const gchar *
item_type_of_backend (ECalBackendGroupwise *cbgw)
{
	switch (e_cal_backend_get_kind (E_CAL_BACKEND (cbgw))) {
	case I_CAL_VTODO_COMPONENT:
		return "Task";
	case I_CAL_VJOURNAL_COMPONENT:
		return "Note";
	default:
		return "Appointment";
	}
}

/* GroupWise errors in terms of EDS */
static void
propagate_error (GError **error,
		 GError *gw_error)
{
	if (!gw_error)
		return;

	if (gw_error->domain == E_GW_ERROR && gw_error->code == E_GW_ERROR_CONNECTION)
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_REPOSITORY_OFFLINE, gw_error->message);
	else if (gw_error->domain == E_GW_ERROR && gw_error->code == E_GW_ERROR_ITEM_NOT_FOUND)
		g_propagate_error (error, e_cal_client_error_create (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND, gw_error->message));
	else if (gw_error->domain == E_GW_ERROR)
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_OTHER_ERROR, gw_error->message);
	else {
		g_propagate_error (error, gw_error);
		return;
	}

	g_error_free (gw_error);
}

static Role
role_from_name (const gchar *name)
{
	if (g_strcmp0 (name, E_GW_SOURCE_ROLE_OWN) == 0)
		return ROLE_OWN;
	if (g_strcmp0 (name, E_GW_SOURCE_ROLE_PROXY) == 0)
		return ROLE_PROXY;
	if (g_strcmp0 (name, E_GW_SOURCE_ROLE_SHARED) == 0)
		return ROLE_SHARED;

	return ROLE_MAIN;
}

/* In a proxy session: whether the other user granted writing this kind */
static gboolean
proxy_may_write (ECalBackendGroupwise *cbgw,
		 EGwConnection *cnc)
{
	EGwProxyRights rights = e_gw_connection_get_proxy_rights (cnc);

	switch (e_cal_backend_get_kind (E_CAL_BACKEND (cbgw))) {
	case I_CAL_VTODO_COMPONENT:
		return (rights & E_GW_PROXY_TASK_WRITE) != 0;
	case I_CAL_VJOURNAL_COMPONENT:
		return (rights & E_GW_PROXY_NOTE_WRITE) != 0;
	default:
		return (rights & E_GW_PROXY_APPOINTMENT_WRITE) != 0;
	}
}

static gboolean
ecb_groupwise_connect_sync (ECalMetaBackend *meta_backend,
			    const ENamedParameters *credentials,
			    ESourceAuthenticationResult *out_auth_result,
			    gchar **out_certificate_pem,
			    GTlsCertificateFlags *out_certificate_errors,
			    GCancellable *cancellable,
			    GError **error)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (meta_backend);
	ESource *source = e_backend_get_source (E_BACKEND (meta_backend));
	ESourceRegistry *registry = e_cal_backend_get_registry (E_CAL_BACKEND (meta_backend));
	CamelGroupwiseSettings *settings;
	EGwConnection *cnc;
	gchar *folder_id = NULL, *role_name = NULL, *proxy = NULL;
	Role role;

	g_rec_mutex_lock (&cbgw->lock);
	if (cbgw->cnc) {
		g_rec_mutex_unlock (&cbgw->lock);
		*out_auth_result = E_SOURCE_AUTHENTICATION_ACCEPTED;
		return TRUE;
	}

	if (e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER)) {
		ESourceGroupwiseFolder *extension = e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER);

		folder_id = e_source_groupwise_folder_dup_id (extension);
		role_name = e_source_groupwise_folder_dup_role (extension);
		proxy = e_source_groupwise_folder_dup_proxy (extension);
	}
	role = role_from_name (role_name);
	g_free (role_name);
	settings = e_gw_backend_ref_settings (registry, source);
	/* A calendar of a proxy account: the other mailbox, its own calendars */
	if (settings && role != ROLE_PROXY) {
		gchar *account_proxy = camel_groupwise_settings_dup_proxy (settings);

		if (account_proxy) {
			g_free (proxy);
			proxy = account_proxy;
		} else {
			g_clear_pointer (&proxy, g_free);
		}
	}
	if (!folder_id || !settings || (role == ROLE_PROXY && (!proxy || !*proxy))) {
		g_rec_mutex_unlock (&cbgw->lock);
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_OTHER_ERROR,
			_("The calendar is not set up for GroupWise"));
		*out_auth_result = E_SOURCE_AUTHENTICATION_ERROR;
		g_free (folder_id);
		g_free (proxy);
		g_clear_object (&settings);
		return FALSE;
	}

	account_proxies_scan (registry);
	cnc = e_gw_backend_connect_sync (registry, source, settings, proxy, credentials,
		out_auth_result, out_certificate_pem, out_certificate_errors, cancellable, error);
	{
		gchar *user = camel_network_settings_dup_user (CAMEL_NETWORK_SETTINGS (settings));
		gchar *host = camel_network_settings_dup_host (CAMEL_NETWORK_SETTINGS (settings));
		gchar *key = g_strdup_printf ("%s@%s", user ? user : "", host ? host : "");

		g_free (cbgw->account_key);
		cbgw->account_key = g_ascii_strdown (key, -1);
		g_free (key);
		g_free (user);
		g_free (host);
	}
	g_object_unref (settings);
	g_free (proxy);

	if (cnc) {
		GPtrArray *folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, cancellable, NULL);
		gboolean email_changed;
		guint ii;

		g_clear_pointer (&cbgw->sent_folder_id, g_free);
		g_clear_pointer (&cbgw->calendar_id, g_free);
		g_ptr_array_set_size (cbgw->subcalendars, 0);
		for (ii = 0; folders && ii < folders->len; ii++) {
			EGwFolder *folder = folders->pdata[ii];
			EGwCalendarRole folder_role = e_gw_folder_get_calendar_role (folder, folders);

			if (!cbgw->sent_folder_id && folder->type == E_GW_FOLDER_TYPE_SENT_ITEMS &&
			    folder->kind == E_GW_FOLDER_KIND_SYSTEM)
				cbgw->sent_folder_id = g_strdup (folder->id);
			if (!cbgw->calendar_id && folder_role == E_GW_CALENDAR_ROLE_MAIN)
				cbgw->calendar_id = g_strdup (folder->id);
			if (role == ROLE_MAIN && folder_role == E_GW_CALENDAR_ROLE_OWN)
				g_ptr_array_add (cbgw->subcalendars, g_strdup (folder->id));
		}
		if (folders)
			g_ptr_array_unref (folders);
		load_categories (cbgw, cnc, cancellable);

		/* A proxy calendar shows the Calendar of the other mailbox; what
		 * someone shared comes without the owner's sent items */
		if (role == ROLE_PROXY && cbgw->calendar_id) {
			g_free (folder_id);
			folder_id = g_strdup (cbgw->calendar_id);
		} else if (role == ROLE_SHARED) {
			g_clear_pointer (&cbgw->sent_folder_id, g_free);
		}
		if (!cbgw->calendar_id)
			cbgw->calendar_id = g_strdup (folder_id);

		cbgw->cnc = cnc;
		cbgw->role = role;
		cbgw->proxy_session = e_gw_connection_get_proxy (cnc) != NULL;
		g_free (cbgw->user_name);
		cbgw->user_name = g_strdup (e_gw_connection_get_user_name (cnc));
		if (cbgw->proxy_session)
			account_proxy_add (cbgw->account_key, e_gw_connection_get_user_email (cnc));
		g_free (cbgw->folder_id);
		cbgw->folder_id = folder_id;
		email_changed = g_strcmp0 (cbgw->user_email, e_gw_connection_get_user_email (cnc)) != 0;
		g_free (cbgw->user_email);
		cbgw->user_email = g_strdup (e_gw_connection_get_user_email (cnc));
		g_rec_mutex_unlock (&cbgw->lock);

		/* Proxy and shared calendars are only read (for now); those of a
		 * proxy account as far as the other user granted it */
		e_cal_backend_set_writable (E_CAL_BACKEND (cbgw), (role == ROLE_MAIN || role == ROLE_OWN) &&
			(!e_gw_connection_get_proxy (cnc) || proxy_may_write (cbgw, cnc)));
		/* What the last session had: proxy calendars leave it out
		 * from the start, not only after the first listing here */
		if (!cbgw->proxy_session && (role == ROLE_MAIN || role == ROLE_OWN) &&
		    e_cal_backend_get_kind (E_CAL_BACKEND (cbgw)) == I_CAL_VEVENT_COMPONENT &&
		    !own_meetings_published (cbgw)) {
			ECalCache *cache = e_cal_meta_backend_ref_cache (meta_backend);
			GSList *ids = NULL, *link;

			if (cache && e_cal_cache_search_ids (cache, NULL, &ids, cancellable, NULL)) {
				GHashTable *uids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

				for (link = ids; link; link = g_slist_next (link))
					g_hash_table_add (uids, g_strdup (e_cal_component_id_get_uid (link->data)));
				own_meetings_publish (cbgw, uids);
			}
			g_slist_free_full (ids, e_cal_component_id_free);
			g_clear_object (&cache);
		}
		if (email_changed) {
			ECalCache *cache = e_cal_meta_backend_ref_cache (meta_backend);

			if (cache) {
				e_cache_set_key (E_CACHE (cache), CACHE_KEY_USER_EMAIL, cbgw->user_email, NULL);
				g_object_unref (cache);
			}
			e_cal_backend_notify_property_changed (E_CAL_BACKEND (cbgw), E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS,
				cbgw->user_email);
		}
	} else {
		g_rec_mutex_unlock (&cbgw->lock);
		g_free (folder_id);
	}

	return cnc != NULL;
}

static gboolean
ecb_groupwise_disconnect_sync (ECalMetaBackend *meta_backend,
			       GCancellable *cancellable,
			       GError **error)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (meta_backend);
	EGwConnection *cnc;

	g_rec_mutex_lock (&cbgw->lock);
	cnc = cbgw->cnc;
	cbgw->cnc = NULL;
	g_rec_mutex_unlock (&cbgw->lock);

	if (cnc) {
		e_gw_connection_logout_sync (cnc, cancellable);
		g_object_unref (cnc);
	}

	return TRUE;
}

static gchar *
item_id (xmlNode *item)
{
	gchar *raw = e_gw_xml_dup_text (item, "id");
	gchar *id = e_gw_clean_id (raw);

	g_free (raw);

	return id;
}

/* The item ID of the extra, and the ID of the user's own copy if there is one */
static gchar *
split_extra (const gchar *extra,
	     gchar **out_own_copy_id)
{
	const gchar *newline = extra ? strchr (extra, '\n') : NULL;

	if (out_own_copy_id)
		*out_own_copy_id = newline && newline[1] ? g_strdup (newline + 1) : NULL;

	return newline ? g_strndup (extra, newline - extra) : g_strdup (extra);
}

/* The alarms of @from on @to instead of its own */
static void
copy_alarms (ICalComponent *from,
	     ICalComponent *to)
{
	ICalComponent *alarm;

	while ((alarm = i_cal_component_get_first_component (to, I_CAL_VALARM_COMPONENT))) {
		i_cal_component_remove_component (to, alarm);
		g_object_unref (alarm);
	}
	for (alarm = i_cal_component_get_first_component (from, I_CAL_VALARM_COMPONENT); alarm;
	     g_object_unref (alarm), alarm = i_cal_component_get_next_component (from, I_CAL_VALARM_COMPONENT))
		i_cal_component_take_component (to, i_cal_component_clone (alarm));
}

/* What belongs to the user on a meeting: the alarm and the subject (GroupWise
 * lets the user rename an appointment for themselves) */
static void
copy_personal (ICalComponent *from,
	       ICalComponent *to)
{
	const gchar *travel[] = { E_GW_CALENDAR_X_TRAVEL_BEFORE, E_GW_CALENDAR_X_TRAVEL_AFTER };
	guint ii;

	copy_alarms (from, to);
	if (i_cal_component_get_summary (from) && *i_cal_component_get_summary (from))
		i_cal_component_set_summary (to, i_cal_component_get_summary (from));
	/* The travel time is the user's too: on the own copy of a meeting */
	for (ii = 0; ii < G_N_ELEMENTS (travel); ii++) {
		gchar *value = e_cal_util_component_dup_x_property (from, travel[ii]);

		e_cal_util_component_set_x_property (to, travel[ii], value);
		g_free (value);
	}
}

/* What a component says about its item, for a move between the calendars
 * of an account: Evolution moves by creating the component in the other
 * calendar and removing it from this one */
#define X_OWN_COPY_ID	"X-GW-OWN-COPY-ID"
#define X_ACCOUNT	"X-GW-ACCOUNT"

/* The user's own copy of a meeting they organized joins its sent item */
static void
merge_own_copy (ECalMetaBackendInfo *info,
		ICalComponent *own_copy,
		const gchar *own_copy_id,
		const gchar *own_copy_revision)
{
	ICalComponent *vcalendar = i_cal_component_new_from_string (info->object);
	ICalComponent *sent;
	gchar *text;

	if (!vcalendar)
		return;
	sent = i_cal_component_isa (vcalendar) == I_CAL_VCALENDAR_COMPONENT ?
		i_cal_component_get_first_component (vcalendar, i_cal_component_isa (own_copy)) : g_object_ref (vcalendar);
	if (sent) {
		GPtrArray *names = e_gw_calendar_dup_categories (own_copy);

		copy_personal (own_copy, sent);
		/* The user's categories are on the own copy */
		g_ptr_array_add (names, NULL);
		e_gw_calendar_set_categories (sent, (const gchar * const *) names->pdata);
		g_ptr_array_unref (names);
		e_cal_util_component_set_x_property (sent, X_OWN_COPY_ID, own_copy_id);
		g_free (info->object);
		info->object = i_cal_component_as_ical_string (vcalendar);
		g_object_unref (sent);
	}
	g_object_unref (vcalendar);

	text = g_strconcat (info->extra, "\n", own_copy_id, NULL);
	g_free (info->extra);
	info->extra = text;

	text = g_strconcat (info->revision, ":", own_copy_revision, NULL);
	g_free (info->revision);
	info->revision = text;
}

/* The item part of an ID, the same in every folder */
static gchar *
item_base (const gchar *id)
{
	return id ? g_strndup (id, strcspn (id, ":")) : NULL;
}

/* The own subcalendars the last listing found an item of the Calendar in */
static gchar **
dup_item_subcalendars (ECalBackendGroupwise *cbgw,
		       const gchar *id)
{
	gchar *base = item_base (id);
	GPtrArray *subs, *copy = g_ptr_array_new ();
	guint ii;

	g_rec_mutex_lock (&cbgw->lock);
	subs = base ? g_hash_table_lookup (cbgw->item_subcalendars, base) : NULL;
	for (ii = 0; subs && ii < subs->len; ii++)
		g_ptr_array_add (copy, g_strdup (subs->pdata[ii]));
	g_rec_mutex_unlock (&cbgw->lock);
	g_ptr_array_add (copy, NULL);
	g_free (base);

	return (gchar **) g_ptr_array_free (copy, FALSE);
}

typedef struct {
	ECalBackendGroupwise *cbgw;
	GSList *infos;
	GHashTable *seen;
	GHashTable *sent_uids;	/* UID -> ECalMetaBackendInfo of a sent item */
	gboolean listing_sent;
	gchar *user_email;
	GHashTable *exclude;	/* item bases of subcalendars: not in this calendar */
	GHashTable *dropped;	/* UIDs of own copies left out: their sent items go too */
	GHashTable *used;	/* UIDs of sent items with an own copy here */
	GPtrArray *travel_todo;	/* own copy ID, UID, ...: a travel time to set */
} ListData;

/* Which items are in which own subcalendar (IDs only) */
static gboolean
collect_membership_cb (xmlNode *item,
		       gpointer user_data)
{
	gpointer *args = user_data;
	GHashTable *membership = args[0];
	const gchar *sub_id = args[1];
	gchar *id = item_id (item), *base = item_base (id);

	if (base && !e_gw_xml_get_bool (item, "status/deleted")) {
		GPtrArray *subs = g_hash_table_lookup (membership, base);

		if (!subs) {
			subs = g_ptr_array_new_with_free_func (g_free);
			g_hash_table_insert (membership, g_strdup (base), subs);
		}
		g_ptr_array_add (subs, g_strdup (sub_id));
	}
	g_free (base);
	g_free (id);

	return TRUE;
}

/* A meeting the user organized and invited themselves to: its received copy */
static gboolean
is_own_copy (xmlNode *item,
	     const gchar *user_email)
{
	gchar *source = e_gw_xml_dup_text (item, "source");
	gchar *organizer = e_gw_xml_dup_text (item, "distribution/from/email");
	gboolean own = g_strcmp0 (source, "received") == 0 && user_email && organizer &&
		g_ascii_strcasecmp (organizer, user_email) == 0;

	g_free (source);
	g_free (organizer);

	return own;
}

/* Whether a subcalendar holds the user's own copy of a meeting */
static gboolean
find_own_copy_cb (xmlNode *item,
		  gpointer user_data)
{
	gpointer *args = user_data;

	if (!e_gw_xml_get_bool (item, "status/deleted") && is_own_copy (item, args[0]))
		*((gboolean *) args[1]) = TRUE;

	return TRUE;
}

static gboolean
list_item_cb (xmlNode *item,
	      gpointer user_data)
{
	ListData *data = user_data;
	ICalComponent *comp;
	gchar *object, *revision, *id;
	const gchar *uid;

	/* A deleted item stays listed where it was */
	if (e_gw_xml_get_bool (item, "status/deleted"))
		return TRUE;

	comp = e_gw_calendar_component_from_item (item, user_zone (), data->user_email);
	if (!comp)
		return TRUE;
	e_cal_util_component_set_x_property (comp, X_ACCOUNT, data->user_email);
	take_item_categories (data->cbgw, comp, item);
	decorate_for_tooltip (data->cbgw, comp);

	id = item_id (item);
	uid = i_cal_component_get_uid (comp);
	if (!uid) {
		g_object_unref (comp);
		g_free (id);
		return TRUE;
	}

	/* An appointment of a subcalendar shows there, not in the Calendar; the
	 * own copy of a meeting takes the sent item along */
	if (data->exclude) {
		gchar *base = item_base (id);
		gboolean excluded = base && g_hash_table_contains (data->exclude, base);

		g_free (base);
		if (excluded) {
			if (!data->listing_sent && is_own_copy (item, data->user_email))
				g_hash_table_add (data->dropped, g_strdup (uid));
			g_object_unref (comp);
			g_free (id);
			return TRUE;
		}
	}

	/* A subcalendar holding a sent item: listed with the Sent Items already */
	if (data->used && !data->listing_sent && g_hash_table_contains (data->sent_uids, uid)) {
		gchar *source = e_gw_xml_dup_text (item, "source");
		gboolean sent = g_strcmp0 (source, "sent") == 0;

		g_free (source);
		if (sent) {
			g_hash_table_add (data->used, g_strdup (uid));
			g_object_unref (comp);
			g_free (id);
			return TRUE;
		}
	}

	/* The organizer's meeting is the sent one (with the answers, and a
	 * change of it reaches the attendees); the copy the user got gives
	 * its alarm */
	if (!data->listing_sent && g_hash_table_contains (data->sent_uids, uid) && is_own_copy (item, data->user_email)) {
		if (data->used)
			g_hash_table_add (data->used, g_strdup (uid));
		revision = e_gw_calendar_revision (item);
		merge_own_copy (g_hash_table_lookup (data->sent_uids, uid), comp, id, revision);
		g_rec_mutex_lock (&data->cbgw->lock);
		if (g_hash_table_contains (data->cbgw->pending_travel, uid)) {
			g_ptr_array_add (data->travel_todo, g_strdup (id));
			g_ptr_array_add (data->travel_todo, g_strdup (uid));
		}
		g_rec_mutex_unlock (&data->cbgw->lock);
		g_free (revision);
		g_object_unref (comp);
		g_free (id);
		return TRUE;
	}

	/* Items imported together can share an iCalId: the later ones get
	 * their item ID appended (load_component keeps the UID asked for) */
	if (g_hash_table_contains (data->seen, uid)) {
		gchar *unique = g_strdup_printf ("%s-%.*s", uid, (gint) strcspn (id, ":"), id);

		i_cal_component_set_uid (comp, unique);
		g_free (unique);
		uid = i_cal_component_get_uid (comp);
	}
	g_hash_table_add (data->seen, g_strdup (uid));

	/* The object comes along: the cache needs no second request per item;
	 * with the VTIMEZONE of the user's zone its times are in */
	{
		ICalComponent *vcalendar = e_gw_calendar_wrap (comp, user_zone ());

		object = i_cal_component_as_ical_string (vcalendar);
		g_object_unref (vcalendar);
	}
	{
		gchar *digest = e_gw_calendar_revision (item);

		gchar *stamp = g_strdup_printf ("%08x:", account_proxies_stamp (data->cbgw->account_key));

		/* The proxies marked among the attendees are part of it */
		revision = g_strconcat (FORMAT_VERSION, stamp, digest, NULL);
		g_free (stamp);
		g_free (digest);
	}
	data->infos = g_slist_prepend (data->infos, e_cal_meta_backend_info_new (uid, revision, object, id));
	if (data->listing_sent && !g_hash_table_contains (data->sent_uids, uid))
		g_hash_table_insert (data->sent_uids, g_strdup (uid), data->infos->data);
	g_free (object);
	g_free (revision);
	g_free (id);
	g_object_unref (comp);

	return TRUE;
}

static gchar *
type_filter (ECalBackendGroupwise *cbgw)
{
	return g_strdup_printf ("<filter><element type=\"FilterEntry\"><op>eq</op><field>@type</field>"
		"<value>%s</value></element></filter>", item_type_of_backend (cbgw));
}

/* Takes out the sent items whose UID is in @uids (@keep_listed FALSE) or
 * is not (@keep_listed TRUE) */
static GSList *
filter_sent (GSList *infos,
	     GHashTable *sent_uids,
	     GHashTable *uids,
	     gboolean keep_listed)
{
	GSList *link, *next;

	for (link = infos; link; link = next) {
		ECalMetaBackendInfo *info = link->data;
		gboolean is_sent = g_hash_table_lookup (sent_uids, info->uid) == info;

		next = g_slist_next (link);
		if (is_sent && g_hash_table_contains (uids, info->uid) != keep_listed) {
			infos = g_slist_delete_link (infos, link);
			e_cal_meta_backend_info_free (info);
		}
	}

	return infos;
}

/* The meetings in the user's own calendars, per account: a calendar of a
 * proxy session leaves out what the user has in an own calendar already,
 * as the GroupWise client shows such a meeting once (in the user's color).
 * The calendars of all accounts share one process. */
static GMutex own_meetings_lock;
static GHashTable *own_meetings;	/* account key -> (backend -> set of UIDs) */
static GHashTable *proxy_watchers;	/* account key -> GPtrArray of GWeakRef */

static void
weak_ref_free (gpointer data)
{
	g_weak_ref_clear (data);
	g_free (data);
}

/* The UIDs of an own calendar (@uids taken); a change refreshes the
 * calendars of the proxy sessions of the account */
static void
own_meetings_publish (ECalBackendGroupwise *cbgw,
		      GHashTable *uids)
{
	GHashTable *per_backend, *old;
	GPtrArray *watchers, *to_refresh = g_ptr_array_new_with_free_func (g_object_unref);
	gboolean changed;
	guint ii;

	g_mutex_lock (&own_meetings_lock);
	if (!own_meetings)
		own_meetings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_hash_table_unref);
	per_backend = g_hash_table_lookup (own_meetings, cbgw->account_key);
	if (!per_backend) {
		per_backend = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_hash_table_unref);
		g_hash_table_insert (own_meetings, g_strdup (cbgw->account_key), per_backend);
	}
	old = g_hash_table_lookup (per_backend, cbgw);
	changed = !old || g_hash_table_size (old) != g_hash_table_size (uids);
	if (!changed) {
		GHashTableIter iter;
		gpointer uid;

		g_hash_table_iter_init (&iter, uids);
		while (!changed && g_hash_table_iter_next (&iter, &uid, NULL))
			changed = !g_hash_table_contains (old, uid);
	}
	g_hash_table_insert (per_backend, cbgw, uids);

	watchers = changed && proxy_watchers ? g_hash_table_lookup (proxy_watchers, cbgw->account_key) : NULL;
	for (ii = 0; watchers && ii < watchers->len; ii++) {
		gpointer watcher = g_weak_ref_get (watchers->pdata[ii]);

		if (watcher)
			g_ptr_array_add (to_refresh, watcher);
	}
	g_mutex_unlock (&own_meetings_lock);

	for (ii = 0; ii < to_refresh->len; ii++)
		e_cal_meta_backend_schedule_refresh (to_refresh->pdata[ii]);
	g_ptr_array_unref (to_refresh);
}

/* Whether a meeting is in an own calendar of the account of @cbgw, a
 * calendar of a proxy session (which then watches for changes) */
static gboolean
own_meetings_contains (ECalBackendGroupwise *cbgw,
		       const gchar *uid,
		       gboolean watch)
{
	GHashTable *per_backend;
	gboolean found = FALSE;

	g_mutex_lock (&own_meetings_lock);
	if (watch) {
		GPtrArray *watchers;
		gboolean known = FALSE;
		guint ii;

		if (!proxy_watchers)
			proxy_watchers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
		watchers = g_hash_table_lookup (proxy_watchers, cbgw->account_key);
		if (!watchers) {
			watchers = g_ptr_array_new_with_free_func (weak_ref_free);
			g_hash_table_insert (proxy_watchers, g_strdup (cbgw->account_key), watchers);
		}
		for (ii = 0; ii < watchers->len && !known; ii++) {
			gpointer watcher = g_weak_ref_get (watchers->pdata[ii]);

			known = watcher == (gpointer) cbgw;
			g_clear_object (&watcher);
		}
		if (!known) {
			GWeakRef *ref = g_new0 (GWeakRef, 1);

			g_weak_ref_init (ref, cbgw);
			g_ptr_array_add (watchers, ref);
		}
	}
	per_backend = own_meetings && uid ? g_hash_table_lookup (own_meetings, cbgw->account_key) : NULL;
	if (per_backend) {
		GHashTableIter iter;
		gpointer uids;

		g_hash_table_iter_init (&iter, per_backend);
		while (!found && g_hash_table_iter_next (&iter, NULL, &uids))
			found = g_hash_table_contains (uids, uid);
	}
	g_mutex_unlock (&own_meetings_lock);

	return found;
}

static void
own_meetings_forget (ECalBackendGroupwise *cbgw)
{
	GPtrArray *watchers;
	GHashTable *per_backend;
	guint ii;

	if (!cbgw->account_key)
		return;

	g_mutex_lock (&own_meetings_lock);
	per_backend = own_meetings ? g_hash_table_lookup (own_meetings, cbgw->account_key) : NULL;
	if (per_backend)
		g_hash_table_remove (per_backend, cbgw);
	/* Gone ones (this one among them, finalized) */
	watchers = proxy_watchers ? g_hash_table_lookup (proxy_watchers, cbgw->account_key) : NULL;
	for (ii = 0; watchers && ii < watchers->len;) {
		gpointer watcher = g_weak_ref_get (watchers->pdata[ii]);

		if (!watcher || watcher == (gpointer) cbgw)
			g_ptr_array_remove_index_fast (watchers, ii);
		else
			ii++;
		g_clear_object (&watcher);
	}
	g_mutex_unlock (&own_meetings_lock);
}

/* The users the account works for as proxy (proxy accounts and proxy
 * calendars): account key -> set of casefolded e-mail addresses */
static GHashTable *account_proxies;

static void
account_proxy_add (const gchar *account_key,
		   const gchar *email)
{
	GHashTable *emails;

	if (!account_key || !email || !*email)
		return;

	g_mutex_lock (&own_meetings_lock);
	if (!account_proxies)
		account_proxies = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_hash_table_unref);
	emails = g_hash_table_lookup (account_proxies, account_key);
	if (!emails) {
		emails = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		g_hash_table_insert (account_proxies, g_strdup (account_key), emails);
	}
	g_hash_table_add (emails, g_ascii_strdown (email, -1));
	g_mutex_unlock (&own_meetings_lock);
}

/* The same for every order the proxies come in */
static guint
account_proxies_stamp (const gchar *account_key)
{
	GHashTable *emails;
	guint stamp = 0;

	if (!account_key)
		return 0;

	g_mutex_lock (&own_meetings_lock);
	emails = account_proxies ? g_hash_table_lookup (account_proxies, account_key) : NULL;
	if (emails) {
		GHashTableIter iter;
		gpointer email;

		g_hash_table_iter_init (&iter, emails);
		while (g_hash_table_iter_next (&iter, &email, NULL))
			stamp += g_str_hash (email);
	}
	g_mutex_unlock (&own_meetings_lock);

	return stamp;
}

/* The proxy accounts and proxy calendars set up, whichever calendar
 * connects first */
static void
account_proxies_scan (ESourceRegistry *registry)
{
	GList *sources, *link;

	sources = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_CALENDAR);
	for (link = sources; link; link = g_list_next (link)) {
		ESource *source = link->data;
		CamelGroupwiseSettings *settings;
		gchar *proxy = NULL;

		if (!e_source_has_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER))
			continue;
		settings = e_gw_backend_ref_settings (registry, source);
		if (!settings)
			continue;
		proxy = camel_groupwise_settings_dup_proxy (settings);
		if (!proxy || !*proxy) {
			g_free (proxy);
			proxy = e_source_groupwise_folder_dup_proxy (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
		}
		if (proxy && *proxy) {
			gchar *user = camel_network_settings_dup_user (CAMEL_NETWORK_SETTINGS (settings));
			gchar *host = camel_network_settings_dup_host (CAMEL_NETWORK_SETTINGS (settings));
			gchar *key = g_strdup_printf ("%s@%s", user ? user : "", host ? host : "");
			gchar *key_down = g_ascii_strdown (key, -1);

			account_proxy_add (key_down, proxy);
			g_free (key_down);
			g_free (key);
			g_free (user);
			g_free (host);
		}
		g_free (proxy);
		g_object_unref (settings);
	}
	g_list_free_full (sources, g_object_unref);
}

static gboolean
account_proxy_contains (const gchar *account_key,
			const gchar *email)
{
	GHashTable *emails;
	gchar *key;
	gboolean found;

	if (!account_key || !email)
		return FALSE;

	key = g_ascii_strdown (email, -1);
	g_mutex_lock (&own_meetings_lock);
	emails = account_proxies ? g_hash_table_lookup (account_proxies, account_key) : NULL;
	found = emails && g_hash_table_contains (emails, key);
	g_mutex_unlock (&own_meetings_lock);
	g_free (key);

	return found;
}

/* Only so many attendees are named with their answer in the tooltip; of
 * more, only the proxies */
#define TOOLTIP_ATTENDEES 8

/* What Evolution's tooltip of an appointment can show of GroupWise: the
 * owner of an appointment of a proxy calendar as its organizer (marked
 * X-GW-OWNER, the appointment editor takes it away again), the answers of
 * the attendees of a meeting as their comments (of many attendees only
 * the proxies'). Only for the display: the backend sends neither to
 * GroupWise. */
static void
decorate_for_tooltip (ECalBackendGroupwise *cbgw,
		      ICalComponent *comp)
{
	ICalProperty *prop, *last_shown;
	guint count, shown;

	if (i_cal_component_isa (comp) != I_CAL_VEVENT_COMPONENT)
		return;

	if (cbgw->proxy_session && cbgw->user_email &&
	    !e_cal_util_component_has_property (comp, I_CAL_ORGANIZER_PROPERTY)) {
		gchar *address = g_strconcat ("mailto:", cbgw->user_email, NULL);

		prop = i_cal_property_new_organizer (address);
		if (cbgw->user_name && *cbgw->user_name)
			i_cal_property_take_parameter (prop, i_cal_parameter_new_cn (cbgw->user_name));
		i_cal_property_set_parameter_from_string (prop, "X-GW-OWNER", "TRUE");
		i_cal_component_take_property (comp, prop);
		g_free (address);
	}

	count = i_cal_component_count_properties (comp, I_CAL_ATTENDEE_PROPERTY);
	shown = 0;
	last_shown = NULL;
	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		const gchar *email = e_cal_util_strip_mailto (i_cal_property_get_attendee (prop));
		gboolean is_proxy = account_proxy_contains (cbgw->account_key, email);
		ICalParameter *param = i_cal_property_get_first_parameter (prop, I_CAL_PARTSTAT_PARAMETER);
		ICalParameterPartstat partstat = param ? i_cal_parameter_get_partstat (param) : I_CAL_PARTSTAT_NEEDSACTION;
		const gchar *answer;

		g_clear_object (&param);
		if (count > TOOLTIP_ATTENDEES && !is_proxy)
			continue;

		switch (partstat) {
		case I_CAL_PARTSTAT_ACCEPTED:
			answer = _("accepted");
			break;
		case I_CAL_PARTSTAT_DECLINED:
			answer = _("declined");
			break;
		case I_CAL_PARTSTAT_TENTATIVE:
			answer = _("tentative");
			break;
		case I_CAL_PARTSTAT_DELEGATED:
			answer = _("delegated");
			break;
		default:
			answer = _("no answer yet");
			break;
		}
		i_cal_property_set_parameter_from_string (prop, "X-RESPONSE-COMMENT", answer);
		g_clear_object (&last_shown);
		last_shown = g_object_ref (prop);
		shown++;
	}

	/* Shortened: the last one says there are more */
	if (last_shown && shown < count) {
		gchar *answer = i_cal_property_get_parameter_as_string (last_shown, "X-RESPONSE-COMMENT");
		gchar *text = g_strdup_printf ("%s … (+%u)", answer ? answer : "", count - shown);

		i_cal_property_set_parameter_from_string (last_shown, "X-RESPONSE-COMMENT", text);
		g_free (text);
		g_free (answer);
	}
	g_clear_object (&last_shown);
}

static gboolean
own_meetings_published (ECalBackendGroupwise *cbgw)
{
	GHashTable *per_backend;
	gboolean published;

	g_mutex_lock (&own_meetings_lock);
	per_backend = own_meetings ? g_hash_table_lookup (own_meetings, cbgw->account_key) : NULL;
	published = per_backend && g_hash_table_contains (per_backend, cbgw);
	g_mutex_unlock (&own_meetings_lock);

	return published;
}

static void	set_pending_travel	(ECalBackendGroupwise *cbgw,
					 EGwConnection *cnc,
					 GPtrArray *todo,
					 GCancellable *cancellable);

static gboolean
ecb_groupwise_list_existing_sync (ECalMetaBackend *meta_backend,
				  gchar **out_new_sync_tag,
				  GSList **out_existing_objects,
				  GCancellable *cancellable,
				  GError **error)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (meta_backend);
	EGwConnection *cnc = ref_connection (cbgw, error);
	ListData data = { cbgw, NULL, NULL, NULL, FALSE, NULL, NULL, NULL, NULL };
	GHashTable *membership = NULL;
	GError *local_error = NULL;
	gchar *filter, *sent_folder_id;
	gboolean success = TRUE;
	guint ii;

	if (!cnc)
		return FALSE;

	data.seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	data.sent_uids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	data.user_email = dup_user_email (cbgw);
	data.travel_todo = g_ptr_array_new_with_free_func (g_free);
	filter = type_filter (cbgw);
	sent_folder_id = g_strdup (cbgw->sent_folder_id);

	/* The Calendar: which items are in which own subcalendar; their
	 * appointments show in the subcalendar only */
	if (cbgw->role == ROLE_MAIN) {
		membership = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
		for (ii = 0; success && ii < cbgw->subcalendars->len; ii++) {
			gpointer args[2] = { membership, cbgw->subcalendars->pdata[ii] };

			success = e_gw_connection_foreach_item_sync (cnc, cbgw->subcalendars->pdata[ii], "id peek", filter, 1000,
				collect_membership_cb, args, cancellable, &local_error);
		}
		if (e_cal_backend_get_kind (E_CAL_BACKEND (cbgw)) == I_CAL_VEVENT_COMPONENT) {
			data.exclude = membership;
			data.dropped = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
			/* A sent meeting shows with the own copy in the Calendar,
			 * as in the GroupWise client (without one it is only in
			 * the Sent Items there) */
			if (sent_folder_id)
				data.used = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		}
	}

	/* A subcalendar: the Sent Items only when it has an own copy of a
	 * meeting, whose sent item is shown */
	if (success && cbgw->role == ROLE_OWN && sent_folder_id) {
		gboolean found = FALSE;
		gpointer args[2] = { data.user_email, &found };

		success = e_gw_connection_foreach_item_sync (cnc, cbgw->folder_id, "id source distribution peek", filter, PAGE_SIZE,
			find_own_copy_cb, args, cancellable, &local_error);
		if (found)
			data.used = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		else
			g_clear_pointer (&sent_folder_id, g_free);
	}

	/* What the user sent to others */
	if (success && sent_folder_id) {
		data.listing_sent = TRUE;
		success = e_gw_connection_foreach_item_sync (cnc, sent_folder_id, ITEM_VIEW, filter, PAGE_SIZE,
			list_item_cb, &data, cancellable, &local_error);
		data.listing_sent = FALSE;
	}
	if (success) {
		success = e_gw_connection_foreach_item_sync (cnc, cbgw->folder_id, ITEM_VIEW, filter, PAGE_SIZE,
			list_item_cb, &data, cancellable, &local_error);
	}

	if (success && data.dropped)
		data.infos = filter_sent (data.infos, data.sent_uids, data.dropped, FALSE);
	if (success && data.used)
		data.infos = filter_sent (data.infos, data.sent_uids, data.used, TRUE);

	if (success && data.travel_todo->len)
		set_pending_travel (cbgw, cnc, data.travel_todo, cancellable);

	/* A meeting the user has in an own calendar shows there only */
	if (success && cbgw->account_key && e_cal_backend_get_kind (E_CAL_BACKEND (cbgw)) == I_CAL_VEVENT_COMPONENT) {
		if (!cbgw->proxy_session && (cbgw->role == ROLE_MAIN || cbgw->role == ROLE_OWN)) {
			GHashTable *uids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
			GSList *link;

			for (link = data.infos; link; link = g_slist_next (link))
				g_hash_table_add (uids, g_strdup (((ECalMetaBackendInfo *) link->data)->uid));
			own_meetings_publish (cbgw, uids);
		} else if (cbgw->proxy_session) {
			GSList *link, *next;

			own_meetings_contains (cbgw, NULL, TRUE);
			for (link = data.infos; link; link = next) {
				ECalMetaBackendInfo *info = link->data;

				next = g_slist_next (link);
				if (own_meetings_contains (cbgw, info->uid, FALSE)) {
					data.infos = g_slist_delete_link (data.infos, link);
					e_cal_meta_backend_info_free (info);
				}
			}
		}
	}

	if (success && membership) {
		g_rec_mutex_lock (&cbgw->lock);
		g_hash_table_destroy (cbgw->item_subcalendars);
		cbgw->item_subcalendars = g_steal_pointer (&membership);
		g_rec_mutex_unlock (&cbgw->lock);
	}
	g_clear_pointer (&membership, g_hash_table_destroy);
	g_clear_pointer (&data.dropped, g_hash_table_destroy);
	g_clear_pointer (&data.used, g_hash_table_destroy);
	g_ptr_array_unref (data.travel_todo);
	g_free (sent_folder_id);
	g_free (filter);
	g_free (data.user_email);
	g_hash_table_destroy (data.seen);
	g_hash_table_destroy (data.sent_uids);
	g_object_unref (cnc);

	if (!success) {
		g_slist_free_full (data.infos, e_cal_meta_backend_info_free);
		propagate_error (error, local_error);
		return FALSE;
	}

	*out_existing_objects = g_slist_reverse (data.infos);
	*out_new_sync_tag = NULL;

	return TRUE;
}

/* The item behind a component, by its ID */
static EGwResponse *
get_item (EGwConnection *cnc,
	  const gchar *id,
	  xmlNode **out_item,
	  GCancellable *cancellable,
	  GError **error)
{
	EGwResponse *response = e_gw_connection_get_item_sync (cnc, id, ITEM_VIEW, cancellable, error);

	*out_item = response ? e_gw_xml_find (e_gw_response_get_node (response), "item") : NULL;
	if (response && !*out_item) {
		e_gw_response_free (response);
		response = NULL;
		g_propagate_error (error, e_cal_client_error_create (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND, NULL));
	}

	return response;
}

static gboolean
ecb_groupwise_load_component_sync (ECalMetaBackend *meta_backend,
				   const gchar *uid,
				   const gchar *extra,
				   ICalComponent **out_component,
				   gchar **out_extra,
				   GCancellable *cancellable,
				   GError **error)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (meta_backend);
	EGwConnection *cnc;
	EGwResponse *response;
	GError *local_error = NULL;
	gchar *user_email, *id, *own_copy_id = NULL;
	xmlNode *item;

	if (!extra || !*extra) {
		g_propagate_error (error, e_cal_client_error_create (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND, uid));
		return FALSE;
	}

	cnc = ref_connection (cbgw, error);
	if (!cnc)
		return FALSE;

	user_email = dup_user_email (cbgw);
	id = split_extra (extra, &own_copy_id);
	response = get_item (cnc, id, &item, cancellable, &local_error);
	*out_component = item ? e_gw_calendar_component_from_item (item, user_zone (), user_email) : NULL;
	if (*out_component)
		take_item_categories (cbgw, *out_component, item);
	*out_extra = g_strdup (extra);
	e_gw_response_free (response);

	/* The alarm from the user's own copy of the meeting */
	if (*out_component && own_copy_id) {
		response = get_item (cnc, own_copy_id, &item, cancellable, NULL);
		if (item) {
			ICalComponent *own_copy = e_gw_calendar_component_from_item (item, user_zone (), user_email);

			copy_personal (own_copy, *out_component);
			take_item_categories (cbgw, *out_component, item);
			g_object_unref (own_copy);
		}
		e_gw_response_free (response);
		e_cal_util_component_set_x_property (*out_component, X_OWN_COPY_ID, own_copy_id);
	}
	if (*out_component) {
		e_cal_util_component_set_x_property (*out_component, X_ACCOUNT, user_email);
		decorate_for_tooltip (cbgw, *out_component);
	}
	g_free (id);
	g_free (own_copy_id);
	/* The UID the listing gave it (see list_item_cb) and the zone of its times */
	if (*out_component && uid && *uid)
		i_cal_component_set_uid (*out_component, uid);
	if (*out_component) {
		ICalComponent *vcalendar = e_gw_calendar_wrap (*out_component, user_zone ());

		g_object_unref (*out_component);
		*out_component = vcalendar;
	}
	g_free (user_email);
	g_object_unref (cnc);

	if (!*out_component) {
		if (local_error)
			propagate_error (error, local_error);
		else
			g_propagate_error (error, e_cal_client_error_create (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND, uid));
		return FALSE;
	}

	return TRUE;
}

/* The component to store: the master, else the first instance */
static ICalComponent *
main_component (const GSList *instances)
{
	const GSList *link;

	for (link = instances; link; link = g_slist_next (link)) {
		if (!e_cal_component_is_instance (link->data))
			break;
	}
	if (!link)
		link = instances;
	if (!link || !link->data)
		return NULL;

	return e_cal_component_get_icalcomponent (link->data);
}

/* The PARTSTAT of the user's attendee */
static ICalParameterPartstat
user_partstat (ICalComponent *comp,
	       const gchar *user_email)
{
	return e_gw_calendar_user_partstat (comp, user_email);
}

/* The comment of the user's answer (Evolution puts it into COMMENT) */
static gchar *
answer_comment (ICalComponent *comp)
{
	ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_COMMENT_PROPERTY);
	gchar *comment = prop ? g_strdup (i_cal_property_get_comment (prop)) : NULL;

	g_clear_object (&prop);

	return comment;
}

/* Only the personal fields of @comp (alarm, subject) onto the item @current */
static gboolean
apply_personal (EGwConnection *cnc,
	     const gchar *id,
	     xmlNode *current,
	     ICalComponent *comp,
	     GCancellable *cancellable,
	     GError **error)
{
	ICalComponent *before = e_gw_calendar_component_from_item (current, user_zone (), NULL);
	gchar *updates;
	gboolean success;

	copy_personal (comp, before);
	updates = e_gw_calendar_updates_xml (current, before, NULL, user_zone (), error);
	success = updates && (!*updates || e_gw_connection_modify_item_sync (cnc, id, updates, cancellable, error));
	g_free (updates);
	g_object_unref (before);

	return success;
}

/* An invitation the user received: only the answer and the alarm are the
 * user's; the rest belongs to the organizer */
static gboolean
save_received (ECalBackendGroupwise *cbgw,
	       EGwConnection *cnc,
	       const gchar *id,
	       xmlNode *current,
	       ICalComponent *comp,
	       GCancellable *cancellable,
	       GError **error)
{
	gchar *user_email = dup_user_email (cbgw);
	ICalComponent *before = e_gw_calendar_component_from_item (current, user_zone (), user_email);
	ICalParameterPartstat old_partstat = user_partstat (before, user_email);
	ICalParameterPartstat new_partstat = user_partstat (comp, user_email);
	gboolean success = TRUE;

	g_debug ("received %s for %s: answer %d -> %d, alarms %d -> %d", id, user_email ? user_email : "?",
		old_partstat, new_partstat, i_cal_component_count_components (before, I_CAL_VALARM_COMPONENT),
		i_cal_component_count_components (comp, I_CAL_VALARM_COMPONENT));

	if (new_partstat != old_partstat && new_partstat != I_CAL_PARTSTAT_NONE) {
		gchar *comment = answer_comment (comp);

		if (new_partstat == I_CAL_PARTSTAT_ACCEPTED || new_partstat == I_CAL_PARTSTAT_TENTATIVE) {
			gchar *level = e_cal_util_component_dup_x_property (comp, E_GW_CALENDAR_X_BUSY_STATUS);

			success = e_gw_connection_accept_sync (cnc, id,
				new_partstat == I_CAL_PARTSTAT_TENTATIVE ? "Tentative" :
				level && !g_ascii_strcasecmp (level, "OOF") ? "OutOfOffice" :
				level && !g_ascii_strcasecmp (level, "FREE") ? "Free" : "Busy",
				comment, cancellable, error);
			g_free (level);
		} else if (new_partstat == I_CAL_PARTSTAT_DECLINED) {
			success = e_gw_connection_decline_sync (cnc, id, comment, cancellable, error);
		}
		g_free (comment);

		/* An answer is only an answer: the alarm stays (an invitation or
		 * a reply has none of its own) */
		g_object_unref (before);
		g_free (user_email);
		return success;
	}

	/* The alarm of the new version on the item as it is */
	success = apply_personal (cnc, id, current, comp, cancellable, error);
	g_object_unref (before);
	g_free (user_email);

	return success;
}

/* The travel time of an appointment (the user's own: the own copy of a
 * meeting) as the component wants it. Set or changed, the POA makes and
 * places its appointments; taken away, the link to the appointment goes and
 * the appointment with it, as the GroupWise client does it (the number
 * stays on the POA, without effect). */
static gboolean
sync_travel_time (ECalBackendGroupwise *cbgw,
		  EGwConnection *cnc,
		  const gchar *item_id,
		  xmlNode *item,
		  ICalComponent *comp,
		  GCancellable *cancellable,
		  GError **error)
{
	static const struct {
		const gchar *x_name;
		const gchar *field;
		const gchar *link;
	} sides[] = {
		{ E_GW_CALENDAR_X_TRAVEL_BEFORE, "travelTimeBefore", "travelAppointmentBefore" },
		{ E_GW_CALENDAR_X_TRAVEL_AFTER, "travelTimeAfter", "travelAppointmentAfter" }
	};
	GString *updates = g_string_new (NULL), *deletes = g_string_new (NULL);
	GPtrArray *gone = g_ptr_array_new_with_free_func (g_free);
	gboolean success = TRUE;
	guint ii;

	if (i_cal_component_isa (comp) != I_CAL_VEVENT_COMPONENT) {
		g_string_free (updates, TRUE);
		g_string_free (deletes, TRUE);
		g_ptr_array_unref (gone);
		return TRUE;
	}

	for (ii = 0; ii < G_N_ELEMENTS (sides); ii++) {
		gchar *wanted = e_cal_util_component_dup_x_property (comp, sides[ii].x_name);
		gchar *link = e_gw_xml_dup_text (item, sides[ii].link);
		gint64 new_seconds = wanted ? g_ascii_strtoll (wanted, NULL, 10) : 0;
		gint64 old_seconds = link ? e_gw_xml_get_int (item, sides[ii].field, 0) : 0;

		if (new_seconds > 0 && new_seconds != old_seconds) {
			gchar *seconds = g_strdup_printf ("%" G_GINT64_FORMAT, new_seconds);

			e_gw_xml_add_leaf (updates, sides[ii].field, seconds);
			g_free (seconds);
		} else if (new_seconds <= 0 && link) {
			e_gw_xml_add_leaf (deletes, sides[ii].link, link);
			g_ptr_array_add (gone, g_strconcat (link, "@4", NULL));
		}
		g_free (wanted);
		g_free (link);
	}

	if (updates->len || deletes->len) {
		GString *inner = g_string_new (NULL);

		if (deletes->len)
			g_string_append_printf (inner, "<delete>%s</delete>", deletes->str);
		if (updates->len)
			g_string_append_printf (inner, "<update>%s</update>", updates->str);
		g_debug ("travel time of %s: %s", item_id, inner->str);
		success = e_gw_connection_modify_item_sync (cnc, item_id, inner->str, cancellable, error);
		g_string_free (inner, TRUE);
	}
	/* The appointment of a travel time taken away: gone for good */
	if (success && gone->len) {
		g_ptr_array_add (gone, NULL);
		if (!e_gw_connection_remove_items_sync (cnc, (const gchar * const *) gone->pdata, cbgw->calendar_id, cancellable, NULL) ||
		    !e_gw_connection_purge_sync (cnc, (const gchar * const *) gone->pdata, cancellable, NULL))
			g_debug ("travel time of %s: the appointment stays", item_id);
	}
	if (success && (updates->len || gone->len))
		e_cal_meta_backend_schedule_refresh (E_CAL_META_BACKEND (cbgw));

	g_string_free (updates, TRUE);
	g_string_free (deletes, TRUE);
	g_ptr_array_unref (gone);

	return success;
}

/* The travel times the listing found own copies for */
static void
set_pending_travel (ECalBackendGroupwise *cbgw,
		    EGwConnection *cnc,
		    GPtrArray *todo,
		    GCancellable *cancellable)
{
	guint ii;

	for (ii = 0; ii + 1 < todo->len; ii += 2) {
		const gchar *own_id = todo->pdata[ii], *uid = todo->pdata[ii + 1];
		ICalComponent *wanted;
		EGwResponse *response;
		xmlNode *item = NULL;

		g_rec_mutex_lock (&cbgw->lock);
		wanted = g_hash_table_lookup (cbgw->pending_travel, uid);
		if (wanted)
			g_object_ref (wanted);
		g_hash_table_remove (cbgw->pending_travel, uid);
		g_rec_mutex_unlock (&cbgw->lock);
		if (!wanted)
			continue;

		response = get_item (cnc, own_id, &item, cancellable, NULL);
		if (item && !sync_travel_time (cbgw, cnc, own_id, item, wanted, cancellable, NULL))
			g_debug ("travel time of the new meeting %s: not set", uid);
		else if (item)
			g_debug ("travel time of the new meeting %s: set on %s", uid, own_id);
		e_gw_response_free (response);
		g_object_unref (wanted);
	}
}

/* The attendee of @comp with that address */
static ICalProperty *
find_attendee (ICalComponent *comp,
	       const gchar *email)
{
	ICalProperty *prop;

	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		const gchar *address = e_cal_util_strip_mailto (i_cal_property_get_attendee (prop));

		if (address && email && g_ascii_strcasecmp (address, email) == 0)
			return prop;
	}

	return NULL;
}

/* The resources among the attendees of a meeting, as the GroupWise client
 * sends them: with their name (without it the POA makes "Beamer Beamer"),
 * and a resource of the kind place (a room) gives a meeting without
 * location its name as location. Which attendees are resources the POA
 * tells (resolve); a resource's entry in the system address book (ID
 * "<uuid>@55") has the name and whether it is a place. */
static void
resources_from_directory (EGwConnection *cnc,
			  ICalComponent *comp,
			  const gchar *user_email,
			  GCancellable *cancellable)
{
	GString *inner;
	EGwResponse *response;
	ICalProperty *prop;
	xmlNode *recipient;
	const gchar *location;
	gboolean any = FALSE;

	if (i_cal_component_isa (comp) != I_CAL_VEVENT_COMPONENT)
		return;

	inner = g_string_new ("<recipients>");
	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		const gchar *email = e_cal_util_strip_mailto (i_cal_property_get_attendee (prop));

		if (!email || !*email || (user_email && g_ascii_strcasecmp (email, user_email) == 0))
			continue;
		g_string_append (inner, "<recipient>");
		e_gw_xml_add_leaf (inner, "email", email);
		g_string_append (inner, "</recipient>");
		any = TRUE;
	}
	g_string_append (inner, "</recipients>");
	if (!any) {
		g_string_free (inner, TRUE);
		return;
	}

	response = e_gw_connection_call_sync (cnc, "resolve", inner->str, cancellable, NULL);
	g_string_free (inner, TRUE);
	if (!response)
		return;

	for (recipient = e_gw_xml_first_child (e_gw_xml_find (e_gw_response_get_node (response), "recipients"), "recipient");
	     recipient; recipient = e_gw_xml_next_sibling (recipient, "recipient")) {
		gchar *type = e_gw_xml_dup_text (recipient, "recipType");
		gchar *uuid = e_gw_xml_dup_text (recipient, "uuid");

		if (g_strcmp0 (type, "Resource") == 0 && uuid && *uuid) {
			gchar *id = g_strconcat (uuid, "@55:GroupWiseSystemAddressBook@52", NULL);
			gchar *email = e_gw_xml_dup_text (recipient, "email");
			EGwResponse *entry_response;
			xmlNode *entry = NULL;

			entry_response = get_item (cnc, id, &entry, cancellable, NULL);
			if (entry) {
				gchar *name = e_gw_xml_dup_text (entry, "name");
				ICalProperty *attendee = find_attendee (comp, email);

				if (attendee && name && *name) {
					i_cal_property_remove_parameter_by_kind (attendee, I_CAL_CN_PARAMETER);
					i_cal_property_take_parameter (attendee, i_cal_parameter_new_cn (name));
					i_cal_property_remove_parameter_by_kind (attendee, I_CAL_CUTYPE_PARAMETER);
					i_cal_property_take_parameter (attendee, i_cal_parameter_new_cutype (
						e_gw_xml_get_bool (entry, "flags/place") ? I_CAL_CUTYPE_ROOM : I_CAL_CUTYPE_RESOURCE));
				}
				location = i_cal_component_get_location (comp);
				if (e_gw_xml_get_bool (entry, "flags/place") && name && *name && !(location && *location)) {
					g_debug ("location of the meeting: the place %s", name);
					i_cal_component_set_location (comp, name);
				}
				g_clear_object (&attendee);
				g_free (name);
			}
			e_gw_response_free (entry_response);
			g_free (email);
			g_free (id);
		}
		g_free (type);
		g_free (uuid);
	}
	e_gw_response_free (response);
}

/* An appointment of the preparation or travel time of another one, as the
 * user moved or resized it: the POA does not keep them together itself.
 * Moved, it moves its appointment (the POA moves the travel time along);
 * resized, it becomes the travel time of its appointment. */
static gboolean
save_travel_time (ECalBackendGroupwise *cbgw,
		  EGwConnection *cnc,
		  xmlNode *current,
		  ICalComponent *comp,
		  GCancellable *cancellable,
		  GError **error)
{
	gchar *old_start = e_gw_xml_dup_text (current, "startDate"), *old_end = e_gw_xml_dup_text (current, "endDate");
	gchar *new_start = NULL, *new_end = NULL, *backlink, *main_id, *text, *this_id;
	GDateTime *os, *oe, *ns, *ne;
	EGwResponse *response;
	xmlNode *main_item;
	GString *updates;
	gboolean success = TRUE;

	if (!e_gw_calendar_event_range (comp, E_TIMEZONE_CACHE (cbgw), user_zone (), &new_start, &new_end) ||
	    !old_start || !old_end) {
		g_free (old_start);
		g_free (old_end);
		g_free (new_start);
		g_free (new_end);
		return TRUE;
	}

	os = g_date_time_new_from_iso8601 (old_start, NULL);
	oe = g_date_time_new_from_iso8601 (old_end, NULL);
	ns = g_date_time_new_from_iso8601 (new_start, NULL);
	ne = g_date_time_new_from_iso8601 (new_end, NULL);
	g_free (old_start);
	g_free (old_end);
	g_free (new_start);
	g_free (new_end);
	if (!os || !oe || !ns || !ne || (g_date_time_equal (os, ns) && g_date_time_equal (oe, ne))) {
		/* Other changes: the POA keeps these appointments as it wants them */
		g_clear_pointer (&os, g_date_time_unref);
		g_clear_pointer (&oe, g_date_time_unref);
		g_clear_pointer (&ns, g_date_time_unref);
		g_clear_pointer (&ne, g_date_time_unref);
		return TRUE;
	}

	backlink = e_gw_xml_dup_text (current, "travelAppointmentBacklink");
	main_id = g_strconcat (backlink, "@4", NULL);
	response = get_item (cnc, main_id, &main_item, cancellable, error);
	if (!response) {
		success = FALSE;
	} else {
		updates = g_string_new ("<update>");
		if (g_date_time_difference (oe, os) == g_date_time_difference (ne, ns)) {
			/* Moved: the appointment by as much */
			GTimeSpan delta = g_date_time_difference (ns, os);
			gchar *main_start = e_gw_xml_dup_text (main_item, "startDate");
			gchar *main_end = e_gw_xml_dup_text (main_item, "endDate");
			GDateTime *ms = main_start ? g_date_time_new_from_iso8601 (main_start, NULL) : NULL;
			GDateTime *me = main_end ? g_date_time_new_from_iso8601 (main_end, NULL) : NULL;

			if (ms && me) {
				GDateTime *moved_start = g_date_time_add (ms, delta), *moved_end = g_date_time_add (me, delta);

				text = g_date_time_format (moved_start, "%Y-%m-%dT%H:%M:%SZ");
				e_gw_xml_add_leaf (updates, "startDate", text);
				g_free (text);
				text = g_date_time_format (moved_end, "%Y-%m-%dT%H:%M:%SZ");
				e_gw_xml_add_leaf (updates, "endDate", text);
				g_free (text);
				g_date_time_unref (moved_start);
				g_date_time_unref (moved_end);
			}
			g_clear_pointer (&ms, g_date_time_unref);
			g_clear_pointer (&me, g_date_time_unref);
			g_free (main_start);
			g_free (main_end);
			g_debug ("travel time %s moved: its appointment %s by %" G_GINT64_FORMAT " s", backlink, main_id,
				delta / G_TIME_SPAN_SECOND);
		} else {
			/* Resized: the travel time of its appointment */
			gchar *before = e_gw_xml_dup_text (main_item, "travelAppointmentBefore");
			gchar *seconds = g_strdup_printf ("%" G_GINT64_FORMAT, g_date_time_difference (ne, ns) / G_TIME_SPAN_SECOND);

			text = e_gw_xml_dup_text (current, "id");
			this_id = e_gw_clean_id (text);
			g_free (text);
			/* "BASE@4:CONTAINER" -> "BASE" */
			if (strchr (this_id, '@'))
				*strchr (this_id, '@') = '\0';
			e_gw_xml_add_leaf (updates, g_strcmp0 (before, this_id) == 0 ? "travelTimeBefore" : "travelTimeAfter", seconds);
			g_debug ("travel time %s of %s: %s s", this_id, main_id, seconds);
			g_free (seconds);
			g_free (before);
			g_free (this_id);
		}
		g_string_append (updates, "</update>");
		if (updates->len > strlen ("<update></update>"))
			success = e_gw_connection_modify_item_sync (cnc, main_id, updates->str, cancellable, error);
		g_string_free (updates, TRUE);
		e_gw_response_free (response);
		/* The POA moved the other appointments: read them anew */
		e_cal_meta_backend_schedule_refresh (E_CAL_META_BACKEND (cbgw));
	}

	g_free (main_id);
	g_free (backlink);
	g_date_time_unref (os);
	g_date_time_unref (oe);
	g_date_time_unref (ns);
	g_date_time_unref (ne);

	return success;
}

static gboolean
save_existing (ECalBackendGroupwise *cbgw,
	       EGwConnection *cnc,
	       const gchar *extra,
	       ICalComponent *comp,
	       GCancellable *cancellable,
	       GError **error)
{
	EGwResponse *response;
	GError *local_error = NULL;
	xmlNode *current;
	gchar *source, *updates, *id, *own_copy_id = NULL;
	gboolean success;

	id = split_extra (extra, &own_copy_id);
	response = get_item (cnc, id, &current, cancellable, &local_error);
	if (!response) {
		propagate_error (error, local_error);
		g_free (id);
		g_free (own_copy_id);
		return FALSE;
	}

	source = e_gw_xml_dup_text (current, "source");
	g_debug ("change %s (%s)", id, source ? source : "?");
	if (e_gw_xml_find (current, "travelAppointmentBacklink")) {
		success = save_travel_time (cbgw, cnc, current, comp, cancellable, &local_error);
	} else if (g_strcmp0 (source, "received") == 0) {
		success = save_received (cbgw, cnc, id, current, comp, cancellable, &local_error);
	} else {
		ICalComponent *to_save = comp;

		/* Alarm and subject of the user's own copy are not the meeting's:
		 * changed there, they do not send the meeting again */
		if (own_copy_id) {
			ICalComponent *before = e_gw_calendar_component_from_item (current, user_zone (), NULL);

			to_save = i_cal_component_clone (comp);
			copy_personal (before, to_save);
			g_object_unref (before);
		}

		/* Resources invited: their names and a place as location, as on a
		 * new meeting */
		if (g_strcmp0 (source, "sent") == 0 && i_cal_component_count_properties (comp, I_CAL_ATTENDEE_PROPERTY) > 0) {
			gchar *user_email = dup_user_email (cbgw);

			if (to_save == comp)
				to_save = i_cal_component_clone (comp);
			resources_from_directory (cnc, to_save, user_email, cancellable);
			g_free (user_email);
		}

		/* A sent appointment changed is sent again to the attendees */
		updates = e_gw_calendar_updates_xml (current, to_save, E_TIMEZONE_CACHE (cbgw), user_zone (), &local_error);
		success = updates && (!*updates || e_gw_connection_modify_item_sync (cnc, id, updates, cancellable, &local_error));
		g_free (updates);
		if (to_save != comp)
			g_object_unref (to_save);

		if (success && own_copy_id) {
			EGwResponse *own_response;
			xmlNode *own_copy;

			own_response = get_item (cnc, own_copy_id, &own_copy, cancellable, NULL);
			if (own_copy)
				success = apply_personal (cnc, own_copy_id, own_copy, comp, cancellable, &local_error);
			e_gw_response_free (own_response);
		}

		if (success && i_cal_component_isa (comp) == I_CAL_VTODO_COMPONENT) {
			ICalComponent *before = e_gw_calendar_component_from_item (current, user_zone (), NULL);
			gboolean completed = e_gw_calendar_is_completed (comp);

			if (completed != e_gw_calendar_is_completed (before))
				success = e_gw_connection_complete_sync (cnc, id, completed, cancellable, &local_error);
			g_object_unref (before);
		}
	}
	/* The travel time is the user's: on the own copy of a meeting (a sent
	 * one without has no place for it) */
	if (success && !e_gw_xml_find (current, "travelAppointmentBacklink") &&
	    (own_copy_id || g_strcmp0 (source, "sent") != 0)) {
		if (own_copy_id) {
			EGwResponse *own_response;
			xmlNode *own_copy = NULL;

			own_response = get_item (cnc, own_copy_id, &own_copy, cancellable, NULL);
			if (own_copy)
				success = sync_travel_time (cbgw, cnc, own_copy_id, own_copy, comp, cancellable, &local_error);
			e_gw_response_free (own_response);
		} else {
			success = sync_travel_time (cbgw, cnc, id, current, comp, cancellable, &local_error);
		}
	}

	/* The categories are the user's: on the own copy of a meeting (a
	 * declined invitation is gone) */
	if (success && e_gw_calendar_user_partstat (comp, cbgw->user_email) != I_CAL_PARTSTAT_DECLINED)
		success = save_categories (cbgw, cnc, own_copy_id ? own_copy_id : id, comp, cancellable, &local_error);

	g_free (source);
	g_free (id);
	g_free (own_copy_id);
	e_gw_response_free (response);

	propagate_error (error, local_error);

	return success;
}

/* The ID of the new item as the listing has it: sendItem returns one in the
 * Mailbox; a personal item is in the calendar folder, a sent one in the
 * Sent Items, with the same item part */
static gchar *
created_item (ECalBackendGroupwise *cbgw,
	      EGwConnection *cnc,
	      const gchar *sent_id,
	      gboolean sent,
	      gchar **out_uid,
	      GCancellable *cancellable)
{
	const gchar *candidates[3];
	gchar *in_folder = e_gw_item_id_in_container (sent_id,
		sent && cbgw->sent_folder_id ? cbgw->sent_folder_id : cbgw->calendar_id);
	gchar *in_sub = cbgw->role == ROLE_OWN && !sent ? e_gw_item_id_in_container (sent_id, cbgw->folder_id) : NULL;
	gchar *found = NULL;
	guint ii;

	/* A subcalendar lists its appointment under its ID there */
	candidates[0] = in_sub;
	candidates[1] = in_folder;
	candidates[2] = sent_id;
	for (ii = 0; ii < G_N_ELEMENTS (candidates) && !found; ii++) {
		EGwResponse *response;
		xmlNode *item;

		if (!candidates[ii])
			continue;
		response = get_item (cnc, candidates[ii], &item, cancellable, NULL);
		if (item) {
			found = g_strdup (candidates[ii]);
			*out_uid = e_gw_calendar_uid (item);
		}
		e_gw_response_free (response);
	}
	g_free (in_folder);
	g_free (in_sub);

	return found;
}

/* Items just moved into another calendar of the account: removing them
 * from the calendar they came from is only the local part of the move.
 * The GroupWise calendars share one process. */
static GMutex moved_lock;
static GHashTable *moved;	/* item base -> monotonic time of the move */

#define MOVED_SECONDS 300

static void
note_moved (const gchar *id)
{
	gchar *base = item_base (id);

	if (!base)
		return;
	g_mutex_lock (&moved_lock);
	if (!moved)
		moved = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_insert (moved, base, GINT_TO_POINTER ((gint) (g_get_monotonic_time () / G_USEC_PER_SEC)));
	g_mutex_unlock (&moved_lock);
}

static gboolean
take_moved (const gchar *id)
{
	gchar *base = item_base (id);
	gpointer when = NULL;
	gboolean found;

	g_mutex_lock (&moved_lock);
	found = base && moved && g_hash_table_lookup_extended (moved, base, NULL, &when);
	if (found)
		g_hash_table_remove (moved, base);
	g_mutex_unlock (&moved_lock);
	g_free (base);

	return found && g_get_monotonic_time () / G_USEC_PER_SEC - GPOINTER_TO_INT (when) < MOVED_SECONDS;
}

static const gchar *
container_of (const gchar *id)
{
	const gchar *colon = id ? strchr (id, ':') : NULL;

	return colon ? colon + 1 : NULL;
}

/* A component of another calendar of this account, to be moved here: the
 * item is linked into this calendar, or moved from the subcalendar it is
 * in, as the GroupWise client does. A copy (Evolution gives it a new UID)
 * or a component of another mailbox is no move. Returns FALSE when it is
 * none; @out_error tells a failed move. */
static gboolean
move_here (ECalBackendGroupwise *cbgw,
	   EGwConnection *cnc,
	   ICalComponent *comp,
	   gchar **out_new_uid,
	   gchar **out_new_extra,
	   GCancellable *cancellable,
	   GError **out_error)
{
	gchar *account = e_cal_util_component_dup_x_property (comp, X_ACCOUNT);
	gchar *item_id = e_cal_util_component_dup_x_property (comp, E_GW_CALENDAR_X_ITEM_ID);
	gchar *own_copy_id = e_cal_util_component_dup_x_property (comp, X_OWN_COPY_ID);
	gchar *source = e_cal_util_component_dup_x_property (comp, E_GW_CALENDAR_X_SOURCE);
	const gchar *uid = i_cal_component_get_uid (comp);
	const gchar *to_move, *from, *here;
	gchar *item_uid = NULL, *moved_id = NULL;
	EGwResponse *response;
	xmlNode *item = NULL;
	gboolean is_move = FALSE;

	if (!account || !item_id || !uid || !cbgw->user_email || g_ascii_strcasecmp (account, cbgw->user_email) != 0 ||
	    (cbgw->role != ROLE_MAIN && cbgw->role != ROLE_OWN))
		goto out;

	/* The own copy of a meeting says in which calendar it is */
	to_move = own_copy_id && *own_copy_id ? own_copy_id : item_id;
	response = get_item (cnc, to_move, &item, cancellable, NULL);
	if (item)
		item_uid = e_gw_calendar_uid (item);
	e_gw_response_free (response);
	if (!item_uid || !g_str_has_prefix (uid, item_uid))
		goto out;

	is_move = TRUE;
	here = cbgw->role == ROLE_OWN ? cbgw->folder_id : cbgw->calendar_id;
	from = container_of (to_move);
	/* Out of a subcalendar; from the Calendar (or the Sent Items) it is
	 * only linked, it stays in the Calendar */
	if (g_strcmp0 (from, cbgw->calendar_id) == 0 || g_strcmp0 (from, cbgw->sent_folder_id) == 0)
		from = NULL;
	if (g_strcmp0 (from, here) != 0 && (from || cbgw->role == ROLE_OWN)) {
		const gchar *ids[] = { to_move, NULL };

		g_debug ("move %s into %s%s%s", to_move, here, from ? " from " : "", from ? from : "");
		if (!e_gw_connection_move_items_sync (cnc, ids, here, from, cancellable, out_error)) {
			g_free (item_uid);
			goto out;
		}
	}

	note_moved (item_id);
	if (own_copy_id && *own_copy_id)
		note_moved (own_copy_id);
	/* Its travel time came along (the POA links it): it shows here at once */
	if (e_cal_util_component_has_x_property (comp, E_GW_CALENDAR_X_TRAVEL_BEFORE) ||
	    e_cal_util_component_has_x_property (comp, E_GW_CALENDAR_X_TRAVEL_AFTER))
		e_cal_meta_backend_schedule_refresh (E_CAL_META_BACKEND (cbgw));

	/* The IDs the listing of this calendar gives */
	moved_id = e_gw_item_id_in_container (to_move, g_strcmp0 (source, "sent") == 0 && !(own_copy_id && *own_copy_id) &&
		cbgw->role == ROLE_MAIN && cbgw->sent_folder_id ? cbgw->sent_folder_id : here);
	if (own_copy_id && *own_copy_id)
		*out_new_extra = g_strconcat (item_id, "\n", moved_id, NULL);
	else
		*out_new_extra = g_strdup (moved_id);
	*out_new_uid = g_strdup (uid);
	g_free (moved_id);
	g_free (item_uid);

 out:
	g_free (account);
	g_free (item_id);
	g_free (own_copy_id);
	g_free (source);

	return is_move;
}

static gboolean
ecb_groupwise_save_component_sync (ECalMetaBackend *meta_backend,
				   gboolean overwrite_existing,
				   EConflictResolution conflict_resolution,
				   const GSList *instances,
				   const gchar *extra,
				   ECalOperationFlags opflags,
				   gchar **out_new_uid,
				   gchar **out_new_extra,
				   GCancellable *cancellable,
				   GError **error)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (meta_backend);
	ICalComponent *comp = main_component (instances);
	EGwConnection *cnc;
	GError *local_error = NULL;
	gboolean success = FALSE;

	if (!comp) {
		g_propagate_error (error, e_cal_client_error_create (E_CAL_CLIENT_ERROR_INVALID_OBJECT, NULL));
		return FALSE;
	}

	cnc = ref_connection (cbgw, error);
	if (!cnc)
		return FALSE;

	if (overwrite_existing && extra && *extra) {
		success = save_existing (cbgw, cnc, extra, comp, cancellable, error);
		if (success) {
			/* Loaded again: a sent appointment may have a new ID now */
			*out_new_uid = g_strdup (i_cal_component_get_uid (comp));
			*out_new_extra = g_strdup (extra);
		}
	} else if (move_here (cbgw, cnc, comp, out_new_uid, out_new_extra, cancellable, &local_error)) {
		success = !local_error;
		propagate_error (error, local_error);
	} else {
		ICalComponent *to_send = i_cal_component_clone (comp);
		gchar *user_email = dup_user_email (cbgw), *xml, *sent_id = NULL;
		gboolean meeting;

		/* Not to be sent: only for the user's own calendar */
		if (opflags & E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE) {
			ICalProperty *prop;

			while ((prop = i_cal_component_get_first_property (to_send, I_CAL_ATTENDEE_PROPERTY))) {
				i_cal_component_remove_property (to_send, prop);
				g_object_unref (prop);
			}
		}

		/* The travel time of a meeting is the organizer's own, not the
		 * attendees': it goes to the own copy afterwards (with the user
		 * among the attendees; else there is none) */
		meeting = e_gw_calendar_is_meeting (to_send, user_email);
		if (meeting) {
			e_cal_util_component_remove_x_property (to_send, E_GW_CALENDAR_X_TRAVEL_BEFORE);
			e_cal_util_component_remove_x_property (to_send, E_GW_CALENDAR_X_TRAVEL_AFTER);
		}

		if (meeting)
			resources_from_directory (cnc, to_send, user_email, cancellable);
		xml = e_gw_calendar_item_xml (to_send, E_TIMEZONE_CACHE (cbgw), user_zone (), user_email, &local_error);
		/* Into the subcalendar, as the GroupWise client does (every
		 * appointment of a series) */
		if (xml && cbgw->role == ROLE_OWN) {
			const gchar *tag_end = strchr (xml, '>');
			GString *with_container = g_string_new_len (xml, tag_end ? tag_end - xml + 1 : 0);

			e_gw_xml_add_leaf (with_container, "container", cbgw->folder_id);
			g_string_append (with_container, tag_end ? tag_end + 1 : xml);
			g_free (xml);
			xml = g_string_free (with_container, FALSE);
		}
		if (xml)
			sent_id = e_gw_connection_send_item_sync (cnc, xml, cancellable, &local_error);
		if (sent_id) {
			gchar *uid = NULL;
			gchar *id = created_item (cbgw, cnc, sent_id, strstr (xml, "<source>sent</source>") != NULL, &uid, cancellable);

			success = TRUE;
			if (id && i_cal_component_isa (comp) == I_CAL_VTODO_COMPONENT && e_gw_calendar_is_completed (comp))
				success = e_gw_connection_complete_sync (cnc, id, TRUE, cancellable, &local_error);
			if (success && id && e_cal_util_component_has_property (comp, I_CAL_CATEGORIES_PROPERTY))
				success = save_categories (cbgw, cnc, id, comp, cancellable, &local_error);
			*out_new_uid = uid ? uid : g_strdup (i_cal_component_get_uid (comp));

			/* The travel time of a new meeting goes to the user's own
			 * copy, which the next listing finds (its ID is not the
			 * sent one's; a search for it made the POA drop out) */
			if (success && meeting && e_gw_calendar_user_partstat (comp, user_email) != I_CAL_PARTSTAT_NONE &&
			    (e_cal_util_component_has_x_property (comp, E_GW_CALENDAR_X_TRAVEL_BEFORE) ||
			     e_cal_util_component_has_x_property (comp, E_GW_CALENDAR_X_TRAVEL_AFTER))) {
				g_rec_mutex_lock (&cbgw->lock);
				g_hash_table_insert (cbgw->pending_travel, g_strdup (*out_new_uid), i_cal_component_clone (comp));
				g_rec_mutex_unlock (&cbgw->lock);
				e_cal_meta_backend_schedule_refresh (meta_backend);
			}
			*out_new_extra = id ? id : g_strdup (sent_id);

			/* A series is one item per instance: they come with the next sync */
			if (e_cal_util_component_has_property (comp, I_CAL_RRULE_PROPERTY))
				e_cal_meta_backend_schedule_refresh (meta_backend);
		}

		g_free (sent_id);
		g_free (xml);
		g_free (user_email);
		g_object_unref (to_send);
		propagate_error (error, local_error);
	}

	g_object_unref (cnc);

	return success;
}

/* Deleting a meeting the user organized, Evolution asks (capability
 * "retract-supported") for a reason and whether the attendees are told, as
 * the GroupWise client asks whether the meeting goes from the recipients'
 * mailboxes too, with a retraction comment. With "yes" the meeting comes as
 * a CANCEL to send (ecb_groupwise_send_objects_sync), the reason its
 * COMMENT, and then the removal without message; with "no" only the
 * removal. The two come from threads of their own: the removal waits a
 * moment for the CANCEL of its meeting. */
#define CANCEL_WAIT_SECONDS 1
#define CANCEL_KEEP_SECONDS 60

static GMutex cancel_lock;
static GCond cancel_cond;
static GHashTable *cancelled;	/* UID -> monotonic time of the retract */

static void
note_cancelled (const gchar *uid)
{
	/* cancel_lock is held */
	if (!cancelled)
		cancelled = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_insert (cancelled, g_strdup (uid), GINT_TO_POINTER ((gint) (g_get_monotonic_time () / G_USEC_PER_SEC)));
	g_cond_broadcast (&cancel_cond);
}

/* Lets a CANCEL of the meeting go first; returns whether there was one */
static gboolean
wait_for_cancel (const gchar *uid)
{
	gint64 until = g_get_monotonic_time () + CANCEL_WAIT_SECONDS * G_USEC_PER_SEC;
	gint now = (gint) (g_get_monotonic_time () / G_USEC_PER_SEC);
	gboolean found = FALSE;
	gpointer when;

	g_mutex_lock (&cancel_lock);
	while (!(cancelled && g_hash_table_lookup_extended (cancelled, uid, NULL, &when))) {
		if (!g_cond_wait_until (&cancel_cond, &cancel_lock, until))
			break;
	}
	if (cancelled && g_hash_table_lookup_extended (cancelled, uid, NULL, &when)) {
		found = now - GPOINTER_TO_INT (when) <= CANCEL_KEEP_SECONDS;
		g_hash_table_remove (cancelled, uid);
	}
	g_mutex_unlock (&cancel_lock);

	return found;
}

/* The meeting of a CANCEL: withdrawn from the mailboxes of the attendees
 * (and the user's own), with the reason. Returns FALSE with an error only. */
static gboolean
retract_meeting (ECalBackendGroupwise *cbgw,
		 EGwConnection *cnc,
		 ECalCache *cache,
		 ICalComponent *comp,
		 GCancellable *cancellable,
		 GError **error)
{
	const gchar *uid = i_cal_component_get_uid (comp);
	const gchar *comment = i_cal_component_get_comment (comp);
	gchar *extra = NULL, *id, *source;
	EGwResponse *response;
	xmlNode *item = NULL;
	gboolean success = TRUE;

	if (!uid || !e_cal_cache_get_component_extra (cache, uid, NULL, &extra, cancellable, NULL) || !extra) {
		g_free (extra);
		return TRUE;
	}

	/* The removal that follows waits for this */
	g_mutex_lock (&cancel_lock);
	id = split_extra (extra, NULL);
	response = get_item (cnc, id, &item, cancellable, NULL);
	source = item ? e_gw_xml_dup_text (item, "source") : NULL;
	e_gw_response_free (response);
	if (g_strcmp0 (source, "sent") == 0) {
		g_debug ("retract %s%s", id, comment && *comment ? " with a comment" : "");
		success = e_gw_connection_retract_sync (cnc, id, comment && *comment ? comment : NULL, cancellable, error);
		if (success)
			note_cancelled (uid);
	}
	g_mutex_unlock (&cancel_lock);

	g_free (source);
	g_free (id);
	g_free (extra);

	return success;
}

static void
ecb_groupwise_send_objects_sync (ECalBackendSync *sync_backend,
				 EDataCal *cal,
				 GCancellable *cancellable,
				 const gchar *calobj,
				 ECalOperationFlags opflags,
				 GSList **users,
				 gchar **modified_calobj,
				 GError **error)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (sync_backend);
	ICalComponent *toplevel = i_cal_component_new_from_string (calobj), *sub;
	ICalComponentKind kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbgw));
	GError *local_error = NULL;

	if (toplevel && i_cal_component_isa (toplevel) == I_CAL_VCALENDAR_COMPONENT &&
	    e_cal_util_component_has_property (toplevel, I_CAL_METHOD_PROPERTY) &&
	    i_cal_component_get_method (toplevel) == I_CAL_METHOD_CANCEL &&
	    e_cal_meta_backend_ensure_connected_sync (E_CAL_META_BACKEND (cbgw), cancellable, NULL)) {
		EGwConnection *cnc = ref_connection (cbgw, NULL);
		ECalCache *cache = e_cal_meta_backend_ref_cache (E_CAL_META_BACKEND (cbgw));
		gboolean success = TRUE;

		for (sub = i_cal_component_get_first_component (toplevel, kind); sub && cnc && cache && success;
		     g_object_unref (sub), sub = i_cal_component_get_next_component (toplevel, kind))
			success = retract_meeting (cbgw, cnc, cache, sub, cancellable, &local_error);
		g_clear_object (&sub);
		g_clear_object (&cache);
		g_clear_object (&cnc);
	}
	g_clear_object (&toplevel);

	if (local_error) {
		propagate_error (error, local_error);
		return;
	}

	/* GroupWise tells the attendees itself: nothing for Evolution to mail */
	E_CAL_BACKEND_SYNC_CLASS (e_cal_backend_groupwise_parent_class)->send_objects_sync (sync_backend, cal,
		cancellable, calobj, opflags, users, modified_calobj, error);
}

static gboolean
ecb_groupwise_remove_component_sync (ECalMetaBackend *meta_backend,
				     EConflictResolution conflict_resolution,
				     const gchar *uid,
				     const gchar *extra,
				     const gchar *object,
				     ECalOperationFlags opflags,
				     GCancellable *cancellable,
				     GError **error)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (meta_backend);
	const gchar *ids[] = { NULL, NULL };
	EGwConnection *cnc;
	EGwResponse *response;
	GError *local_error = NULL;
	xmlNode *item;
	gchar *source, *id, *own_copy_id = NULL;
	gboolean success, retracted = FALSE;

	if (!extra || !*extra)
		return TRUE;

	/* Moved into another calendar of the account: gone from this one only */
	{
		gchar *own_copy_id = NULL, *main_id = split_extra (extra, &own_copy_id);
		gboolean was_moved = take_moved (own_copy_id ? own_copy_id : main_id);

		if (own_copy_id)
			take_moved (main_id);
		g_free (own_copy_id);
		g_free (main_id);
		if (was_moved) {
			g_debug ("%s moved into another calendar", extra);
			/* Its travel time went along: out of this calendar too */
			if (object && strstr (object, "X-GW-TRAVEL-"))
				e_cal_meta_backend_schedule_refresh (meta_backend);
			return TRUE;
		}
	}

	/* A meeting deleted "without telling": its CANCEL may still be on the
	 * way, and has withdrawn it then */
	if ((opflags & E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE) && uid && object && strstr (object, "ATTENDEE") &&
	    wait_for_cancel (uid)) {
		retracted = TRUE;
		g_debug ("%s was retracted", uid);
	}

	cnc = ref_connection (cbgw, error);
	if (!cnc)
		return FALSE;

	/* A retract takes the user's own copy of a meeting along */
	id = split_extra (extra, &own_copy_id);
	ids[0] = id;
	response = get_item (cnc, id, &item, cancellable, &local_error);
	if (!response) {
		g_object_unref (cnc);
		g_free (id);
		g_free (own_copy_id);
		/* Gone already (a retracted invitation) */
		if (g_error_matches (local_error, E_CAL_CLIENT_ERROR, E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND) ||
		    g_error_matches (local_error, E_GW_ERROR, E_GW_ERROR_ITEM_NOT_FOUND)) {
			g_clear_error (&local_error);
			return TRUE;
		}
		propagate_error (error, local_error);
		return FALSE;
	}
	/* The preparation or travel time of an appointment: the POA would
	 * leave the appointment with it, and it cannot be taken away there */
	if (e_gw_xml_find (item, "travelAppointmentBacklink")) {
		gchar *subject = e_gw_xml_dup_text (item, "subject");

		e_gw_response_free (response);
		g_object_unref (cnc);
		g_free (id);
		g_free (own_copy_id);
		g_set_error (error, E_CLIENT_ERROR, E_CLIENT_ERROR_PERMISSION_DENIED,
			_("“%s” is the preparation or travel time of an appointment. Move or delete the appointment "
			  "itself; the travel time is changed or taken away on the page “Travel Time” of the appointment."), subject ? subject : "");
		g_free (subject);
		return FALSE;
	}

	source = e_gw_xml_dup_text (item, "source");
	e_gw_response_free (response);

	/* A meeting of the user's deleted without telling the attendees: the
	 * user leaves it, as "only my mailbox" in the GroupWise client — the
	 * own copy goes (and with it the meeting from the calendars), what
	 * was sent stays in the Sent Items with the answers, to be retracted
	 * from there later; the attendees keep it */
	if (g_strcmp0 (source, "sent") == 0 && (opflags & E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE) &&
	    !retracted && own_copy_id) {
		g_debug ("%s: only the own copy %s goes", id, own_copy_id);
		g_free (id);
		id = g_steal_pointer (&own_copy_id);
		ids[0] = id;
		g_free (source);
		source = g_strdup ("received");
	}

	/* An appointment the user sent is withdrawn from the attendees, unless
	 * the user chose not to tell them */
	if (g_strcmp0 (source, "sent") == 0) {
		/* The user's own copy in the Sent Items goes as well */
		success = (opflags & E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE) ||
			e_gw_connection_retract_sync (cnc, id, NULL, cancellable, &local_error);
		if (success)
			success = e_gw_connection_remove_items_sync (cnc, ids,
				cbgw->sent_folder_id ? cbgw->sent_folder_id : cbgw->folder_id, cancellable, &local_error);
		/* ... and its link into the subcalendar */
		if (success && cbgw->role == ROLE_OWN) {
			gchar *in_sub = e_gw_item_id_in_container (id, cbgw->folder_id);
			const gchar *sub_ids[] = { in_sub, NULL };

			if (!e_gw_connection_remove_items_sync (cnc, sub_ids, cbgw->folder_id, cancellable, NULL))
				g_debug ("%s was not linked into the subcalendar", id);
			g_free (in_sub);
		}
	} else {
		/* Out of every folder the appointment is in: the Calendar shows
		 * the content of its subcalendars, a subcalendar's appointment is
		 * in the Calendar too */
		gchar **subs = cbgw->role == ROLE_OWN ? g_strdupv ((gchar **) (const gchar *[]) { cbgw->folder_id, NULL }) :
			dup_item_subcalendars (cbgw, id);
		guint ii;

		success = TRUE;
		for (ii = 0; success && subs[ii]; ii++) {
			gchar *in_sub = e_gw_item_id_in_container (id, subs[ii]);
			const gchar *sub_ids[] = { in_sub, NULL };

			success = e_gw_connection_remove_items_sync (cnc, sub_ids, subs[ii], cancellable, &local_error);
			g_free (in_sub);
		}
		g_strfreev (subs);
		if (success) {
			gchar *in_calendar = e_gw_item_id_in_container (id, cbgw->calendar_id);
			const gchar *calendar_ids[] = { in_calendar, NULL };

			success = e_gw_connection_remove_items_sync (cnc, calendar_ids, cbgw->calendar_id, cancellable, &local_error);
			/* A subcalendar's appointment need not be in the Calendar */
			if (!success && cbgw->role == ROLE_OWN &&
			    g_error_matches (local_error, E_GW_ERROR, E_GW_ERROR_ITEM_NOT_FOUND)) {
				g_clear_error (&local_error);
				success = TRUE;
			}
			g_free (in_calendar);
		}
	}

	g_free (source);
	g_free (id);
	g_free (own_copy_id);
	g_object_unref (cnc);
	propagate_error (error, local_error);

	return success;
}

/* "2026-09-28T07:30:00Z" */
static ICalTime *
parse_utc (const gchar *text)
{
	GDateTime *dt = text && *text ? g_date_time_new_from_iso8601 (text, NULL) : NULL;
	ICalTime *tt;

	if (!dt)
		return NULL;
	tt = i_cal_time_new_from_timet_with_zone (g_date_time_to_unix (dt), FALSE, i_cal_timezone_get_utc_timezone ());
	g_date_time_unref (dt);

	return tt;
}

static gchar *
utc_text (time_t tt)
{
	GDateTime *dt = g_date_time_new_from_unix_utc (tt);
	gchar *text = g_date_time_format (dt, "%Y-%m-%dT%H:%M:%SZ");

	g_date_time_unref (dt);

	return text;
}

/* The busy search of GroupWise: one VFREEBUSY per user it knows */
static void
ecb_groupwise_get_free_busy_sync (ECalBackendSync *sync_backend,
				  EDataCal *cal,
				  GCancellable *cancellable,
				  const GSList *users,
				  time_t start,
				  time_t end,
				  GSList **freebusyobjs,
				  GError **error)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (sync_backend);
	GPtrArray *emails = g_ptr_array_new_with_free_func (g_free);
	EGwConnection *cnc;
	EGwResponse *response;
	GError *local_error = NULL;
	xmlNode *user;
	const GSList *link;
	gchar *from, *to;

	*freebusyobjs = NULL;

	if (!e_cal_meta_backend_ensure_connected_sync (E_CAL_META_BACKEND (cbgw), cancellable, error))
		return;
	cnc = ref_connection (cbgw, error);
	if (!cnc)
		return;

	for (link = users; link; link = g_slist_next (link)) {
		const gchar *address = link->data;

		if (address && !g_ascii_strncasecmp (address, "mailto:", 7))
			address += 7;
		if (address && *address)
			g_ptr_array_add (emails, g_strdup (address));
	}
	g_ptr_array_add (emails, NULL);

	from = utc_text (start);
	to = utc_text (end);
	response = emails->len > 1 ? e_gw_connection_get_free_busy_sync (cnc, (const gchar * const *) emails->pdata,
		from, to, 15, cancellable, &local_error) : NULL;

	user = response ? e_gw_xml_find (e_gw_response_get_node (response), "freeBusyInfo") : NULL;
	for (user = e_gw_xml_first_child (user, "user"); user; user = e_gw_xml_next_sibling (user, "user")) {
		ICalComponent *vfb = i_cal_component_new_vfreebusy ();
		gchar *email = e_gw_xml_dup_text (user, "email"), *mailto, *name;
		ICalProperty *attendee;
		ICalTime *tt;
		xmlNode *block;

		mailto = g_strconcat ("mailto:", email, NULL);
		attendee = i_cal_property_new_attendee (mailto);
		name = e_gw_xml_dup_text (user, "displayName");
		if (name && *name)
			i_cal_property_take_parameter (attendee, i_cal_parameter_new_cn (name));
		i_cal_component_take_property (vfb, attendee);
		tt = i_cal_time_new_from_timet_with_zone (start, FALSE, i_cal_timezone_get_utc_timezone ());
		i_cal_component_set_dtstart (vfb, tt);
		g_object_unref (tt);
		tt = i_cal_time_new_from_timet_with_zone (end, FALSE, i_cal_timezone_get_utc_timezone ());
		i_cal_component_set_dtend (vfb, tt);
		g_object_unref (tt);

		for (block = e_gw_xml_first_child (e_gw_xml_find (user, "blocks"), "block"); block;
		     block = e_gw_xml_next_sibling (block, "block")) {
			gchar *level = e_gw_xml_dup_text (block, "acceptLevel");
			gchar *text_start = e_gw_xml_dup_text (block, "startDate");
			gchar *text_end = e_gw_xml_dup_text (block, "endDate");
			ICalTime *block_start = parse_utc (text_start), *block_end = parse_utc (text_end);
			ICalParameterFbtype fbtype = g_strcmp0 (level, "Tentative") == 0 ? I_CAL_FBTYPE_BUSYTENTATIVE :
				g_strcmp0 (level, "OutOfOffice") == 0 ? I_CAL_FBTYPE_BUSYUNAVAILABLE : I_CAL_FBTYPE_BUSY;

			if (block_start && block_end && g_strcmp0 (level, "Free") != 0) {
				ICalPeriod *period = i_cal_period_new_null_period ();
				ICalProperty *prop;

				i_cal_period_set_start (period, block_start);
				i_cal_period_set_end (period, block_end);
				prop = i_cal_property_new_freebusy (period);
				i_cal_property_take_parameter (prop, i_cal_parameter_new_fbtype (fbtype));
				i_cal_component_take_property (vfb, prop);
				g_object_unref (period);
			}
			g_clear_object (&block_start);
			g_clear_object (&block_end);
			g_free (level);
			g_free (text_start);
			g_free (text_end);
		}

		*freebusyobjs = g_slist_prepend (*freebusyobjs, i_cal_component_as_ical_string (vfb));
		g_object_unref (vfb);
		g_free (email);
		g_free (mailto);
		g_free (name);
	}
	*freebusyobjs = g_slist_reverse (*freebusyobjs);

	e_gw_response_free (response);
	g_ptr_array_unref (emails);
	g_free (from);
	g_free (to);
	g_object_unref (cnc);
	propagate_error (error, local_error);
}

/* @comp with the user's attendee answering @partstat */
static void
set_user_partstat (ICalComponent *comp,
		   const gchar *user_email,
		   ICalParameterPartstat partstat)
{
	ICalProperty *prop;

	for (prop = i_cal_component_get_first_property (comp, I_CAL_ATTENDEE_PROPERTY); prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_ATTENDEE_PROPERTY)) {
		const gchar *address = i_cal_property_get_attendee (prop);

		if (address && !g_ascii_strncasecmp (address, "mailto:", 7))
			address += 7;
		if (address && user_email && !g_ascii_strcasecmp (address, user_email)) {
			ICalParameter *param;

			/* GroupWise writes PARTSTAT twice: all of them go */
			while ((param = i_cal_property_get_first_parameter (prop, I_CAL_PARTSTAT_PARAMETER))) {
				i_cal_property_remove_parameter_by_kind (prop, I_CAL_PARTSTAT_PARAMETER);
				g_object_unref (param);
			}
			i_cal_property_take_parameter (prop, i_cal_parameter_new_partstat (partstat));
			g_object_unref (prop);
			return;
		}
	}
}

/* An answer to an invitation in the calendar (Evolution's invitation view
 * sends the request with the user's PARTSTAT, then a reply): accept or
 * decline in GroupWise. FALSE when @comp is not about such an invitation. */
static gboolean
receive_answer (ECalBackendGroupwise *cbgw,
		EGwConnection *cnc,
		ECalCache *cache,
		ICalComponent *comp,
		ICalPropertyMethod method,
		GCancellable *cancellable,
		GError **error)
{
	const gchar *uid = i_cal_component_get_uid (comp);
	gchar *extra = NULL, *id, *source, *user_email;
	ICalParameterPartstat partstat;
	ICalComponent *answer;
	ECalMetaBackendInfo *info;
	EGwResponse *response;
	GSList *changed;
	xmlNode *current;
	gboolean success;

	user_email = dup_user_email (cbgw);
	partstat = user_partstat (comp, user_email);
	g_debug ("receive %s method %d for %s: answer %d", uid ? uid : "?", method, user_email ? user_email : "?", partstat);
	if (!uid || !user_email || !e_cal_cache_get_component_extra (cache, uid, NULL, &extra, cancellable, NULL) || !extra) {
		g_free (user_email);
		g_free (extra);
		return FALSE;
	}

	id = split_extra (extra, NULL);
	response = get_item (cnc, id, &current, cancellable, NULL);
	source = current ? e_gw_xml_dup_text (current, "source") : NULL;

	/* The user's own meeting: an answer for the user is not possible here,
	 * one for another attendee belongs into that attendee's calendar (when
	 * several accounts are in Evolution, the invitation view may pick this
	 * one for the same UID) */
	if (g_strcmp0 (source, "sent") == 0) {
		/* An attendee's reply by mail: GroupWise keeps the answers itself */
		if (method != I_CAL_METHOD_REPLY)
			g_set_error (error, E_CLIENT_ERROR, E_CLIENT_ERROR_PERMISSION_DENIED,
			_("“%s” is a meeting you organized in this calendar. Answer the invitation in the calendar of the "
			  "account that received it."), i_cal_component_get_summary (comp) ? i_cal_component_get_summary (comp) : uid);
		e_gw_response_free (response);
		g_free (source);
		g_free (id);
		g_free (extra);
		g_free (user_email);
		return TRUE;
	}
	if (g_strcmp0 (source, "received") != 0 || (partstat != I_CAL_PARTSTAT_ACCEPTED &&
	    partstat != I_CAL_PARTSTAT_TENTATIVE && partstat != I_CAL_PARTSTAT_DECLINED)) {
		e_gw_response_free (response);
		g_free (source);
		g_free (id);
		g_free (extra);
		g_free (user_email);
		return FALSE;
	}

	/* The item as it is, only the answer (and its comment) is new */
	answer = e_gw_calendar_component_from_item (current, user_zone (), user_email);
	set_user_partstat (answer, user_email, partstat);
	{
		ICalProperty *comment = i_cal_component_get_first_property (comp, I_CAL_COMMENT_PROPERTY);

		if (comment) {
			i_cal_component_take_property (answer, i_cal_property_clone (comment));
			g_object_unref (comment);
		}
	}
	success = save_received (cbgw, cnc, id, current, answer, cancellable, error);
	g_object_unref (answer);
	e_gw_response_free (response);

	/* The cache follows at once: the reply after the request finds it answered */
	if (success) {
		info = e_cal_meta_backend_info_new (uid, NULL, NULL, extra);
		changed = g_slist_prepend (NULL, info);
		if (partstat == I_CAL_PARTSTAT_DECLINED)
			e_cal_meta_backend_process_changes_sync (E_CAL_META_BACKEND (cbgw), NULL, NULL, changed, cancellable, NULL);
		else
			e_cal_meta_backend_process_changes_sync (E_CAL_META_BACKEND (cbgw), NULL, changed, NULL, cancellable, NULL);
		g_slist_free_full (changed, e_cal_meta_backend_info_free);
	}

	g_free (source);
	g_free (id);
	g_free (extra);
	g_free (user_email);

	return TRUE;
}

static void
ecb_groupwise_receive_objects_sync (ECalBackendSync *sync_backend,
				   EDataCal *cal,
				   GCancellable *cancellable,
				   const gchar *calobj,
				   ECalOperationFlags opflags,
				   GError **error)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (sync_backend);
	ICalComponent *toplevel = i_cal_component_new_from_string (calobj), *sub;
	ICalComponentKind kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbgw));
	ICalPropertyMethod method = I_CAL_METHOD_PUBLISH;
	gboolean handled = FALSE, success = TRUE;
	EGwConnection *cnc = NULL;
	ECalCache *cache;
	GError *local_error = NULL;

	if (toplevel && i_cal_component_isa (toplevel) == I_CAL_VCALENDAR_COMPONENT &&
	    e_cal_util_component_has_property (toplevel, I_CAL_METHOD_PROPERTY))
		method = i_cal_component_get_method (toplevel);

	cache = e_cal_meta_backend_ref_cache (E_CAL_META_BACKEND (cbgw));
	if (toplevel && cache && (method == I_CAL_METHOD_REQUEST || method == I_CAL_METHOD_REPLY || method == I_CAL_METHOD_PUBLISH) &&
	    e_cal_meta_backend_ensure_connected_sync (E_CAL_META_BACKEND (cbgw), cancellable, NULL))
		cnc = ref_connection (cbgw, NULL);

	if (cnc && i_cal_component_isa (toplevel) == I_CAL_VCALENDAR_COMPONENT) {
		for (sub = i_cal_component_get_first_component (toplevel, kind); sub && success;
		     g_object_unref (sub), sub = i_cal_component_get_next_component (toplevel, kind)) {
			if (receive_answer (cbgw, cnc, cache, sub, method, cancellable, &local_error)) {
				handled = TRUE;
				success = !local_error;
			}
		}
		g_clear_object (&sub);
	} else if (cnc && i_cal_component_isa (toplevel) == kind) {
		handled = receive_answer (cbgw, cnc, cache, toplevel, method, cancellable, &local_error);
	}

	g_clear_object (&cnc);
	g_clear_object (&cache);
	g_clear_object (&toplevel);

	if (local_error) {
		propagate_error (error, local_error);
		return;
	}

	/* Anything else as EDS does it (an invitation from outside GroupWise
	 * becomes a personal appointment, a cancellation removes it) */
	if (!handled)
		E_CAL_BACKEND_SYNC_CLASS (e_cal_backend_groupwise_parent_class)->receive_objects_sync (sync_backend, cal,
			cancellable, calobj, opflags, error);
}

static gchar *
ecb_groupwise_get_backend_property (ECalBackend *cal_backend,
				    const gchar *prop_name)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (cal_backend);

	if (g_str_equal (prop_name, CLIENT_BACKEND_PROPERTY_CAPABILITIES)) {
		return g_strjoin (",",
			/* GroupWise sends and answers meetings itself */
			E_CAL_STATIC_CAPABILITY_CREATE_MESSAGES,
			E_CAL_STATIC_CAPABILITY_SAVE_SCHEDULES,
			E_CAL_STATIC_CAPABILITY_HAS_UNACCEPTED_MEETING,
			/* Deleting a meeting asks for a reason and whether the
			 * attendees are told (the retraction comment) */
			E_CAL_STATIC_CAPABILITY_RETRACT_SUPPORTED,
			/* One alarm, shown before the start */
			E_CAL_STATIC_CAPABILITY_ONE_ALARM_ONLY,
			E_CAL_STATIC_CAPABILITY_NO_ALARM_REPEAT,
			E_CAL_STATIC_CAPABILITY_NO_ALARM_AFTER_START,
			E_CAL_STATIC_CAPABILITY_NO_AUDIO_ALARMS,
			E_CAL_STATIC_CAPABILITY_NO_EMAIL_ALARMS,
			E_CAL_STATIC_CAPABILITY_NO_PROCEDURE_ALARMS,
			E_CAL_STATIC_CAPABILITY_TASK_NO_ALARM,
			E_CAL_STATIC_CAPABILITY_TASK_DATE_ONLY,
			/* The instances of a series are items of their own */
			E_CAL_STATIC_CAPABILITY_REMOVE_ONLY_THIS,
			E_CAL_STATIC_CAPABILITY_NO_THISANDFUTURE,
			E_CAL_STATIC_CAPABILITY_NO_THISANDPRIOR,
			E_CAL_STATIC_CAPABILITY_NO_CONV_TO_RECUR,
			e_cal_meta_backend_get_capabilities (E_CAL_META_BACKEND (cal_backend)),
			NULL);
	} else if (g_str_equal (prop_name, E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS) ||
		   g_str_equal (prop_name, E_CAL_BACKEND_PROPERTY_ALARM_EMAIL_ADDRESS)) {
		return dup_user_email (cbgw);
	}

	return E_CAL_BACKEND_CLASS (e_cal_backend_groupwise_parent_class)->impl_get_backend_property (cal_backend, prop_name);
}

static void
ecb_groupwise_finalize (GObject *object)
{
	ECalBackendGroupwise *cbgw = E_CAL_BACKEND_GROUPWISE (object);

	g_clear_object (&cbgw->cnc);
	g_free (cbgw->folder_id);
	g_free (cbgw->calendar_id);
	g_free (cbgw->sent_folder_id);
	g_ptr_array_unref (cbgw->subcalendars);
	g_hash_table_destroy (cbgw->item_subcalendars);
	g_hash_table_destroy (cbgw->category_names);
	g_hash_table_destroy (cbgw->category_ids);
	g_free (cbgw->user_email);
	own_meetings_forget (cbgw);
	g_free (cbgw->account_key);
	g_free (cbgw->user_name);
	g_hash_table_destroy (cbgw->pending_travel);
	g_rec_mutex_clear (&cbgw->lock);

	G_OBJECT_CLASS (e_cal_backend_groupwise_parent_class)->finalize (object);
}

static void
e_cal_backend_groupwise_class_init (ECalBackendGroupwiseClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	ECalBackendClass *cal_backend_class = E_CAL_BACKEND_CLASS (class);
	ECalMetaBackendClass *meta_class = E_CAL_META_BACKEND_CLASS (class);

	object_class->finalize = ecb_groupwise_finalize;

	meta_class->connect_sync = ecb_groupwise_connect_sync;
	meta_class->disconnect_sync = ecb_groupwise_disconnect_sync;
	meta_class->list_existing_sync = ecb_groupwise_list_existing_sync;
	meta_class->load_component_sync = ecb_groupwise_load_component_sync;
	meta_class->save_component_sync = ecb_groupwise_save_component_sync;
	meta_class->remove_component_sync = ecb_groupwise_remove_component_sync;

	cal_backend_class->impl_get_backend_property = ecb_groupwise_get_backend_property;
	E_CAL_BACKEND_SYNC_CLASS (class)->get_free_busy_sync = ecb_groupwise_get_free_busy_sync;
	E_CAL_BACKEND_SYNC_CLASS (class)->receive_objects_sync = ecb_groupwise_receive_objects_sync;
	E_CAL_BACKEND_SYNC_CLASS (class)->send_objects_sync = ecb_groupwise_send_objects_sync;
}

static void
e_cal_backend_groupwise_init (ECalBackendGroupwise *cbgw)
{
	g_rec_mutex_init (&cbgw->lock);
	cbgw->subcalendars = g_ptr_array_new_with_free_func (g_free);
	cbgw->item_subcalendars = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
	cbgw->category_names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	cbgw->category_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	cbgw->pending_travel = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}

/* ------------------------------------------------------------------ */
/* Module: one factory per component kind */

typedef ECalBackendFactory ECalBackendGroupwiseEventsFactory;
typedef ECalBackendFactoryClass ECalBackendGroupwiseEventsFactoryClass;
typedef ECalBackendFactory ECalBackendGroupwiseTodosFactory;
typedef ECalBackendFactoryClass ECalBackendGroupwiseTodosFactoryClass;
typedef ECalBackendFactory ECalBackendGroupwiseJournalFactory;
typedef ECalBackendFactoryClass ECalBackendGroupwiseJournalFactoryClass;

static EModule *e_module;

GType e_cal_backend_groupwise_events_factory_get_type (void);
GType e_cal_backend_groupwise_todos_factory_get_type (void);
GType e_cal_backend_groupwise_journal_factory_get_type (void);
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

G_DEFINE_DYNAMIC_TYPE (ECalBackendGroupwiseEventsFactory, e_cal_backend_groupwise_events_factory, E_TYPE_CAL_BACKEND_FACTORY)
G_DEFINE_DYNAMIC_TYPE (ECalBackendGroupwiseTodosFactory, e_cal_backend_groupwise_todos_factory, E_TYPE_CAL_BACKEND_FACTORY)
G_DEFINE_DYNAMIC_TYPE (ECalBackendGroupwiseJournalFactory, e_cal_backend_groupwise_journal_factory, E_TYPE_CAL_BACKEND_FACTORY)

static void
factory_class_setup (ECalBackendFactoryClass *class,
		     ICalComponentKind kind)
{
	E_BACKEND_FACTORY_CLASS (class)->e_module = e_module;
	E_BACKEND_FACTORY_CLASS (class)->share_subprocess = TRUE;

	class->factory_name = "groupwise";
	class->component_kind = kind;
	class->backend_type = E_TYPE_CAL_BACKEND_GROUPWISE;
}

static void
e_cal_backend_groupwise_events_factory_class_init (ECalBackendFactoryClass *class)
{
	factory_class_setup (class, I_CAL_VEVENT_COMPONENT);
}

static void
e_cal_backend_groupwise_todos_factory_class_init (ECalBackendFactoryClass *class)
{
	factory_class_setup (class, I_CAL_VTODO_COMPONENT);
}

static void
e_cal_backend_groupwise_journal_factory_class_init (ECalBackendFactoryClass *class)
{
	factory_class_setup (class, I_CAL_VJOURNAL_COMPONENT);
}

static void
e_cal_backend_groupwise_events_factory_class_finalize (ECalBackendFactoryClass *class)
{
}

static void
e_cal_backend_groupwise_todos_factory_class_finalize (ECalBackendFactoryClass *class)
{
}

static void
e_cal_backend_groupwise_journal_factory_class_finalize (ECalBackendFactoryClass *class)
{
}

static void
e_cal_backend_groupwise_events_factory_init (ECalBackendFactory *factory)
{
}

static void
e_cal_backend_groupwise_todos_factory_init (ECalBackendFactory *factory)
{
}

static void
e_cal_backend_groupwise_journal_factory_init (ECalBackendFactory *factory)
{
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

	e_module = E_MODULE (type_module);
	e_gw_backend_ensure_types ();
	e_cal_backend_groupwise_events_factory_register_type (type_module);
	e_cal_backend_groupwise_todos_factory_register_type (type_module);
	e_cal_backend_groupwise_journal_factory_register_type (type_module);
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
	e_module = NULL;
}

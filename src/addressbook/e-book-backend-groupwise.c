/*
 * e-book-backend-groupwise.c: GroupWise address books for evolution-data-server
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
 * An EBookMetaBackend: the whole address book is kept in the local cache
 * (searches, autocompletion and offline work locally). A sync lists every
 * item with its revision; the cache takes what is new or changed. The
 * system address book is read-only, the user's personal ones writable.
 */

#include <glib/gi18n-lib.h>
#include <libedata-book/libedata-book.h>

#include "e-gw-addressbook.h"
#include "e-gw-backend-utils.h"
#include "e-gw-contact.h"
#include "e-gw-xml.h"
#include "e-source-groupwise-folder.h"

#define LIST_VIEW "default peek"
#define PAGE_SIZE 500

/* EDS declares no cleanup function for its backends */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (EBookMetaBackend, g_object_unref)

#define E_TYPE_BOOK_BACKEND_GROUPWISE (e_book_backend_groupwise_get_type ())
G_DECLARE_FINAL_TYPE (EBookBackendGroupwise, e_book_backend_groupwise, E, BOOK_BACKEND_GROUPWISE, EBookMetaBackend)

struct _EBookBackendGroupwise {
	EBookMetaBackend parent;

	GRecMutex lock;
	EGwConnection *cnc;
	gchar *book_id;
	gboolean is_system;
};

G_DEFINE_TYPE (EBookBackendGroupwise, e_book_backend_groupwise, E_TYPE_BOOK_META_BACKEND)

static EGwConnection *
ref_connection (EBookBackendGroupwise *bbgw)
{
	EGwConnection *cnc;

	g_rec_mutex_lock (&bbgw->lock);
	cnc = bbgw->cnc ? g_object_ref (bbgw->cnc) : NULL;
	g_rec_mutex_unlock (&bbgw->lock);

	return cnc;
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
		g_set_error_literal (error, E_BOOK_CLIENT_ERROR, E_BOOK_CLIENT_ERROR_CONTACT_NOT_FOUND, gw_error->message);
	else if (gw_error->domain == E_GW_ERROR)
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_OTHER_ERROR, gw_error->message);
	else {
		g_propagate_error (error, gw_error);
		return;
	}

	g_error_free (gw_error);
}

static gboolean
ebb_groupwise_connect_sync (EBookMetaBackend *meta_backend,
			    const ENamedParameters *credentials,
			    ESourceAuthenticationResult *out_auth_result,
			    gchar **out_certificate_pem,
			    GTlsCertificateFlags *out_certificate_errors,
			    GCancellable *cancellable,
			    GError **error)
{
	EBookBackendGroupwise *bbgw = E_BOOK_BACKEND_GROUPWISE (meta_backend);
	ESource *source = e_backend_get_source (E_BACKEND (meta_backend));
	ESourceRegistry *registry = e_book_backend_get_registry (E_BOOK_BACKEND (meta_backend));
	CamelGroupwiseSettings *settings;
	EGwConnection *cnc;
	gchar *book_id, *proxy;

	g_rec_mutex_lock (&bbgw->lock);
	if (bbgw->cnc) {
		g_rec_mutex_unlock (&bbgw->lock);
		*out_auth_result = E_SOURCE_AUTHENTICATION_ACCEPTED;
		return TRUE;
	}

	book_id = e_source_groupwise_folder_dup_id (e_source_get_extension (source, E_SOURCE_EXTENSION_GROUPWISE_FOLDER));
	settings = e_gw_backend_ref_settings (registry, source);
	if (!book_id || !settings) {
		g_rec_mutex_unlock (&bbgw->lock);
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_OTHER_ERROR,
			_("The address book is not set up for GroupWise"));
		*out_auth_result = E_SOURCE_AUTHENTICATION_ERROR;
		g_free (book_id);
		g_clear_object (&settings);
		return FALSE;
	}

	/* An address book of a proxy account: the other mailbox's */
	proxy = camel_groupwise_settings_dup_proxy (settings);
	cnc = e_gw_backend_connect_sync (registry, source, settings, proxy, credentials, out_auth_result,
		out_certificate_pem, out_certificate_errors, cancellable, error);
	g_object_unref (settings);
	g_free (proxy);

	if (cnc) {
		bbgw->cnc = cnc;
		g_free (bbgw->book_id);
		bbgw->book_id = book_id;
		bbgw->is_system = g_strcmp0 (book_id, E_GW_SYSTEM_ADDRESS_BOOK_ID) == 0;
		e_book_backend_set_writable (E_BOOK_BACKEND (bbgw), !bbgw->is_system);
	} else {
		g_free (book_id);
	}

	g_rec_mutex_unlock (&bbgw->lock);

	return cnc != NULL;
}

static gboolean
ebb_groupwise_disconnect_sync (EBookMetaBackend *meta_backend,
			       GCancellable *cancellable,
			       GError **error)
{
	EBookBackendGroupwise *bbgw = E_BOOK_BACKEND_GROUPWISE (meta_backend);
	EGwConnection *cnc;

	g_rec_mutex_lock (&bbgw->lock);
	cnc = bbgw->cnc;
	bbgw->cnc = NULL;
	g_rec_mutex_unlock (&bbgw->lock);

	if (cnc) {
		e_gw_connection_logout_sync (cnc, cancellable);
		g_object_unref (cnc);
	}

	return TRUE;
}

/* A group with its members (a second request: the list view has none) */
static EContact *
contact_with_members (EGwConnection *cnc,
		      xmlNode *item,
		      GCancellable *cancellable)
{
	gchar *type = e_gw_xml_dup_attr (item, "type");
	EContact *contact;

	if (type && g_str_has_suffix (type, "Group")) {
		gchar *raw = e_gw_xml_dup_text (item, "id");
		gchar *id = e_gw_clean_id (raw);
		EGwResponse *response = e_gw_connection_get_item_sync (cnc, id, "members peek", cancellable, NULL);

		contact = e_gw_contact_from_item (item, response ? e_gw_xml_find (e_gw_response_get_node (response), "item") : NULL);
		e_gw_response_free (response);
		g_free (raw);
		g_free (id);
	} else {
		contact = e_gw_contact_from_item (item, NULL);
	}

	g_free (type);

	return contact;
}

typedef struct {
	EGwConnection *cnc;
	GSList *infos;
	GCancellable *cancellable;
} ListData;

static gboolean
list_item_cb (xmlNode *item,
	      gpointer user_data)
{
	ListData *data = user_data;
	EContact *contact = contact_with_members (data->cnc, item, data->cancellable);
	gchar *vcard;

	if (!contact)
		return TRUE;

	/* The object comes along: the cache needs no second request per contact */
	vcard = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
	data->infos = g_slist_prepend (data->infos, e_book_meta_backend_info_new (
		e_contact_get_const (contact, E_CONTACT_UID),
		e_contact_get_const (contact, E_CONTACT_REV),
		vcard, NULL));
	g_free (vcard);
	g_object_unref (contact);

	return TRUE;
}

static gboolean
ebb_groupwise_list_existing_sync (EBookMetaBackend *meta_backend,
				  gchar **out_new_sync_tag,
				  GSList **out_existing_objects,
				  GCancellable *cancellable,
				  GError **error)
{
	EBookBackendGroupwise *bbgw = E_BOOK_BACKEND_GROUPWISE (meta_backend);
	EGwConnection *cnc = ref_connection (bbgw);
	ListData data = { cnc, NULL, cancellable };
	GError *local_error = NULL;
	gboolean success;

	if (!cnc) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_REPOSITORY_OFFLINE, _("Not connected"));
		return FALSE;
	}

	success = e_gw_connection_foreach_item_sync (cnc, bbgw->book_id, LIST_VIEW, NULL, PAGE_SIZE,
		list_item_cb, &data, cancellable, &local_error);
	g_object_unref (cnc);

	if (!success) {
		g_slist_free_full (data.infos, e_book_meta_backend_info_free);
		propagate_error (error, local_error);
		return FALSE;
	}

	*out_existing_objects = g_slist_reverse (data.infos);
	*out_new_sync_tag = NULL;

	return TRUE;
}

static gboolean
ebb_groupwise_load_contact_sync (EBookMetaBackend *meta_backend,
				 const gchar *uid,
				 const gchar *extra,
				 EContact **out_contact,
				 gchar **out_extra,
				 GCancellable *cancellable,
				 GError **error)
{
	EGwConnection *cnc = ref_connection (E_BOOK_BACKEND_GROUPWISE (meta_backend));
	EGwResponse *response;
	GError *local_error = NULL;
	xmlNode *item;

	if (!cnc) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_REPOSITORY_OFFLINE, _("Not connected"));
		return FALSE;
	}

	response = e_gw_connection_get_item_sync (cnc, uid, LIST_VIEW, cancellable, &local_error);
	item = response ? e_gw_xml_find (e_gw_response_get_node (response), "item") : NULL;
	*out_contact = item ? contact_with_members (cnc, item, cancellable) : NULL;
	e_gw_response_free (response);
	g_object_unref (cnc);

	if (!*out_contact) {
		if (local_error)
			propagate_error (error, local_error);
		else
			g_set_error (error, E_BOOK_CLIENT_ERROR, E_BOOK_CLIENT_ERROR_CONTACT_NOT_FOUND, _("Contact %s not found"), uid);
		return FALSE;
	}

	return TRUE;
}

static gboolean
ebb_groupwise_save_contact_sync (EBookMetaBackend *meta_backend,
				 gboolean overwrite_existing,
				 EConflictResolution conflict_resolution,
				 EContact *contact,
				 const gchar *extra,
				 guint32 opflags,
				 gchar **out_new_uid,
				 gchar **out_new_extra,
				 GCancellable *cancellable,
				 GError **error)
{
	EBookBackendGroupwise *bbgw = E_BOOK_BACKEND_GROUPWISE (meta_backend);
	EGwConnection *cnc;
	GError *local_error = NULL;
	gboolean success = FALSE;

	if (bbgw->is_system) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_PERMISSION_DENIED,
			_("The GroupWise Address Book of the system cannot be changed"));
		return FALSE;
	}

	cnc = ref_connection (bbgw);
	if (!cnc) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_REPOSITORY_OFFLINE, _("Not connected"));
		return FALSE;
	}

	if (overwrite_existing) {
		const gchar *uid = e_contact_get_const (contact, E_CONTACT_UID);
		EGwResponse *response = e_gw_connection_get_item_sync (cnc, uid, LIST_VIEW, cancellable, &local_error);
		xmlNode *current = response ? e_gw_xml_find (e_gw_response_get_node (response), "item") : NULL;
		gchar *updates = current ? e_gw_contact_updates_xml (current, contact, error) : NULL;

		if (updates) {
			success = !*updates || e_gw_connection_modify_item_sync (cnc, uid, updates, cancellable, &local_error);
			if (success)
				*out_new_uid = g_strdup (uid);
		}
		e_gw_response_free (response);
		g_free (updates);
	} else {
		gchar *item = e_gw_contact_to_item_xml (contact, bbgw->book_id, error);

		if (item) {
			*out_new_uid = e_gw_connection_create_item_sync (cnc, item, cancellable, &local_error);
			success = *out_new_uid != NULL;
		}
		g_free (item);
	}

	g_object_unref (cnc);
	propagate_error (error, local_error);

	return success;
}

static gboolean
ebb_groupwise_remove_contact_sync (EBookMetaBackend *meta_backend,
				   EConflictResolution conflict_resolution,
				   const gchar *uid,
				   const gchar *extra,
				   const gchar *object,
				   guint32 opflags,
				   GCancellable *cancellable,
				   GError **error)
{
	EBookBackendGroupwise *bbgw = E_BOOK_BACKEND_GROUPWISE (meta_backend);
	const gchar *ids[] = { uid, NULL };
	EGwConnection *cnc;
	GError *local_error = NULL;
	gboolean success;

	if (bbgw->is_system) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_PERMISSION_DENIED,
			_("The GroupWise Address Book of the system cannot be changed"));
		return FALSE;
	}

	cnc = ref_connection (bbgw);
	if (!cnc) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_REPOSITORY_OFFLINE, _("Not connected"));
		return FALSE;
	}

	/* With the address book: an entry linked into another one stays there */
	success = e_gw_connection_remove_items_sync (cnc, ids, bbgw->book_id, cancellable, &local_error);
	g_object_unref (cnc);
	propagate_error (error, local_error);

	return success;
}

static gchar *
ebb_groupwise_get_backend_property (EBookBackend *book_backend,
				    const gchar *prop_name)
{
	if (g_str_equal (prop_name, CLIENT_BACKEND_PROPERTY_CAPABILITIES)) {
		return g_strjoin (",", "net", "do-initial-query",
			e_book_meta_backend_get_capabilities (E_BOOK_META_BACKEND (book_backend)), NULL);
	} else if (g_str_equal (prop_name, E_BOOK_BACKEND_PROPERTY_REQUIRED_FIELDS)) {
		return g_strdup (e_contact_field_name (E_CONTACT_FILE_AS));
	} else if (g_str_equal (prop_name, E_BOOK_BACKEND_PROPERTY_SUPPORTED_FIELDS)) {
		static const EContactField fields[] = {
			E_CONTACT_UID, E_CONTACT_REV, E_CONTACT_FILE_AS, E_CONTACT_FULL_NAME, E_CONTACT_NAME,
			E_CONTACT_GIVEN_NAME, E_CONTACT_FAMILY_NAME, E_CONTACT_EMAIL_1, E_CONTACT_EMAIL_2,
			E_CONTACT_EMAIL_3, E_CONTACT_EMAIL_4, E_CONTACT_PHONE_BUSINESS, E_CONTACT_PHONE_HOME,
			E_CONTACT_PHONE_MOBILE, E_CONTACT_PHONE_BUSINESS_FAX, E_CONTACT_PHONE_PAGER,
			E_CONTACT_PHONE_OTHER, E_CONTACT_ADDRESS_WORK, E_CONTACT_ADDRESS_HOME,
			E_CONTACT_ADDRESS_OTHER, E_CONTACT_ORG, E_CONTACT_ORG_UNIT, E_CONTACT_TITLE,
			E_CONTACT_HOMEPAGE_URL, E_CONTACT_BLOG_URL, E_CONTACT_BIRTH_DATE, E_CONTACT_NOTE
		};
		GString *names = g_string_new (NULL);
		guint ii;

		for (ii = 0; ii < G_N_ELEMENTS (fields); ii++)
			g_string_append_printf (names, "%s%s", ii ? "," : "", e_contact_field_name (fields[ii]));

		return g_string_free (names, FALSE);
	}

	return E_BOOK_BACKEND_CLASS (e_book_backend_groupwise_parent_class)->impl_get_backend_property (book_backend, prop_name);
}

static void
ebb_groupwise_finalize (GObject *object)
{
	EBookBackendGroupwise *bbgw = E_BOOK_BACKEND_GROUPWISE (object);

	g_clear_object (&bbgw->cnc);
	g_free (bbgw->book_id);
	g_rec_mutex_clear (&bbgw->lock);

	G_OBJECT_CLASS (e_book_backend_groupwise_parent_class)->finalize (object);
}

static void
e_book_backend_groupwise_class_init (EBookBackendGroupwiseClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	EBookBackendClass *book_backend_class = E_BOOK_BACKEND_CLASS (class);
	EBookMetaBackendClass *meta_class = E_BOOK_META_BACKEND_CLASS (class);

	object_class->finalize = ebb_groupwise_finalize;

	/* Direct read access: Evolution reads the cache through this module itself */
	meta_class->backend_module_directory = GW_BOOK_BACKENDDIR;
	meta_class->backend_module_filename = "libebookbackendgroupwise.so";
	meta_class->backend_factory_type_name = "EBookBackendGroupwiseFactory";
	meta_class->connect_sync = ebb_groupwise_connect_sync;
	meta_class->disconnect_sync = ebb_groupwise_disconnect_sync;
	meta_class->list_existing_sync = ebb_groupwise_list_existing_sync;
	meta_class->load_contact_sync = ebb_groupwise_load_contact_sync;
	meta_class->save_contact_sync = ebb_groupwise_save_contact_sync;
	meta_class->remove_contact_sync = ebb_groupwise_remove_contact_sync;

	book_backend_class->impl_get_backend_property = ebb_groupwise_get_backend_property;
}

static void
e_book_backend_groupwise_init (EBookBackendGroupwise *bbgw)
{
	g_rec_mutex_init (&bbgw->lock);
}

/* ------------------------------------------------------------------ */
/* Module */

typedef EBookBackendFactory EBookBackendGroupwiseFactory;
typedef EBookBackendFactoryClass EBookBackendGroupwiseFactoryClass;

static EModule *e_module;

GType e_book_backend_groupwise_factory_get_type (void);
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

G_DEFINE_DYNAMIC_TYPE (EBookBackendGroupwiseFactory, e_book_backend_groupwise_factory, E_TYPE_BOOK_BACKEND_FACTORY)

static void
e_book_backend_groupwise_factory_class_init (EBookBackendFactoryClass *class)
{
	E_BACKEND_FACTORY_CLASS (class)->e_module = e_module;
	E_BACKEND_FACTORY_CLASS (class)->share_subprocess = TRUE;

	class->factory_name = "groupwise";
	class->backend_type = E_TYPE_BOOK_BACKEND_GROUPWISE;
}

static void
e_book_backend_groupwise_factory_class_finalize (EBookBackendFactoryClass *class)
{
}

static void
e_book_backend_groupwise_factory_init (EBookBackendFactory *factory)
{
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

	e_module = E_MODULE (type_module);
	e_gw_backend_ensure_types ();
	e_book_backend_groupwise_factory_register_type (type_module);
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
	e_module = NULL;
}

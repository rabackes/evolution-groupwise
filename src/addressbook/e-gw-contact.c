/*
 * e-gw-contact.c: GroupWise address book items and vCards
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
 * The element names follow what gwmcp reads and writes on a real POA; so
 * does modifyItem: single fields are added, updated or deleted, the email,
 * phone and address lists are replaced as a whole.
 */

#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-xml.h"

#include "e-gw-contact.h"

/* Single text fields: element path <-> EContact field */
static const struct {
	const gchar *path;
	EContactField field;
} text_fields[] = {
	{ "officeInfo/organization", E_CONTACT_ORG },
	{ "officeInfo/department", E_CONTACT_ORG_UNIT },
	{ "officeInfo/title", E_CONTACT_TITLE },
	{ "officeInfo/website", E_CONTACT_HOMEPAGE_URL },
	{ "personalInfo/website", E_CONTACT_BLOG_URL },
	{ "comment", E_CONTACT_NOTE }
};

static const struct {
	const gchar *type;
	EContactField field;
} phone_types[] = {
	{ "Office", E_CONTACT_PHONE_BUSINESS },
	{ "Home", E_CONTACT_PHONE_HOME },
	{ "Mobile", E_CONTACT_PHONE_MOBILE },
	{ "Fax", E_CONTACT_PHONE_BUSINESS_FAX },
	{ "Pager", E_CONTACT_PHONE_PAGER },
	{ "Other", E_CONTACT_PHONE_OTHER }
};

static const struct {
	const gchar *type;
	EContactField field;
} address_types[] = {
	{ "Office", E_CONTACT_ADDRESS_WORK },
	{ "Home", E_CONTACT_ADDRESS_HOME },
	{ "Other", E_CONTACT_ADDRESS_OTHER }
};

/* ------------------------------------------------------------------ */
/* Reading */

static gchar *
item_type (xmlNode *item)
{
	gchar *type = e_gw_xml_dup_attr (item, "type");

	/* "xsi:type" arrives as the attribute "type" in the xsi namespace */
	if (type && strchr (type, ':')) {
		gchar *plain = g_strdup (strchr (type, ':') + 1);

		g_free (type);
		type = plain;
	}

	return type;
}

static gchar *
non_empty (gchar *text)
{
	if (text && !*text) {
		g_free (text);
		return NULL;
	}

	return text;
}

static void
set_text (EContact *contact,
	  EContactField field,
	  xmlNode *item,
	  const gchar *path)
{
	gchar *text = non_empty (e_gw_xml_dup_text (item, path));

	if (text)
		e_contact_set (contact, field, text);
	g_free (text);
}

static EContactField
phone_field (const gchar *type)
{
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (phone_types); ii++) {
		if (type && g_ascii_strcasecmp (type, phone_types[ii].type) == 0)
			return phone_types[ii].field;
	}

	return E_CONTACT_PHONE_OTHER;
}

static EContactField
address_field (const gchar *type)
{
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (address_types); ii++) {
		if (type && g_ascii_strcasecmp (type, address_types[ii].type) == 0)
			return address_types[ii].field;
	}

	return E_CONTACT_ADDRESS_OTHER;
}

static void
read_address (EContact *contact,
	      xmlNode *node)
{
	EContactAddress *address = e_contact_address_new ();
	gchar *type = e_gw_xml_dup_attr (node, "type");

	address->street = non_empty (e_gw_xml_dup_text (node, "streetAddress"));
	address->ext = non_empty (e_gw_xml_dup_text (node, "location"));
	address->locality = non_empty (e_gw_xml_dup_text (node, "city"));
	address->region = non_empty (e_gw_xml_dup_text (node, "state"));
	address->code = non_empty (e_gw_xml_dup_text (node, "postalCode"));
	address->country = non_empty (e_gw_xml_dup_text (node, "country"));

	if (address->street || address->locality || address->code || address->country)
		e_contact_set (contact, address_field (type), address);

	e_contact_address_free (address);
	g_free (type);
}

static void
read_emails (EContact *contact,
	     xmlNode *item)
{
	GList *emails = NULL;
	xmlNode *list = e_gw_xml_find (item, "emailList"), *node;
	gchar *primary = e_gw_xml_dup_attr (list, "primary");

	/* The primary address first: Evolution takes the first one */
	if (primary && *primary)
		emails = g_list_append (emails, g_strdup (primary));
	for (node = e_gw_xml_first_child (list, "email"); node; node = e_gw_xml_next_sibling (node, "email")) {
		gchar *email = non_empty (e_gw_xml_dup_text (node, NULL));

		if (email && (!primary || g_ascii_strcasecmp (email, primary) != 0))
			emails = g_list_append (emails, email);
		else
			g_free (email);
	}
	/* Resources and organizations have a single <email> */
	if (!emails) {
		gchar *email = non_empty (e_gw_xml_dup_text (item, "email"));

		if (email)
			emails = g_list_append (emails, email);
	}

	if (emails)
		e_contact_set (contact, E_CONTACT_EMAIL, emails);

	g_list_free_full (emails, g_free);
	g_free (primary);
}

static void
read_members (EContact *contact,
	      xmlNode *members)
{
	EVCard *vcard = E_VCARD (contact);
	xmlNode *node = e_gw_xml_find (members, "members");

	for (node = e_gw_xml_first_child (node, "member"); node; node = e_gw_xml_next_sibling (node, "member")) {
		gchar *email = non_empty (e_gw_xml_dup_text (node, "email"));
		gchar *name = non_empty (e_gw_xml_dup_text (node, "name"));
		EVCardAttribute *attr;
		gchar *value;

		if (!name)
			name = non_empty (e_gw_xml_dup_text (node, "displayName"));
		if (!email) {
			g_free (name);
			continue;
		}

		/* As Evolution stores list members (EDestination) */
		value = name ? g_strdup_printf ("%s <%s>", name, email) : g_strdup (email);
		attr = e_vcard_attribute_new (NULL, EVC_EMAIL);
		if (name)
			e_vcard_attribute_add_param_with_value (attr, e_vcard_attribute_param_new (EVC_X_DEST_NAME), name);
		e_vcard_attribute_add_param_with_value (attr, e_vcard_attribute_param_new (EVC_X_DEST_EMAIL), email);
		e_vcard_attribute_add_param_with_value (attr, e_vcard_attribute_param_new (EVC_X_DEST_HTML_MAIL), "FALSE");
		e_vcard_append_attribute_with_value (vcard, attr, value);

		g_free (value);
		g_free (email);
		g_free (name);
	}
}

gchar *
e_gw_contact_revision (xmlNode *item)
{
	gchar *revision = non_empty (e_gw_xml_dup_text (item, "modified"));
	xmlBuffer *buffer;

	if (!revision)
		revision = non_empty (e_gw_xml_dup_text (item, "version"));
	if (revision)
		return revision;

	buffer = xmlBufferCreate ();
	xmlNodeDump (buffer, item->doc, item, 0, 0);
	revision = g_compute_checksum_for_data (G_CHECKSUM_SHA1, xmlBufferContent (buffer), xmlBufferLength (buffer));
	xmlBufferFree (buffer);

	return revision;
}

EContact *
e_gw_contact_from_item (xmlNode *item,
			xmlNode *members)
{
	EContact *contact;
	EContactName *name;
	EContactDate *birthday;
	xmlNode *node;
	gchar *type, *id, *raw, *text, *revision;
	guint ii;

	g_return_val_if_fail (item != NULL, NULL);

	type = item_type (item);
	if (!type || (g_strcmp0 (type, "Contact") && g_strcmp0 (type, "Group") &&
	    g_strcmp0 (type, "Organization") && g_strcmp0 (type, "Resource"))) {
		g_free (type);
		return NULL;
	}

	raw = e_gw_xml_dup_text (item, "id");
	id = raw && *raw ? e_gw_clean_id (raw) : NULL;
	g_free (raw);
	if (!id) {
		g_free (type);
		return NULL;
	}

	contact = e_contact_new ();
	e_contact_set (contact, E_CONTACT_UID, id);
	revision = e_gw_contact_revision (item);
	/* Resources carry whether they are a place since then */
	if (g_strcmp0 (type, "Resource") == 0) {
		gchar *marked = g_strconcat ("r2:", revision, NULL);

		g_free (revision);
		revision = marked;
	}
	e_contact_set (contact, E_CONTACT_REV, revision);
	g_free (revision);

	if (g_strcmp0 (type, "Contact") != 0)
		e_vcard_append_attribute_with_value (E_VCARD (contact), e_vcard_attribute_new (NULL, E_GW_CONTACT_X_TYPE), type);
	if (g_strcmp0 (type, "Resource") == 0 && e_gw_xml_get_bool (item, "flags/place"))
		e_vcard_append_attribute_with_value (E_VCARD (contact), e_vcard_attribute_new (NULL, E_GW_CONTACT_X_PLACE), "1");

	if (g_strcmp0 (type, "Group") == 0) {
		e_contact_set (contact, E_CONTACT_IS_LIST, GINT_TO_POINTER (TRUE));
		e_contact_set (contact, E_CONTACT_LIST_SHOW_ADDRESSES, GINT_TO_POINTER (TRUE));
		if (members)
			read_members (contact, members);
	}

	/* Names: the display name, its parts, the flat name ("Last, First") */
	set_text (contact, E_CONTACT_FULL_NAME, item, "fullName/displayName");
	name = e_contact_name_new ();
	name->given = non_empty (e_gw_xml_dup_text (item, "fullName/firstName"));
	name->family = non_empty (e_gw_xml_dup_text (item, "fullName/lastName"));
	name->additional = non_empty (e_gw_xml_dup_text (item, "fullName/middleName"));
	name->prefixes = non_empty (e_gw_xml_dup_text (item, "fullName/namePrefix"));
	name->suffixes = non_empty (e_gw_xml_dup_text (item, "fullName/nameSuffix"));
	if (name->given || name->family || name->additional)
		e_contact_set (contact, E_CONTACT_NAME, name);
	e_contact_name_free (name);
	set_text (contact, E_CONTACT_FILE_AS, item, "name");

	for (ii = 0; ii < G_N_ELEMENTS (text_fields); ii++)
		set_text (contact, text_fields[ii].field, item, text_fields[ii].path);

	read_emails (contact, item);

	for (node = e_gw_xml_first_child (e_gw_xml_find (item, "phoneList"), "phone"); node; node = e_gw_xml_next_sibling (node, "phone")) {
		gchar *number = non_empty (e_gw_xml_dup_text (node, NULL));
		gchar *phone_type = e_gw_xml_dup_attr (node, "type");

		/* The first number of a type wins */
		if (number && !e_contact_get_const (contact, phone_field (phone_type)))
			e_contact_set (contact, phone_field (phone_type), number);
		g_free (phone_type);
		g_free (number);
	}
	if (!e_contact_get_const (contact, E_CONTACT_PHONE_BUSINESS))
		set_text (contact, E_CONTACT_PHONE_BUSINESS, item, "phone");

	for (node = e_gw_xml_first_child (e_gw_xml_find (item, "addressList"), "address"); node; node = e_gw_xml_next_sibling (node, "address"))
		read_address (contact, node);
	node = e_gw_xml_find (item, "address");
	if (node)
		read_address (contact, node);

	text = non_empty (e_gw_xml_dup_text (item, "personalInfo/birthday"));
	birthday = text ? e_contact_date_from_string (text) : NULL;
	if (birthday) {
		e_contact_set (contact, E_CONTACT_BIRTH_DATE, birthday);
		e_contact_date_free (birthday);
	}
	g_free (text);

	/* Something to show: entries of the system address book may lack a display name */
	if (!e_contact_get_const (contact, E_CONTACT_FULL_NAME)) {
		text = non_empty (e_gw_xml_dup_text (item, "name"));
		if (!text)
			text = non_empty (e_gw_xml_dup_text (item, "userid"));
		if (!text && e_contact_get_const (contact, E_CONTACT_EMAIL_1))
			text = g_strdup (e_contact_get_const (contact, E_CONTACT_EMAIL_1));
		if (text)
			e_contact_set (contact, E_CONTACT_FULL_NAME, text);
		g_free (text);
	}
	if (!e_contact_get_const (contact, E_CONTACT_FILE_AS) && e_contact_get_const (contact, E_CONTACT_FULL_NAME))
		e_contact_set (contact, E_CONTACT_FILE_AS, e_contact_get_const (contact, E_CONTACT_FULL_NAME));
	if (g_strcmp0 (type, "Organization") == 0 && !e_contact_get_const (contact, E_CONTACT_ORG))
		e_contact_set (contact, E_CONTACT_ORG, e_contact_get_const (contact, E_CONTACT_FULL_NAME));

	g_free (type);
	g_free (id);

	return contact;
}

/* ------------------------------------------------------------------ */
/* Writing */

static gchar *
contact_text (EContact *contact,
	      EContactField field)
{
	const gchar *text = e_contact_get_const (contact, field);

	return text && *text ? g_strdup (text) : NULL;
}

static gchar *
file_as (EContact *contact)
{
	gchar *text = contact_text (contact, E_CONTACT_FILE_AS);

	if (!text)
		text = contact_text (contact, E_CONTACT_FULL_NAME);
	if (!text) {
		EContactName *name = e_contact_get (contact, E_CONTACT_NAME);

		if (name && name->family && name->given)
			text = g_strdup_printf ("%s, %s", name->family, name->given);
		else if (name)
			text = g_strdup (name->family ? name->family : name->given);
		e_contact_name_free (name);
	}
	if (!text)
		text = contact_text (contact, E_CONTACT_ORG);
	if (!text)
		text = contact_text (contact, E_CONTACT_EMAIL_1);

	return text;
}

/* The flat single fields of a contact: path -> value (NULL: none) */
static GHashTable *
single_fields (EContact *contact)
{
	GHashTable *fields = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
	EContactName *name = e_contact_get (contact, E_CONTACT_NAME);
	EContactDate *birthday = e_contact_get (contact, E_CONTACT_BIRTH_DATE);
	guint ii;

	g_hash_table_insert (fields, (gpointer) "name", file_as (contact));
	g_hash_table_insert (fields, (gpointer) "fullName/displayName", contact_text (contact, E_CONTACT_FULL_NAME));
	g_hash_table_insert (fields, (gpointer) "fullName/firstName", name && name->given && *name->given ? g_strdup (name->given) : NULL);
	g_hash_table_insert (fields, (gpointer) "fullName/middleName", name && name->additional && *name->additional ? g_strdup (name->additional) : NULL);
	g_hash_table_insert (fields, (gpointer) "fullName/lastName", name && name->family && *name->family ? g_strdup (name->family) : NULL);
	g_hash_table_insert (fields, (gpointer) "fullName/namePrefix", name && name->prefixes && *name->prefixes ? g_strdup (name->prefixes) : NULL);
	g_hash_table_insert (fields, (gpointer) "fullName/nameSuffix", name && name->suffixes && *name->suffixes ? g_strdup (name->suffixes) : NULL);
	for (ii = 0; ii < G_N_ELEMENTS (text_fields); ii++)
		g_hash_table_insert (fields, (gpointer) text_fields[ii].path, contact_text (contact, text_fields[ii].field));
	g_hash_table_insert (fields, (gpointer) "personalInfo/birthday", birthday ? e_contact_date_to_string (birthday) : NULL);

	e_contact_name_free (name);
	e_contact_date_free (birthday);

	return fields;
}

/* The single fields in the order of a GroupWise contact */
static const gchar *single_paths[] = {
	"name",
	"fullName/displayName", "fullName/namePrefix", "fullName/firstName", "fullName/middleName",
	"fullName/lastName", "fullName/nameSuffix",
	"officeInfo/organization", "officeInfo/department", "officeInfo/title", "officeInfo/website",
	"personalInfo/birthday", "personalInfo/website",
	"comment"
};

static gchar *
email_list_xml (EContact *contact)
{
	GList *emails = e_contact_get (contact, E_CONTACT_EMAIL), *link;
	GString *xml;
	gchar *primary;

	if (!emails)
		return NULL;

	primary = g_markup_escape_text (emails->data, -1);
	xml = g_string_new (NULL);
	g_string_append_printf (xml, "<emailList primary=\"%s\">", primary);
	for (link = emails; link; link = g_list_next (link))
		e_gw_xml_add_leaf (xml, "email", link->data);
	g_string_append (xml, "</emailList>");

	g_free (primary);
	g_list_free_full (emails, g_free);

	return g_string_free (xml, FALSE);
}

static gchar *
phone_list_xml (EContact *contact)
{
	GString *xml = g_string_new (NULL);
	const gchar *first = NULL;
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (phone_types); ii++) {
		const gchar *number = e_contact_get_const (contact, phone_types[ii].field);
		gchar *escaped;

		if (!number || !*number)
			continue;
		if (!first)
			first = number;
		escaped = g_markup_escape_text (number, -1);
		g_string_append_printf (xml, "<phone type=\"%s\">%s</phone>", phone_types[ii].type, escaped);
		g_free (escaped);
	}

	if (!first) {
		g_string_free (xml, TRUE);
		return NULL;
	}

	/* default names the preferred number */
	{
		gchar *escaped = g_markup_escape_text (first, -1);

		g_string_prepend (xml, "\">");
		g_string_prepend (xml, escaped);
		g_string_prepend (xml, "<phoneList default=\"");
		g_string_append (xml, "</phoneList>");
		g_free (escaped);
	}

	return g_string_free (xml, FALSE);
}

static gchar *
address_list_xml (EContact *contact)
{
	GString *xml = g_string_new (NULL);
	const gchar *mailing = NULL;
	guint ii;

	for (ii = 0; ii < G_N_ELEMENTS (address_types); ii++) {
		EContactAddress *address = e_contact_get (contact, address_types[ii].field);

		if (!address)
			continue;
		if (!mailing)
			mailing = address_types[ii].type;

		g_string_append_printf (xml, "<address type=\"%s\">", address_types[ii].type);
		if (address->street && *address->street)
			e_gw_xml_add_leaf (xml, "streetAddress", address->street);
		if (address->ext && *address->ext)
			e_gw_xml_add_leaf (xml, "location", address->ext);
		if (address->locality && *address->locality)
			e_gw_xml_add_leaf (xml, "city", address->locality);
		if (address->region && *address->region)
			e_gw_xml_add_leaf (xml, "state", address->region);
		if (address->code && *address->code)
			e_gw_xml_add_leaf (xml, "postalCode", address->code);
		if (address->country && *address->country)
			e_gw_xml_add_leaf (xml, "country", address->country);
		g_string_append (xml, "</address>");

		e_contact_address_free (address);
	}

	if (!mailing) {
		g_string_free (xml, TRUE);
		return NULL;
	}

	g_string_prepend (xml, "\">");
	g_string_prepend (xml, mailing);
	g_string_prepend (xml, "<addressList mailingAddress=\"");
	g_string_append (xml, "</addressList>");

	return g_string_free (xml, FALSE);
}

/* Appends path=value as nested elements, grouping the fields of one parent:
 * ("fullName/firstName", "A"), ("fullName/lastName", "B") ->
 * <fullName><firstName>A</firstName><lastName>B</lastName></fullName> */
static void
append_fields (GString *xml,
	       GPtrArray *paths,
	       GPtrArray *values)
{
	const gchar *open_parent = NULL;
	gsize open_len = 0;
	guint ii;

	for (ii = 0; ii < paths->len; ii++) {
		const gchar *path = paths->pdata[ii];
		const gchar *slash = strchr (path, '/');
		gsize parent_len = slash ? (gsize) (slash - path) : 0;

		if (open_parent && (!slash || parent_len != open_len || strncmp (open_parent, path, open_len) != 0)) {
			g_string_append_printf (xml, "</%.*s>", (gint) open_len, open_parent);
			open_parent = NULL;
		}
		if (slash && !open_parent) {
			g_string_append_printf (xml, "<%.*s>", (gint) parent_len, path);
			open_parent = path;
			open_len = parent_len;
		}
		e_gw_xml_add_leaf (xml, slash ? slash + 1 : path, values->pdata[ii]);
	}

	if (open_parent)
		g_string_append_printf (xml, "</%.*s>", (gint) open_len, open_parent);
}

gchar *
e_gw_contact_to_item_xml (EContact *contact,
			  const gchar *container,
			  GError **error)
{
	GHashTable *fields;
	GPtrArray *paths, *values;
	GString *xml;
	gchar *list;
	guint ii;

	g_return_val_if_fail (E_IS_CONTACT (contact), NULL);
	g_return_val_if_fail (container != NULL, NULL);

	if (e_contact_get (contact, E_CONTACT_IS_LIST)) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED,
			_("Contact lists cannot be stored in GroupWise address books"));
		return NULL;
	}

	fields = single_fields (contact);
	if (!g_hash_table_lookup (fields, "name")) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_INVALID_ARG,
			_("A contact needs a name"));
		g_hash_table_destroy (fields);
		return NULL;
	}

	xml = g_string_new ("<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Contact\">");
	e_gw_xml_add_leaf (xml, "container", container);

	paths = g_ptr_array_new ();
	values = g_ptr_array_new ();
	for (ii = 0; ii < G_N_ELEMENTS (single_paths); ii++) {
		const gchar *value = g_hash_table_lookup (fields, single_paths[ii]);

		/* Lists go in between, as gwmcp builds a contact */
		if (g_str_equal (single_paths[ii], "officeInfo/organization")) {
			append_fields (xml, paths, values);
			g_ptr_array_set_size (paths, 0);
			g_ptr_array_set_size (values, 0);

			list = email_list_xml (contact);
			if (list)
				g_string_append (xml, list);
			g_free (list);
			list = phone_list_xml (contact);
			if (list)
				g_string_append (xml, list);
			g_free (list);
			list = address_list_xml (contact);
			if (list)
				g_string_append (xml, list);
			g_free (list);
		}

		if (value) {
			g_ptr_array_add (paths, (gpointer) single_paths[ii]);
			g_ptr_array_add (values, (gpointer) value);
		}
	}
	append_fields (xml, paths, values);
	g_string_append (xml, "</item>");

	g_ptr_array_unref (paths);
	g_ptr_array_unref (values);
	g_hash_table_destroy (fields);

	return g_string_free (xml, FALSE);
}

/* The current list element as the POA keeps it, for <delete> */
static gchar *
current_list_xml (xmlNode *current,
		  const gchar *name)
{
	xmlNode *node = e_gw_xml_find (current, name);
	xmlBuffer *buffer;
	gchar *xml;

	if (!node)
		return NULL;

	buffer = xmlBufferCreate ();
	xmlNodeDump (buffer, node->doc, node, 0, 0);
	xml = g_strndup ((const gchar *) xmlBufferContent (buffer), xmlBufferLength (buffer));
	xmlBufferFree (buffer);

	return xml;
}

/* Two list elements alike, compared through the contact they make */
static gboolean
lists_equal (const gchar *a,
	     const gchar *b)
{
	return g_strcmp0 (a, b) == 0;
}

gchar *
e_gw_contact_updates_xml (xmlNode *current,
			  EContact *contact,
			  GError **error)
{
	GPtrArray *del_paths, *del_values, *add_paths, *add_values, *upd_paths, *upd_values;
	GPtrArray *del_lists, *add_lists;
	EContact *before;
	GHashTable *new_fields;
	GString *xml;
	guint ii;
	static const struct {
		const gchar *element;
		gchar * (*build) (EContact *contact);
	} lists[] = {
		{ "emailList", email_list_xml },
		{ "phoneList", phone_list_xml },
		{ "addressList", address_list_xml }
	};

	g_return_val_if_fail (current != NULL, NULL);
	g_return_val_if_fail (E_IS_CONTACT (contact), NULL);

	if (e_contact_get (contact, E_CONTACT_IS_LIST)) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED,
			_("Contact lists cannot be stored in GroupWise address books"));
		return NULL;
	}

	/* The current item seen as a contact: the same rules on both sides */
	before = e_gw_contact_from_item (current, NULL);
	if (!before) {
		g_set_error_literal (error, E_CLIENT_ERROR, E_CLIENT_ERROR_INVALID_ARG, _("Not a contact"));
		return NULL;
	}
	new_fields = single_fields (contact);

	del_paths = g_ptr_array_new (); del_values = g_ptr_array_new ();
	add_paths = g_ptr_array_new (); add_values = g_ptr_array_new ();
	upd_paths = g_ptr_array_new (); upd_values = g_ptr_array_new ();
	del_lists = g_ptr_array_new_with_free_func (g_free);
	add_lists = g_ptr_array_new_with_free_func (g_free);

	for (ii = 0; ii < G_N_ELEMENTS (single_paths); ii++) {
		const gchar *path = single_paths[ii];
		/* What the POA has, not what Evolution derived from it (a file-as from a display name) */
		gchar *raw = non_empty (e_gw_xml_dup_text (current, path));
		const gchar *new_value = g_hash_table_lookup (new_fields, path);

		if (raw && !new_value) {
			g_ptr_array_add (del_paths, (gpointer) path);
			g_ptr_array_add (del_values, raw);
			continue;
		}
		if (!raw && new_value) {
			g_ptr_array_add (add_paths, (gpointer) path);
			g_ptr_array_add (add_values, (gpointer) new_value);
		} else if (raw && new_value && g_strcmp0 (raw, new_value) != 0) {
			g_ptr_array_add (upd_paths, (gpointer) path);
			g_ptr_array_add (upd_values, (gpointer) new_value);
		}
		g_free (raw);
	}

	for (ii = 0; ii < G_N_ELEMENTS (lists); ii++) {
		gchar *old_list = current_list_xml (current, lists[ii].element);
		gchar *old_built = lists[ii].build (before);
		gchar *new_list = lists[ii].build (contact);

		if (!lists_equal (old_built, new_list)) {
			if (old_list)
				g_ptr_array_add (del_lists, old_list), old_list = NULL;
			if (new_list)
				g_ptr_array_add (add_lists, new_list), new_list = NULL;
		}
		g_free (old_list);
		g_free (old_built);
		g_free (new_list);
	}

	/* Deletions first, as gwmcp does */
	xml = g_string_new (NULL);
	if (del_paths->len || del_lists->len) {
		g_string_append (xml, "<delete>");
		append_fields (xml, del_paths, del_values);
		for (ii = 0; ii < del_lists->len; ii++)
			g_string_append (xml, del_lists->pdata[ii]);
		g_string_append (xml, "</delete>");
	}
	if (add_paths->len || add_lists->len) {
		g_string_append (xml, "<add>");
		append_fields (xml, add_paths, add_values);
		for (ii = 0; ii < add_lists->len; ii++)
			g_string_append (xml, add_lists->pdata[ii]);
		g_string_append (xml, "</add>");
	}
	if (upd_paths->len) {
		g_string_append (xml, "<update>");
		append_fields (xml, upd_paths, upd_values);
		g_string_append (xml, "</update>");
	}

	for (ii = 0; ii < del_values->len; ii++)
		g_free (del_values->pdata[ii]);
	g_ptr_array_unref (del_paths); g_ptr_array_unref (del_values);
	g_ptr_array_unref (add_paths); g_ptr_array_unref (add_values);
	g_ptr_array_unref (upd_paths); g_ptr_array_unref (upd_values);
	g_ptr_array_unref (del_lists);
	g_ptr_array_unref (add_lists);
	g_hash_table_destroy (new_fields);
	g_object_unref (before);

	return g_string_free (xml, FALSE);
}

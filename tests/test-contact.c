/*
 * test-contact.c: GroupWise address book items and vCards, with XML as a
 * GroupWise 26.2 POA returns it
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

#include <libxml/parser.h>

#include "e-gw-contact.h"

/* Karl's own entry in his Frequent Contacts */
static const gchar *karl =
	"<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Contact\">"
	"<id>6AB6AA10.dom.po1.104.1777A36.1.74.1@56:3ED1386A.dom.po2.104.1777A36.1.3.1@53</id>"
	"<name>Karl Napp</name><version>2</version><modified>2026-09-25T15:06:24Z</modified>"
	"<container>3ED1386A.dom.po2.104.1777A36.1.3.1@53</container>"
	"<fullName><displayName>Karl Napp</displayName><firstName>Karl</firstName><lastName>Napp</lastName></fullName>"
	"<emailList primary=\"KNapp@example.com\"><email>KNapp@example.com</email></emailList>"
	"<officeInfo><title>Dr.</title></officeInfo>"
	"<referenceInfo><referenceCount>1</referenceCount></referenceInfo></item>";

/* A contact of the mock with everything */
static const gchar *meier =
	"<item type='Contact'><id>CON1@56:AB1@5</id><name>Meier, Hans</name><container>AB1@5</container>"
	"<modified>2026-01-02T03:04:05Z</modified>"
	"<fullName><displayName>Meier, Hans</displayName><firstName>Hans</firstName><lastName>Meier</lastName></fullName>"
	"<emailList primary='hans@acme.example'><email>h.meier@privat.example</email><email>hans@acme.example</email></emailList>"
	"<phoneList default='+49 170 1234567'><phone type='Mobile'>+49 170 1234567</phone><phone type='Office'>+49 6221 222</phone></phoneList>"
	"<addressList mailingAddress='Office'><address type='Office'><streetAddress>Hauptstr. 1</streetAddress><city>Heidelberg</city>"
	"<postalCode>69117</postalCode><country>Deutschland</country></address></addressList>"
	"<officeInfo><organization uid='ORG1@53'>Acme GmbH</organization><department>Einkauf</department><website>www.acme.example</website></officeInfo>"
	"<personalInfo><birthday>1970-05-17</birthday></personalInfo>"
	"<comment>Ruft gern freitags an</comment></item>";

static xmlDoc *
parse (const gchar *xml)
{
	xmlDoc *doc = xmlReadMemory (xml, strlen (xml), NULL, "UTF-8", 0);

	g_assert_nonnull (doc);
	return doc;
}

static EContact *
contact_of (const gchar *xml)
{
	xmlDoc *doc = parse (xml);
	EContact *contact = e_gw_contact_from_item (xmlDocGetRootElement (doc), NULL);

	xmlFreeDoc (doc);
	return contact;
}

static void
test_read_contact (void)
{
	EContact *contact = contact_of (meier);
	EContactAddress *address;
	EContactDate *birthday;
	GList *emails;

	g_assert_nonnull (contact);
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_UID), ==, "CON1@56:AB1@5");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_REV), ==, "2026-01-02T03:04:05Z");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_FULL_NAME), ==, "Meier, Hans");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_GIVEN_NAME), ==, "Hans");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_FAMILY_NAME), ==, "Meier");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_FILE_AS), ==, "Meier, Hans");

	/* The primary address first, each once */
	emails = e_contact_get (contact, E_CONTACT_EMAIL);
	g_assert_cmpuint (g_list_length (emails), ==, 2);
	g_assert_cmpstr (emails->data, ==, "hans@acme.example");
	g_assert_cmpstr (emails->next->data, ==, "h.meier@privat.example");
	g_list_free_full (emails, g_free);

	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_PHONE_MOBILE), ==, "+49 170 1234567");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_PHONE_BUSINESS), ==, "+49 6221 222");
	g_assert_null (e_contact_get_const (contact, E_CONTACT_PHONE_HOME));

	address = e_contact_get (contact, E_CONTACT_ADDRESS_WORK);
	g_assert_nonnull (address);
	g_assert_cmpstr (address->street, ==, "Hauptstr. 1");
	g_assert_cmpstr (address->locality, ==, "Heidelberg");
	g_assert_cmpstr (address->code, ==, "69117");
	g_assert_cmpstr (address->country, ==, "Deutschland");
	e_contact_address_free (address);

	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_ORG), ==, "Acme GmbH");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_ORG_UNIT), ==, "Einkauf");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_HOMEPAGE_URL), ==, "www.acme.example");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_NOTE), ==, "Ruft gern freitags an");

	birthday = e_contact_get (contact, E_CONTACT_BIRTH_DATE);
	g_assert_nonnull (birthday);
	g_assert_cmpuint (birthday->year, ==, 1970);
	g_assert_cmpuint (birthday->month, ==, 5);
	g_assert_cmpuint (birthday->day, ==, 17);
	e_contact_date_free (birthday);

	g_object_unref (contact);
}

static void
test_read_system_entries (void)
{
	EContact *contact;
	xmlDoc *doc;
	gchar *revision1, *revision2;

	/* No display name in the system address book: the name or the user ID shows */
	contact = contact_of (
		"<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Contact\">"
		"<id>33333333-0256-0000-B062-000000000003@56:GroupWiseSystemAddressBook@52</id>"
		"<container>GroupWiseSystemAddressBook@52</container><userid>3cx00</userid>"
		"<fullName><displayName/></fullName><emailList primary=\"3cx00@example.com\"><email>3cx00@example.com</email></emailList></item>");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_FULL_NAME), ==, "3cx00");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_EMAIL_1), ==, "3cx00@example.com");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_FILE_AS), ==, "3cx00");
	g_object_unref (contact);

	/* Neither modified nor version: the revision is a digest, stable and changing with the content */
	doc = parse ("<item xsi:type=\"Contact\" xmlns:xsi=\"x\"><id>A@56:S@52</id><name>Backes, Rainer</name></item>");
	revision1 = e_gw_contact_revision (xmlDocGetRootElement (doc));
	xmlFreeDoc (doc);
	doc = parse ("<item xsi:type=\"Contact\" xmlns:xsi=\"x\"><id>A@56:S@52</id><name>Backes, Rainer</name></item>");
	revision2 = e_gw_contact_revision (xmlDocGetRootElement (doc));
	g_assert_cmpstr (revision1, ==, revision2);
	xmlFreeDoc (doc);
	g_free (revision2);
	doc = parse ("<item xsi:type=\"Contact\" xmlns:xsi=\"x\"><id>A@56:S@52</id><name>Backes, R.</name></item>");
	revision2 = e_gw_contact_revision (xmlDocGetRootElement (doc));
	g_assert_cmpstr (revision1, !=, revision2);
	xmlFreeDoc (doc);
	g_free (revision1);
	g_free (revision2);

	/* A resource from Karl's Frequent Contacts */
	contact = contact_of (
		"<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Resource\">"
		"<id>5E74D01B.dom.po1.104.1777A36.1.73.1@55:3ED1386A.dom.po2.104.1777A36.1.3.1@53</id>"
		"<name>Besprechung</name><email>Besprechung@example.com</email></item>");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_FULL_NAME), ==, "Besprechung");
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_EMAIL_1), ==, "Besprechung@example.com");
	g_assert_nonnull (e_vcard_get_attribute (E_VCARD (contact), E_GW_CONTACT_X_TYPE));
	g_object_unref (contact);

	/* Not an address book item */
	g_assert_null (contact_of ("<item xsi:type=\"Mail\" xmlns:xsi=\"x\"><id>M@1:F</id></item>"));
}

static void
test_read_group (void)
{
	xmlDoc *item = parse ("<item type='Group'><id>GRP1@58:AB1@5</id><name>Team</name></item>");
	xmlDoc *members = parse ("<item type='Group'><members>"
		"<member><id>CON1@56:AB1@5</id><name>Meier, Hans</name><email>hans@acme.example</email></member>"
		"<member><id>CONX@56:AB1@5</id><email>x@example.org</email></member></members></item>");
	EContact *contact = e_gw_contact_from_item (xmlDocGetRootElement (item), xmlDocGetRootElement (members));
	GList *attrs, *link;
	guint count = 0;

	g_assert_true (GPOINTER_TO_INT (e_contact_get (contact, E_CONTACT_IS_LIST)));
	g_assert_cmpstr (e_contact_get_const (contact, E_CONTACT_FULL_NAME), ==, "Team");

	attrs = e_contact_get_attributes (contact, E_CONTACT_EMAIL);
	for (link = attrs; link; link = g_list_next (link), count++) {
		EVCardAttribute *attr = link->data;
		GList *param = e_vcard_attribute_get_param (attr, EVC_X_DEST_EMAIL);

		g_assert_nonnull (param);
		if (count == 0) {
			g_assert_cmpstr (param->data, ==, "hans@acme.example");
			g_assert_cmpstr (e_vcard_attribute_get_value (attr), ==, "Meier, Hans <hans@acme.example>");
		} else {
			g_assert_cmpstr (e_vcard_attribute_get_value (attr), ==, "x@example.org");
		}
	}
	g_assert_cmpuint (count, ==, 2);
	g_list_free_full (attrs, (GDestroyNotify) e_vcard_attribute_free);

	g_object_unref (contact);
	xmlFreeDoc (members);
	xmlFreeDoc (item);
}

static void
test_write_new (void)
{
	EContact *contact = e_contact_new ();
	EContactName name = { (gchar *) "Weber", (gchar *) "Anna", NULL, NULL, NULL };
	EContactAddress address = { NULL, NULL, (gchar *) "Hauptstr. 1", (gchar *) "Heidelberg", NULL, (gchar *) "69117", NULL, NULL };
	GList *emails = NULL;
	GError *error = NULL;
	xmlDoc *doc;
	EContact *back;
	gchar *xml;

	e_contact_set (contact, E_CONTACT_NAME, &name);
	e_contact_set (contact, E_CONTACT_FULL_NAME, "Anna Weber");
	emails = g_list_append (emails, (gpointer) "anna@example.com");
	emails = g_list_append (emails, (gpointer) "a.weber@privat.example");
	e_contact_set (contact, E_CONTACT_EMAIL, emails);
	g_list_free (emails);
	e_contact_set (contact, E_CONTACT_PHONE_MOBILE, "+49 170 1");
	e_contact_set (contact, E_CONTACT_ADDRESS_WORK, &address);
	e_contact_set (contact, E_CONTACT_ORG, "Acme GmbH");
	e_contact_set (contact, E_CONTACT_TITLE, "Einkäuferin");

	xml = e_gw_contact_to_item_xml (contact, "AB1@5", &error);
	g_assert_no_error (error);
	g_assert_nonnull (strstr (xml, "xsi:type=\"Contact\""));
	g_assert_nonnull (strstr (xml, "<container>AB1@5</container>"));
	g_assert_nonnull (strstr (xml, "<fullName><displayName>Anna Weber</displayName><firstName>Anna</firstName><lastName>Weber</lastName></fullName>"));
	g_assert_nonnull (strstr (xml, "<emailList primary=\"anna@example.com\"><email>anna@example.com</email><email>a.weber@privat.example</email></emailList>"));
	g_assert_nonnull (strstr (xml, "<phoneList default=\"+49 170 1\"><phone type=\"Mobile\">+49 170 1</phone></phoneList>"));
	g_assert_nonnull (strstr (xml, "<addressList mailingAddress=\"Office\"><address type=\"Office\">"));
	g_assert_nonnull (strstr (xml, "<officeInfo><organization>Acme GmbH</organization><title>Einkäuferin</title></officeInfo>"));

	/* ... and read back the same */
	doc = parse (xml);
	back = e_gw_contact_from_item (xmlDocGetRootElement (doc), NULL);
	g_assert_null (back);	/* no ID yet: not an item of the server */
	xmlFreeDoc (doc);

	g_free (xml);
	g_object_unref (contact);
}

static void
test_write_list_refused (void)
{
	EContact *contact = e_contact_new ();
	GError *error = NULL;

	e_contact_set (contact, E_CONTACT_IS_LIST, GINT_TO_POINTER (TRUE));
	e_contact_set (contact, E_CONTACT_FULL_NAME, "Liste");
	g_assert_null (e_gw_contact_to_item_xml (contact, "AB1@5", &error));
	g_assert_error (error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED);
	g_clear_error (&error);
	g_object_unref (contact);
}

static gchar *
updates_for (const gchar *current_xml,
	     EContact *contact)
{
	xmlDoc *doc = parse (current_xml);
	GError *error = NULL;
	gchar *updates = e_gw_contact_updates_xml (xmlDocGetRootElement (doc), contact, &error);

	g_assert_no_error (error);
	xmlFreeDoc (doc);

	return updates;
}

static void
test_updates (void)
{
	EContact *contact;
	gchar *updates;

	/* Unchanged: nothing to send */
	contact = contact_of (karl);
	updates = updates_for (karl, contact);
	g_assert_cmpstr (updates, ==, "");
	g_free (updates);

	/* A changed field, a new list, a removed field */
	e_contact_set (contact, E_CONTACT_TITLE, "Prof.");
	e_contact_set (contact, E_CONTACT_PHONE_MOBILE, "+49 171 2");
	e_contact_set (contact, E_CONTACT_ORG_UNIT, "Vertrieb");
	updates = updates_for (karl, contact);
	g_assert_cmpstr (updates, ==,
		"<add><officeInfo><department>Vertrieb</department></officeInfo>"
		"<phoneList default=\"+49 171 2\"><phone type=\"Mobile\">+49 171 2</phone></phoneList></add>"
		"<update><officeInfo><title>Prof.</title></officeInfo></update>");
	g_free (updates);
	g_object_unref (contact);

	/* Removed: the current value in <delete>; a changed list: old out, new in */
	contact = contact_of (meier);
	e_contact_set (contact, E_CONTACT_NOTE, NULL);
	e_contact_set (contact, E_CONTACT_PHONE_MOBILE, NULL);
	updates = updates_for (meier, contact);
	g_assert_nonnull (strstr (updates, "<delete><comment>Ruft gern freitags an</comment>"
		"<phoneList default=\"+49 170 1234567\"><phone type=\"Mobile\">+49 170 1234567</phone>"
		"<phone type=\"Office\">+49 6221 222</phone></phoneList></delete>"));
	g_assert_nonnull (strstr (updates, "<add><phoneList default=\"+49 6221 222\"><phone type=\"Office\">+49 6221 222</phone></phoneList></add>"));
	g_assert_null (strstr (updates, "emailList"));
	g_assert_null (strstr (updates, "<update>"));
	g_free (updates);

	g_object_unref (contact);

	/* Emails in another order with the same primary: unchanged */
	contact = contact_of (meier);
	updates = updates_for (meier, contact);
	g_assert_cmpstr (updates, ==, "");
	g_free (updates);
	g_object_unref (contact);
}

int
main (int argc,
      char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/contact/read", test_read_contact);
	g_test_add_func ("/contact/read-system", test_read_system_entries);
	g_test_add_func ("/contact/read-group", test_read_group);
	g_test_add_func ("/contact/write-new", test_write_new);
	g_test_add_func ("/contact/write-list-refused", test_write_list_refused);
	g_test_add_func ("/contact/updates", test_updates);

	return g_test_run ();
}

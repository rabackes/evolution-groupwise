/*
 * test-utils.c: GroupWise items to Camel message infos, with XML as a real
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

#include "camel-groupwise-mime.h"
#include "camel-groupwise-utils.h"
#include "e-gw-category.h"
#include "e-gw-folder.h"
#include "e-gw-signature-mime.h"
#include "e-gw-xml.h"

static CamelMessageInfo *
info_from_xml (const gchar *xml)
{
	static CamelFolderSummary *summary = NULL;
	xmlDoc *doc = xmlReadMemory (xml, strlen (xml), NULL, "UTF-8", 0);
	CamelMessageInfo *info;

	g_assert_nonnull (doc);
	if (!summary)
		summary = camel_folder_summary_new (NULL);

	info = camel_groupwise_message_info_new_from_item (summary, xmlDocGetRootElement (doc));
	xmlFreeDoc (doc);

	return info;
}

static void
test_internal_unread (void)
{
	CamelMessageInfo *info = info_from_xml (
		"<item xsi:type=\"Mail\" xmlns:xsi=\"x\">"
		"<id>6AB7EAA5.dom.po1.100.1777A36.1.421.1@1:7.dom.po1.100.0.1.0.1@16</id>"
		"<modified>2026-09-26T13:54:13Z</modified><created>2026-09-26T13:54:13Z</created>"
		"<source>received</source><delivered>2026-09-26T13:54:14Z</delivered>"
		"<subject>Kannst du ein Geschenk für Bettina besorgen?</subject>"
		"<distribution><from><displayName>Rainer Backes</displayName><email>RBackes@example.com</email>"
		"<uuid>05B22960</uuid></from><to>Napp, Karl</to></distribution>"
		"<options><priority>Standard</priority></options><size>1712</size>"
		"<messageId>&lt;6AB7CE85020000AF000473EA@example.com&gt;</messageId></item>");

	g_assert_cmpstr (camel_message_info_get_uid (info), ==, "6AB7EAA5.dom.po1.100.1777A36.1.421.1@1:7.dom.po1.100.0.1.0.1@16");
	g_assert_cmpstr (camel_message_info_get_subject (info), ==, "Kannst du ein Geschenk für Bettina besorgen?");
	g_assert_cmpstr (camel_message_info_get_from (info), ==, "Rainer Backes <RBackes@example.com>");
	g_assert_cmpstr (camel_message_info_get_to (info), ==, "Napp, Karl");
	g_assert_cmpint (camel_message_info_get_date_received (info), ==, 1790430854);	/* 2026-09-26T13:54:14Z */
	g_assert_cmpuint (camel_message_info_get_size (info), ==, 1712);
	/* No <status>: unread */
	g_assert_cmpuint (camel_message_info_get_flags (info), ==, 0);
	g_assert_cmpuint (camel_message_info_get_message_id (info),
		==, camel_folder_search_util_hash_message_id ("<6AB7CE85020000AF000473EA@example.com>", TRUE));

	g_object_unref (info);
}

static void
test_internet_sender (void)
{
	CamelMessageInfo *info = info_from_xml (
		"<item><id>X1@1:F</id><status><opened>1</opened><read>1</read><replied>1</replied></status>"
		"<distribution><from><displayName>Rainer Backes &lt;rbackes@gmail.com&gt;</displayName>"
		"<email>rbackes@gmail.com</email></from></distribution><hasAttachment>1</hasAttachment>"
		"<options><priority>High</priority></options></item>");

	g_assert_cmpstr (camel_message_info_get_from (info), ==, "Rainer Backes <rbackes@gmail.com>");
	g_assert_cmpuint (camel_message_info_get_flags (info), ==,
		CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_ANSWERED | CAMEL_MESSAGE_ATTACHMENTS | CAMEL_MESSAGE_FLAGGED);

	g_object_unref (info);
}

static void
test_draft (void)
{
	CamelMessageInfo *info = info_from_xml (
		"<item><id>\n  D1@1:\n  F </id><source>draft</source><created>2026-09-25T18:00:00Z</created>"
		"<subject>Entwurf</subject></item>");

	/* Wrapped IDs are cleaned, drafts dated by creation */
	g_assert_cmpstr (camel_message_info_get_uid (info), ==, "D1@1:F");
	g_assert_true (camel_message_info_get_flags (info) & CAMEL_MESSAGE_DRAFT);
	g_assert_cmpint (camel_message_info_get_date_received (info), ==, 1790359200);	/* 2026-09-25T18:00:00Z */
	g_assert_null (camel_message_info_get_from (info));

	g_object_unref (info);
}

static void
test_invitation_date (void)
{
	/* Listed by the day of the appointment, like in the GroupWise client */
	CamelMessageInfo *info = info_from_xml (
		"<item xsi:type=\"Appointment\" xmlns:xsi=\"x\"><id>A1@4:F</id><source>received</source>"
		"<delivered>2026-09-27T15:05:06Z</delivered><subject>Testbesprechung von Karl</subject>"
		"<startDate>2026-10-09T08:00:00Z</startDate></item>");

	g_assert_cmpint (camel_message_info_get_date_sent (info), ==, 1791532800);	/* 2026-10-09T08:00:00Z */
	g_assert_cmpint (camel_message_info_get_date_received (info), ==, 1790521506);	/* 2026-09-27T15:05:06Z */
	g_object_unref (info);

	/* A task by its start day */
	info = info_from_xml (
		"<item xsi:type=\"Task\" xmlns:xsi=\"x\"><id>T1@3:F</id><delivered>2026-09-27T15:05:06Z</delivered>"
		"<startDate>2026-10-09</startDate></item>");
	g_assert_cmpint (camel_message_info_get_date_sent (info), ==, 1791504000);	/* 2026-10-09T00:00:00Z */
	g_object_unref (info);

	/* Mail keeps its delivery */
	info = info_from_xml ("<item xsi:type=\"Mail\" xmlns:xsi=\"x\"><id>M1@1:F</id>"
		"<delivered>2026-09-27T15:05:06Z</delivered><startDate>2026-10-09T08:00:00Z</startDate></item>");
	g_assert_cmpint (camel_message_info_get_date_sent (info), ==, 1790521506);
	g_object_unref (info);
}

static gchar *
fake_embed (const gchar *item_id,
	    gchar **out_link_xml,
	    gpointer user_data,
	    GCancellable *cancellable)
{
	/* The server knows only this item */
	if (g_strcmp0 (item_id, "ORIG1@1:INBOX") != 0)
		return NULL;

	*out_link_xml = g_strdup ("<link><id>0:ORIG1</id><type>forward</type></link>");

	return g_strdup ("<attachment><id itemReference=\"1\">REF1@49:X</id><name>FILE</name>"
		"<contentType>Mail</contentType></attachment>");
}

/* Forwarded as attachment: a message of the account goes as the item */
static void
test_mime_forward_item (void)
{
	const gchar *raw_template =
		"From: <a@example.com>\r\nTo: <b@example.com>\r\nSubject: Fwd: Hallo\r\nMIME-Version: 1.0\r\n"
		"Content-Type: multipart/mixed; boundary=\"MIX\"\r\n\r\n"
		"--MIX\r\nContent-Type: text/plain\r\n\r\nSiehe Anhang\r\n"
		"--MIX\r\nContent-Type: message/rfc822\r\nContent-Disposition: attachment\r\n\r\n"
		"%sFrom: <c@example.com>\r\nSubject: Hallo & Grüße\r\nContent-Type: text/plain\r\n\r\nOriginal\r\n"
		"--MIX--\r\n";
	gchar *raw, *xml;
	CamelMimeMessage *message;
	GInputStream *in;
	GError *error = NULL;

	/* Known to the server: the reference, named after the message, and the link */
	raw = g_strdup_printf (raw_template, "X-GroupWise-Item-Id: ORIG1@1:INBOX\r\n");
	message = camel_mime_message_new ();
	in = g_memory_input_stream_new_from_data (raw, -1, NULL);
	camel_data_wrapper_construct_from_input_stream_sync (CAMEL_DATA_WRAPPER (message), in, NULL, NULL);
	xml = camel_groupwise_item_from_message (message, NULL, FALSE, fake_embed, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (strstr (xml, "<attachment><id itemReference=\"1\">REF1@49:X</id><name>Hallo &amp; Grüße</name>"));
	g_assert_nonnull (strstr (xml, "</attachments><link><id>0:ORIG1</id><type>forward</type></link></item>"));
	g_assert_null (strstr (xml, "message/rfc822"));
	g_free (xml);
	g_object_unref (in);
	g_object_unref (message);
	g_free (raw);

	/* Not known, or without the header: attached as MIME */
	raw = g_strdup_printf (raw_template, "X-GroupWise-Item-Id: OTHER@1:INBOX\r\n");
	message = camel_mime_message_new ();
	in = g_memory_input_stream_new_from_data (raw, -1, NULL);
	camel_data_wrapper_construct_from_input_stream_sync (CAMEL_DATA_WRAPPER (message), in, NULL, NULL);
	xml = camel_groupwise_item_from_message (message, NULL, FALSE, fake_embed, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (strstr (xml, "message/rfc822"));
	g_assert_null (strstr (xml, "itemReference"));
	g_assert_null (strstr (xml, "<link>"));
	g_free (xml);
	g_object_unref (in);
	g_object_unref (message);
	g_free (raw);
}

static void
test_trash_date (void)
{
	/* The Trash lists every folder an item left, with the time */
	CamelMessageInfo *info = info_from_xml (
		"<item><id>T1@1:TR</id><delivered>2026-09-25T15:18:17Z</delivered>"
		"<container deleted=\"2026-09-25T15:18:18Z\">7@16</container>"
		"<container deleted=\"2026-09-26T10:00:00Z\">X@13</container><container>9@18</container></item>");

	g_assert_cmpint (camel_message_info_get_date_received (info), ==, 1790416800);	/* 2026-09-26T10:00:00Z */
	g_assert_cmpint (camel_message_info_get_date_sent (info), ==, 1790349497);	/* 2026-09-25T15:18:17Z */
	g_object_unref (info);
}

/* The calendar folders of a GroupWise 26.2 mailbox (names changed) */
static void
test_own_subcalendars (void)
{
	static const gchar *xml =
		"<folders xmlns:xsi=\"x\">"
		"<folder xsi:type=\"SystemFolder\"><id>ROOT@15</id><name>Home</name><folderType>Root</folderType></folder>"
		"<folder xsi:type=\"SystemFolder\"><id>CAL@19</id><name>Calendar</name><parent>ROOT@15</parent>"
		"<folderType>Calendar</folderType><calendarAttribute><flags>ShowInList</flags></calendarAttribute></folder>"
		"<folder xsi:type=\"SharedFolder\"><id>PRIV@35</id><name>Privat</name><parent>CAL@19</parent>"
		"<description>Private Termine</description><calendarAttribute><flags>ShowInList</flags><color>16763904</color></calendarAttribute>"
		"<isSharedByMe>1</isSharedByMe></folder>"
		"<folder xsi:type=\"Folder\"><id>WEB@13</id><name>webinar</name><parent>CAL@19</parent>"
		"<calendarAttribute><flags>ShowInList</flags><flags>DontIncludeContent</flags></calendarAttribute></folder>"
		"<folder xsi:type=\"ProxyFolder\"><id>PROXY@81</id><name>Anna Kalender</name><parent>CAL@19</parent>"
		"<description>Muster, Anna-Proxy-Kalender.</description><calendarAttribute><flags>ShowInList</flags><color>255</color></calendarAttribute>"
		"<folderType>Proxy</folderType><proxy><email>anna@example.com</email><uuid>U1</uuid></proxy></folder>"
		"<folder xsi:type=\"SharedFolder\"><id>SH@34:X</id><name>Termine</name><parent>CAL@19</parent><isSharedToMe>1</isSharedToMe>"
		"<calendarAttribute><flags>ShowInList</flags></calendarAttribute>"
		"<owner><displayName>Egon Mueller</displayName><email>egon@example.com</email><uuid>U2</uuid></owner></folder>"
		"<folder xsi:type=\"SystemFolder\"><id>GOO@13</id><name>google</name><parent>CAL@19</parent>"
		"<calendarAttribute><flags>ShowInList</flags><flags>DontIncludeContent</flags></calendarAttribute>"
		"<URL>http://example.com/basic.ics</URL><isSystemFolder>1</isSystemFolder><folderType>Subscribe</folderType></folder>"
		"<folder xsi:type=\"Folder\"><id>COLS@13</id><name>Mehrfachbenutzer-Spalten</name><parent>CAL@19</parent>"
		"<calendarAttribute><flags>ShowInList</flags><flags>DontIncludeContent</flags></calendarAttribute></folder>"
		"<folder xsi:type=\"ProxyFolder\"><id>COL1@81</id><name>Bob Kalender</name><parent>COLS@13</parent>"
		"<description>Bob-Proxy-Kalender.</description><calendarAttribute><flags>ShowInList</flags></calendarAttribute>"
		"<folderType>Proxy</folderType><proxy><email>bob@example.com</email></proxy></folder>"
		"<folder xsi:type=\"Folder\"><id>MAIL@13</id><name>Projekte</name><parent>ROOT@15</parent></folder>"
		"</folders>";
	xmlDoc *doc = xmlReadMemory (xml, strlen (xml), NULL, "UTF-8", 0);
	GPtrArray *folders = g_ptr_array_new_with_free_func ((GDestroyNotify) e_gw_folder_free);
	GString *own = g_string_new (NULL);
	xmlNode *node;
	guint ii;

	for (node = xmlDocGetRootElement (doc)->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE)
			g_ptr_array_add (folders, e_gw_folder_new_from_node (node));
	}
	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];

		if (e_gw_folder_is_own_subcalendar (folder, folders))
			g_string_append_printf (own, "%s%s ", folder->name, folder->includes_content ? "" : "(only)");
	}
	g_assert_cmpstr (own->str, ==, "Privat webinar(only) ");

	/* Each calendar with its role ("Privat" the user shares to others is the
	 * user's own); the columns folder, the subscribed calendar and the mail
	 * folder have none */
	g_string_truncate (own, 0);
	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];
		static const gchar *roles[] = { "-", "main", "own", "proxy", "shared" };

		g_string_append_printf (own, "%s:%s ", folder->name, roles[e_gw_folder_get_calendar_role (folder, folders)]);
	}
	g_assert_cmpstr (own->str, ==, "Home:- Calendar:main Privat:own webinar:own Anna Kalender:proxy Termine:shared "
		"google:- Mehrfachbenutzer-Spalten:- Bob Kalender:proxy Projekte:- ");

	g_assert_cmpstr (((EGwFolder *) folders->pdata[4])->proxy_email, ==, "anna@example.com");
	{
		gchar *color = e_gw_folder_dup_color (folders->pdata[2]);

		/* COLORREF 16763904 = 0x00FFCC00: red 00, green CC, blue FF */
		g_assert_cmpstr (color, ==, "#00ccff");
		g_free (color);
		color = e_gw_folder_dup_color (folders->pdata[4]);
		g_assert_cmpstr (color, ==, "#ff0000");
		g_free (color);
		g_assert_null (e_gw_folder_dup_color (folders->pdata[3]));
	}
	g_assert_cmpstr (((EGwFolder *) folders->pdata[5])->owner, ==, "egon@example.com");
	g_assert_cmpstr (((EGwFolder *) folders->pdata[5])->owner_name, ==, "Egon Mueller");

	g_string_free (own, TRUE);
	g_ptr_array_unref (folders);
	xmlFreeDoc (doc);
}

/* Categories: the list names the built-in ones @61, items @12 */
static void
test_categories (void)
{
	static const gchar *xml =
		"<item><id>M1@1:7@16</id><categories>"
		"<category>3.dom.po1.100.0.1.0.1@12</category><category>\n  524AA5D3.dom.po1.100.1613372.1.152109.1@12</category>"
		"</categories></item>";
	xmlDoc *doc = xmlReadMemory (xml, strlen (xml), NULL, "UTF-8", 0);
	const gchar *after[] = { "3.dom.po1.100.0.1.0.1@12", "K9@12", NULL };
	gchar **ids, *ref, *updates;

	ref = e_gw_category_ref ("1.dom.po1.100.0.1.0.1@61");
	g_assert_cmpstr (ref, ==, "1.dom.po1.100.0.1.0.1@12");
	g_free (ref);

	ids = e_gw_item_dup_categories (xmlDocGetRootElement (doc));
	g_assert_cmpuint (g_strv_length (ids), ==, 2);
	g_assert_cmpstr (ids[1], ==, "524AA5D3.dom.po1.100.1613372.1.152109.1@12");

	updates = e_gw_categories_updates_xml ((const gchar * const *) ids, after);
	g_assert_cmpstr (updates, ==, "<add><categories><category>K9@12</category></categories></add>"
		"<delete><categories><category>524AA5D3.dom.po1.100.1613372.1.152109.1@12</category></categories></delete>");
	g_free (updates);
	updates = e_gw_categories_updates_xml ((const gchar * const *) ids, (const gchar * const *) ids);
	g_assert_cmpstr (updates, ==, "");
	g_free (updates);

	g_strfreev (ids);
	xmlFreeDoc (doc);
}

static void
test_no_id (void)
{
	g_assert_null (info_from_xml ("<item><subject>kaputt</subject></item>"));
}

/* ------------------------------------------------------------------ */
/* MIME to GroupWise items */

static CamelMimeMessage *
parse_message (const gchar *raw)
{
	CamelMimeMessage *message = camel_mime_message_new ();
	GInputStream *in = g_memory_input_stream_new_from_data (raw, strlen (raw), NULL);

	g_assert_true (camel_data_wrapper_construct_from_input_stream_sync (CAMEL_DATA_WRAPPER (message), in, NULL, NULL));
	g_object_unref (in);

	return message;
}

/* Converts and parses the item; the returned doc owns the node */
static xmlNode *
item_of (const gchar *raw,
	 CamelAddress *envelope,
	 gboolean draft,
	 xmlDoc **doc)
{
	CamelMimeMessage *message = parse_message (raw);
	GError *error = NULL;
	gchar *xml = camel_groupwise_item_from_message (message, envelope, draft, NULL, NULL, NULL, &error);

	g_assert_no_error (error);
	g_assert_nonnull (xml);
	*doc = xmlReadMemory (xml, strlen (xml), NULL, "UTF-8", 0);
	g_assert_nonnull (*doc);

	g_free (xml);
	g_object_unref (message);

	return xmlDocGetRootElement (*doc);
}

static gchar *
base64_text (xmlNode *node,
	     const gchar *path)
{
	gchar *encoded = e_gw_xml_dup_text (node, path);
	gsize len;
	guchar *data = g_base64_decode (encoded, &len);
	gchar *text = g_strndup ((const gchar *) data, len);

	g_free (data);
	g_free (encoded);

	return text;
}

static xmlNode *
nth_child (xmlNode *parent,
	   const gchar *name,
	   guint nth)
{
	xmlNode *node = e_gw_xml_first_child (parent, name);

	while (node && nth--)
		node = e_gw_xml_next_sibling (node, name);

	return node;
}

static void
assert_text (xmlNode *node,
	     const gchar *path,
	     const gchar *expected)
{
	gchar *text = e_gw_xml_dup_text (node, path);

	g_assert_cmpstr (text, ==, expected);
	g_free (text);
}

static void
test_mime_plain (void)
{
	xmlDoc *doc;
	xmlNode *item, *recipients;
	gchar *text;

	item = item_of (
		"From: Karl Napp <KNapp@example.com>\r\n"
		"To: Rainer Backes <RBackes@example.com>, anna@example.com\r\n"
		"Cc: \"Weber, Max\" <max@example.com>\r\n"
		"Subject: =?UTF-8?Q?Gr=C3=BC=C3=9Fe?=\r\n"
		"X-Priority: 1\r\n"
		"Content-Type: text/plain; charset=ISO-8859-1\r\n"
		"Content-Transfer-Encoding: quoted-printable\r\n"
		"\r\n"
		"Viele Gr=FC=DFe\r\n", NULL, FALSE, &doc);

	assert_text (item, "subject", "Grüße");
	g_assert_null (e_gw_xml_find (item, "source"));
	assert_text (item, "options/priority", "High");

	recipients = e_gw_xml_find (item, "distribution/recipients");
	assert_text (nth_child (recipients, "recipient", 0), "displayName", "Rainer Backes");
	assert_text (nth_child (recipients, "recipient", 0), "email", "RBackes@example.com");
	assert_text (nth_child (recipients, "recipient", 0), "distType", "TO");
	assert_text (nth_child (recipients, "recipient", 1), "email", "anna@example.com");
	g_assert_null (e_gw_xml_find (nth_child (recipients, "recipient", 1), "displayName"));
	assert_text (nth_child (recipients, "recipient", 2), "displayName", "Weber, Max");
	assert_text (nth_child (recipients, "recipient", 2), "distType", "CC");
	g_assert_null (nth_child (recipients, "recipient", 3));

	/* Latin-1 arrives as UTF-8 */
	text = base64_text (item, "message/part");
	g_assert_cmpstr (text, ==, "Viele Grüße\n");
	g_free (text);
	g_assert_null (e_gw_xml_find (item, "attachments"));

	xmlFreeDoc (doc);
}

static const gchar *html_message =
	"From: Karl Napp <KNapp@example.com>\r\n"
	"To: RBackes@example.com\r\n"
	"Subject: HTML\r\n"
	"MIME-Version: 1.0\r\n"
	"Content-Type: multipart/mixed; boundary=\"MIX\"\r\n"
	"\r\n"
	"--MIX\r\n"
	"Content-Type: multipart/alternative; boundary=\"ALT\"\r\n"
	"\r\n"
	"--ALT\r\n"
	"Content-Type: text/plain; charset=UTF-8\r\n"
	"\r\n"
	"Hallo\r\n"
	"--ALT\r\n"
	"Content-Type: multipart/related; boundary=\"REL\"\r\n"
	"\r\n"
	"--REL\r\n"
	"Content-Type: text/html; charset=UTF-8\r\n"
	"\r\n"
	"<p>Hallo <img src=\"cid:logo@bond\"></p>\r\n"
	"--REL\r\n"
	"Content-Type: image/png\r\n"
	"Content-ID: <logo@bond>\r\n"
	"Content-Transfer-Encoding: base64\r\n"
	"\r\n"
	"iVBORw0=\r\n"
	"--REL--\r\n"
	"--ALT--\r\n"
	"--MIX\r\n"
	"Content-Type: application/pdf; name=\"Angebot.pdf\"\r\n"
	"Content-Disposition: attachment; filename=\"Angebot.pdf\"\r\n"
	"Content-Transfer-Encoding: base64\r\n"
	"\r\n"
	"JVBERi0=\r\n"
	"--MIX--\r\n";

static void
test_mime_html (void)
{
	xmlDoc *doc;
	xmlNode *item, *attachments, *att;
	gchar *text;

	item = item_of (html_message, NULL, FALSE, &doc);

	text = base64_text (item, "message/part");
	g_assert_cmpstr (text, ==, "Hallo");
	g_free (text);

	/* HTML body, its picture, then the real attachment (like gwmcp) */
	attachments = e_gw_xml_find (item, "attachments");
	att = nth_child (attachments, "attachment", 0);
	assert_text (att, "name", "Text.htm");
	/* Without the charset the POA takes the UTF-8 text for US-ASCII, and
	 * the GroupWise client shows its umlauts as two characters */
	assert_text (att, "charset", "UTF-8");
	assert_text (att, "contentType", "TEXT/HTML");
	assert_text (att, "hidden", "1");
	text = base64_text (att, "data");
	g_assert_nonnull (strstr (text, "<img src=\"cid:logo@bond\">"));
	g_assert_true (g_str_has_prefix (text, "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">"));
	g_free (text);

	att = nth_child (attachments, "attachment", 1);
	assert_text (att, "contentType", "image/png");
	assert_text (att, "contentId", "<logo@bond>");
	assert_text (att, "hidden", "1");
	assert_text (att, "size", "5");

	att = nth_child (attachments, "attachment", 2);
	assert_text (att, "name", "Angebot.pdf");
	assert_text (att, "contentType", "application/pdf");
	g_assert_null (e_gw_xml_find (att, "hidden"));
	text = base64_text (att, "data");
	g_assert_cmpstr (text, ==, "%PDF-");
	g_free (text);

	g_assert_null (nth_child (attachments, "attachment", 3));
	xmlFreeDoc (doc);
}

static void
test_mime_html_only (void)
{
	xmlDoc *doc;
	xmlNode *item;
	gchar *text;

	item = item_of (
		"To: RBackes@example.com\r\nSubject: Nur HTML\r\n"
		"Content-Type: text/html; charset=UTF-8\r\n\r\n"
		"<html><body><p>Erste Zeile</p><p>Zweite <b>Zeile</b></p></body></html>\r\n", NULL, FALSE, &doc);

	/* The body declares UTF-8 in its head; another declaration goes */
	{
		xmlDoc *doc2;
		xmlNode *item2 = item_of (
			"To: RBackes@example.com\r\nSubject: Umlaute\r\n"
			"Content-Type: text/html; charset=UTF-8\r\n\r\n"
			"<html><head><meta charset=\"iso-8859-1\"><title>T</title></head><body>Grüße</body></html>\r\n",
			NULL, FALSE, &doc2);
		gchar *html = base64_text (nth_child (e_gw_xml_find (item2, "attachments"), "attachment", 0), "data");

		g_assert_nonnull (strstr (html, "<head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\"><title>"));
		g_assert_null (strstr (html, "iso-8859-1"));
		g_assert_nonnull (strstr (html, "Grüße"));
		g_free (html);
		xmlFreeDoc (doc2);
	}

	/* The text alternative comes from the HTML */
	text = base64_text (item, "message/part");
	g_assert_nonnull (strstr (text, "Erste Zeile"));
	g_assert_nonnull (strstr (text, "Zweite Zeile"));
	g_assert_null (strstr (text, "<"));
	g_free (text);
	assert_text (item, "attachments/attachment/name", "Text.htm");

	xmlFreeDoc (doc);
}

static void
test_mime_envelope (void)
{
	CamelInternetAddress *envelope = camel_internet_address_new ();
	xmlDoc *doc;
	xmlNode *item, *recipients;

	/* Bcc is only in the envelope when sending */
	camel_internet_address_add (envelope, "Rainer Backes", "RBackes@example.com");
	camel_internet_address_add (envelope, NULL, "geheim@example.com");
	item = item_of ("To: Rainer Backes <RBackes@example.com>\r\nSubject: x\r\n\r\nText\r\n",
		CAMEL_ADDRESS (envelope), FALSE, &doc);

	recipients = e_gw_xml_find (item, "distribution/recipients");
	assert_text (nth_child (recipients, "recipient", 0), "distType", "TO");
	assert_text (nth_child (recipients, "recipient", 1), "email", "geheim@example.com");
	assert_text (nth_child (recipients, "recipient", 1), "distType", "BC");

	xmlFreeDoc (doc);
	g_object_unref (envelope);
}

static void
test_mime_draft (void)
{
	CamelMimeMessage *message = parse_message ("Subject: Entwurf\r\n\r\nNoch ohne Empfänger\r\n");
	GError *error = NULL;
	xmlDoc *doc;
	xmlNode *item;

	/* Sending needs recipients ... */
	g_assert_null (camel_groupwise_item_from_message (message, NULL, FALSE, NULL, NULL, NULL, &error));
	g_assert_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_INVALID);
	g_clear_error (&error);
	g_object_unref (message);

	/* ... a draft does not */
	item = item_of ("Subject: Entwurf\r\n\r\nNoch ohne Empfänger\r\n", NULL, TRUE, &doc);
	assert_text (item, "source", "draft");
	g_assert_null (e_gw_xml_find (item, "distribution/recipients/recipient"));
	xmlFreeDoc (doc);
}

static void
assert_refused (const gchar *raw)
{
	CamelMimeMessage *message = parse_message (raw);
	GError *error = NULL;

	g_assert_null (camel_groupwise_item_from_message (message, NULL, FALSE, NULL, NULL, NULL, &error));
	g_assert_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_INVALID);
	g_clear_error (&error);
	g_object_unref (message);
}

static void
test_mime_signed (void)
{
	/* The POA cannot pass a signature or an encryption on: refused, not
	 * sent unsigned or as an unreadable attachment */
	assert_refused (
		"To: RBackes@example.com\r\nSubject: signiert\r\n"
		"Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; boundary=\"S\"\r\n\r\n"
		"--S\r\nContent-Type: text/plain\r\n\r\nInhalt\r\n"
		"--S\r\nContent-Type: application/pkcs7-signature; name=smime.p7s\r\n"
		"Content-Disposition: attachment; filename=smime.p7s\r\n\r\nSIG\r\n--S--\r\n");
	assert_refused (
		"To: RBackes@example.com\r\nSubject: S/MIME\r\n"
		"Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name=smime.p7m\r\n"
		"Content-Transfer-Encoding: base64\r\n\r\nMIAGCSqGSIb3DQEHA6CAMIACAQA=\r\n");
	assert_refused (
		"To: RBackes@example.com\r\nSubject: PGP\r\n"
		"Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"E\"\r\n\r\n"
		"--E\r\nContent-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n"
		"--E\r\nContent-Type: application/octet-stream\r\n\r\n-----BEGIN PGP MESSAGE-----\r\n--E--\r\n");
}

/* A signature as the GroupWise client writes it: HTML with a picture */
static const gchar *gw_signature =
	"Mime-Version: 1.0\r\n"
	"Content-Type: multipart/mixed; boundary=\"M\"\r\n\r\n"
	"--M\r\nContent-Type: multipart/alternative; boundary=\"A\"\r\n\r\n"
	"--A\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nViele Gr\xc3\xbc\xc3\x9f""e\r\n"
	"--A\r\nContent-Type: multipart/related; boundary=\"R\"\r\n\r\n"
	"--R\r\nContent-Type: text/html; charset=windows-1252\r\n\r\n"
	"<html><body><p>Viele Gr\xfc\xdf""e</p><img src=\"cid:LOGO.bond.png\"></body></html>\r\n"
	"--R\r\nContent-ID: <LOGO.bond.png>\r\nContent-Type: image/png\r\nContent-Transfer-Encoding: base64\r\n\r\n"
	"iVBORw0=\r\n"
	"--R--\r\n--A--\r\n--M--\r\n";

static void
test_signature_mime (void)
{
	GBytes *mime = g_bytes_new_static (gw_signature, strlen (gw_signature));
	gchar *content, *mime_type = NULL, *again, *again_type = NULL, *sum1, *sum2;
	GBytes *built;
	const gchar *data;
	gsize len;

	/* Into Evolution: UTF-8 HTML, the picture as data: URI */
	content = e_gw_signature_mime_to_evolution (mime, &mime_type);
	g_assert_cmpstr (mime_type, ==, "text/html");
	g_assert_nonnull (strstr (content, "Viele Grüße"));
	g_assert_nonnull (strstr (content, "src=\"data:image/png;base64,iVBORw0=\""));
	g_assert_null (strstr (content, "cid:"));

	/* Back to GroupWise: text and HTML, the picture a part of its own */
	built = e_gw_signature_mime_from_evolution (content, mime_type);
	data = g_bytes_get_data (built, &len);
	g_assert_nonnull (g_strstr_len (data, len, "Content-Type: multipart/related"));
	g_assert_nonnull (g_strstr_len (data, len, "Content-ID: <IMAGE1.png@evolution>"));
	g_assert_nonnull (g_strstr_len (data, len, "Content-Type: text/plain; charset=utf-8"));

	/* ... and the same once more */
	again = e_gw_signature_mime_to_evolution (built, &again_type);
	g_assert_cmpstr (again_type, ==, "text/html");
	g_assert_nonnull (strstr (again, "Viele Grüße"));
	g_assert_nonnull (strstr (again, "src=\"data:image/png;base64,iVBORw0=\""));
	sum1 = e_gw_signature_checksum (content, mime_type);
	sum2 = e_gw_signature_checksum (again, again_type);
	g_assert_cmpstr (sum1, ==, sum2);
	g_free (sum1);
	g_free (sum2);
	g_free (again);
	g_free (again_type);
	g_bytes_unref (built);
	g_free (content);
	g_free (mime_type);

	/* A text signature becomes text and HTML */
	built = e_gw_signature_mime_from_evolution ("Rainer Backes\n<bond>", "text/plain");
	content = e_gw_signature_mime_to_evolution (built, &mime_type);
	g_assert_cmpstr (mime_type, ==, "text/html");
	g_assert_nonnull (strstr (content, "Rainer Backes<br>\n&lt;bond&gt;"));
	g_free (content);
	g_free (mime_type);
	g_bytes_unref (built);

	g_bytes_unref (mime);
}

int
main (int argc,
      char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/utils/internal-unread", test_internal_unread);
	g_test_add_func ("/utils/internet-sender", test_internet_sender);
	g_test_add_func ("/utils/draft", test_draft);
	g_test_add_func ("/utils/no-id", test_no_id);
	g_test_add_func ("/utils/invitation-date", test_invitation_date);
	g_test_add_func ("/utils/trash-date", test_trash_date);
	g_test_add_func ("/mime/forward-item", test_mime_forward_item);
	g_test_add_func ("/utils/own-subcalendars", test_own_subcalendars);
	g_test_add_func ("/utils/categories", test_categories);
	g_test_add_func ("/mime/plain", test_mime_plain);
	g_test_add_func ("/mime/html", test_mime_html);
	g_test_add_func ("/mime/html-only", test_mime_html_only);
	g_test_add_func ("/mime/envelope", test_mime_envelope);
	g_test_add_func ("/mime/draft", test_mime_draft);
	g_test_add_func ("/mime/signed", test_mime_signed);
	g_test_add_func ("/mime/signature", test_signature_mime);

	return g_test_run ();
}

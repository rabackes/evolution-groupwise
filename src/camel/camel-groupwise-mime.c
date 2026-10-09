/*
 * camel-groupwise-mime.c: MIME messages to GroupWise items for sending
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

#include <stdlib.h>
#include <string.h>

#include <glib/gi18n-lib.h>

#include "e-gw-xml.h"

#include "camel-groupwise-mime.h"

typedef struct {
	gchar *name;
	gchar *content_type;
	gchar *charset;		/* of a text, NULL: none known */
	gchar *content_id;	/* with <>, as the POA keeps it */
	GBytes *data;
	gboolean hidden;
	gchar *xml;		/* a reference to a GroupWise item instead */
} Attachment;

typedef struct {
	gchar *text;		/* UTF-8 */
	gchar *html;		/* UTF-8 */
	GPtrArray *images;	/* Attachment, pictures of the HTML body */
	GPtrArray *attachments;	/* Attachment */
	CamelGroupwiseEmbedFunc embed_func;
	gpointer embed_data;
	gchar *link;		/* marks a forwarded item as forwarded */
} Parts;

static void
attachment_free (Attachment *attachment)
{
	g_free (attachment->name);
	g_free (attachment->content_type);
	g_free (attachment->charset);
	g_free (attachment->content_id);
	g_free (attachment->xml);
	g_bytes_unref (attachment->data);
	g_free (attachment);
}

/* The content without transfer encoding */
static GBytes *
decoded_bytes (CamelDataWrapper *content,
	       GCancellable *cancellable,
	       GError **error)
{
	GOutputStream *out = g_memory_output_stream_new_resizable ();
	GBytes *bytes = NULL;

	if (camel_data_wrapper_decode_to_output_stream_sync (content, out, cancellable, error) >= 0 &&
	    g_output_stream_close (out, cancellable, error))
		bytes = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
	g_object_unref (out);

	return bytes;
}

/* A message attached as it is (forwarded as attachment) */
static GBytes *
written_bytes (CamelDataWrapper *content,
	       GCancellable *cancellable,
	       GError **error)
{
	GOutputStream *out = g_memory_output_stream_new_resizable ();
	GBytes *bytes = NULL;

	if (camel_data_wrapper_write_to_output_stream_sync (content, out, cancellable, error) >= 0 &&
	    g_output_stream_close (out, cancellable, error))
		bytes = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
	g_object_unref (out);

	return bytes;
}

static gchar *
to_utf8 (GBytes *bytes,
	 CamelContentType *ct)
{
	const gchar *charset = camel_content_type_param (ct, "charset");
	gsize size;
	const gchar *data = g_bytes_get_data (bytes, &size);
	gchar *text;

	if (!charset || !g_ascii_strcasecmp (charset, "utf-8") || !g_ascii_strcasecmp (charset, "us-ascii")) {
		if (g_utf8_validate (data, size, NULL))
			return g_strndup (data, size);
		charset = "ISO-8859-1";
	}

	text = g_convert_with_fallback (data, size, "UTF-8", camel_iconv_charset_name (charset), "?", NULL, NULL, NULL);

	return text ? text : g_utf8_make_valid (data, size);
}

/* The HTML body declares UTF-8 itself, as the GroupWise client writes it:
 * the client shows the body by what it declares. A declaration of another
 * charset would contradict the UTF-8 text and goes. */
static gchar *
html_with_charset (const gchar *html)
{
	static const gchar *meta = "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">";
	GRegex *other, *head, *root;
	gchar *cleaned, *result;

	other = g_regex_new ("<meta[^>]*charset[^>]*>", G_REGEX_CASELESS, 0, NULL);
	cleaned = g_regex_replace_literal (other, html, -1, 0, "", 0, NULL);
	g_regex_unref (other);
	if (!cleaned)
		cleaned = g_strdup (html);

	head = g_regex_new ("<head(\\s[^>]*)?>", G_REGEX_CASELESS, 0, NULL);
	root = g_regex_new ("<html(\\s[^>]*)?>", G_REGEX_CASELESS, 0, NULL);
	if (g_regex_match (head, cleaned, 0, NULL)) {
		gchar *replacement = g_strconcat ("\\0", meta, NULL);

		result = g_regex_replace (head, cleaned, -1, 0, replacement, G_REGEX_MATCH_DEFAULT, NULL);
		g_free (replacement);
	} else if (g_regex_match (root, cleaned, 0, NULL)) {
		gchar *replacement = g_strconcat ("\\0<head>", meta, "</head>", NULL);

		result = g_regex_replace (root, cleaned, -1, 0, replacement, G_REGEX_MATCH_DEFAULT, NULL);
		g_free (replacement);
	} else {
		result = g_strconcat (meta, cleaned, NULL);
	}
	g_regex_unref (head);
	g_regex_unref (root);

	if (!result)
		result = g_steal_pointer (&cleaned);
	g_free (cleaned);

	return result;
}

static gchar *
html_to_text (const gchar *html)
{
	CamelMimeFilter *filter = camel_mime_filter_html_new ();
	gchar *out = NULL, *text;
	gsize out_len = 0, prespace = 0;

	camel_mime_filter_complete (filter, (gchar *) html, strlen (html), 0, &out, &out_len, &prespace);
	text = g_strndup (out, out_len);
	g_object_unref (filter);

	return g_strstrip (text);
}

static gboolean
is_attachment (CamelMimePart *part)
{
	const gchar *disposition = camel_mime_part_get_disposition (part);

	return disposition && g_ascii_strcasecmp (disposition, "attachment") == 0;
}

static gboolean
collect_parts (CamelMimePart *part,
	       gboolean in_related,
	       Parts *parts,
	       GCancellable *cancellable,
	       GError **error)
{
	CamelDataWrapper *content = camel_medium_get_content (CAMEL_MEDIUM (part));
	CamelContentType *ct = camel_mime_part_get_content_type (part);
	Attachment *attachment;
	GBytes *bytes;
	gchar *type;

	if (!content)
		return TRUE;

	/* The POA builds the MIME for the recipients itself: a signature or an
	 * encryption would not survive. Better no mail than a silently unsigned
	 * or unreadable one. */
	if (camel_content_type_is (ct, "multipart", "signed") || camel_content_type_is (ct, "multipart", "encrypted") ||
	    camel_content_type_is (ct, "application", "pkcs7-mime") || camel_content_type_is (ct, "application", "x-pkcs7-mime")) {
		g_set_error_literal (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_INVALID,
			_("GroupWise cannot send signed or encrypted messages: the server builds the message for the "
			  "recipients itself. Send it without signature and encryption."));
		return FALSE;
	}

	if (CAMEL_IS_MULTIPART (content)) {
		CamelMultipart *multipart = CAMEL_MULTIPART (content);
		gboolean related = camel_content_type_is (ct, "multipart", "related");
		guint ii;

		for (ii = 0; ii < camel_multipart_get_number (multipart); ii++) {
			if (!collect_parts (camel_multipart_get_part (multipart, ii), related, parts, cancellable, error))
				return FALSE;
		}
		return TRUE;
	}

	if (!is_attachment (part) && !CAMEL_IS_MIME_MESSAGE (content)) {
		if (camel_content_type_is (ct, "text", "plain") && !parts->text) {
			bytes = decoded_bytes (content, cancellable, error);
			if (!bytes)
				return FALSE;
			parts->text = to_utf8 (bytes, ct);
			g_bytes_unref (bytes);
			return TRUE;
		}
		if (camel_content_type_is (ct, "text", "html") && !parts->html) {
			bytes = decoded_bytes (content, cancellable, error);
			if (!bytes)
				return FALSE;
			parts->html = to_utf8 (bytes, ct);
			g_bytes_unref (bytes);
			return TRUE;
		}
	}

	/* A GroupWise message forwarded as attachment: the item itself, as the
	 * GroupWise client forwards it */
	if (CAMEL_IS_MIME_MESSAGE (content) && parts->embed_func &&
	    camel_medium_get_header (CAMEL_MEDIUM (content), CAMEL_GROUPWISE_ITEM_ID_HEADER)) {
		gchar *link = NULL;
		gchar *reference = parts->embed_func (camel_medium_get_header (CAMEL_MEDIUM (content), CAMEL_GROUPWISE_ITEM_ID_HEADER),
			&link, parts->embed_data, cancellable);

		if (reference) {
			const gchar *subject = camel_mime_message_get_subject (CAMEL_MIME_MESSAGE (content));
			gchar *name_start = strstr (reference, "<name>"), *name_end = strstr (reference, "</name>");

			/* Named after the message, as the GroupWise client does ("FILE" otherwise) */
			if (subject && *subject && name_start && name_end && name_end > name_start) {
				GString *named = g_string_new_len (reference, name_start - reference);

				e_gw_xml_add_leaf (named, "name", subject);
				g_string_append (named, name_end + strlen ("</name>"));
				g_free (reference);
				reference = g_string_free (named, FALSE);
			}

			attachment = g_new0 (Attachment, 1);
			attachment->xml = reference;
			g_ptr_array_add (parts->attachments, attachment);
			if (!parts->link)
				parts->link = link, link = NULL;
			g_free (link);
			return TRUE;
		}
		g_free (link);
	}

	bytes = CAMEL_IS_MIME_MESSAGE (content) ? written_bytes (content, cancellable, error) :
		decoded_bytes (content, cancellable, error);
	if (!bytes)
		return FALSE;

	type = camel_content_type_simple (ct);
	attachment = g_new0 (Attachment, 1);
	attachment->data = bytes;
	attachment->content_type = type;
	if (camel_content_type_is (ct, "text", "*") && camel_content_type_param (ct, "charset"))
		attachment->charset = g_strdup (camel_iconv_charset_name (camel_content_type_param (ct, "charset")));
	attachment->name = g_strdup (camel_mime_part_get_filename (part));
	if (!attachment->name)
		attachment->name = g_strdup (CAMEL_IS_MIME_MESSAGE (content) ?
			(camel_mime_message_get_subject (CAMEL_MIME_MESSAGE (content)) ?
			 camel_mime_message_get_subject (CAMEL_MIME_MESSAGE (content)) : "message.eml") :
			"attachment");

	/* A picture the HTML body refers to */
	if (in_related && camel_mime_part_get_content_id (part) && !is_attachment (part)) {
		attachment->content_id = g_strdup_printf ("<%s>", camel_mime_part_get_content_id (part));
		attachment->hidden = TRUE;
		g_ptr_array_add (parts->images, attachment);
	} else {
		g_ptr_array_add (parts->attachments, attachment);
	}

	return TRUE;
}

static void
add_attachment_xml (GString *xml,
		    Attachment *attachment)
{
	gsize size;
	const guchar *data;
	gchar *encoded;

	if (attachment->xml) {
		g_string_append (xml, attachment->xml);
		return;
	}

	data = g_bytes_get_data (attachment->data, &size);
	encoded = g_base64_encode (data, size);

	g_string_append (xml, "<attachment>");
	e_gw_xml_add_leaf (xml, "name", attachment->name);
	/* Without it the POA takes a text for US-ASCII */
	if (attachment->charset)
		e_gw_xml_add_leaf (xml, "charset", attachment->charset);
	if (attachment->content_type)
		e_gw_xml_add_leaf (xml, "contentType", attachment->content_type);
	if (attachment->content_id)
		e_gw_xml_add_leaf (xml, "contentId", attachment->content_id);
	e_gw_xml_add_int (xml, "size", size);
	if (attachment->hidden)
		g_string_append (xml, "<hidden>1</hidden>");
	g_string_append_printf (xml, "<data>%s</data></attachment>", encoded);

	g_free (encoded);
}

static void
add_recipient (GString *xml,
	       const gchar *name,
	       const gchar *email,
	       const gchar *dist_type)
{
	g_string_append (xml, "<recipient>");
	if (name && *name)
		e_gw_xml_add_leaf (xml, "displayName", name);
	e_gw_xml_add_leaf (xml, "email", email);
	e_gw_xml_add_leaf (xml, "distType", dist_type);
	g_string_append (xml, "</recipient>");
}

static gboolean
address_in (CamelInternetAddress *list,
	    const gchar *email)
{
	return list && camel_internet_address_find_address (list, email, NULL) >= 0;
}

/* TO and CC as the headers say, everything else of the envelope is BC */
static guint
add_recipients (GString *xml,
		CamelMimeMessage *message,
		CamelAddress *envelope)
{
	CamelInternetAddress *to = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO);
	CamelInternetAddress *cc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC);
	CamelInternetAddress *bcc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_BCC);
	const gchar *name, *email;
	guint count = 0;
	gint ii;

	if (envelope && CAMEL_IS_INTERNET_ADDRESS (envelope)) {
		for (ii = 0; camel_internet_address_get (CAMEL_INTERNET_ADDRESS (envelope), ii, &name, &email); ii++, count++)
			add_recipient (xml, name, email, address_in (to, email) ? "TO" : address_in (cc, email) ? "CC" : "BC");
		return count;
	}

	for (ii = 0; to && camel_internet_address_get (to, ii, &name, &email); ii++, count++)
		add_recipient (xml, name, email, "TO");
	for (ii = 0; cc && camel_internet_address_get (cc, ii, &name, &email); ii++, count++)
		add_recipient (xml, name, email, "CC");
	for (ii = 0; bcc && camel_internet_address_get (bcc, ii, &name, &email); ii++, count++)
		add_recipient (xml, name, email, "BC");

	return count;
}

/* X-Priority 1-2 / Importance: high are high, 4-5 / low are low */
static const gchar *
priority_of (CamelMimeMessage *message)
{
	const gchar *x_priority = camel_medium_get_header (CAMEL_MEDIUM (message), "X-Priority");
	const gchar *importance = camel_medium_get_header (CAMEL_MEDIUM (message), "Importance");
	gint level = x_priority ? atoi (x_priority) : 3;

	while (importance && g_ascii_isspace (*importance))
		importance++;
	if (importance && g_ascii_strncasecmp (importance, "high", 4) == 0)
		level = 1;
	else if (importance && g_ascii_strncasecmp (importance, "low", 3) == 0)
		level = 5;

	return level == 1 || level == 2 ? "High" : level == 4 || level == 5 ? "Low" : NULL;
}

gchar *
camel_groupwise_item_from_message (CamelMimeMessage *message,
				   CamelAddress *recipients,
				   gboolean draft,
				   CamelGroupwiseEmbedFunc embed_func,
				   gpointer embed_data,
				   GCancellable *cancellable,
				   GError **error)
{
	Parts parts = { NULL, NULL, NULL, NULL, embed_func, embed_data, NULL };
	GString *xml;
	const gchar *priority;
	gchar *text;
	guint ii, count;

	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	parts.images = g_ptr_array_new_with_free_func ((GDestroyNotify) attachment_free);
	parts.attachments = g_ptr_array_new_with_free_func ((GDestroyNotify) attachment_free);

	if (!collect_parts (CAMEL_MIME_PART (message), FALSE, &parts, cancellable, error)) {
		g_ptr_array_unref (parts.images);
		g_ptr_array_unref (parts.attachments);
		g_free (parts.text);
		g_free (parts.html);
		g_free (parts.link);
		return NULL;
	}

	xml = g_string_new ("<item xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Mail\">");
	if (draft)
		e_gw_xml_add_leaf (xml, "source", "draft");
	e_gw_xml_add_leaf (xml, "subject", camel_mime_message_get_subject (message));

	g_string_append (xml, "<distribution><recipients>");
	count = add_recipients (xml, message, recipients);
	g_string_append (xml, "</recipients></distribution>");

	if (!count && !draft) {
		g_set_error_literal (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_INVALID,
			_("The message has no recipients"));
		g_string_free (xml, TRUE);
		g_ptr_array_unref (parts.images);
		g_ptr_array_unref (parts.attachments);
		g_free (parts.text);
		g_free (parts.html);
		g_free (parts.link);
		return NULL;
	}

	priority = priority_of (message);
	if (priority)
		g_string_append_printf (xml, "<options><priority>%s</priority></options>", priority);

	/* The text part; for an HTML-only message its text */
	text = parts.text ? g_strdup (parts.text) : parts.html ? html_to_text (parts.html) : NULL;
	if (text && *text) {
		gchar *encoded = g_base64_encode ((const guchar *) text, strlen (text));

		g_string_append_printf (xml, "<message><part length=\"%" G_GSIZE_FORMAT "\" contentType=\"text/plain\">%s</part></message>",
			strlen (text), encoded);
		g_free (encoded);
	}
	g_free (text);

	if (parts.html || parts.images->len || parts.attachments->len) {
		g_string_append (xml, "<attachments>");
		if (parts.html) {
			gchar *html = html_with_charset (parts.html);
			Attachment body = { (gchar *) "Text.htm", (gchar *) "TEXT/HTML", (gchar *) "UTF-8", NULL,
				g_bytes_new_static (html, strlen (html)), TRUE };

			add_attachment_xml (xml, &body);
			g_bytes_unref (body.data);
			g_free (html);
		}
		for (ii = 0; ii < parts.images->len; ii++)
			add_attachment_xml (xml, parts.images->pdata[ii]);
		for (ii = 0; ii < parts.attachments->len; ii++)
			add_attachment_xml (xml, parts.attachments->pdata[ii]);
		g_string_append (xml, "</attachments>");
	}
	if (parts.link)
		g_string_append (xml, parts.link);

	g_string_append (xml, "</item>");

	g_ptr_array_unref (parts.images);
	g_ptr_array_unref (parts.attachments);
	g_free (parts.text);
	g_free (parts.html);
	g_free (parts.link);

	return g_string_free (xml, FALSE);
}

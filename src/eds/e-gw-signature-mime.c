/*
 * e-gw-signature-mime.c: GroupWise signatures as Evolution signatures
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

#include <camel/camel.h>

#include "e-gw-signature-mime.h"

typedef struct {
	gchar *html;		/* UTF-8 */
	gchar *text;		/* UTF-8 */
	GPtrArray *images;	/* Image */
} Parts;

typedef struct {
	gchar *content_id;	/* without <> */
	gchar *content_type;
	GBytes *data;
} Image;

static void
image_free (Image *image)
{
	g_free (image->content_id);
	g_free (image->content_type);
	g_bytes_unref (image->data);
	g_free (image);
}

static GBytes *
decoded_bytes (CamelDataWrapper *content)
{
	GOutputStream *out = g_memory_output_stream_new_resizable ();
	GBytes *bytes = NULL;

	if (camel_data_wrapper_decode_to_output_stream_sync (content, out, NULL, NULL) >= 0 &&
	    g_output_stream_close (out, NULL, NULL))
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
		charset = "windows-1252";
	}

	text = g_convert_with_fallback (data, size, "UTF-8", camel_iconv_charset_name (charset), "?", NULL, NULL, NULL);

	return text ? text : g_utf8_make_valid (data, size);
}

static void
collect_parts (CamelMimePart *part,
	       Parts *parts)
{
	CamelDataWrapper *content = camel_medium_get_content (CAMEL_MEDIUM (part));
	CamelContentType *ct = camel_mime_part_get_content_type (part);
	GBytes *bytes;

	if (!content)
		return;

	if (CAMEL_IS_MULTIPART (content)) {
		CamelMultipart *multipart = CAMEL_MULTIPART (content);
		guint ii;

		for (ii = 0; ii < camel_multipart_get_number (multipart); ii++)
			collect_parts (camel_multipart_get_part (multipart, ii), parts);
		return;
	}

	if (camel_content_type_is (ct, "text", "html") && !parts->html) {
		bytes = decoded_bytes (content);
		if (bytes) {
			parts->html = to_utf8 (bytes, ct);
			g_bytes_unref (bytes);
		}
	} else if (camel_content_type_is (ct, "text", "plain") && !parts->text) {
		bytes = decoded_bytes (content);
		if (bytes) {
			parts->text = to_utf8 (bytes, ct);
			g_bytes_unref (bytes);
		}
	} else if (camel_content_type_is (ct, "image", "*") && camel_mime_part_get_content_id (part)) {
		bytes = decoded_bytes (content);
		if (bytes) {
			Image *image = g_new0 (Image, 1);

			image->content_id = g_strdup (camel_mime_part_get_content_id (part));
			image->content_type = camel_content_type_simple (ct);
			image->data = bytes;
			g_ptr_array_add (parts->images, image);
		}
	}
}

/* Replaces every occurrence of @find in @text */
static gchar *
replace_all (const gchar *text,
	     const gchar *find,
	     const gchar *replacement)
{
	GString *result = g_string_new (NULL);
	const gchar *pos = text, *hit;
	gsize find_len = strlen (find);

	while ((hit = strstr (pos, find))) {
		g_string_append_len (result, pos, hit - pos);
		g_string_append (result, replacement);
		pos = hit + find_len;
	}
	g_string_append (result, pos);

	return g_string_free (result, FALSE);
}

gchar *
e_gw_signature_mime_to_evolution (GBytes *mime,
				  gchar **out_mime_type)
{
	CamelMimeMessage *message;
	CamelStream *stream;
	Parts parts = { NULL, NULL, NULL };
	gchar *result = NULL;
	gsize len;
	gconstpointer data;
	guint ii;

	g_return_val_if_fail (mime != NULL, NULL);
	g_return_val_if_fail (out_mime_type != NULL, NULL);

	*out_mime_type = NULL;
	data = g_bytes_get_data (mime, &len);
	message = camel_mime_message_new ();
	stream = camel_stream_mem_new_with_buffer (data, len);
	if (!camel_data_wrapper_construct_from_stream_sync (CAMEL_DATA_WRAPPER (message), stream, NULL, NULL)) {
		g_object_unref (stream);
		g_object_unref (message);
		return NULL;
	}
	g_object_unref (stream);

	parts.images = g_ptr_array_new_with_free_func ((GDestroyNotify) image_free);
	collect_parts (CAMEL_MIME_PART (message), &parts);
	g_object_unref (message);

	if (parts.html) {
		/* The pictures go into the HTML: Evolution keeps a signature as one file */
		result = g_steal_pointer (&parts.html);
		for (ii = 0; ii < parts.images->len; ii++) {
			Image *image = parts.images->pdata[ii];
			const guchar *image_data;
			gsize image_len;
			gchar *encoded, *uri, *cid, *replaced;

			image_data = g_bytes_get_data (image->data, &image_len);
			encoded = g_base64_encode (image_data, image_len);
			uri = g_strdup_printf ("data:%s;base64,%s", image->content_type, encoded);
			cid = g_strconcat ("cid:", image->content_id, NULL);
			replaced = replace_all (result, cid, uri);
			g_free (result);
			result = replaced;
			g_free (cid);
			g_free (uri);
			g_free (encoded);
		}
		*out_mime_type = g_strdup ("text/html");
	} else if (parts.text) {
		result = g_steal_pointer (&parts.text);
		*out_mime_type = g_strdup ("text/plain");
	}

	g_free (parts.html);
	g_free (parts.text);
	g_ptr_array_unref (parts.images);

	return result;
}

/* ------------------------------------------------------------------ */

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

static gchar *
text_to_html (const gchar *text)
{
	gchar *escaped = g_markup_escape_text (text, -1);
	gchar *lines = replace_all (escaped, "\n", "<br>\n");
	gchar *html = g_strdup_printf ("<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">"
		"</head><body>%s</body></html>", lines);

	g_free (lines);
	g_free (escaped);

	return html;
}

static void
append_base64 (GString *mime,
	       gconstpointer data,
	       gsize len)
{
	gchar *encoded = g_base64_encode (data, len);
	gsize encoded_len = strlen (encoded), ii;

	for (ii = 0; ii < encoded_len; ii += 76) {
		g_string_append_len (mime, encoded + ii, MIN (76, encoded_len - ii));
		g_string_append (mime, "\r\n");
	}
	g_free (encoded);
}

/* The pictures of the HTML (src="data:..."): out of it, into @images */
static gchar *
take_images (const gchar *html,
	     GPtrArray *images)
{
	GRegex *regex = g_regex_new ("(src\\s*=\\s*[\"'])data:([a-zA-Z0-9.+/-]+);base64,([A-Za-z0-9+/=\\s]+)([\"'])",
		G_REGEX_CASELESS, 0, NULL);
	GMatchInfo *match = NULL;
	GString *result = g_string_new (NULL);
	gint last_end = 0;

	g_regex_match (regex, html, 0, &match);
	while (g_match_info_matches (match)) {
		gint start, end;
		gchar *prefix = g_match_info_fetch (match, 1);
		gchar *content_type = g_match_info_fetch (match, 2);
		gchar *encoded = g_match_info_fetch (match, 3);
		gchar *quote = g_match_info_fetch (match, 4);
		const gchar *subtype = strchr (content_type, '/');
		Image *image = g_new0 (Image, 1);
		guchar *decoded;
		gsize len = 0;

		g_match_info_fetch_pos (match, 0, &start, &end);
		decoded = g_base64_decode (encoded, &len);
		image->data = g_bytes_new_take (decoded, len);
		image->content_type = g_ascii_strdown (content_type, -1);
		image->content_id = g_strdup_printf ("IMAGE%u.%s@evolution", images->len + 1, subtype ? subtype + 1 : "bin");
		g_ptr_array_add (images, image);

		g_string_append_len (result, html + last_end, start - last_end);
		g_string_append_printf (result, "%scid:%s%s", prefix, image->content_id, quote);
		last_end = end;

		g_free (prefix);
		g_free (content_type);
		g_free (encoded);
		g_free (quote);
		g_match_info_next (match, NULL);
	}
	g_string_append (result, html + last_end);
	g_match_info_free (match);
	g_regex_unref (regex);

	return g_string_free (result, FALSE);
}

GBytes *
e_gw_signature_mime_from_evolution (const gchar *content,
				    const gchar *mime_type)
{
	GPtrArray *images = g_ptr_array_new_with_free_func ((GDestroyNotify) image_free);
	GString *mime = g_string_new (NULL);
	gchar *html, *text, *boundaries[3];
	guint ii;

	g_return_val_if_fail (content != NULL, NULL);

	if (g_strcmp0 (mime_type, "text/html") == 0) {
		html = take_images (content, images);
		text = html_to_text (content);
	} else {
		html = text_to_html (content);
		text = g_strdup (content);
	}

	for (ii = 0; ii < G_N_ELEMENTS (boundaries); ii++)
		boundaries[ii] = g_strdup_printf ("____EVOLUTION%u%08X____", ii, g_random_int ());

	/* As the GroupWise client writes it */
	g_string_append_printf (mime, "Mime-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n",
		boundaries[0]);
	g_string_append_printf (mime, "--%s\r\nContent-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n",
		boundaries[0], boundaries[1]);
	g_string_append_printf (mime, "--%s\r\nContent-Type: text/plain; charset=utf-8\r\n"
		"Content-Transfer-Encoding: base64\r\n\r\n", boundaries[1]);
	append_base64 (mime, text, strlen (text));
	g_string_append_printf (mime, "\r\n--%s\r\nContent-Type: multipart/related; boundary=\"%s\"\r\n\r\n",
		boundaries[1], boundaries[2]);
	g_string_append_printf (mime, "--%s\r\nContent-Type: text/html; charset=utf-8\r\n"
		"Content-Transfer-Encoding: base64\r\n\r\n", boundaries[2]);
	append_base64 (mime, html, strlen (html));
	for (ii = 0; ii < images->len; ii++) {
		Image *image = images->pdata[ii];
		gsize len;
		gconstpointer data = g_bytes_get_data (image->data, &len);

		g_string_append_printf (mime, "\r\n--%s\r\nContent-ID: <%s>\r\nContent-Type: %s\r\n"
			"Content-Transfer-Encoding: base64\r\nContent-Disposition: inline\r\n\r\n",
			boundaries[2], image->content_id, image->content_type);
		append_base64 (mime, data, len);
	}
	g_string_append_printf (mime, "\r\n--%s--\r\n\r\n--%s--\r\n\r\n--%s--\r\n", boundaries[2], boundaries[1], boundaries[0]);

	for (ii = 0; ii < G_N_ELEMENTS (boundaries); ii++)
		g_free (boundaries[ii]);
	g_ptr_array_unref (images);
	g_free (html);
	g_free (text);

	return g_string_free_to_bytes (mime);
}

gchar *
e_gw_signature_checksum (const gchar *content,
			 const gchar *mime_type)
{
	GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA1);
	gchar *result;

	g_checksum_update (checksum, (const guchar *) (mime_type ? mime_type : ""), -1);
	g_checksum_update (checksum, (const guchar *) "\n", 1);
	g_checksum_update (checksum, (const guchar *) (content ? content : ""), -1);
	result = g_strdup (g_checksum_get_string (checksum));
	g_checksum_free (checksum);

	return result;
}

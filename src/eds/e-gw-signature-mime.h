/*
 * e-gw-signature-mime.h: GroupWise signatures as Evolution signatures
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

#ifndef E_GW_SIGNATURE_MIME_H
#define E_GW_SIGNATURE_MIME_H

#include <glib.h>

G_BEGIN_DECLS

/* The content of an Evolution signature from the MIME document of a
 * GroupWise signature: its HTML with the pictures as data: URIs (UTF-8,
 * @out_mime_type "text/html"), or its text ("text/plain"). NULL when it has
 * neither. */
gchar *		e_gw_signature_mime_to_evolution
						(GBytes *mime,
						 gchar **out_mime_type);

/* The MIME document GroupWise keeps for an Evolution signature, built as
 * the GroupWise client builds it: the text and the HTML with its pictures
 * (data: URIs become parts of their own) */
GBytes *	e_gw_signature_mime_from_evolution
						(const gchar *content,
						 const gchar *mime_type);

/* A checksum of signature content, to see what changed since a sync */
gchar *		e_gw_signature_checksum		(const gchar *content,
						 const gchar *mime_type);

G_END_DECLS

#endif /* E_GW_SIGNATURE_MIME_H */

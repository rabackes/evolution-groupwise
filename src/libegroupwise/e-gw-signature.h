/*
 * e-gw-signature.h: the signatures of the user
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

#ifndef E_GW_SIGNATURE_H
#define E_GW_SIGNATURE_H

#include "e-gw-connection.h"

G_BEGIN_DECLS

/*
 * A signature is a MIME document in the mailbox (as the GroupWise client
 * writes it: multipart/mixed, in it multipart/alternative of text/plain and
 * multipart/related of text/html and its pictures). One of them can be the
 * default; setting one takes it from the other.
 */
typedef struct {
	gchar *id;
	gchar *name;
	gboolean is_default;
	GBytes *mime;
} EGwSignature;

void		e_gw_signature_free		(EGwSignature *signature);

typedef struct {
	GPtrArray *signatures;	/* EGwSignature */
	gboolean enabled;	/* signatures are switched on */
	gboolean automatic;	/* added automatically, else asked for before sending */
} EGwSignatures;

void		e_gw_signatures_free		(EGwSignatures *signatures);

EGwSignatures *	e_gw_connection_get_signatures_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);
/* Returns the ID of the new signature */
gchar *		e_gw_connection_create_signature_sync
						(EGwConnection *cnc,
						 const gchar *name,
						 GBytes *mime,
						 gboolean is_default,
						 GCancellable *cancellable,
						 GError **error);
/* Changes what is given: @name and @mime may be NULL, @is_default < 0
 * leaves the default as it is */
gboolean	e_gw_connection_modify_signature_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 const gchar *name,
						 GBytes *mime,
						 gint is_default,
						 GCancellable *cancellable,
						 GError **error);
gboolean	e_gw_connection_remove_signature_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_SIGNATURE_H */

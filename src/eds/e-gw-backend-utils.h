/*
 * e-gw-backend-utils.h: GroupWise connections for EDS backends
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

#ifndef E_GW_BACKEND_UTILS_H
#define E_GW_BACKEND_UTILS_H

#include <libedataserver/libedataserver.h>

#include "camel-groupwise-settings.h"
#include "e-gw-connection.h"

G_BEGIN_DECLS

/* Makes the "Groupwise Backend" extension and the "GroupWise Folder"
 * extension known in the process; call before reading sources. */
void		e_gw_backend_ensure_types	(void);

/* The settings of the account: in the collection source above @source
 * (or @source itself). Returns: (transfer full) (nullable) */
CamelGroupwiseSettings *
		e_gw_backend_ref_settings	(ESourceRegistry *registry,
						 ESource *source);

/* Logs in with the password of @credentials. Certificates the system does
 * not trust count when the user trusted them for @source or its collection
 * (ESourceWebdav "ssl-trust"). Maps the result for EDS: a wrong password
 * is REJECTED, a missing one REQUIRED, an untrusted certificate
 * ERROR_SSL_FAILED with the certificate for the trust prompt. Evolution
 * propagates an answer to the collection and all its children (those with
 * the WebDAV extension, where EDS keeps the trust). With @proxy (an e-mail
 * address) the session is a proxy login into that user's mailbox.
 * Returns: (transfer full) (nullable): the connection when accepted */
EGwConnection *	e_gw_backend_connect_sync	(ESourceRegistry *registry,
						 ESource *source,
						 CamelGroupwiseSettings *settings,
						 const gchar *proxy,
						 const ENamedParameters *credentials,
						 ESourceAuthenticationResult *out_result,
						 gchar **out_certificate_pem,
						 GTlsCertificateFlags *out_certificate_errors,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_BACKEND_UTILS_H */

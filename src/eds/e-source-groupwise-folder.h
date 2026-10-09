/*
 * e-source-groupwise-folder.h: the GroupWise folder or address book behind an ESource
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

#ifndef E_SOURCE_GROUPWISE_FOLDER_H
#define E_SOURCE_GROUPWISE_FOLDER_H

#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

#define E_SOURCE_EXTENSION_GROUPWISE_FOLDER "GroupWise Folder"

#define E_TYPE_SOURCE_GROUPWISE_FOLDER (e_source_groupwise_folder_get_type ())
G_DECLARE_FINAL_TYPE (ESourceGroupwiseFolder, e_source_groupwise_folder, E, SOURCE_GROUPWISE_FOLDER, ESourceExtension)

gchar *		e_source_groupwise_folder_dup_id
						(ESourceGroupwiseFolder *extension);
void		e_source_groupwise_folder_set_id
						(ESourceGroupwiseFolder *extension,
						 const gchar *id);

/* The role of a calendar folder: E_GW_SOURCE_ROLE_* or NULL (the Calendar) */
#define E_GW_SOURCE_ROLE_OWN	"own"
#define E_GW_SOURCE_ROLE_PROXY	"proxy"
#define E_GW_SOURCE_ROLE_SHARED	"shared"

gchar *		e_source_groupwise_folder_dup_role
						(ESourceGroupwiseFolder *extension);
void		e_source_groupwise_folder_set_role
						(ESourceGroupwiseFolder *extension,
						 const gchar *role);
/* The user of a proxy calendar (e-mail address) */
gchar *		e_source_groupwise_folder_dup_proxy
						(ESourceGroupwiseFolder *extension);
void		e_source_groupwise_folder_set_proxy
						(ESourceGroupwiseFolder *extension,
						 const gchar *email);

/* A signature: the checksum of its content as last synchronized with
 * GroupWise (e_gw_signature_checksum) */
gchar *		e_source_groupwise_folder_dup_checksum
						(ESourceGroupwiseFolder *extension);
void		e_source_groupwise_folder_set_checksum
						(ESourceGroupwiseFolder *extension,
						 const gchar *checksum);

G_END_DECLS

#endif /* E_SOURCE_GROUPWISE_FOLDER_H */

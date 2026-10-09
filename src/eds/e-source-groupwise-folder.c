/*
 * e-source-groupwise-folder.c: the GroupWise folder or address book behind an ESource
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

#include "e-source-groupwise-folder.h"

struct _ESourceGroupwiseFolder {
	ESourceExtension parent;

	gchar *id;
	gchar *role;
	gchar *proxy;
	gchar *checksum;
};

enum {
	PROP_0,
	PROP_ID,
	PROP_ROLE,
	PROP_PROXY,
	PROP_CHECKSUM
};

static gchar *
dup_locked (ESourceGroupwiseFolder *extension,
	    gchar **field)
{
	gchar *value;

	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));
	value = g_strdup (*field);
	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	return value;
}

static void
set_locked (ESourceGroupwiseFolder *extension,
	    gchar **field,
	    const gchar *value,
	    const gchar *property)
{
	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));
	if (g_strcmp0 (*field, value) == 0) {
		e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));
		return;
	}
	g_free (*field);
	*field = e_util_strdup_strip (value);
	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	g_object_notify (G_OBJECT (extension), property);
}

G_DEFINE_TYPE (ESourceGroupwiseFolder, e_source_groupwise_folder, E_TYPE_SOURCE_EXTENSION)

static void
source_groupwise_folder_set_property (GObject *object,
				      guint property_id,
				      const GValue *value,
				      GParamSpec *pspec)
{
	switch (property_id) {
	case PROP_ID:
		e_source_groupwise_folder_set_id (E_SOURCE_GROUPWISE_FOLDER (object), g_value_get_string (value));
		return;
	case PROP_ROLE:
		e_source_groupwise_folder_set_role (E_SOURCE_GROUPWISE_FOLDER (object), g_value_get_string (value));
		return;
	case PROP_PROXY:
		e_source_groupwise_folder_set_proxy (E_SOURCE_GROUPWISE_FOLDER (object), g_value_get_string (value));
		return;
	case PROP_CHECKSUM:
		e_source_groupwise_folder_set_checksum (E_SOURCE_GROUPWISE_FOLDER (object), g_value_get_string (value));
		return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
source_groupwise_folder_get_property (GObject *object,
				      guint property_id,
				      GValue *value,
				      GParamSpec *pspec)
{
	switch (property_id) {
	case PROP_ID:
		g_value_take_string (value, e_source_groupwise_folder_dup_id (E_SOURCE_GROUPWISE_FOLDER (object)));
		return;
	case PROP_ROLE:
		g_value_take_string (value, e_source_groupwise_folder_dup_role (E_SOURCE_GROUPWISE_FOLDER (object)));
		return;
	case PROP_PROXY:
		g_value_take_string (value, e_source_groupwise_folder_dup_proxy (E_SOURCE_GROUPWISE_FOLDER (object)));
		return;
	case PROP_CHECKSUM:
		g_value_take_string (value, e_source_groupwise_folder_dup_checksum (E_SOURCE_GROUPWISE_FOLDER (object)));
		return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
source_groupwise_folder_finalize (GObject *object)
{
	g_free (E_SOURCE_GROUPWISE_FOLDER (object)->id);
	g_free (E_SOURCE_GROUPWISE_FOLDER (object)->role);
	g_free (E_SOURCE_GROUPWISE_FOLDER (object)->proxy);
	g_free (E_SOURCE_GROUPWISE_FOLDER (object)->checksum);

	G_OBJECT_CLASS (e_source_groupwise_folder_parent_class)->finalize (object);
}

static void
e_source_groupwise_folder_class_init (ESourceGroupwiseFolderClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);

	object_class->set_property = source_groupwise_folder_set_property;
	object_class->get_property = source_groupwise_folder_get_property;
	object_class->finalize = source_groupwise_folder_finalize;

	E_SOURCE_EXTENSION_CLASS (class)->name = E_SOURCE_EXTENSION_GROUPWISE_FOLDER;

	/* Stored in the key file as "Id" */
	g_object_class_install_property (object_class, PROP_ID,
		g_param_spec_string ("id", "ID", "GroupWise ID of the folder or address book", NULL,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_EXPLICIT_NOTIFY |
			G_PARAM_STATIC_STRINGS | E_SOURCE_PARAM_SETTING));

	/* "Role": what a calendar folder is to the user (see the header) */
	g_object_class_install_property (object_class, PROP_ROLE,
		g_param_spec_string ("role", "Role", "Role of a calendar folder: own, proxy or shared (empty: the Calendar)", NULL,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_EXPLICIT_NOTIFY |
			G_PARAM_STATIC_STRINGS | E_SOURCE_PARAM_SETTING));

	/* "Proxy": e-mail address of the user of a proxy calendar */
	g_object_class_install_property (object_class, PROP_PROXY,
		g_param_spec_string ("proxy", "Proxy", "E-mail address of the user whose calendar a proxy calendar is", NULL,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_EXPLICIT_NOTIFY |
			G_PARAM_STATIC_STRINGS | E_SOURCE_PARAM_SETTING));

	/* "Checksum": of a signature as last synchronized with GroupWise */
	g_object_class_install_property (object_class, PROP_CHECKSUM,
		g_param_spec_string ("checksum", "Checksum", "Checksum of the content as last synchronized with GroupWise", NULL,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_EXPLICIT_NOTIFY |
			G_PARAM_STATIC_STRINGS | E_SOURCE_PARAM_SETTING));
}

static void
e_source_groupwise_folder_init (ESourceGroupwiseFolder *extension)
{
}

gchar *
e_source_groupwise_folder_dup_id (ESourceGroupwiseFolder *extension)
{
	gchar *id;

	g_return_val_if_fail (E_IS_SOURCE_GROUPWISE_FOLDER (extension), NULL);

	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));
	id = g_strdup (extension->id);
	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	return id;
}

void
e_source_groupwise_folder_set_id (ESourceGroupwiseFolder *extension,
				  const gchar *id)
{
	g_return_if_fail (E_IS_SOURCE_GROUPWISE_FOLDER (extension));

	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));
	if (g_strcmp0 (extension->id, id) == 0) {
		e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));
		return;
	}
	g_free (extension->id);
	extension->id = e_util_strdup_strip (id);
	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	g_object_notify (G_OBJECT (extension), "id");
}

gchar *
e_source_groupwise_folder_dup_role (ESourceGroupwiseFolder *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_GROUPWISE_FOLDER (extension), NULL);

	return dup_locked (extension, &extension->role);
}

void
e_source_groupwise_folder_set_role (ESourceGroupwiseFolder *extension,
				    const gchar *role)
{
	g_return_if_fail (E_IS_SOURCE_GROUPWISE_FOLDER (extension));

	set_locked (extension, &extension->role, role, "role");
}

gchar *
e_source_groupwise_folder_dup_proxy (ESourceGroupwiseFolder *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_GROUPWISE_FOLDER (extension), NULL);

	return dup_locked (extension, &extension->proxy);
}

void
e_source_groupwise_folder_set_proxy (ESourceGroupwiseFolder *extension,
				     const gchar *email)
{
	g_return_if_fail (E_IS_SOURCE_GROUPWISE_FOLDER (extension));

	set_locked (extension, &extension->proxy, email, "proxy");
}

gchar *
e_source_groupwise_folder_dup_checksum (ESourceGroupwiseFolder *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_GROUPWISE_FOLDER (extension), NULL);

	return dup_locked (extension, &extension->checksum);
}

void
e_source_groupwise_folder_set_checksum (ESourceGroupwiseFolder *extension,
					const gchar *checksum)
{
	g_return_if_fail (E_IS_SOURCE_GROUPWISE_FOLDER (extension));

	set_locked (extension, &extension->checksum, checksum, "checksum");
}

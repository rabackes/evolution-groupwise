/*
 * e-groupwise-source-config.c: the properties of GroupWise calendars and
 * address books in Evolution
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
 * Evolution's properties dialog of a calendar, task list, memo list or
 * address book (name, color, offline, ...) needs a config backend for the
 * backend name of the source; without one it cannot be saved. GroupWise
 * sources come from the account: none is created in the dialog.
 */

#include <e-util/e-util.h>

#include "e-groupwise-source-config.h"

typedef ESourceConfigBackend ECalConfigGroupwise;
typedef ESourceConfigBackendClass ECalConfigGroupwiseClass;
typedef ESourceConfigBackend EBookConfigGroupwise;
typedef ESourceConfigBackendClass EBookConfigGroupwiseClass;

GType e_cal_config_groupwise_get_type (void);
GType e_book_config_groupwise_get_type (void);

G_DEFINE_DYNAMIC_TYPE (ECalConfigGroupwise, e_cal_config_groupwise, E_TYPE_SOURCE_CONFIG_BACKEND)
G_DEFINE_DYNAMIC_TYPE (EBookConfigGroupwise, e_book_config_groupwise, E_TYPE_SOURCE_CONFIG_BACKEND)

static gboolean
groupwise_config_allow_creation (ESourceConfigBackend *backend)
{
	return FALSE;
}

static void
groupwise_config_insert_widgets (ESourceConfigBackend *backend,
				 ESource *scratch_source)
{
	if (scratch_source)
		e_source_config_add_refresh_interval (e_source_config_backend_get_config (backend), scratch_source);
}

static void
e_cal_config_groupwise_class_init (ECalConfigGroupwiseClass *class)
{
	ESourceConfigBackendClass *backend_class = E_SOURCE_CONFIG_BACKEND_CLASS (class);

	E_EXTENSION_CLASS (class)->extensible_type = E_TYPE_CAL_SOURCE_CONFIG;
	backend_class->backend_name = "groupwise";
	backend_class->allow_creation = groupwise_config_allow_creation;
	backend_class->insert_widgets = groupwise_config_insert_widgets;
}

static void
e_cal_config_groupwise_class_finalize (ECalConfigGroupwiseClass *class)
{
}

static void
e_cal_config_groupwise_init (ECalConfigGroupwise *backend)
{
}

static void
e_book_config_groupwise_class_init (EBookConfigGroupwiseClass *class)
{
	ESourceConfigBackendClass *backend_class = E_SOURCE_CONFIG_BACKEND_CLASS (class);

	E_EXTENSION_CLASS (class)->extensible_type = E_TYPE_BOOK_SOURCE_CONFIG;
	backend_class->backend_name = "groupwise";
	backend_class->allow_creation = groupwise_config_allow_creation;
}

static void
e_book_config_groupwise_class_finalize (EBookConfigGroupwiseClass *class)
{
}

static void
e_book_config_groupwise_init (EBookConfigGroupwise *backend)
{
}

void
e_groupwise_source_config_type_register (GTypeModule *type_module)
{
	e_cal_config_groupwise_register_type (type_module);
	e_book_config_groupwise_register_type (type_module);
}

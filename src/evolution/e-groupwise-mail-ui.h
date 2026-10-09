/*
 * e-groupwise-mail-ui.h: GroupWise actions in Evolution's mail view
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

#ifndef E_GROUPWISE_MAIL_UI_H
#define E_GROUPWISE_MAIL_UI_H

#include <glib-object.h>

G_BEGIN_DECLS

void		e_groupwise_mail_ui_type_register
						(GTypeModule *type_module);

G_END_DECLS

#endif /* E_GROUPWISE_MAIL_UI_H */

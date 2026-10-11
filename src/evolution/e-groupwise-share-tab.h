/*
 * e-groupwise-share-tab.h: sharing a folder, a page of the folder properties
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

#ifndef E_GROUPWISE_SHARE_TAB_H
#define E_GROUPWISE_SHARE_TAB_H

#include <glib.h>

G_BEGIN_DECLS

/* From now on the properties of a GroupWise mail folder have the page */
void		e_groupwise_share_tab_start	(void);

/* The properties opened next show that page at once */
void		e_groupwise_share_tab_select_next
						(void);

G_END_DECLS

#endif /* E_GROUPWISE_SHARE_TAB_H */

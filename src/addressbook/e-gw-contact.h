/*
 * e-gw-contact.h: GroupWise address book items and vCards
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

#ifndef E_GW_CONTACT_H
#define E_GW_CONTACT_H

#include <libebook-contacts/libebook-contacts.h>
#include <libxml/tree.h>

G_BEGIN_DECLS

/* The item types of GroupWise address books other than Contact */
#define E_GW_CONTACT_X_TYPE "X-GROUPWISE-TYPE"
/* A resource of the kind place (a room): "1". The appointment editor takes
 * its name as location, as the GroupWise client does. */
#define E_GW_CONTACT_X_PLACE "X-GROUPWISE-PLACE"

/* A Contact, Organization, Resource or Group item as an EContact (NULL for
 * other items). The UID is the item ID, the revision its modification
 * time. Groups become contact lists; @members is the item read with the
 * view "members" (NULL: the group without members). */
EContact *	e_gw_contact_from_item		(xmlNode *item,
						 xmlNode *members);

/* What changes when the item changes: its modification time, else its
 * version, else a digest of its XML (the system address book has neither). */
gchar *		e_gw_contact_revision		(xmlNode *item);

/* The <item> for createItemRequest in @container. Contact lists cannot be
 * written (GroupWise groups need member IDs). */
gchar *		e_gw_contact_to_item_xml	(EContact *contact,
						 const gchar *container,
						 GError **error);

/* The <add>/<update>/<delete> parts for modifyItemRequest that make the
 * item @current into @contact. Empty when nothing changed. */
gchar *		e_gw_contact_updates_xml	(xmlNode *current,
						 EContact *contact,
						 GError **error);

G_END_DECLS

#endif /* E_GW_CONTACT_H */

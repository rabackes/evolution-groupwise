/*
 * e-gw-share.h: sharing folders with other users
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

#ifndef E_GW_SHARE_H
#define E_GW_SHARE_H

#include <libxml/tree.h>

#include "e-gw-connection.h"

G_BEGIN_DECLS

/* What a user may do in a shared folder; reading is always part of it */
typedef enum {
	E_GW_SHARE_READ		= 1 << 0,
	E_GW_SHARE_ADD		= 1 << 1,
	E_GW_SHARE_EDIT		= 1 << 2,
	E_GW_SHARE_DELETE	= 1 << 3
} EGwShareRights;

#define E_GW_SHARE_ALL (E_GW_SHARE_READ | E_GW_SHARE_ADD | E_GW_SHARE_EDIT | E_GW_SHARE_DELETE)

/* How far the user of an entry got with the share */
typedef enum {
	E_GW_SHARE_STATUS_PENDING,	/* notified, nothing happened yet (no status) */
	E_GW_SHARE_STATUS_OPENED,	/* read the notification */
	E_GW_SHARE_STATUS_ACCEPTED,
	E_GW_SHARE_STATUS_DELETED,	/* gave the folder up */
	E_GW_SHARE_STATUS_OWNER
} EGwShareStatus;

/* A user of a shared folder */
typedef struct {
	gchar *display_name;
	gchar *email;
	gchar *uuid;
	EGwShareRights rights;
	EGwShareStatus status;
} EGwShareEntry;

EGwShareEntry *	e_gw_share_entry_new		(const gchar *display_name,
						 const gchar *email,
						 EGwShareRights rights);
EGwShareEntry *	e_gw_share_entry_copy		(const EGwShareEntry *entry);
void		e_gw_share_entry_free		(EGwShareEntry *entry);

/* How a folder is shared (getFolderRequest) */
typedef struct {
	gchar *id;			/* the ID of the folder as it is now: sharing
					 * a folder changes its ID */
	gchar *name;
	gchar *description;
	gboolean shared_by_me;
	gboolean shared_to_me;
	gboolean share_tree;		/* with all its subfolders */
	gchar *owner_name;		/* shared to me: who shares it */
	gchar *owner_email;
	GPtrArray *entries;		/* EGwShareEntry: shared by me, all users;
					 * shared to me, the user alone */
} EGwShare;

void		e_gw_share_free			(EGwShare *share);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (EGwShare, e_gw_share_free)

/* The rights the user has in the folder: all of them in an own one */
EGwShareRights	e_gw_share_get_my_rights	(const EGwShare *share);

/* <rights><read>1</read>...</rights> below @parent */
EGwShareRights	e_gw_share_rights_from_node	(xmlNode *parent);

EGwShare *	e_gw_connection_get_share_sync	(EGwConnection *cnc,
						 const gchar *folder_id,
						 GCancellable *cancellable,
						 GError **error);

/*
 * Changes who a folder of the user is shared with, in one request: the users
 * in @added get it (they are sent a notification with @subject and @message;
 * @description is what they are offered as description of the folder), those
 * in @changed (EGwShareEntry with the new rights) other rights, those in
 * @removed lose it. Any of the lists may be NULL. @share_tree shares the
 * subfolders as well; the POA takes it only along with new users.
 */
gboolean	e_gw_connection_modify_share_sync
						(EGwConnection *cnc,
						 const gchar *folder_id,
						 GPtrArray *added,
						 GPtrArray *changed,
						 GPtrArray *removed,
						 gboolean share_tree,
						 const gchar *subject,
						 const gchar *message,
						 const gchar *description,
						 GCancellable *cancellable,
						 GError **error);

/* The message telling the user that someone shared a folder */
typedef struct {
	gchar *id;
	gchar *folder_name;
	gchar *subject;
	gchar *sender_name;
	gchar *sender_email;
	gchar *message;
	gchar *description;
	EGwShareRights rights;
} EGwShareNotification;

void		e_gw_share_notification_free	(EGwShareNotification *notification);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (EGwShareNotification, e_gw_share_notification_free)

/* Whether the item of @id is such a message, by its ID alone */
gboolean	e_gw_share_is_notification_id	(const gchar *id);

/* Reads it without marking it read. E_GW_ERROR_ITEM_NOT_FOUND if the item is
 * no notification of a shared folder. */
EGwShareNotification *
		e_gw_connection_get_share_notification_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 GCancellable *cancellable,
						 GError **error);

/* Accepts the share: the folder appears below @container (NULL: the Cabinet)
 * as @name (NULL: its name at the owner). The notification is gone after it.
 * Returns the ID of the new folder. */
gchar *		e_gw_connection_accept_share_sync
						(EGwConnection *cnc,
						 const gchar *notification_id,
						 const gchar *name,
						 const gchar *description,
						 const gchar *container,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_SHARE_H */

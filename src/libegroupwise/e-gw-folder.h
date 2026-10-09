/*
 * e-gw-folder.h: GroupWise folders (getFolderListRequest)
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

#ifndef E_GW_FOLDER_H
#define E_GW_FOLDER_H

#include "e-gw-connection.h"

G_BEGIN_DECLS

/* The <folderType> values that matter to Evolution; anything else is OTHER. */
typedef enum {
	E_GW_FOLDER_TYPE_NORMAL,	/* user folder, no folderType */
	E_GW_FOLDER_TYPE_ROOT,
	E_GW_FOLDER_TYPE_MAILBOX,
	E_GW_FOLDER_TYPE_SENT_ITEMS,
	E_GW_FOLDER_TYPE_DRAFT,
	E_GW_FOLDER_TYPE_TRASH,
	E_GW_FOLDER_TYPE_JUNK,
	E_GW_FOLDER_TYPE_CALENDAR,
	E_GW_FOLDER_TYPE_CONTACTS,
	E_GW_FOLDER_TYPE_CABINET,
	E_GW_FOLDER_TYPE_CHECKLIST,
	E_GW_FOLDER_TYPE_DOCUMENTS,
	E_GW_FOLDER_TYPE_QUERY,
	E_GW_FOLDER_TYPE_NOTES,
	E_GW_FOLDER_TYPE_OTHER
} EGwFolderType;

typedef enum {
	E_GW_FOLDER_KIND_FOLDER,	/* type="Folder" */
	E_GW_FOLDER_KIND_SYSTEM,	/* type="SystemFolder" */
	E_GW_FOLDER_KIND_SHARED		/* type="SharedFolder": shared by or to this user */
} EGwFolderKind;

typedef struct {
	gchar *id;
	gchar *name;
	gchar *parent_id;		/* NULL for top level folders */
	gchar *folder_type;		/* raw <folderType>, NULL if absent */
	EGwFolderType type;
	EGwFolderKind kind;
	gint64 count;			/* -1 if not reported */
	gint64 unread_count;		/* -1 if not reported */
	gboolean is_shared_to_me;
	gboolean is_shared_by_me;
	gchar *owner;			/* e-mail of the owner of a shared folder */
	gchar *owner_name;		/* and the owner's name, if given */
	gchar *proxy_email;		/* a proxy calendar: e-mail of its user as typed
					 * when it was added (not always valid) */
	gchar *proxy_uuid;		/* and the user's UUID (see getProxyList) */
	gchar *description;
	gboolean is_calendar;		/* the Calendar or a folder with <calendarAttribute> */
	gboolean includes_content;	/* its appointments show in the main calendar
					 * (no DontIncludeContent flag) */
	gint64 color;			/* calendar color as COLORREF (0x00BBGGRR), -1 if none */
} EGwFolder;

/* What a calendar folder is to the user */
typedef enum {
	E_GW_CALENDAR_ROLE_NONE,	/* no calendar, or one not shown (subscribed ones) */
	E_GW_CALENDAR_ROLE_MAIN,	/* the Calendar */
	E_GW_CALENDAR_ROLE_OWN,		/* an own subcalendar */
	E_GW_CALENDAR_ROLE_PROXY,	/* the calendar of another user, read by proxy login */
	E_GW_CALENDAR_ROLE_SHARED	/* a calendar another user shared to the user */
} EGwCalendarRole;

EGwFolder *	e_gw_folder_new_from_node	(xmlNode *node);
void		e_gw_folder_free		(EGwFolder *folder);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (EGwFolder, e_gw_folder_free)

EGwFolderType	e_gw_folder_type_from_string	(const gchar *folder_type);

/* An own subcalendar: a calendar folder of the user below the Calendar, not
 * the calendar of another user (a proxy calendar, a shared one) nor a folder
 * that only holds such calendars, nor a subscribed Internet calendar. Its
 * appointments are in the Calendar as well, unless the folder has
 * DontIncludeContent. @folders is the whole list. */
gboolean	e_gw_folder_is_own_subcalendar	(const EGwFolder *folder,
						 GPtrArray *folders);

/* The role of @folder among @folders (the whole list). Subscribed
 * calendars (Internet calendars GroupWise fetches) have none. */
EGwCalendarRole	e_gw_folder_get_calendar_role	(const EGwFolder *folder,
						 GPtrArray *folders);

/* The calendar color as "#rrggbb" (NULL: none) */
gchar *		e_gw_folder_dup_color		(const EGwFolder *folder);

/* All folders below @parent ("folders" for the whole mailbox), in server
 * order, parents before their children.
 * Returns: (element-type EGwFolder) (transfer full) */
GPtrArray *	e_gw_connection_get_folder_list_sync
						(EGwConnection *cnc,
						 const gchar *parent,
						 gboolean recurse,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_FOLDER_H */

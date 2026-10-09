/*
 * e-gw-connection.h: a session with the SOAP interface of a GroupWise POA
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

#ifndef E_GW_CONNECTION_H
#define E_GW_CONNECTION_H

#include <gio/gio.h>
#include <libxml/tree.h>

G_BEGIN_DECLS

/* The SOAP API version sent at login */
#define E_GW_SOAP_VERSION "1.15"

/* Error codes are the GroupWise status codes; these are local ones. */
#define E_GW_ERROR (e_gw_error_quark ())

typedef enum {
	E_GW_ERROR_CONNECTION = 1000,		/* transport failure, HTTP error */
	E_GW_ERROR_XML = 1001,			/* unparsable response, SOAP fault */
	E_GW_ERROR_EMPTY = 1002,		/* empty or unparsable response, retried */
	E_GW_ERROR_NO_PASSWORD = 1003,
	E_GW_ERROR_NOT_LOGGED_IN = 1004,
	E_GW_ERROR_INVALID_PASSWORD = 53273,	/* 0xD019, sent without description */
	E_GW_ERROR_UNKNOWN_USER = 53505,	/* not on this post office */
	E_GW_ERROR_TOO_MANY_RECORDS = 53531,
	E_GW_ERROR_ITEM_NOT_FOUND = 59906,
	E_GW_ERROR_INVALID_SESSION = 59910,	/* 0xEA06: lost or timed out, sent without description */
	E_GW_ERROR_NOT_SUPPORTED = 59916,
	E_GW_ERROR_REDIRECT = 59923
} EGwError;

GQuark		e_gw_error_quark		(void);

/* The response element of a successful call (e.g. <getItemsResponse>). */
typedef struct _EGwResponse EGwResponse;

xmlNode *	e_gw_response_get_node		(EGwResponse *response);
void		e_gw_response_free		(EGwResponse *response);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (EGwResponse, e_gw_response_free)

#define E_GW_DEFAULT_PORT 7191

#define E_TYPE_GW_CONNECTION (e_gw_connection_get_type ())
G_DECLARE_FINAL_TYPE (EGwConnection, e_gw_connection, E, GW_CONNECTION, GObject)

EGwConnection *	e_gw_connection_new		(const gchar *host,
						 guint16 port,
						 gboolean use_ssl);

void		e_gw_connection_set_verify_ssl	(EGwConnection *cnc,
						 gboolean verify);
void		e_gw_connection_set_timeout	(EGwConnection *cnc,
						 guint seconds);

/* Decides about a certificate GIO does not trust (unknown CA, name mismatch).
 * Called in the thread of the request, at most once per certificate and
 * connection. Without a function such certificates are refused. */
typedef gboolean (*EGwCertificateFunc)		(EGwConnection *cnc,
						 GTlsCertificate *certificate,
						 GTlsCertificateFlags errors,
						 gpointer user_data);

void		e_gw_connection_set_certificate_func
						(EGwConnection *cnc,
						 EGwCertificateFunc func,
						 gpointer user_data,
						 GDestroyNotify destroy);

/* Host and port can change after login (POA redirect), SSL after an HTTP redirect. */
const gchar *	e_gw_connection_get_host	(EGwConnection *cnc);
guint16		e_gw_connection_get_port	(EGwConnection *cnc);
gboolean	e_gw_connection_get_use_ssl	(EGwConnection *cnc);

/* What another user granted in a proxy login */
typedef enum {
	E_GW_PROXY_APPOINTMENT_READ	= 1 << 0,
	E_GW_PROXY_APPOINTMENT_WRITE	= 1 << 1,
	E_GW_PROXY_TASK_READ		= 1 << 2,
	E_GW_PROXY_TASK_WRITE		= 1 << 3,
	E_GW_PROXY_NOTE_READ		= 1 << 4,
	E_GW_PROXY_NOTE_WRITE		= 1 << 5,
	E_GW_PROXY_MAIL_READ		= 1 << 6,
	E_GW_PROXY_MAIL_WRITE		= 1 << 7,
	E_GW_PROXY_ALARMS		= 1 << 8,	/* receive the alarms */
	E_GW_PROXY_NOTIFY		= 1 << 9,	/* receive the notifications */
	E_GW_PROXY_READ_HIDDEN		= 1 << 10,	/* read items marked private */
	E_GW_PROXY_SETUP		= 1 << 11,	/* change settings, rules and folders */
	E_GW_PROXY_SETUP_SECURITY	= 1 << 12	/* change the security options (not in the SDK) */
} EGwProxyRights;

/* Before login: log in as proxy of the user with this e-mail address, with
 * the user's own credentials; the session then acts in the other mailbox,
 * with the rights granted there. NULL: a normal login. */
void		e_gw_connection_set_proxy	(EGwConnection *cnc,
						 const gchar *email);
const gchar *	e_gw_connection_get_proxy	(EGwConnection *cnc);
/* Valid after a proxy login */
EGwProxyRights	e_gw_connection_get_proxy_rights
						(EGwConnection *cnc);

/* getProxyListRequest: the users who granted this user proxy rights, as a
 * table UUID -> e-mail address (the address to log in as their proxy; the
 * UUID stays when a user is renamed).
 * Returns: (transfer full) (nullable) */
GHashTable *	e_gw_connection_get_proxy_list_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);

/* The same with the names, in the order of the server */
typedef struct {
	gchar *uuid;
	gchar *email;
	gchar *display_name;
} EGwProxyUser;

void		e_gw_proxy_user_free		(EGwProxyUser *user);
/* Returns: (transfer container) (element-type EGwProxyUser) (nullable) */
GPtrArray *	e_gw_connection_get_proxy_users_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);

/* Plain-text login. The credentials are kept to log in again once when the
 * POA reports a lost session (it drops sessions after 30 idle minutes). */
gboolean	e_gw_connection_login_sync	(EGwConnection *cnc,
						 const gchar *user,
						 const gchar *password,
						 GCancellable *cancellable,
						 GError **error);
void		e_gw_connection_logout_sync	(EGwConnection *cnc,
						 GCancellable *cancellable);
gboolean	e_gw_connection_is_logged_in	(EGwConnection *cnc);

/* Valid after login */
const gchar *	e_gw_connection_get_session	(EGwConnection *cnc);
const gchar *	e_gw_connection_get_user_name	(EGwConnection *cnc);
const gchar *	e_gw_connection_get_user_email	(EGwConnection *cnc);
const gchar *	e_gw_connection_get_user_id	(EGwConnection *cnc);
const gchar *	e_gw_connection_get_user_uuid	(EGwConnection *cnc);
const gchar *	e_gw_connection_get_server_version
						(EGwConnection *cnc);

/* Sends <ACTIONRequest>INNER_XML</ACTIONRequest>. Fails with the GroupWise
 * status code as error code when the status is not 0. */
EGwResponse *	e_gw_connection_call_sync	(EGwConnection *cnc,
						 const gchar *action,
						 const gchar *inner_xml,
						 GCancellable *cancellable,
						 GError **error);

/* Streams an attachment, or with @as_mime the whole item @id as RFC 822,
 * through the attachment URL of the POA (GroupWise 8 and later). */
gboolean	e_gw_connection_download_sync	(EGwConnection *cnc,
						 const gchar *id,
						 gboolean as_mime,
						 GOutputStream *output,
						 GCancellable *cancellable,
						 GError **error);

/* Requests */

/* The elements of getItemsResponse/items/item. @view, @filter_xml (a complete
 * <filter> element) may be NULL; @count < 0 means all. */
EGwResponse *	e_gw_connection_get_items_sync	(EGwConnection *cnc,
						 const gchar *container,
						 const gchar *view,
						 const gchar *filter_xml,
						 gint count,
						 GCancellable *cancellable,
						 GError **error);
EGwResponse *	e_gw_connection_get_item_sync	(EGwConnection *cnc,
						 const gchar *id,
						 const gchar *view,
						 GCancellable *cancellable,
						 GError **error);

/* Called for each item; returning FALSE stops the iteration (the error is then the caller's). */
typedef gboolean (*EGwItemFunc)			(xmlNode *item,
						 gpointer user_data);

/* Reads all items of @container through a server-side cursor, @page_size
 * items per request. */
gboolean	e_gw_connection_foreach_item_sync
						(EGwConnection *cnc,
						 const gchar *container,
						 const gchar *view,
						 const gchar *filter_xml,
						 guint page_size,
						 EGwItemFunc func,
						 gpointer user_data,
						 GCancellable *cancellable,
						 GError **error);

/* getQuickMessagesRequest: the items of @container changed since @since (a
 * server time; new items count as changed with list "Modified"), from the
 * index of the POA. Deleted items are not reported, trash and proxy
 * sessions are not supported. @out_server_time gets the time to pass as
 * @since next time. */
EGwResponse *	e_gw_connection_get_quick_messages_sync
						(EGwConnection *cnc,
						 const gchar *list,
						 const gchar *since,
						 const gchar *container,
						 const gchar *view,
						 gchar **out_server_time,
						 GCancellable *cancellable,
						 GError **error);

/* The ID of an item in another container: item IDs are "<item>@<n>:<container>" */
gchar *		e_gw_item_id_in_container	(const gchar *id,
						 const gchar *container);

/* moveItemsRequest: moves @ids from @from to @container, or with @from NULL
 * copies (links) them. The new IDs follow e_gw_item_id_in_container(); the
 * POA does not return them. */
gboolean	e_gw_connection_move_items_sync	(EGwConnection *cnc,
						 const gchar * const *ids,
						 const gchar *container,
						 const gchar *from,
						 GCancellable *cancellable,
						 GError **error);

/* removeItemsRequest: takes the items out of @container; mailbox items go
 * to the Trash (as a link where they are still in other folders). */
gboolean	e_gw_connection_remove_items_sync
						(EGwConnection *cnc,
						 const gchar * const *ids,
						 const gchar *container,
						 GCancellable *cancellable,
						 GError **error);

/* purgeRequest: deletes the items permanently, from EVERY folder they are
 * in, not only from the one the ID names. */
gboolean	e_gw_connection_purge_sync	(EGwConnection *cnc,
						 const gchar * const *ids,
						 GCancellable *cancellable,
						 GError **error);

/* Folders: returns the new ID / changes name or parent / removes the folder with its content */
gchar *		e_gw_connection_create_folder_sync
						(EGwConnection *cnc,
						 const gchar *parent,
						 const gchar *name,
						 GCancellable *cancellable,
						 GError **error);
gboolean	e_gw_connection_modify_folder_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 const gchar *new_name,
						 const gchar *new_parent,
						 GCancellable *cancellable,
						 GError **error);
gboolean	e_gw_connection_remove_folder_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 GCancellable *cancellable,
						 GError **error);

gboolean	e_gw_connection_mark_read_sync	(EGwConnection *cnc,
						 const gchar * const *ids,
						 gboolean read,
						 GCancellable *cancellable,
						 GError **error);

/* The last certificate refused (not trusted by GIO nor by the certificate
 * function), as PEM with its errors: what EDS shows in its trust prompt. */
gboolean	e_gw_connection_get_refused_certificate
						(EGwConnection *cnc,
						 gchar **out_pem,
						 GTlsCertificateFlags *out_errors);

/* createItemRequest with a complete <item>; returns the new ID */
gchar *		e_gw_connection_create_item_sync
						(EGwConnection *cnc,
						 const gchar *item_xml,
						 GCancellable *cancellable,
						 GError **error);
/* modifyItemRequest; @updates_xml holds <add>, <update> and <delete> parts */
gboolean	e_gw_connection_modify_item_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 const gchar *updates_xml,
						 GCancellable *cancellable,
						 GError **error);

/* sendItemRequest with a complete <item>: sends it, or with the source
 * "personal" puts it into the user's own calendar. Returns the first ID. */
gchar *		e_gw_connection_send_item_sync	(EGwConnection *cnc,
						 const gchar *item_xml,
						 GCancellable *cancellable,
						 GError **error);

/* Answers to an appointment the user received; the organizer is told.
 * @accept_level: Busy, Tentative, Free or OutOfOffice. */
gboolean	e_gw_connection_accept_sync	(EGwConnection *cnc,
						 const gchar *id,
						 const gchar *accept_level,
						 const gchar *comment,
						 GCancellable *cancellable,
						 GError **error);
gboolean	e_gw_connection_decline_sync	(EGwConnection *cnc,
						 const gchar *id,
						 const gchar *comment,
						 GCancellable *cancellable,
						 GError **error);

/* Withdraws an item the user sent from all mailboxes */
gboolean	e_gw_connection_retract_sync	(EGwConnection *cnc,
						 const gchar *id,
						 const gchar *comment,
						 GCancellable *cancellable,
						 GError **error);

/* Withdraws items the user sent (mail too; @ids with their container)
 * from the recipients' mailboxes; the sent items stay, their recipients
 * "retracted" */
gboolean	e_gw_connection_retract_from_recipients_sync
						(EGwConnection *cnc,
						 const gchar * const *ids,
						 GCancellable *cancellable,
						 GError **error);

/* Marks a task completed or open again */
gboolean	e_gw_connection_complete_sync	(EGwConnection *cnc,
						 const gchar *id,
						 gboolean completed,
						 GCancellable *cancellable,
						 GError **error);

/* The busy times of @emails between two UTC times (2026-09-28T00:00:00Z):
 * the getFreeBusyResponse with freeBusyInfo/user/blocks/block (startDate,
 * endDate, acceptLevel). Waits up to @wait_seconds for other post offices;
 * external addresses are left out. */
EGwResponse *	e_gw_connection_get_free_busy_sync
						(EGwConnection *cnc,
						 const gchar * const *emails,
						 const gchar *start_utc,
						 const gchar *end_utc,
						 guint wait_seconds,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_CONNECTION_H */

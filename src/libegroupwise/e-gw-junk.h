/*
 * e-gw-junk.h: the junk mail handling of GroupWise
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

#ifndef E_GW_JUNK_H
#define E_GW_JUNK_H

#include "e-gw-connection.h"

G_BEGIN_DECLS

/*
 * GroupWise sorts Internet mail by three lists of addresses and domains:
 * the junk list (into the Junk Mail folder), the block list (not
 * delivered) and the trust list (never junk, before the other two). The
 * settings switch the lists and more (see E_GW_JUNK_SETTING_*).
 */
typedef enum {
	E_GW_JUNK_LIST_JUNK,
	E_GW_JUNK_LIST_BLOCK,
	E_GW_JUNK_LIST_TRUST
} EGwJunkList;

typedef struct {
	gchar *id;
	gchar *match;		/* an address or a domain */
	gboolean is_domain;
	EGwJunkList list;
} EGwJunkEntry;

void		e_gw_junk_entry_free		(EGwJunkEntry *entry);

/* Returns: (element-type EGwJunkEntry) (transfer full) */
GPtrArray *	e_gw_connection_get_junk_entries_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);
/* Returns the ID of the new entry */
gchar *		e_gw_connection_create_junk_entry_sync
						(EGwConnection *cnc,
						 const gchar *match,
						 gboolean is_domain,
						 EGwJunkList list,
						 GCancellable *cancellable,
						 GError **error);
gboolean	e_gw_connection_remove_junk_entry_sync
						(EGwConnection *cnc,
						 const gchar *id,
						 GCancellable *cancellable,
						 GError **error);

/* The fields of the junk mail settings */
#define E_GW_JUNK_SETTING_USE_JUNK_LIST		"useJunkList"
#define E_GW_JUNK_SETTING_USE_BLOCK_LIST	"useBlockList"
#define E_GW_JUNK_SETTING_USE_PAB		"usePAB"	/* junk from senders in no personal address book */
#define E_GW_JUNK_SETTING_ICAL_USE_PAB		"spamJunkICALPABEnable"	/* the same for appointments */
#define E_GW_JUNK_SETTING_AUTO_DELETE		"autoDelete"	/* empty the Junk Mail folder ... */
#define E_GW_JUNK_SETTING_PERSISTENCE		"persistence"	/* ... after this many days */

/* Returns: (transfer full): field -> value */
GHashTable *	e_gw_connection_get_junk_settings_sync
						(EGwConnection *cnc,
						 GCancellable *cancellable,
						 GError **error);
/* Sets the fields of @settings (field -> value) */
gboolean	e_gw_connection_modify_junk_settings_sync
						(EGwConnection *cnc,
						 GHashTable *settings,
						 GCancellable *cancellable,
						 GError **error);

/* Puts @match (an address or, with @is_domain, a domain) onto @list, off
 * the lists it contradicts: trust takes it off the junk and block lists,
 * junk and block off the trust list, block also off the junk list.
 * @entries: the lists as known, NULL to read them. */
gboolean	e_gw_connection_put_junk_entry_sync
						(EGwConnection *cnc,
						 const gchar *match,
						 gboolean is_domain,
						 EGwJunkList list,
						 GPtrArray *entries,
						 GCancellable *cancellable,
						 GError **error);

/* The user said an Internet sender's mail is junk (@junk TRUE: onto the
 * junk list, off the trust list) or is not (@junk FALSE: onto the trust
 * list, off the junk and block lists). Entries of the address only; a
 * domain entry stays. @entries: the lists as known, NULL to read them. */
gboolean	e_gw_connection_learn_junk_sender_sync
						(EGwConnection *cnc,
						 const gchar *address,
						 gboolean junk,
						 GPtrArray *entries,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* E_GW_JUNK_H */

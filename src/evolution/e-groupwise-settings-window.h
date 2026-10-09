/*
 * e-groupwise-settings-window.h: the window "GroupWise Settings" of an account
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

#ifndef E_GROUPWISE_SETTINGS_WINDOW_H
#define E_GROUPWISE_SETTINGS_WINDOW_H

#include <gtk/gtk.h>
#include <libedataserver/libedataserver.h>

#include "e-gw-connection.h"

G_BEGIN_DECLS

/*
 * A tab of the window: settings of the server. The window reads them for
 * all tabs in one login when it opens and writes the changes of all tabs
 * in one login on OK; the _sync functions run in a thread.
 */
typedef struct {
	const gchar *title;
	gpointer	(*new_tab)	(void);			/* the tab's state */
	GtkWidget *	(*get_widget)	(gpointer tab);
	gpointer	(*load_sync)	(EGwConnection *cnc,	/* the server's data */
					 GCancellable *cancellable,
					 GError **error);
	void		(*fill)		(gpointer tab,		/* shows the data (takes it) */
					 gpointer data);
	gpointer	(*collect)	(gpointer tab);		/* the changes, NULL: none */
	gboolean	(*apply_sync)	(gpointer changes,
					 EGwConnection *cnc,
					 GCancellable *cancellable,
					 GError **error);
	GDestroyNotify	free_data;
	GDestroyNotify	free_changes;
	GDestroyNotify	free_tab;
	/* Optional: the account, after new_tab */
	void		(*set_account)	(gpointer tab,
					 ESourceRegistry *registry,
					 ESource *account_source);
} EGroupwiseSettingsTab;

/* The tabs */
const EGroupwiseSettingsTab *
		e_groupwise_vacation_tab	(void);
const EGroupwiseSettingsTab *
		e_groupwise_rules_tab		(void);
const EGroupwiseSettingsTab *
		e_groupwise_junk_tab		(void);
const EGroupwiseSettingsTab *
		e_groupwise_signatures_tab	(void);
const EGroupwiseSettingsTab *
		e_groupwise_proxy_tab		(void);
const EGroupwiseSettingsTab *
		e_groupwise_access_tab		(void);

/* Opens the window of the account (or brings an open one to the front),
 * showing the tab @tab (NULL: the first one) */
void		e_groupwise_settings_window_show
						(GtkWindow *parent,
						 ESourceRegistry *registry,
						 ESource *account_source,
						 const EGroupwiseSettingsTab *tab);

G_END_DECLS

#endif /* E_GROUPWISE_SETTINGS_WINDOW_H */

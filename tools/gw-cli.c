/*
 * gw-cli.c: command line access to a GroupWise POA through libegroupwise
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <gio/gunixoutputstream.h>
#include <libxml/tree.h>

#include "e-gw-connection.h"
#include "e-gw-folder.h"
#include "e-gw-proxy.h"
#include "e-gw-rule.h"
#include "e-gw-vacation.h"
#include "e-gw-xml.h"

static gchar *opt_host = NULL;
static gint opt_port = E_GW_DEFAULT_PORT;
static gboolean opt_ssl = FALSE;
static gboolean opt_insecure = FALSE;
static gchar *opt_user = NULL;
static gchar *opt_proxy = NULL;

static GOptionEntry entries[] = {
	{ "host", 'H', 0, G_OPTION_ARG_STRING, &opt_host, "POA host (or $GW_HOST)", "HOST" },
	{ "port", 'p', 0, G_OPTION_ARG_INT, &opt_port, "SOAP port (default 7191)", "PORT" },
	{ "ssl", 's', 0, G_OPTION_ARG_NONE, &opt_ssl, "Use https", NULL },
	{ "insecure", 'k', 0, G_OPTION_ARG_NONE, &opt_insecure, "Do not verify the certificate", NULL },
	{ "user", 'u', 0, G_OPTION_ARG_STRING, &opt_user, "User (or $GW_USER)", "USER" },
	{ "proxy", 'P', 0, G_OPTION_ARG_STRING, &opt_proxy, "Act as proxy of the user with this e-mail address", "EMAIL" },
	{ NULL }
};

static const gchar *summary =
	"Commands:\n"
	"  login                     show the user info of the session\n"
	"  folders                   list all folders\n"
	"  vacation                  show the out of office rule\n"
	"  vacation-set ON SUBJECT MESSAGE [FROM TO]  set it (FROM/TO: days or ISO times)\n"
	"  rules                     list the rules (conditions and actions)\n"
	"  rule-demo FOLDER-ID       create a disabled demo rule (test accounts)\n"
	"  rule-demo-change ID       change the demo rule: other filter, actions, enabled\n"
	"  rule-clone ID             create a copy of a rule through the library (test accounts)\n"
	"  rule-run ID               run a rule now\n"
	"  rule-remove ID            remove a rule\n"
	"  access                    list who may log in as proxy, with the rights (hex)\n"
	"  access-grant EMAIL RIGHTS grant rights (hex, EGwProxyRights) to a user\n"
	"  access-set ID OLD NEW     change the rights of an entry\n"
	"  access-remove ID          take the access away\n"
	"  items CONTAINER [COUNT]   list the items of a folder\n"
	"  item ID [VIEW]            print an item as XML\n"
	"  mime ID                   write an item as RFC 822 to stdout\n"
	"  attachment ID             write an attachment to stdout\n"
	"  raw ACTION [INNER-XML]    send ACTIONRequest, print the response\n"
	"\n"
	"The password is read from $GW_PASSWORD or asked for.";

static gchar *
read_password (void)
{
	const gchar *env = g_getenv ("GW_PASSWORD");
	struct termios old, silent;
	gchar buffer[256];
	gboolean tty = isatty (STDIN_FILENO);

	if (env && *env)
		return g_strdup (env);

	fprintf (stderr, "Password: ");
	if (tty && tcgetattr (STDIN_FILENO, &old) == 0) {
		silent = old;
		silent.c_lflag &= ~ECHO;
		tcsetattr (STDIN_FILENO, TCSANOW, &silent);
	}
	if (!fgets (buffer, sizeof (buffer), stdin))
		buffer[0] = '\0';
	if (tty) {
		tcsetattr (STDIN_FILENO, TCSANOW, &old);
		fprintf (stderr, "\n");
	}

	buffer[strcspn (buffer, "\r\n")] = '\0';

	return g_strdup (buffer);
}

static void
print_node (xmlNode *node)
{
	xmlBuffer *buffer = xmlBufferCreate ();

	xmlNodeDump (buffer, node->doc, node, 0, 1);
	printf ("%s\n", (const gchar *) xmlBufferContent (buffer));
	xmlBufferFree (buffer);
}

static const gchar *
folder_type_label (EGwFolder *folder)
{
	if (folder->folder_type)
		return folder->folder_type;

	switch (folder->kind) {
	case E_GW_FOLDER_KIND_SHARED:
		return "(shared)";
	case E_GW_FOLDER_KIND_SYSTEM:
		return "(system)";
	default:
		return "";
	}
}

/* Prints the folders as a tree, children below their parent */
static void
print_folders (GPtrArray *folders,
	       const gchar *parent_id,
	       gint depth)
{
	guint ii;

	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii];
		gboolean is_child = parent_id ? g_strcmp0 (folder->parent_id, parent_id) == 0 : folder->parent_id == NULL;

		if (!is_child)
			continue;

		printf ("%*s%-*s %-10s", depth * 2, "", 30 - depth * 2, folder->name ? folder->name : "?",
			folder_type_label (folder));
		if (folder->count >= 0)
			printf (" %5" G_GINT64_FORMAT, folder->count);
		if (folder->unread_count > 0)
			printf (" (%" G_GINT64_FORMAT " unread)", folder->unread_count);
		printf ("  %s\n", folder->id);

		print_folders (folders, folder->id, depth + 1);
	}
}

static gboolean
cmd_folders (EGwConnection *cnc,
	     GError **error)
{
	GPtrArray *folders = e_gw_connection_get_folder_list_sync (cnc, NULL, TRUE, NULL, error);
	guint ii;

	if (!folders)
		return FALSE;

	print_folders (folders, NULL, 0);

	/* Folders whose parent is not in the list would be lost in the tree */
	for (ii = 0; ii < folders->len; ii++) {
		EGwFolder *folder = folders->pdata[ii], *other = NULL;
		guint jj;

		for (jj = 0; folder->parent_id && jj < folders->len && !other; jj++) {
			if (g_strcmp0 (((EGwFolder *) folders->pdata[jj])->id, folder->parent_id) == 0)
				other = folders->pdata[jj];
		}
		if (folder->parent_id && !other)
			printf ("%-30s %-10s        %s (parent %s not listed)\n", folder->name, folder_type_label (folder),
				folder->id, folder->parent_id);
	}

	/* What the calendars are to Evolution */
	for (ii = 0; ii < folders->len; ii++) {
		static const gchar *roles[] = { NULL, "main calendar", "own subcalendar", "proxy calendar", "shared calendar" };
		EGwFolder *folder = folders->pdata[ii];
		EGwCalendarRole role = e_gw_folder_get_calendar_role (folder, folders);

		if (role != E_GW_CALENDAR_ROLE_NONE)
			printf ("calendar: %-30s %s%s%s\n", folder->name, roles[role],
				folder->proxy_email ? " of " : "", folder->proxy_email ? folder->proxy_email : "");
	}

	g_ptr_array_unref (folders);

	return TRUE;
}

static gboolean
cmd_items (EGwConnection *cnc,
	   const gchar *container,
	   gint count,
	   GError **error)
{
	g_autoptr (EGwResponse) response = NULL;
	xmlNode *item;

	response = e_gw_connection_get_items_sync (cnc, container, "default", NULL, count, NULL, error);
	if (!response)
		return FALSE;

	item = e_gw_xml_find (e_gw_response_get_node (response), "items");
	for (item = e_gw_xml_first_child (item, "item"); item; item = e_gw_xml_next_sibling (item, "item")) {
		g_autofree gchar *type = e_gw_xml_dup_attr (item, "type");
		g_autofree gchar *raw_id = e_gw_xml_dup_text (item, "id");
		g_autofree gchar *id = e_gw_clean_id (raw_id);
		g_autofree gchar *subject = e_gw_xml_dup_text (item, "subject");
		g_autofree gchar *from = e_gw_xml_dup_text (item, "distribution/from/displayName");
		g_autofree gchar *date = e_gw_xml_dup_text (item, "delivered");
		gboolean read = e_gw_xml_get_bool (item, "status/read");

		printf ("%c %-12s %-20s %-24s %s\n  %s\n", read ? ' ' : 'N', type ? type : "?",
			date ? date : "", from ? from : "", subject ? subject : "", id ? id : "");
	}

	return TRUE;
}

static gboolean
cmd_download (EGwConnection *cnc,
	      const gchar *id,
	      gboolean as_mime,
	      GError **error)
{
	GOutputStream *out = g_unix_output_stream_new (STDOUT_FILENO, FALSE);
	gboolean success = e_gw_connection_download_sync (cnc, id, as_mime, out, NULL, error);

	g_object_unref (out);

	return success;
}

static gboolean
cmd_response (EGwResponse *response,
	      GError **error)
{
	if (!response)
		return FALSE;

	print_node (e_gw_response_get_node (response));
	e_gw_response_free (response);

	return TRUE;
}

static gboolean
run_command (EGwConnection *cnc,
	     gint argc,
	     gchar **argv,
	     GError **error)
{
	const gchar *cmd = argv[1];

	if (g_str_equal (cmd, "vacation-set") && argc >= 5) {
		EGwVacation *vacation = e_gw_vacation_new ();
		gboolean success;

		vacation->enabled = atoi (argv[2]) != 0;
		vacation->subject = g_strdup (argv[3]);
		vacation->message = g_strdup (argv[4]);
		/* YYYY-MM-DD YYYY-MM-DD: whole days; ISO times: from, to */
		if (argc >= 7) {
			vacation->has_range = TRUE;
			vacation->all_day = strlen (argv[5]) == 10;
			if (vacation->all_day) {
				vacation->start_day = g_strdup (argv[5]);
				vacation->end_day = g_strdup (argv[6]);
			} else {
				vacation->start = g_date_time_new_from_iso8601 (argv[5], NULL);
				vacation->end = g_date_time_new_from_iso8601 (argv[6], NULL);
			}
		}
		success = e_gw_connection_set_vacation_sync (cnc, vacation, NULL, error);
		e_gw_vacation_free (vacation);
		return success;
	}
	if (g_str_equal (cmd, "rules")) {
		GPtrArray *rules = e_gw_connection_get_rules_sync (cnc, NULL, error);
		guint ii, jj;

		if (!rules)
			return FALSE;
		for (ii = 0; ii < rules->len; ii++) {
			EGwRule *rule = rules->pdata[ii];
			gchar *filter = e_gw_filter_node_to_xml (rule->filter);

			printf ("%2d %s %-30s %s types=%s source=%s%s%s\n   %s\n", rule->sequence, rule->enabled ? "on " : "off",
				rule->name ? rule->name : "", rule->execution ? rule->execution : "",
				rule->types ? rule->types : "", rule->source ? rule->source : "",
				rule->container ? " folder=" : "", rule->container ? rule->container : "", rule->id);
			printf ("   filter: %s\n", filter);
			for (jj = 0; jj < rule->actions->len; jj++) {
				EGwRuleAction *action = rule->actions->pdata[jj];

				printf ("   action %s%s%s%s%s%s%s\n", action->type,
					action->container ? " -> " : "", action->container ? action->container : "",
					action->has_item ? " subject=" : "", action->subject ? action->subject : "",
					action->recipients->len ? " recipients=" : "",
					action->recipients->len ? ((EGwRuleRecipient *) action->recipients->pdata[0])->email : "");
			}
			g_free (filter);
		}
		g_ptr_array_unref (rules);
		return TRUE;
	}
	if (g_str_equal (cmd, "rule-demo") && argc >= 3) {
		EGwRule *rule = e_gw_rule_new ();
		EGwRuleAction *action;
		gchar *id;

		rule->name = g_strdup ("Evolution-Demoregel");
		rule->execution = g_strdup ("New");
		rule->types = g_strdup ("Mail");
		rule->source = g_strdup ("received");
		rule->filter = e_gw_filter_node_new_group ("or");
		g_ptr_array_add (rule->filter->children, e_gw_filter_node_new_entry ("subject", "contains", "EVO-DEMO"));
		g_ptr_array_add (rule->filter->children, e_gw_filter_node_new_entry ("from", "contains", "nobody@example.com"));
		action = e_gw_rule_action_new ("Move");
		action->container = g_strdup (argv[2]);
		g_ptr_array_add (rule->actions, action);
		action = e_gw_rule_action_new ("Reply");
		action->has_item = TRUE;
		action->subject = g_strdup ("Automatische Antwort");
		action->text = g_strdup ("Danke für Ihre Nachricht – Grüße");
		g_ptr_array_add (rule->actions, action);
		id = e_gw_connection_create_rule_sync (cnc, rule, NULL, error);
		if (id)
			printf ("id: %s\n", id);
		g_free (id);
		e_gw_rule_free (rule);
		return id != NULL;
	}
	if (g_str_equal (cmd, "rule-demo-change") && argc >= 3) {
		GPtrArray *rules = e_gw_connection_get_rules_sync (cnc, NULL, error);
		EGwRule *old_rule = NULL, *rule;
		EGwRuleAction *action;
		gboolean success;
		guint ii;

		for (ii = 0; rules && ii < rules->len; ii++) {
			if (g_strcmp0 (((EGwRule *) rules->pdata[ii])->id, argv[2]) == 0)
				old_rule = rules->pdata[ii];
		}
		if (!old_rule) {
			if (rules)
				g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no rule %s", argv[2]);
			g_clear_pointer (&rules, g_ptr_array_unref);
			return FALSE;
		}
		rule = e_gw_rule_copy (old_rule);
		g_free (rule->name);
		rule->name = g_strdup ("Evolution-Demoregel geändert");
		e_gw_filter_node_free (rule->filter);
		rule->filter = e_gw_filter_node_new_entry ("created", "fieldLT", "-3");
		rule->filter->date = g_strdup ("Today");
		g_ptr_array_set_size (rule->actions, 0);
		g_ptr_array_add (rule->actions, e_gw_rule_action_new ("MarkRead"));
		action = e_gw_rule_action_new ("Forward");
		action->has_item = TRUE;
		action->subject = g_strdup ("Weitergeleitet");
		/* To the account itself */
		e_gw_rule_action_add_recipient (action, e_gw_connection_get_user_email (cnc), e_gw_connection_get_user_name (cnc), "TO");
		g_ptr_array_add (rule->actions, action);
		success = e_gw_connection_modify_rule_sync (cnc, old_rule, rule, NULL, error);
		e_gw_rule_free (rule);
		g_ptr_array_unref (rules);
		return success;
	}
	if (g_str_equal (cmd, "rule-clone") && argc >= 3) {
		GPtrArray *rules = e_gw_connection_get_rules_sync (cnc, NULL, error);
		gchar *id = NULL;
		guint ii;

		for (ii = 0; rules && ii < rules->len && !id; ii++) {
			EGwRule *rule = rules->pdata[ii];

			if (g_strcmp0 (rule->id, argv[2]) == 0) {
				EGwRule *copy = e_gw_rule_copy (rule);

				g_free (copy->name);
				copy->name = g_strconcat (rule->name, " (Kopie)", NULL);
				copy->sequence = rules->len;
				id = e_gw_connection_create_rule_sync (cnc, copy, NULL, error);
				e_gw_rule_free (copy);
				if (!id)
					break;
			}
		}
		g_clear_pointer (&rules, g_ptr_array_unref);
		if (id)
			printf ("id: %s\n", id);
		g_free (id);
		return id != NULL;
	}
	if (g_str_equal (cmd, "rule-run") && argc >= 3)
		return e_gw_connection_execute_rule_sync (cnc, argv[2], NULL, error);
	if (g_str_equal (cmd, "rule-remove") && argc >= 3)
		return e_gw_connection_remove_rule_sync (cnc, argv[2], NULL, error);
	if (g_str_equal (cmd, "access")) {
		GPtrArray *list = e_gw_connection_get_proxy_access_list_sync (cnc, NULL, error);
		guint ii;

		if (!list)
			return FALSE;
		for (ii = 0; ii < list->len; ii++) {
			EGwProxyAccess *access = list->pdata[ii];

			printf ("%-30s %-28s 0x%04x %s\n", access->display_name ? access->display_name : "",
				access->email ? access->email : "", access->rights, access->id);
		}
		g_ptr_array_unref (list);
		return TRUE;
	}
	if (g_str_equal (cmd, "access-grant") && argc >= 4) {
		gchar *id = e_gw_connection_create_proxy_access_sync (cnc, argv[2],
			(EGwProxyRights) g_ascii_strtoull (argv[3], NULL, 16), NULL, error);

		if (id)
			printf ("id: %s\n", id);
		g_free (id);
		return id != NULL;
	}
	if (g_str_equal (cmd, "access-set") && argc >= 5)
		return e_gw_connection_modify_proxy_access_sync (cnc, argv[2],
			(EGwProxyRights) g_ascii_strtoull (argv[3], NULL, 16),
			(EGwProxyRights) g_ascii_strtoull (argv[4], NULL, 16), NULL, error);
	if (g_str_equal (cmd, "access-remove") && argc >= 3)
		return e_gw_connection_remove_proxy_access_sync (cnc, argv[2], NULL, error);
	if (g_str_equal (cmd, "vacation")) {
		EGwVacation *vacation = e_gw_connection_get_vacation_sync (cnc, NULL, error);
		gchar *tz;

		if (!vacation)
			return FALSE;
		printf ("enabled: %d\nsubject: %s\nmessage: %s\nexternal: %d (contacts only %d) %s / %s\n"
			"include sender message: %d\nrange: %d all day %d\n",
			vacation->enabled, vacation->subject ? vacation->subject : "", vacation->message ? vacation->message : "",
			vacation->reply_to_external, vacation->my_contacts_only,
			vacation->external_subject ? vacation->external_subject : "",
			vacation->external_message ? vacation->external_message : "",
			vacation->include_sender_message, vacation->has_range, vacation->all_day);
		if (vacation->start_day)
			printf ("days: %s .. %s\n", vacation->start_day, vacation->end_day ? vacation->end_day : "");
		if (vacation->start && vacation->end) {
			gchar *a = g_date_time_format_iso8601 (vacation->start), *b = g_date_time_format_iso8601 (vacation->end);

			printf ("times: %s .. %s\n", a, b);
			g_free (a);
			g_free (b);
		}
		e_gw_vacation_free (vacation);
		tz = e_gw_connection_dup_local_timezone_xml_sync (cnc, NULL, NULL);
		printf ("local time zone of the server: %.60s\n", tz ? tz : "(none)");
		g_free (tz);
		return TRUE;
	}
	if (g_str_equal (cmd, "login")) {
		printf ("name:    %s\nemail:   %s\nuserid:  %s\nuuid:    %s\nversion: %s\nserver:  %s://%s:%u\n",
			e_gw_connection_get_user_name (cnc), e_gw_connection_get_user_email (cnc),
			e_gw_connection_get_user_id (cnc), e_gw_connection_get_user_uuid (cnc),
			e_gw_connection_get_server_version (cnc),
			e_gw_connection_get_use_ssl (cnc) ? "https" : "http",
			e_gw_connection_get_host (cnc), e_gw_connection_get_port (cnc));
		if (e_gw_connection_get_proxy (cnc)) {
			EGwProxyRights rights = e_gw_connection_get_proxy_rights (cnc);

			printf ("proxy of %s: appointments %s%s, tasks %s%s, notes %s%s, mail %s%s\n",
				e_gw_connection_get_proxy (cnc),
				rights & E_GW_PROXY_APPOINTMENT_READ ? "r" : "-", rights & E_GW_PROXY_APPOINTMENT_WRITE ? "w" : "-",
				rights & E_GW_PROXY_TASK_READ ? "r" : "-", rights & E_GW_PROXY_TASK_WRITE ? "w" : "-",
				rights & E_GW_PROXY_NOTE_READ ? "r" : "-", rights & E_GW_PROXY_NOTE_WRITE ? "w" : "-",
				rights & E_GW_PROXY_MAIL_READ ? "r" : "-", rights & E_GW_PROXY_MAIL_WRITE ? "w" : "-");
		}
		return TRUE;
	}
	if (g_str_equal (cmd, "folders"))
		return cmd_folders (cnc, error);
	if (g_str_equal (cmd, "items") && argc >= 3)
		return cmd_items (cnc, argv[2], argc >= 4 ? atoi (argv[3]) : -1, error);
	if (g_str_equal (cmd, "item") && argc >= 3)
		return cmd_response (e_gw_connection_get_item_sync (cnc, argv[2], argc >= 4 ? argv[3] : "default message attachments",
			NULL, error), error);
	if (g_str_equal (cmd, "mime") && argc >= 3)
		return cmd_download (cnc, argv[2], TRUE, error);
	if (g_str_equal (cmd, "attachment") && argc >= 3)
		return cmd_download (cnc, argv[2], FALSE, error);
	if (g_str_equal (cmd, "raw") && argc >= 3)
		return cmd_response (e_gw_connection_call_sync (cnc, argv[2], argc >= 4 ? argv[3] : "", NULL, error), error);

	g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Unknown command or missing argument: %s", cmd);
	return FALSE;
}

int
main (int argc,
      char **argv)
{
	GOptionContext *context;
	GError *error = NULL;
	EGwConnection *cnc;
	gchar *password;
	gboolean success;

	context = g_option_context_new ("COMMAND [ARGS] - access a GroupWise mailbox");
	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_set_description (context, summary);
	if (!g_option_context_parse (context, &argc, &argv, &error) || argc < 2) {
		gchar *help = g_option_context_get_help (context, TRUE, NULL);

		fprintf (stderr, "%s%s", error ? error->message : "", help);
		return 2;
	}
	g_option_context_free (context);

	if (!opt_host)
		opt_host = g_strdup (g_getenv ("GW_HOST"));
	if (!opt_user)
		opt_user = g_strdup (g_getenv ("GW_USER"));
	if (!opt_host || !opt_user || opt_port <= 0 || opt_port > G_MAXUINT16) {
		fprintf (stderr, "Host and user are required (--host/--user or GW_HOST/GW_USER)\n");
		return 2;
	}

	cnc = e_gw_connection_new (opt_host, (guint16) opt_port, opt_ssl);
	e_gw_connection_set_verify_ssl (cnc, !opt_insecure);
	e_gw_connection_set_proxy (cnc, opt_proxy);

	password = read_password ();
	success = e_gw_connection_login_sync (cnc, opt_user, password, NULL, &error);
	memset (password, 0, strlen (password));
	g_free (password);

	if (success)
		success = run_command (cnc, argc, argv, &error);

	if (!success) {
		if (error && error->domain == E_GW_ERROR)
			fprintf (stderr, "GroupWise error %d: %s\n", error->code, error->message);
		else
			fprintf (stderr, "Error: %s\n", error ? error->message : "unknown");
	}

	e_gw_connection_logout_sync (cnc, NULL);
	g_object_unref (cnc);
	g_clear_error (&error);

	return success ? 0 : 1;
}

/*
 * camel-groupwise-mime.h: MIME messages to GroupWise items for sending
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

#ifndef CAMEL_GROUPWISE_MIME_H
#define CAMEL_GROUPWISE_MIME_H

#include <camel/camel.h>

G_BEGIN_DECLS

/* The Mail item for sendItemRequest, built like gwmcp builds it for a real
 * POA: recipients, the plain text as the message, an HTML body as the
 * hidden attachment Text.htm with its pictures (hidden, with content IDs),
 * everything else as attachments. The POA builds the MIME for Internet
 * recipients itself: own headers do not survive, signed or encrypted
 * messages are refused.
 *
 * @recipients: the envelope (with Bcc) when sending; NULL takes To, Cc and
 * Bcc of the message. With @draft the item is saved, not sent. */
/* The header the provider gives a message it hands out: the ID of its
 * GroupWise item. A message forwarded as attachment carries it along. */
#define CAMEL_GROUPWISE_ITEM_ID_HEADER "X-GroupWise-Item-Id"

/* For an attached message of the account (@item_id from its header): the
 * <attachment> that refers to that item, and the <link> that marks it
 * forwarded (may be NULL); NULL attaches the message as MIME. */
typedef gchar *	(*CamelGroupwiseEmbedFunc)	(const gchar *item_id,
						 gchar **out_link_xml,
						 gpointer user_data,
						 GCancellable *cancellable);

gchar *		camel_groupwise_item_from_message
						(CamelMimeMessage *message,
						 CamelAddress *recipients,
						 gboolean draft,
						 CamelGroupwiseEmbedFunc embed_func,
						 gpointer embed_data,
						 GCancellable *cancellable,
						 GError **error);

G_END_DECLS

#endif /* CAMEL_GROUPWISE_MIME_H */

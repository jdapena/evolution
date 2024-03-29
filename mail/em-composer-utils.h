/*
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 *
 * Authors:
 *		Jeffrey Stedfast <fejj@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef EM_COMPOSER_UTILS_H
#define EM_COMPOSER_UTILS_H

#include <em-format/em-format.h>
#include <mail/e-mail-backend.h>
#include <mail/e-mail-enums.h>
#include <mail/e-mail-reader.h>
#include <composer/e-msg-composer.h>

G_BEGIN_DECLS

void		em_utils_compose_new_message	(EShell *shell,
						 CamelFolder *folder);
EMsgComposer *	em_utils_compose_new_message_with_mailto
						(EShell *shell,
						 const gchar *mailto,
						 CamelFolder *folder);
GtkWidget *	em_utils_edit_message		(EShell *shell,
						 CamelFolder *folder,
						 CamelMimeMessage *message,
						 const gchar *message_uid);
void		em_utils_edit_messages		(EMailReader *reader,
						 CamelFolder *folder,
						 GPtrArray *uids,
						 gboolean replace);
EMsgComposer *	em_utils_forward_message	(EShell *shell,
						 CamelMimeMessage *msg,
						 EMailForwardStyle style,
						 CamelFolder *folder,
						 const gchar *uid);
void		em_utils_forward_messages	(EMailReader *reader,
						 CamelFolder *folder,
						 GPtrArray *uids,
						 EMailForwardStyle style,
						 GtkWidget *destroy_when_done);
void		em_utils_redirect_message	(EShell *shell,
						 CamelMimeMessage *message);
void		em_utils_handle_receipt		(EMailBackend *backend,
						 CamelFolder *folder,
						 const gchar *message_uid,
						 CamelMimeMessage *message);
void		em_utils_send_receipt		(EMailBackend *backend,
						 CamelFolder *folder,
						 CamelMimeMessage *message);
gchar *		em_utils_construct_composer_text
						(CamelMimeMessage *message,
						 EMFormat *source_formatter);
gboolean	em_utils_is_munged_list_message	(CamelMimeMessage *message);
void		em_utils_get_reply_sender	(CamelMimeMessage *message,
						 CamelInternetAddress *to,
						 CamelNNTPAddress *postto);
void		em_utils_get_reply_all		(CamelMimeMessage *message,
						 CamelInternetAddress *to,
						 CamelInternetAddress *cc,
						 CamelNNTPAddress *postto);
EMsgComposer *	em_utils_reply_to_message	(EShell *shell,
						 CamelMimeMessage *message,
						 CamelFolder *folder,
						 const gchar *message_uid,
						 EMailReplyType type,
						 EMailReplyStyle style,
						 EMFormat *source,
						 CamelInternetAddress *address);
EDestination **	em_utils_camel_address_to_destination
						(CamelInternetAddress *iaddr);
void		em_configure_new_composer	(EMsgComposer *composer);

G_END_DECLS

#endif /* EM_COMPOSER_UTILS_H */

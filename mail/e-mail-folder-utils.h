/*
 * e-mail-folder-utils.h
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
 */

#ifndef E_MAIL_FOLDER_UTILS_H
#define E_MAIL_FOLDER_UTILS_H

/* CamelFolder wrappers with Evolution-specific policies. */

#include <camel/camel.h>

G_BEGIN_DECLS

void		e_mail_folder_append_message	(CamelFolder *folder,
						 CamelMimeMessage *message,
						 CamelMessageInfo *info,
						 gint io_priority,
						 GCancellable *cancellable,
						 GAsyncReadyCallback callback,
						 gpointer user_data);
gboolean	e_mail_folder_append_message_finish
						(CamelFolder *folder,
						 GAsyncResult *result,
						 gchar **appended_uid,
						 GError **error);

G_END_DECLS

#endif /* E_MAIL_FOLDER_UTILS_H */
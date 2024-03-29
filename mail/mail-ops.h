/*
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
 *		Peter Williams <peterw@ximian.com>
 *		Michael Zucchi <notzed@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef MAIL_OPS_H
#define MAIL_OPS_H

G_BEGIN_DECLS

#include <camel/camel.h>

#include <mail/mail-mt.h>
#include <mail/e-mail-backend.h>

void		mail_transfer_messages		(EMailSession *session,
						 CamelFolder *source,
						 GPtrArray *uids,
						 gboolean delete_from_source,
						 const gchar *dest_uri,
						 guint32 dest_flags,
						 void (*done) (gboolean ok, gpointer data),
						 gpointer data);

void mail_sync_folder (CamelFolder *folder,
		       void (*done) (CamelFolder *folder, gpointer data), gpointer data);

void mail_sync_store (CamelStore *store, gint expunge,
		     void (*done) (CamelStore *store, gpointer data), gpointer data);

void mail_refresh_folder (CamelFolder *folder,
			  void (*done) (CamelFolder *folder, gpointer data),
			  gpointer data);

void		mail_expunge_folder		(EMailBackend *backend,
						 CamelFolder *folder);

void		mail_empty_trash		(EMailBackend *backend,
						 CamelStore *store);

/* transfer (copy/move) a folder */
void mail_xfer_folder (const gchar *src_uri, const gchar *dest_uri, gboolean remove_source,
		       void (*done) (gchar *src_uri, gchar *dest_uri, gboolean remove_source,
				     CamelFolder *folder, gpointer data),
		       gpointer data);

/* yeah so this is messy, but it does a lot, maybe i can consolidate all user_data's to be the one */
void		mail_send_queue			(EMailBackend *backend,
						 CamelFolder *queue,
						 CamelTransport *transport,
						 const gchar *type,
						 GCancellable *cancellable,
						 CamelFilterGetFolderFunc get_folder,
						 gpointer get_data,
						 CamelFilterStatusFunc *status,
						 gpointer status_data,
						 void (*done)(gpointer data),
						 gpointer data);

void		mail_fetch_mail			(CamelStore *store,
						 gint keep,
						 const gchar *type,
						 GCancellable *cancellable,
						 CamelFilterGetFolderFunc get_folder,
						 gpointer get_data,
						 CamelFilterStatusFunc *status,
						 gpointer status_data,
						 void (*done)(gpointer data),
						 gpointer data);

void		mail_filter_folder		(EMailSession *session,
						 CamelFolder *source_folder,
						 GPtrArray *uids,
						 const gchar *type,
						 gboolean notify);

/* filter driver execute shell command async callback */
void mail_execute_shell_command (CamelFilterDriver *driver, gint argc, gchar **argv, gpointer data);

gint mail_disconnect_store (CamelStore *store);

G_END_DECLS

#endif /* MAIL_OPS_H */

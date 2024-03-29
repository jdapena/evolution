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
 *		Michael Zucchi <NotZed@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef MAIL_SEND_RECV_H
#define MAIL_SEND_RECV_H

#include <gtk/gtk.h>
#include <camel/camel.h>
#include <mail/e-mail-backend.h>
#include <libedataserver/e-account.h>

G_BEGIN_DECLS

/* send/receive all uri's */
GtkWidget *	mail_send_receive		(GtkWindow *parent,
						 EMailBackend *backend);

GtkWidget *	mail_receive			(GtkWindow *parent,
						 EMailBackend *backend);

/* receive a single account */
void		mail_receive_account		(EMailBackend *backend,
						 EAccount *account);

void		mail_send			(EMailBackend *backend);

/* setup auto receive stuff */
void		mail_autoreceive_init		(EMailBackend *backend);

G_END_DECLS

#endif /* MAIL_SEND_RECV_H */

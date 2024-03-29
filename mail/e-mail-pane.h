/*
 * e-mail-pane.h
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
 *		Srinivasa Ragavan <sragavan@gnome.org>
 *
 * Copyright (C) 2010 Intel corporation. (www.intel.com)
 *
 */

#ifndef E_MAIL_PANE_H
#define E_MAIL_PANE_H

#include <gtk/gtk.h>

#define E_TYPE_MAIL_PANE \
	(e_mail_pane_get_type ())
#define E_MAIL_PANE(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_MAIL_PANE, MailFolderView))
#define E_MAIL_PANE_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_MAIL_PANE, MailFolderViewClass))
#define E_IS_MAIL_PANE(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_MAIL_PANE))
#define E_IS_MAIL_PANE_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_MAIL_PANE))
#define E_MAIL_PANE_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_MAIL_PANE, EMailPaneClass))

G_BEGIN_DECLS

typedef struct _EMailPane EMailPane;
typedef struct _EMailPaneClass EMailPaneClass;
typedef struct _EMailPanePrivate EMailPanePrivate;

struct _EMailPane {
	GtkVBox parent;
	EMailPanePrivate *priv;
};

struct _EMailPaneClass {
	GtkVBoxClass parent_class;
};

G_END_DECLS

#endif /* E_MAIL_PANE_H */

/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* msg-composer-hdrs.h
 *
 * Copyright (C) 1999 Helix Code, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Ettore Perazzoli
 */

#ifndef ___E_MSG_COMPOSER_HDRS_H__
#define ___E_MSG_COMPOSER_HDRS_H__

#include <gnome.h>
#include <camel/camel-mime-message.h>
#include <mail/mail-config.h>

#ifdef __cplusplus
extern "C" {
#pragma }
#endif /* __cplusplus */

#define E_TYPE_MSG_COMPOSER_HDRS		(e_msg_composer_hdrs_get_type ())
#define E_MSG_COMPOSER_HDRS(obj)		(GTK_CHECK_CAST ((obj), E_TYPE_MSG_COMPOSER_HDRS, EMsgComposerHdrs))
#define E_MSG_COMPOSER_HDRS_CLASS(klass)	(GTK_CHECK_CLASS_CAST ((klass), E_TYPE_MSG_COMPOSER_HDRS, EMsgComposerHdrsClass))
#define E_IS_MSG_COMPOSER_HDRS(obj)		(GTK_CHECK_TYPE ((obj), E_TYPE_MSG_COMPOSER_HDRS))
#define E_IS_MSG_COMPOSER_HDRS_CLASS(klass)	(GTK_CHECK_CLASS_TYPE ((obj), E_TYPE_MSG_COMPOSER_HDRS))


typedef struct _EMsgComposerHdrs        EMsgComposerHdrs;
typedef struct _EMsgComposerHdrsClass   EMsgComposerHdrsClass;
typedef struct _EMsgComposerHdrsPrivate EMsgComposerHdrsPrivate;

struct _EMsgComposerHdrs {
	GtkTable parent;

	EMsgComposerHdrsPrivate *priv;
	
	const MailConfigAccount *account;
	
	gboolean has_changed;
};

struct _EMsgComposerHdrsClass {
	GtkTableClass parent_class;

	void (* show_address_dialog) (EMsgComposerHdrs *hdrs);
};


GtkType     e_msg_composer_hdrs_get_type           (void);
GtkWidget  *e_msg_composer_hdrs_new                (void);

void        e_msg_composer_hdrs_to_message         (EMsgComposerHdrs *hdrs,
						    CamelMimeMessage *msg);

void        e_msg_composer_hdrs_set_from_account   (EMsgComposerHdrs *hdrs,
						    const char *account_name);
void        e_msg_composer_hdrs_set_to             (EMsgComposerHdrs *hdrs,
						    const GList      *to_list);
void        e_msg_composer_hdrs_set_cc             (EMsgComposerHdrs *hdrs,
						    const GList      *cc_list);
void        e_msg_composer_hdrs_set_bcc            (EMsgComposerHdrs *hdrs,
						    const GList      *bcc_list);
void        e_msg_composer_hdrs_set_subject        (EMsgComposerHdrs *hdrs,
						    const char       *subject);

char       *e_msg_composer_hdrs_get_from           (EMsgComposerHdrs *hdrs);
GList      *e_msg_composer_hdrs_get_to             (EMsgComposerHdrs *hdrs);
GList      *e_msg_composer_hdrs_get_cc             (EMsgComposerHdrs *hdrs);
GList      *e_msg_composer_hdrs_get_bcc            (EMsgComposerHdrs *hdrs);
char       *e_msg_composer_hdrs_get_subject        (EMsgComposerHdrs *hdrs);

GtkWidget  *e_msg_composer_hdrs_get_to_entry       (EMsgComposerHdrs *hdrs);
GtkWidget  *e_msg_composer_hdrs_get_cc_entry       (EMsgComposerHdrs *hdrs);
GtkWidget  *e_msg_composer_hdrs_get_bcc_entry      (EMsgComposerHdrs *hdrs);
GtkWidget  *e_msg_composer_hdrs_get_subject_entry  (EMsgComposerHdrs *hdrs);

#ifdef _cplusplus
}
#endif /* _cplusplus */


#endif /* __E_MSG_COMPOSER_HDRS_H__ */

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
 *		Michael Zucchi <notzed@ximian.com>
 *		Ettore Perazzoli <ettore@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef MAIL_AUTOFILTER_H
#define MAIL_AUTOFILTER_H

#include <camel/camel.h>

#include <filter/e-filter-rule.h>
#include <mail/e-mail-backend.h>
#include <mail/em-filter-context.h>
#include <mail/em-vfolder-context.h>

enum {
	AUTO_SUBJECT = 1,
	AUTO_FROM = 2,
	AUTO_TO = 4,
	AUTO_MLIST = 8
};

EFilterRule *	em_vfolder_rule_from_message	(EMVFolderContext *context,
						 CamelMimeMessage *msg,
						 gint flags,
						 CamelFolder *folder);
EFilterRule *	filter_rule_from_message	(EMFilterContext *context,
						 CamelMimeMessage *msg,
						 gint flags);
EFilterRule *	em_vfolder_rule_from_address	(EMVFolderContext *context,
						 CamelInternetAddress *addr,
						 gint flags,
						 CamelFolder *folder);

/* easiest place to put this */
void		filter_gui_add_from_message	(EMailBackend *backend,
						 CamelMimeMessage *msg,
						 const gchar *source,
						 gint flags);

/* Also easiest place for these, we should really
 * share a global rule context for this stuff ... */
void		mail_filter_rename_folder	(EMailBackend *backend,
						 CamelStore *store,
						 const gchar *old_folder_name,
						 const gchar *new_folder_name);
void		mail_filter_delete_folder	(EMailBackend *backend,
						 CamelStore *store,
						 const gchar *folder_name);

#endif /* MAIL_AUTOFILTER_H */

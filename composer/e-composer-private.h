/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

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
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 */

#ifndef E_COMPOSER_PRIVATE_H
#define E_COMPOSER_PRIVATE_H

#include "e-msg-composer.h"

#include <errno.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include <gconf/gconf.h>
#include <gconf/gconf-client.h>

#include <libebackend/e-extensible.h>

#include "e-composer-actions.h"
#include "e-composer-activity.h"
#include "e-composer-header-table.h"
#include "e-util/e-alert-sink.h"
#include "e-util/e-charset.h"
#include "e-util/e-marshal.h"
#include "e-util/e-mktemp.h"
#include "e-util/e-plugin-ui.h"
#include "e-util/e-selection.h"
#include "e-util/e-util.h"
#include "e-util/gconf-bridge.h"
#include "widgets/misc/e-activity-bar.h"
#include "widgets/misc/e-alert-bar.h"
#include "widgets/misc/e-attachment.h"
#include "widgets/misc/e-attachment-icon-view.h"
#include "widgets/misc/e-attachment-paned.h"
#include "widgets/misc/e-attachment-store.h"
#include "widgets/misc/e-picture-gallery.h"
#include "widgets/misc/e-preferences-window.h"
#include "widgets/misc/e-signature-combo-box.h"
#include "widgets/misc/e-web-view-gtkhtml.h"
#include "shell/e-shell.h"

#ifdef HAVE_XFREE
#include <X11/XF86keysym.h>
#endif

/* Mail configuration keys */
#define MAIL_GCONF_PREFIX \
	"/apps/evolution/mail"
#define MAIL_GCONF_CHARSET_KEY \
	MAIL_GCONF_PREFIX "/format/charset"

/* Composer configuration keys */
#define COMPOSER_GCONF_PREFIX \
	MAIL_GCONF_PREFIX "/composer"
#define COMPOSER_GCONF_CHARSET_KEY \
	COMPOSER_GCONF_PREFIX "/charset"
#define COMPOSER_GCONF_INLINE_SPELLING_KEY \
	COMPOSER_GCONF_PREFIX "/inline_spelling"
#define COMPOSER_GCONF_MAGIC_LINKS_KEY \
	COMPOSER_GCONF_PREFIX "/magic_links"
#define COMPOSER_GCONF_MAGIC_SMILEYS_KEY \
	COMPOSER_GCONF_PREFIX "/magic_smileys"
#define COMPOSER_GCONF_REQUEST_RECEIPT_KEY \
	COMPOSER_GCONF_PREFIX "/request_receipt"
#define COMPOSER_GCONF_TOP_SIGNATURE_KEY \
	COMPOSER_GCONF_PREFIX "/top_signature"
#define COMPOSER_GCONF_SEND_HTML_KEY \
	COMPOSER_GCONF_PREFIX "/send_html"
#define COMPOSER_GCONF_SPELL_LANGUAGES_KEY \
	COMPOSER_GCONF_PREFIX "/spell_languages"
#define COMPOSER_GCONF_WINDOW_PREFIX \
	COMPOSER_GCONF_PREFIX "/window"

/* Shorthand, requires a variable named "composer". */
#define ACTION(name)	(E_COMPOSER_ACTION_##name (composer))

/* For use in dispose() methods. */
#define DISPOSE(obj) \
	G_STMT_START { \
	if ((obj) != NULL) { g_object_unref (obj); (obj) = NULL; } \
	} G_STMT_END

G_BEGIN_DECLS

struct _EMsgComposerPrivate {

	gpointer shell;  /* weak pointer */

	/*** UI Management ***/

	GtkWidget *header_table;
	GtkWidget *activity_bar;
	GtkWidget *alert_bar;
	GtkWidget *attachment_paned;

	EFocusTracker *focus_tracker;
	GtkWindowGroup *window_group;

	GtkActionGroup *async_actions;
	GtkActionGroup *charset_actions;
	GtkActionGroup *composer_actions;

	GPtrArray *extra_hdr_names;
	GPtrArray *extra_hdr_values;
	GArray *gconf_bridge_binding_ids;

	GtkWidget *focused_entry;

	GtkWidget *gallery_icon_view;
	GtkWidget *gallery_scrolled_window;

	GtkWidget *address_dialog;

	GHashTable *inline_images;
	GHashTable *inline_images_by_url;
	GList *current_images;

	gchar *mime_type;
	gchar *mime_body;
	gchar *charset;

	guint32 autosaved              : 1;
	guint32 mode_post              : 1;
	guint32 in_signature_insert    : 1;
	guint32 application_exiting    : 1;

	CamelMimeMessage *redirect;

	gboolean is_from_message;
};

void		e_composer_private_constructed	(EMsgComposer *composer);
void		e_composer_private_dispose	(EMsgComposer *composer);
void		e_composer_private_finalize	(EMsgComposer *composer);

/* Private Utilities */

void		e_composer_actions_init		(EMsgComposer *composer);
gchar *		e_composer_find_data_file	(const gchar *basename);
gchar *		e_composer_get_default_charset	(void);
gboolean	e_composer_paste_html		(EMsgComposer *composer,
						 GtkClipboard *clipboard);
gboolean	e_composer_paste_image		(EMsgComposer *composer,
						 GtkClipboard *clipboard);
gboolean	e_composer_paste_text		(EMsgComposer *composer,
						 GtkClipboard *clipboard);
gboolean	e_composer_paste_uris		(EMsgComposer *composer,
						 GtkClipboard *clipboard);
gboolean	e_composer_selection_is_image_uris
						(EMsgComposer *composer,
						 GtkSelectionData *selection);

G_END_DECLS

#endif /* E_COMPOSER_PRIVATE_H */

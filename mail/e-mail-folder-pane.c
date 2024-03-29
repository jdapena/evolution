/*
 * e-mail-folder-pane.c
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
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "e-mail-folder-pane.h"

#include <string.h>
#include <glib/gi18n.h>

#include "e-util/e-util.h"
#include "e-util/e-plugin-ui.h"
#include "e-util/gconf-bridge.h"
#include "shell/e-shell.h"
#include "shell/e-shell-utils.h"
#include "widgets/misc/e-popup-action.h"
#include "widgets/misc/e-preview-pane.h"

#include "mail/e-mail-reader.h"
#include "mail/e-mail-reader-utils.h"
#include "mail/em-folder-tree-model.h"
#include "mail/em-format-html-display.h"
#include "mail/em-composer-utils.h"
#include "mail/em-utils.h"
#include "mail/mail-tools.h"
#include "mail/message-list.h"

struct _EMailFolderPanePrivate {
	gint placeholder;
};

G_DEFINE_TYPE (EMailFolderPane, e_mail_folder_pane, E_TYPE_MAIL_PANED_VIEW)

static gboolean
folder_pane_get_preview_visible (EMailView *view)
{
	return FALSE;
}

static void
folder_pane_set_preview_visible (EMailView *view,
                                 gboolean preview_visible)
{
	/* Ignore the request. */
}

static guint
mail_paned_view_open_selected_mail (EMailPanedView *view)
{
	EMailReader *reader;
	GPtrArray *uids;
	gint i;
	GtkWindow *window;
	CamelFolder *folder;
	GPtrArray *views;
	guint n_views, ii;

	reader = E_MAIL_READER (view);
	folder = e_mail_reader_get_folder (reader);
	window = e_mail_reader_get_window (reader);
	uids = e_mail_reader_get_selected_uids (reader);
	g_return_val_if_fail (uids != NULL, 0);

	/* XXX Either e_mail_reader_get_selected_uids()
	 *     or MessageList should do this itself. */
	g_ptr_array_set_free_func (uids, (GDestroyNotify) g_free);

	if (!em_utils_ask_open_many (window, uids->len)) {
		g_ptr_array_unref (uids);
		return 0;
	}

	if (em_utils_folder_is_drafts (folder) ||
		em_utils_folder_is_outbox (folder) ||
		em_utils_folder_is_templates (folder)) {
		em_utils_edit_messages (reader, folder, uids, TRUE);
		g_ptr_array_unref (uids);
		return 0;
	}

	views = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);

	/* For vfolders we need to edit the original, not the vfolder copy. */
	for (ii = 0; ii < uids->len; ii++) {
		const gchar *uid = uids->pdata[ii];
		CamelFolder *real_folder;
		CamelMessageInfo *info;
		gchar *real_uid;

		if (!CAMEL_IS_VEE_FOLDER (folder)) {
			g_ptr_array_add (views, g_strdup (uid));
			continue;
		}

		info = camel_folder_get_message_info (folder, uid);
		if (info == NULL)
			continue;

		real_folder = camel_vee_folder_get_location (
			CAMEL_VEE_FOLDER (folder),
			(CamelVeeMessageInfo *) info, &real_uid);

		if (em_utils_folder_is_drafts (real_folder) ||
			em_utils_folder_is_outbox (real_folder)) {
			GPtrArray *edits;

			edits = g_ptr_array_new_with_free_func (
				(GDestroyNotify) g_free);
			g_ptr_array_add (edits, real_uid);
			em_utils_edit_messages (
				reader, real_folder, edits, TRUE);
			g_ptr_array_unref (edits);
		} else {
			g_free (real_uid);
			g_ptr_array_add (views, g_strdup (uid));
		}

		camel_folder_free_message_info (folder, info);
	}

	n_views = views->len;

	for (i = 0; i < n_views; i++)
		g_signal_emit_by_name (view, "open-mail", views->pdata[i]);

	g_ptr_array_unref (views);
	g_ptr_array_unref (uids);

	return n_views;
}

static void
e_mail_folder_pane_class_init (EMailFolderPaneClass *class)
{
	EMailViewClass *mail_view_class;
	EMailPanedViewClass *mail_paned_view_class;

	g_type_class_add_private (class, sizeof (EMailFolderPanePrivate));

	mail_view_class = E_MAIL_VIEW_CLASS (class);
	mail_view_class->get_preview_visible = folder_pane_get_preview_visible;
	mail_view_class->set_preview_visible = folder_pane_set_preview_visible;

	mail_paned_view_class = E_MAIL_PANED_VIEW_CLASS (class);
	mail_paned_view_class->open_selected_mail = mail_paned_view_open_selected_mail;
}

static void
e_mail_folder_pane_init (EMailFolderPane *browser)
{
	browser->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		browser, E_TYPE_MAIL_FOLDER_PANE, EMailFolderPanePrivate);
}

EMailView *
e_mail_folder_pane_new (EShellView *shell_view)
{
	g_return_val_if_fail (E_IS_SHELL_VIEW (shell_view), NULL);

	return g_object_new (
		E_TYPE_MAIL_FOLDER_PANE,
		"shell-view", shell_view, NULL);
}


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
 *		Jeffrey Stedfast <fejj@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <libxml/tree.h>

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gi18n.h>

#include "e-util/e-mktemp.h"

#include "e-util/e-alert-dialog.h"

#include "em-vfolder-rule.h"

#include "mail-mt.h"
#include "mail-ops.h"
#include "mail-tools.h"
#include "mail-vfolder.h"
#include "mail-folder-cache.h"

#include "em-utils.h"
#include "em-folder-tree.h"
#include "em-folder-tree-model.h"
#include "em-folder-utils.h"
#include "em-folder-selector.h"
#include "em-folder-properties.h"

#include "e-mail-folder-utils.h"
#include "e-mail-local.h"
#include "e-mail-session.h"
#include "e-mail-store.h"
#include "e-mail-store-utils.h"

#define d(x)

typedef struct _AsyncContext AsyncContext;

struct _AsyncContext {
	EMFolderTree *folder_tree;
	gchar *folder_uri;
};

static void
async_context_free (AsyncContext *context)
{
	if (context->folder_tree != NULL)
		g_object_unref (context->folder_tree);

	g_free (context->folder_uri);

	g_slice_free (AsyncContext, context);
}

static gboolean
emfu_is_special_local_folder (const gchar *name)
{
	return (!strcmp (name, "Drafts") ||
		!strcmp (name, "Inbox") ||
		!strcmp (name, "Outbox") ||
		!strcmp (name, "Sent") ||
		!strcmp (name, "Templates"));
}

struct _EMCopyFolders {
	MailMsg base;

	/* input data */
	CamelStore *fromstore;
	CamelStore *tostore;

	gchar *frombase;
	gchar *tobase;

	gint delete;
};

static gchar *
emft_copy_folders__desc (struct _EMCopyFolders *m,
                         gint complete)
{
	if (m->delete)
		return g_strdup_printf (_("Moving folder %s"), m->frombase);
	else
		return g_strdup_printf (_("Copying folder %s"), m->frombase);
}

static void
emft_copy_folders__exec (struct _EMCopyFolders *m,
                         GCancellable *cancellable,
                         GError **error)
{
	guint32 flags;
	GList *pending = NULL, *deleting = NULL, *l;
	GString *fromname, *toname;
	CamelFolderInfo *fi;
	const gchar *tmp;
	gint fromlen;

	flags = CAMEL_STORE_FOLDER_INFO_FAST |
		CAMEL_STORE_FOLDER_INFO_SUBSCRIBED;

	/* If we're copying, then we need to copy every subfolder. If we're
	 * *moving*, though, then we only need to rename the top-level folder */
	if (!m->delete)
		flags |= CAMEL_STORE_FOLDER_INFO_RECURSIVE;

	fi = camel_store_get_folder_info_sync (
		m->fromstore, m->frombase, flags, cancellable, error);
	if (fi == NULL)
		return;

	pending = g_list_append (pending, fi);

	toname = g_string_new ("");
	fromname = g_string_new ("");

	tmp = strrchr (m->frombase, '/');
	if (tmp == NULL)
		fromlen = 0;
	else
		fromlen = tmp - m->frombase + 1;

	d(printf ("top name is '%s'\n", fi->full_name));

	while (pending) {
		CamelFolderInfo *info = pending->data;

		pending = g_list_remove_link (pending, pending);
		while (info) {
			CamelFolder *fromfolder, *tofolder;
			GPtrArray *uids;
			gint deleted = 0;

			/* We still get immediate children even without the
			 * CAMEL_STORE_FOLDER_INFO_RECURSIVE flag. But we only
			 * want to process the children too if we're *copying * */
			if (info->child && !m->delete)
				pending = g_list_append (pending, info->child);

			if (m->tobase[0])
				g_string_printf (
					toname, "%s/%s", m->tobase,
					info->full_name + fromlen);
			else
				g_string_printf (
					toname, "%s",
					info->full_name + fromlen);

			d(printf ("Copying from '%s' to '%s'\n", info->full_name, toname->str));

			/* This makes sure we create the same tree,
			 * e.g. from a nonselectable source. */
			/* Not sure if this is really the 'right thing',
			 * e.g. for spool stores, but it makes the ui work. */
			if ((info->flags & CAMEL_FOLDER_NOSELECT) == 0) {
				d(printf ("this folder is selectable\n"));
				if (m->tostore == m->fromstore && m->delete) {
					camel_store_rename_folder_sync (
						m->fromstore, info->full_name, toname->str,
						cancellable, error);
					if (error && *error)
						goto exception;

					/* this folder no longer exists, unsubscribe it */
					if (CAMEL_IS_SUBSCRIBABLE (m->fromstore))
						camel_subscribable_unsubscribe_folder_sync (
							CAMEL_SUBSCRIBABLE (m->fromstore),
							info->full_name, NULL, NULL);

					deleted = 1;
				} else {
					fromfolder = camel_store_get_folder_sync (
						m->fromstore, info->full_name, 0,
						cancellable, error);
					if (fromfolder == NULL)
						goto exception;

					tofolder = camel_store_get_folder_sync (
						m->tostore, toname->str,
						CAMEL_STORE_FOLDER_CREATE,
						cancellable, error);
					if (tofolder == NULL) {
						g_object_unref (fromfolder);
						goto exception;
					}

					uids = camel_folder_get_uids (fromfolder);
					camel_folder_transfer_messages_to_sync (
						fromfolder, uids, tofolder,
						m->delete, NULL,
						cancellable, error);
					camel_folder_free_uids (fromfolder, uids);

					if (m->delete && (!error || !*error))
						camel_folder_synchronize_sync (
							fromfolder, TRUE,
							NULL, NULL);

					g_object_unref (fromfolder);
					g_object_unref (tofolder);
				}
			}

			if (error && *error)
				goto exception;
			else if (m->delete && !deleted)
				deleting = g_list_prepend (deleting, info);

			/* subscribe to the new folder if appropriate */
			if (CAMEL_IS_SUBSCRIBABLE (m->tostore)
			    && !camel_subscribable_folder_is_subscribed (
					CAMEL_SUBSCRIBABLE (m->tostore),
					toname->str))
				camel_subscribable_subscribe_folder_sync (
					CAMEL_SUBSCRIBABLE (m->tostore),
					toname->str, NULL, NULL);

			info = info->next;
		}
	}

	/* Delete the folders in reverse order from how we copied them,
	 * if we are deleting any. */
	l = deleting;
	while (l) {
		CamelFolderInfo *info = l->data;

		d(printf ("deleting folder '%s'\n", info->full_name));

		/* FIXME: we need to do something with the exception
		 * since otherwise the users sees a failed operation
		 * with no error message or even any warnings */
		if (CAMEL_IS_SUBSCRIBABLE (m->fromstore))
			camel_subscribable_unsubscribe_folder_sync (
				CAMEL_SUBSCRIBABLE (m->fromstore),
				info->full_name, NULL, NULL);

		camel_store_delete_folder_sync (
			m->fromstore, info->full_name, NULL, NULL);
		l = l->next;
	}

 exception:

	camel_store_free_folder_info (m->fromstore, fi);
	g_list_free (deleting);

	g_string_free (toname, TRUE);
	g_string_free (fromname, TRUE);
}

static void
emft_copy_folders__free (struct _EMCopyFolders *m)
{
	g_object_unref (m->fromstore);
	g_object_unref (m->tostore);

	g_free (m->frombase);
	g_free (m->tobase);
}

static MailMsgInfo copy_folders_info = {
	sizeof (struct _EMCopyFolders),
	(MailMsgDescFunc) emft_copy_folders__desc,
	(MailMsgExecFunc) emft_copy_folders__exec,
	(MailMsgDoneFunc) NULL,
	(MailMsgFreeFunc) emft_copy_folders__free
};

gint
em_folder_utils_copy_folders (CamelStore *fromstore,
                              const gchar *frombase,
                              CamelStore *tostore,
                              const gchar *tobase,
                              gint delete)
{
	struct _EMCopyFolders *m;
	gint seq;

	m = mail_msg_new (&copy_folders_info);
	g_object_ref (fromstore);
	m->fromstore = fromstore;
	g_object_ref (tostore);
	m->tostore = tostore;
	m->frombase = g_strdup (frombase);
	m->tobase = g_strdup (tobase);
	m->delete = delete;
	seq = m->base.seq;

	mail_msg_unordered_push (m);

	return seq;
}

struct _copy_folder_data {
	CamelStore *source_store;
	gchar *source_folder_name;
	gboolean delete;
};

static void
emfu_copy_folder_selected (EMailBackend *backend,
                           const gchar *uri,
                           gpointer data)
{
	EMailSession *session;
	struct _copy_folder_data *cfd = data;
	CamelStore *tostore = NULL;
	CamelStore *local_store;
	CamelService *service;
	gchar *tobase = NULL;
	GError *local_error = NULL;

	if (uri == NULL)
		goto fail;

	local_store = e_mail_local_get_store ();
	session = e_mail_backend_get_session (backend);

	service = CAMEL_SERVICE (cfd->source_store);
	em_utils_connect_service_sync (service, NULL, &local_error);

	if (local_error != NULL) {
		e_mail_backend_submit_alert (
			backend, cfd->delete ?
				"mail:no-move-folder-notexist" :
				"mail:no-copy-folder-notexist",
			cfd->source_folder_name, uri,
			local_error->message, NULL);
		goto fail;
	}

	g_return_if_fail (CAMEL_IS_STORE (service));

	if (cfd->delete && cfd->source_store == local_store &&
		emfu_is_special_local_folder (cfd->source_folder_name)) {
		e_mail_backend_submit_alert (
			backend, "mail:no-rename-special-folder",
			cfd->source_folder_name, NULL);
		goto fail;
	}

	if (!e_mail_folder_uri_parse (
		CAMEL_SESSION (session), uri,
		&tostore, &tobase, &local_error))
		tostore = NULL;

	if (tostore != NULL)
		em_utils_connect_service_sync (
			CAMEL_SERVICE (tostore), NULL, &local_error);

	if (local_error != NULL) {
		e_mail_backend_submit_alert (
			backend, cfd->delete ?
				"mail:no-move-folder-to-notexist" :
				"mail:no-copy-folder-to-notexist",
			cfd->source_folder_name, uri,
			local_error->message, NULL);
		goto fail;
	}

	g_return_if_fail (CAMEL_IS_STORE (tostore));

	em_folder_utils_copy_folders (
		cfd->source_store, cfd->source_folder_name,
		tostore, tobase ? tobase : "", cfd->delete);

fail:
	g_clear_error (&local_error);

	g_object_unref (cfd->source_store);
	g_free (cfd->source_folder_name);
	g_free (cfd);

	if (tostore)
		g_object_unref (tostore);
	g_free (tobase);
}

/* tree here is the 'destination' selector, not 'self' */
static gboolean
emfu_copy_folder_exclude (EMFolderTree *tree,
                          GtkTreeModel *model,
                          GtkTreeIter *iter,
                          gpointer data)
{
	struct _copy_folder_data *cfd = data;
	CamelStore *store;
	const gchar *uid;
	gint fromvfolder, tovfolder;
	guint flags;

	/* handles moving to/from vfolders */

	uid = camel_service_get_uid (CAMEL_SERVICE (cfd->source_store));
	fromvfolder = (g_strcmp0 (uid, "vfolder") == 0);

	gtk_tree_model_get (
		model, iter,
		COL_UINT_FLAGS, &flags,
		COL_POINTER_CAMEL_STORE, &store, -1);

	uid = camel_service_get_uid (CAMEL_SERVICE (store));
	tovfolder = (g_strcmp0 (uid, "vfolder") == 0);

	/* moving from vfolder to normal- not allowed */
	if (fromvfolder && !tovfolder && cfd->delete)
		return FALSE;
	/* copy/move from normal folder to vfolder - not allowed */
	if (!fromvfolder && tovfolder)
		return FALSE;
	/* copying to vfolder - not allowed */
	if (tovfolder && !cfd->delete)
		return FALSE;

	return (flags & EMFT_EXCLUDE_NOINFERIORS) == 0;
}

void
em_folder_utils_copy_folder (GtkWindow *parent,
                             EMailBackend *backend,
                             const gchar *folder_uri,
                             gint delete)
{
	GtkWidget *dialog;
	EMFolderSelector *selector;
	EMFolderTree *folder_tree;
	EMFolderTreeModel *model;
	EMailSession *session;
	const gchar *label;
	const gchar *title;
	struct _copy_folder_data *cfd;
	GError *error = NULL;

	g_return_if_fail (E_IS_MAIL_BACKEND (backend));
	g_return_if_fail (folder_uri != NULL);

	session = e_mail_backend_get_session (backend);

	cfd = g_malloc (sizeof (*cfd));
	cfd->delete = delete;

	e_mail_folder_uri_parse (
		CAMEL_SESSION (session), folder_uri,
		&cfd->source_store, &cfd->source_folder_name, &error);

	if (error != NULL) {
		g_warning ("%s", error->message);
		g_error_free (error);
		g_free (cfd);
		return;
	}

	label = delete ? _("_Move") : _("C_opy");
	title = delete ? _("Move Folder To") : _("Copy Folder To");

	model = em_folder_tree_model_get_default ();

	dialog = em_folder_selector_new (
		parent, backend, model,
		EM_FOLDER_SELECTOR_CAN_CREATE,
		title, NULL, label);

	selector = EM_FOLDER_SELECTOR (dialog);
	folder_tree = em_folder_selector_get_folder_tree (selector);

	em_folder_tree_set_excluded_func (
		folder_tree, emfu_copy_folder_exclude, cfd);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		const gchar *uri;

		uri = em_folder_selector_get_selected_uri (selector);
		emfu_copy_folder_selected (backend, uri, cfd);
	}

	gtk_widget_destroy (dialog);
}

static void
new_folder_created_cb (CamelStore *store,
                       GAsyncResult *result,
                       AsyncContext *context)
{
	GError *error = NULL;

	e_mail_store_create_folder_finish (store, result, &error);

	/* FIXME Use an EActivity here. */
	if (error != NULL) {
		g_warning ("%s", error->message);
		g_error_free (error);

	} else if (context->folder_tree != NULL) {
		gpointer data;
		gboolean expand_only;

		/* XXX What in the hell kind of lazy hack is this? */
		data = g_object_get_data (
			G_OBJECT (context->folder_tree), "select");
		expand_only = GPOINTER_TO_INT (data) ? TRUE : FALSE;

		em_folder_tree_set_selected (
			context->folder_tree,
			context->folder_uri, expand_only);
	}

	async_context_free (context);
}

void
em_folder_utils_create_folder (GtkWindow *parent,
                               EMailBackend *backend,
                               EMFolderTree *emft,
                               const gchar *initial_uri)
{
	EShell *shell;
	EShellSettings *shell_settings;
	EMailSession *session;
	EMFolderSelector *selector;
	EMFolderTree *folder_tree;
	EMFolderTreeModel *model;
	CamelStore *store = NULL;
	gchar *folder_name = NULL;
	GtkWidget *dialog;
	GList *list, *link;
	GError *error = NULL;

	g_return_if_fail (GTK_IS_WINDOW (parent));
	g_return_if_fail (E_IS_MAIL_BACKEND (backend));

	shell = e_shell_backend_get_shell (E_SHELL_BACKEND (backend));
	shell_settings = e_shell_get_shell_settings (shell);

	model = em_folder_tree_model_new ();
	em_folder_tree_model_set_backend (model, backend);

	session = e_mail_backend_get_session (backend);
	list = camel_session_list_services (CAMEL_SESSION (session));

	for (link = list; link != NULL; link = g_list_next (link)) {
		CamelService *service;
		CamelStore *store;
		const gchar *uid, *prop = NULL;

		service = CAMEL_SERVICE (link->data);

		if (!CAMEL_IS_STORE (service))
			continue;

		store = CAMEL_STORE (service);

		if ((store->flags & CAMEL_STORE_CAN_EDIT_FOLDERS) == 0)
			continue;

		uid = camel_service_get_uid (service);
		if (g_strcmp0 (uid, "local") == 0)
			prop = "mail-enable-local-folders";
		else if (g_strcmp0 (uid, "vfolder") == 0)
			prop = "mail-enable-search-folders";

		if (prop && !e_shell_settings_get_boolean (shell_settings, prop))
			continue;

		em_folder_tree_model_add_store (model, store);
	}

	g_list_free (list);

	dialog = em_folder_selector_create_new (
		parent, backend, model, 0,
		_("Create Folder"),
		_("Specify where to create the folder:"));

	g_object_unref (model);

	selector = EM_FOLDER_SELECTOR (dialog);
	folder_tree = em_folder_selector_get_folder_tree (selector);

	if (initial_uri != NULL)
		em_folder_tree_set_selected (folder_tree, initial_uri, FALSE);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) != GTK_RESPONSE_OK)
		goto exit;

	if (em_folder_tree_store_root_selected (folder_tree, &store)) {
		const gchar *folder_uri;

		folder_uri = em_folder_selector_get_selected_uri (selector);

		if (!folder_uri || !strrchr (folder_uri, '/'))
			g_set_error (
				&error, CAMEL_FOLDER_ERROR,
				CAMEL_FOLDER_ERROR_INVALID,
				_("Invalid folder URI '%s'"),
				folder_uri ? folder_uri : "null");
		else
			folder_name = g_strdup (strrchr (folder_uri, '/'));
	} else {
		const gchar *folder_uri;

		folder_uri = em_folder_selector_get_selected_uri (selector);

		e_mail_folder_uri_parse (
			CAMEL_SESSION (session), folder_uri,
			&store, &folder_name, &error);
	}

	/* XXX This is unlikely to fail since the URI comes straight from
	 *     EMFolderSelector, but leave a breadcrumb if it does fail. */
	if (error != NULL) {
		g_warn_if_fail (store == NULL);
		g_warn_if_fail (folder_name == NULL);
		g_warning ("%s", error->message);
		g_error_free (error);
		goto exit;
	}

	/* HACK: we need to create vfolders using the vfolder editor */
	if (CAMEL_IS_VEE_STORE (store)) {
		EFilterRule *rule;
		const gchar *skip_slash;

		if (*folder_name == '/')
			skip_slash = folder_name + 1;
		else
			skip_slash = folder_name;

		rule = em_vfolder_rule_new (backend);
		e_filter_rule_set_name (rule, skip_slash);
		vfolder_gui_add_rule (EM_VFOLDER_RULE (rule));
	} else {
		AsyncContext *context;

		context = g_slice_new0 (AsyncContext);
		context->folder_uri = e_mail_folder_uri_build (store, folder_name);

		if (EM_IS_FOLDER_TREE (emft))
			context->folder_tree = g_object_ref (emft);

		/* FIXME Not passing a GCancellable. */
		e_mail_store_create_folder (
			store, folder_name, G_PRIORITY_DEFAULT, NULL,
			(GAsyncReadyCallback) new_folder_created_cb,
			context);
	}

	g_free (folder_name);
	g_object_unref (store);

exit:
	gtk_widget_destroy (dialog);
}

const gchar *
em_folder_utils_get_icon_name (guint32 flags)
{
	const gchar *icon_name;

	switch (flags & CAMEL_FOLDER_TYPE_MASK) {
		case CAMEL_FOLDER_TYPE_INBOX:
			icon_name = "mail-inbox";
			break;
		case CAMEL_FOLDER_TYPE_OUTBOX:
			icon_name = "mail-outbox";
			break;
		case CAMEL_FOLDER_TYPE_TRASH:
			icon_name = "user-trash";
			break;
		case CAMEL_FOLDER_TYPE_JUNK:
			icon_name = "mail-mark-junk";
			break;
		case CAMEL_FOLDER_TYPE_SENT:
			icon_name = "mail-sent";
			break;
		case CAMEL_FOLDER_TYPE_CONTACTS:
			icon_name = "x-office-address-book";
			break;
		case CAMEL_FOLDER_TYPE_EVENTS:
			icon_name = "x-office-calendar";
			break;
		case CAMEL_FOLDER_TYPE_MEMOS:
			icon_name = "evolution-memos";
			break;
		case CAMEL_FOLDER_TYPE_TASKS:
			icon_name = "evolution-tasks";
			break;
		default:
			if (flags & CAMEL_FOLDER_SHARED_TO_ME)
				icon_name = "stock_shared-to-me";
			else if (flags & CAMEL_FOLDER_SHARED_BY_ME)
				icon_name = "stock_shared-by-me";
			else if (flags & CAMEL_FOLDER_VIRTUAL)
				icon_name = "folder-saved-search";
			else
				icon_name = "folder";
	}

	return icon_name;
}

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

#include <glib/gi18n.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>

#include "e-util/e-mktemp.h"
#include "e-util/e-icon-factory.h"
#include "e-util/e-alert-dialog.h"
#include "e-util/e-util.h"

#include "misc/e-selectable.h"

#include "em-vfolder-rule.h"

#include "mail-mt.h"
#include "mail-ops.h"
#include "mail-tools.h"
#include "mail-send-recv.h"
#include "mail-vfolder.h"

#include "em-utils.h"
#include "em-folder-tree.h"
#include "em-folder-utils.h"
#include "em-folder-selector.h"
#include "em-folder-properties.h"
#include "em-event.h"

#include "e-mail-folder-utils.h"
#include "e-mail-local.h"
#include "e-mail-session.h"
#include "e-mail-store.h"

#define d(x)

#define EM_FOLDER_TREE_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), EM_TYPE_FOLDER_TREE, EMFolderTreePrivate))

typedef struct _AsyncContext AsyncContext;

struct _selected_uri {
	gchar *key;		/* store:path or account/path */
	gchar *uri;
	CamelService *service;
	gchar *path;
};

struct _EMFolderTreePrivate {
	EMailBackend *backend;
	EAlertSink *alert_sink;

	/* selected_uri structures of each path pending selection. */
	GSList *select_uris;

	/* Removed as they're encountered, so use this
	 * to find URI's not presnet but selected. */
	GHashTable *select_uris_table;

	guint32 excluded;
	gboolean	(*excluded_func)	(EMFolderTree *folder_tree,
						 GtkTreeModel *model,
						 GtkTreeIter *iter,
						 gpointer data);
	gpointer excluded_data;

	guint cursor_set:1;	/* set to TRUE means we or something
				 * else has set the cursor, otherwise
				 * we need to set it when we set the
				 * selection */

	guint autoscroll_id;
	guint autoexpand_id;
	GtkTreeRowReference *autoexpand_row;

	guint loading_row_id;
	guint loaded_row_id;

	GtkTreeRowReference *drag_row;
	gboolean skip_double_click;

	GtkCellRenderer *text_renderer;
	PangoEllipsizeMode ellipsize;

	GtkWidget *selectable; /* an ESelectable, where to pass selectable calls */

	/* Signal handler IDs */
	gulong selection_changed_handler_id;
};

struct _AsyncContext {
	EActivity *activity;
	EMFolderTree *folder_tree;
	GtkTreeRowReference *root;
	gchar *full_name;
};

enum {
	PROP_0,
	PROP_ALERT_SINK,
	PROP_BACKEND,
	PROP_COPY_TARGET_LIST,
	PROP_ELLIPSIZE,
	PROP_MODEL,
	PROP_PASTE_TARGET_LIST
};

enum {
	FOLDER_ACTIVATED,  /* aka double-clicked or user hit enter */
	FOLDER_SELECTED,
	POPUP_EVENT,
	HIDDEN_KEY_EVENT,
	LAST_SIGNAL
};

/* Drag & Drop types */
enum DndDragType {
	DND_DRAG_TYPE_FOLDER,          /* drag an evo folder */
	DND_DRAG_TYPE_TEXT_URI_LIST,   /* drag to an mbox file */
	NUM_DRAG_TYPES
};

enum DndDropType {
	DND_DROP_TYPE_UID_LIST,        /* drop a list of message uids */
	DND_DROP_TYPE_FOLDER,          /* drop an evo folder */
	DND_DROP_TYPE_MESSAGE_RFC822,  /* drop a message/rfc822 stream */
	DND_DROP_TYPE_TEXT_URI_LIST,   /* drop an mbox file */
	NUM_DROP_TYPES
};

static GtkTargetEntry drag_types[] = {
	{ (gchar *) "x-folder",         0, DND_DRAG_TYPE_FOLDER         },
	{ (gchar *) "text/uri-list",    0, DND_DRAG_TYPE_TEXT_URI_LIST  },
};

static GtkTargetEntry drop_types[] = {
	{ (gchar *) "x-uid-list" ,      0, DND_DROP_TYPE_UID_LIST       },
	{ (gchar *) "x-folder",         0, DND_DROP_TYPE_FOLDER         },
	{ (gchar *) "message/rfc822",   0, DND_DROP_TYPE_MESSAGE_RFC822 },
	{ (gchar *) "text/uri-list",    0, DND_DROP_TYPE_TEXT_URI_LIST  },
};

static GdkAtom drag_atoms[NUM_DRAG_TYPES];
static GdkAtom drop_atoms[NUM_DROP_TYPES];

static guint signals[LAST_SIGNAL] = { 0 };

extern CamelStore *vfolder_store;

struct _folder_tree_selection_data {
	GtkTreeModel *model;
	GtkTreeIter *iter;
	gboolean set;
};

/* Forward Declarations */
static void em_folder_tree_selectable_init (ESelectableInterface *interface);

G_DEFINE_TYPE_WITH_CODE (
	EMFolderTree,
	em_folder_tree,
	GTK_TYPE_TREE_VIEW,
	G_IMPLEMENT_INTERFACE (
		E_TYPE_SELECTABLE,
		em_folder_tree_selectable_init))

static void
async_context_free (AsyncContext *context)
{
	if (context->activity != NULL)
		g_object_unref (context->activity);

	if (context->folder_tree != NULL)
		g_object_unref (context->folder_tree);

	gtk_tree_row_reference_free (context->root);

	g_free (context->full_name);

	g_slice_free (AsyncContext, context);
}

static void
folder_tree_get_folder_info_cb (CamelStore *store,
                                GAsyncResult *result,
                                AsyncContext *context)
{
	struct _EMFolderTreeModelStoreInfo *si;
	CamelFolderInfo *folder_info;
	CamelFolderInfo *child_info;
	EAlertSink *alert_sink;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkTreeIter root;
	GtkTreeIter iter;
	GtkTreeIter titer;
	gboolean is_store;
	gboolean iter_is_placeholder;
	gboolean valid;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	folder_info = camel_store_get_folder_info_finish (
		store, result, &error);

	tree_view = GTK_TREE_VIEW (context->folder_tree);
	model = gtk_tree_view_get_model (tree_view);

	/* Check if our parent folder has been deleted/unsubscribed. */
	if (!gtk_tree_row_reference_valid (context->root)) {
		g_clear_error (&error);
		goto exit;
	}

	path = gtk_tree_row_reference_get_path (context->root);
	valid = gtk_tree_model_get_iter (model, &root, path);
	g_return_if_fail (valid);

	gtk_tree_model_get (model, &root, COL_BOOL_IS_STORE, &is_store, -1);

	/* If we had an error, then we need to re-set the
	 * load subdirs state and collapse the node. */
	if (error != NULL) {
		gtk_tree_store_set (
			GTK_TREE_STORE (model), &root,
			COL_BOOL_LOAD_SUBDIRS, TRUE, -1);
		gtk_tree_view_collapse_row (tree_view, path);
	}

	gtk_tree_path_free (path);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (folder_info == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	/* XXX POP3 stores always return a "no folder" error because they
	 *     have no folder hierarchy to scan.  Just ignore the error. */
	} else if (g_error_matches (
			error, CAMEL_STORE_ERROR,
			CAMEL_STORE_ERROR_NO_FOLDER)) {
		g_warn_if_fail (folder_info == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (folder_info == NULL);
		e_alert_submit (
			alert_sink, "mail:folder-open",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	/* If we've just set up an NNTP account, for example, and haven't
	 * subscribed to any folders yet, folder_info may legitimately be
	 * NULL at this point.  We handle that case below.  Proceed. */

	/* Check if the store has been removed. */
	si = em_folder_tree_model_lookup_store_info (
		EM_FOLDER_TREE_MODEL (model), store);
	if (si == NULL)
		goto exit;

	/* Make sure we still need to load the tree subfolders. */

	iter_is_placeholder = FALSE;

	/* Get the first child (which will be a placeholder row). */
	valid = gtk_tree_model_iter_children (model, &iter, &root);

	/* Traverse to the last valid iter, or the placeholder row. */
	while (valid) {
		gboolean is_store_node = FALSE;
		gboolean is_folder_node = FALSE;

		titer = iter; /* Preserve the last valid iter */

		gtk_tree_model_get (
			model, &iter,
			COL_BOOL_IS_STORE, &is_store_node,
			COL_BOOL_IS_FOLDER, &is_folder_node, -1);

		/* Stop on a "Loading..." placeholder row. */
		if (!is_store_node && !is_folder_node) {
			iter_is_placeholder = TRUE;
			break;
		}

		valid = gtk_tree_model_iter_next (model, &iter);
	}

	iter = titer;
	child_info = folder_info;

	/* FIXME Camel's IMAP code is totally on crack here: the
	 *       folder_info we got back should be for the folder
	 *       we're expanding, and folder_info->child should be
	 *       what we want to fill our tree with... *sigh* */
	if (folder_info != NULL) {
		gboolean names_match;

		names_match = (g_strcmp0 (
			folder_info->full_name,
			context->full_name) == 0);
		if (names_match) {
			child_info = folder_info->child;
			if (child_info == NULL)
				child_info = folder_info->next;
		}
	}

	/* The folder being expanded has no children after all.  Remove
	 * the "Loading..." placeholder row and collapse the parent. */
	if (child_info == NULL) {
		if (iter_is_placeholder)
			gtk_tree_store_remove (GTK_TREE_STORE (model), &iter);

		if (is_store) {
			path = gtk_tree_model_get_path (model, &root);
			gtk_tree_view_collapse_row (tree_view, path);
			gtk_tree_path_free (path);
			goto exit;
		}

	} else {
		while (child_info != NULL) {
			GtkTreeRowReference *reference;

			/* Check if we already have this row cached. */
			reference = g_hash_table_lookup (
				si->full_hash, child_info->full_name);

			if (reference == NULL) {
				/* If we're on a placeholder row, reuse
				 * the row for the first child folder. */
				if (iter_is_placeholder)
					iter_is_placeholder = FALSE;
				else
					gtk_tree_store_append (
						GTK_TREE_STORE (model),
						&iter, &root);

				em_folder_tree_model_set_folder_info (
					EM_FOLDER_TREE_MODEL (model),
					&iter, si, child_info, TRUE);
			}

			child_info = child_info->next;
		}

		/* Remove the "Loading..." placeholder row. */
		if (iter_is_placeholder)
			gtk_tree_store_remove (GTK_TREE_STORE (model), &iter);
	}

	gtk_tree_store_set (
		GTK_TREE_STORE (model), &root,
		COL_BOOL_LOAD_SUBDIRS, FALSE, -1);

exit:
	if (folder_info != NULL)
		camel_store_free_folder_info (store, folder_info);

	async_context_free (context);
}

static void
folder_tree_emit_popup_event (EMFolderTree *folder_tree,
                              GdkEvent *event)
{
	g_signal_emit (folder_tree, signals[POPUP_EVENT], 0, event);
}

static void
folder_tree_free_select_uri (struct _selected_uri *u)
{
	g_free (u->uri);
	if (u->service)
		g_object_unref (u->service);
	g_free (u->key);
	g_free (u->path);
	g_free (u);
}

static gboolean
folder_tree_select_func (GtkTreeSelection *selection,
                         GtkTreeModel *model,
                         GtkTreePath *path,
                         gboolean selected)
{
	EMFolderTreePrivate *priv;
	GtkTreeView *tree_view;
	gboolean is_store;
	guint32 flags;
	GtkTreeIter iter;

	tree_view = gtk_tree_selection_get_tree_view (selection);

	priv = EM_FOLDER_TREE (tree_view)->priv;

	if (selected)
		return TRUE;

	if (priv->excluded == 0 && priv->excluded_func == NULL)
		return TRUE;

	if (!gtk_tree_model_get_iter (model, &iter, path))
		return TRUE;

	if (priv->excluded_func != NULL)
		return priv->excluded_func (
			EM_FOLDER_TREE (tree_view), model,
			&iter, priv->excluded_data);

	gtk_tree_model_get (
		model, &iter, COL_UINT_FLAGS, &flags,
		COL_BOOL_IS_STORE, &is_store, -1);

	if (is_store)
		flags |= CAMEL_FOLDER_NOSELECT;

	return (flags & priv->excluded) == 0;
}

/* NOTE: Removes and frees the selected uri structure */
static void
folder_tree_select_uri (EMFolderTree *folder_tree,
                        GtkTreePath *path,
                        struct _selected_uri *u)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	GtkTreeView *tree_view;
	GtkTreeSelection *selection;

	tree_view = GTK_TREE_VIEW (folder_tree);
	selection = gtk_tree_view_get_selection (tree_view);
	gtk_tree_selection_select_path (selection, path);
	if (!priv->cursor_set) {
		gtk_tree_view_set_cursor (tree_view, path, NULL, FALSE);
		priv->cursor_set = TRUE;
	}
	gtk_tree_view_scroll_to_cell (tree_view, path, NULL, TRUE, 0.8f, 0.0f);
	g_hash_table_remove (priv->select_uris_table, u->key);
	priv->select_uris = g_slist_remove (priv->select_uris, u);
	folder_tree_free_select_uri (u);
}

static void
folder_tree_expand_node (const gchar *key,
                         EMFolderTree *folder_tree)
{
	struct _EMFolderTreeModelStoreInfo *si;
	GtkTreeRowReference *row;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	GtkTreePath *path;
	EMailBackend *backend;
	EMailSession *session;
	CamelService *service;
	const gchar *p;
	gchar *uid;
	gsize n;
	struct _selected_uri *u;

	if (!(p = strchr (key, '/')))
		n = strlen (key);
	else
		n = (p - key);

	uid = g_alloca (n + 1);
	memcpy (uid, key, n);
	uid[n] = '\0';

	tree_view = GTK_TREE_VIEW (folder_tree);
	model = gtk_tree_view_get_model (tree_view);

	backend = em_folder_tree_get_backend (folder_tree);
	session = e_mail_backend_get_session (backend);

	service = camel_session_get_service (CAMEL_SESSION (session), uid);

	if (!CAMEL_IS_STORE (service))
		return;

	g_object_ref (service);

	si = em_folder_tree_model_lookup_store_info (
		EM_FOLDER_TREE_MODEL (model), CAMEL_STORE (service));
	if (si == NULL) {
		g_object_unref (service);
		return;
	}

	g_object_unref (service);

	if (p != NULL) {
		if (!(row = g_hash_table_lookup (si->full_hash, p + 1)))
			return;
	} else
		row = si->row;

	path = gtk_tree_row_reference_get_path (row);
	gtk_tree_view_expand_to_path (tree_view, path);

	u = g_hash_table_lookup (folder_tree->priv->select_uris_table, key);
	if (u)
		folder_tree_select_uri (folder_tree, path, u);

	gtk_tree_path_free (path);
}

static void
folder_tree_maybe_expand_row (EMFolderTreeModel *model,
                              GtkTreePath *tree_path,
                              GtkTreeIter *iter,
                              EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	CamelStore *store;
	gchar *full_name;
	gchar *key;
	const gchar *uid;
	struct _selected_uri *u;

	gtk_tree_model_get (
		GTK_TREE_MODEL (model), iter,
		COL_STRING_FULL_NAME, &full_name,
		COL_POINTER_CAMEL_STORE, &store, -1);

	uid = camel_service_get_uid (CAMEL_SERVICE (store));
	key = g_strdup_printf ("%s/%s", uid, full_name ? full_name : "");

	u = g_hash_table_lookup (priv->select_uris_table, key);
	if (u) {
		gchar *c = strrchr (key, '/');

		*c = '\0';
		folder_tree_expand_node (key, folder_tree);

		folder_tree_select_uri (folder_tree, tree_path, u);
	}

	g_free (full_name);
	g_free (key);
}

static void
folder_tree_clear_selected_list (EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;

	g_slist_foreach (priv->select_uris, (GFunc) folder_tree_free_select_uri, NULL);
	g_slist_free (priv->select_uris);
	g_hash_table_destroy (priv->select_uris_table);
	priv->select_uris = NULL;
	priv->select_uris_table = g_hash_table_new (g_str_hash, g_str_equal);
	priv->cursor_set = FALSE;
}

static void
folder_tree_cell_edited_cb (EMFolderTree *folder_tree,
                            const gchar *path_string,
                            const gchar *new_name)
{
	CamelFolderInfo *folder_info;
	CamelStore *store;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkTreeIter iter;
	gchar *old_name = NULL;
	gchar *old_full_name = NULL;
	gchar *new_full_name = NULL;
	gchar **strv;
	gpointer parent;
	guint index;
	GError *local_error = NULL;

	/* XXX Consider splitting this into separate async functions:
	 *     em_folder_tree_rename_folder_async()
	 *     em_folder_tree_rename_folder_finish() */

	parent = gtk_widget_get_toplevel (GTK_WIDGET (folder_tree));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	tree_view = GTK_TREE_VIEW (folder_tree);
	model = gtk_tree_view_get_model (tree_view);
	path = gtk_tree_path_new_from_string (path_string);
	gtk_tree_model_get_iter (model, &iter, path);
	gtk_tree_path_free (path);

	gtk_tree_model_get (
		model, &iter,
		COL_POINTER_CAMEL_STORE, &store,
		COL_STRING_DISPLAY_NAME, &old_name,
		COL_STRING_FULL_NAME, &old_full_name, -1);

	if (!old_name || !old_full_name || g_strcmp0 (new_name, old_name) == 0)
		goto exit;

	/* Check for invalid characters. */
	if (strchr (new_name, '/') != NULL) {
		e_alert_run_dialog_for_args (
			parent, "mail:no-rename-folder",
			old_name, new_name,
			_("Folder names cannot contain '/'"), NULL);
		goto exit;
	}

	/* Build the new name from the old name. */
	strv = g_strsplit_set (old_full_name, "/", 0);
	index = g_strv_length (strv) - 1;
	g_free (strv[index]);
	strv[index] = g_strdup (new_name);
	new_full_name = g_strjoinv ("/", strv);
	g_strfreev (strv);

	/* Check for duplicate folder name. */
	/* FIXME camel_store_get_folder_info() may block. */
	folder_info = camel_store_get_folder_info_sync (
		store, new_full_name,
		CAMEL_STORE_FOLDER_INFO_FAST, NULL, NULL);
	if (folder_info != NULL) {
		e_alert_run_dialog_for_args (
			parent, "mail:no-rename-folder-exists",
			old_name, new_name, NULL);
		camel_store_free_folder_info (store, folder_info);
		goto exit;
	}

	/* FIXME camel_store_rename_folder_sync() may block. */
	if (!camel_store_rename_folder_sync (
		store, old_full_name, new_full_name, NULL, &local_error)) {
		e_alert_run_dialog_for_args (
			parent, "mail:no-rename-folder",
			old_full_name, new_full_name,
			local_error ? local_error->message : _("Unknown error"), NULL);
		if (local_error)
			g_clear_error (&local_error);
		goto exit;
	}

exit:

	g_free (old_name);
	g_free (old_full_name);
	g_free (new_full_name);
}

static gboolean
subdirs_contain_unread (GtkTreeModel *model,
                        GtkTreeIter *root)
{
	guint unread;
	GtkTreeIter iter;

	if (!gtk_tree_model_iter_children (model, &iter, root))
		return FALSE;

	do {
		gtk_tree_model_get (model, &iter, COL_UINT_UNREAD, &unread, -1);
		if (unread)
			return TRUE;

		if (gtk_tree_model_iter_has_child (model, &iter))
			if (subdirs_contain_unread (model, &iter))
				return TRUE;
	} while (gtk_tree_model_iter_next (model, &iter));

	return FALSE;
}

static void
folder_tree_render_display_name (GtkTreeViewColumn *column,
                                 GtkCellRenderer *renderer,
                                 GtkTreeModel *model,
                                 GtkTreeIter *iter)
{
	PangoWeight weight;
	gboolean is_store, bold, subdirs_unread = FALSE;
	gboolean editable;
	guint unread;
	gchar *display;
	gchar *name;

	gtk_tree_model_get (
		model, iter, COL_STRING_DISPLAY_NAME, &name,
		COL_BOOL_IS_STORE, &is_store,
		COL_UINT_UNREAD, &unread, -1);

	g_object_get (renderer, "editable", &editable, NULL);

	bold = is_store || unread;

	if (gtk_tree_model_iter_has_child (model, iter)) {
		gboolean expanded = TRUE;

		g_object_get (renderer, "is-expanded", &expanded, NULL);

		if (!bold || !expanded)
			subdirs_unread = subdirs_contain_unread (model, iter);
	}

	bold = !editable && (bold || subdirs_unread);
	weight = bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL;

	if (!is_store && !editable && unread) {
		/* Translators: This is the string used for displaying the
		 * folder names in folder trees. The first "%s" will be
		 * replaced by the folder's name and "%u" will be replaced
		 * with the number of unread messages in the folder. The
		 * second %s will be replaced with a "+" letter for collapsed
		 * folders with unread messages in some subfolder too,
		 * or with an empty string for other cases.
		 *
		 * Most languages should translate this as "%s (%u%s)". The
		 * languages that use localized digits (like Persian) may
		 * need to replace "%u" with "%Iu". Right-to-left languages
		 * (like Arabic and Hebrew) may need to add bidirectional
		 * formatting codes to take care of the cases the folder
		 * name appears in either direction.
		 *
		 * Do not translate the "folder-display|" part. Remove it
		 * from your translation.
		 */
		display = g_strdup_printf (
			C_("folder-display", "%s (%u%s)"),
			name, unread, subdirs_unread ? "+" : "");
		g_free (name);
	} else
		display = name;

	g_object_set (renderer, "text", display, "weight", weight, NULL);

	g_free (display);
}

static void
folder_tree_render_icon (GtkTreeViewColumn *column,
                         GtkCellRenderer *renderer,
                         GtkTreeModel *model,
                         GtkTreeIter *iter)
{
	GtkTreeSelection *selection;
	GtkTreePath *drag_dest_row;
	GtkWidget *tree_view;
	GIcon *icon;
	guint unread;
	guint old_unread;
	gchar *icon_name;
	gboolean is_selected;
	gboolean is_drafts = FALSE;
	gboolean is_drag_dest = FALSE;

	gtk_tree_model_get (
		model, iter,
		COL_STRING_ICON_NAME, &icon_name,
		COL_UINT_UNREAD_LAST_SEL, &old_unread,
		COL_UINT_UNREAD, &unread,
		COL_BOOL_IS_DRAFT, &is_drafts,
		-1);

	if (icon_name == NULL)
		return;

	tree_view = gtk_tree_view_column_get_tree_view (column);
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tree_view));
	is_selected = gtk_tree_selection_iter_is_selected (selection, iter);

	gtk_tree_view_get_drag_dest_row (
		GTK_TREE_VIEW (tree_view), &drag_dest_row, NULL);
	if (drag_dest_row != NULL) {
		GtkTreePath *path;

		path = gtk_tree_model_get_path (model, iter);
		if (gtk_tree_path_compare (path, drag_dest_row) == 0)
			is_drag_dest = TRUE;
		gtk_tree_path_free (path);

		gtk_tree_path_free (drag_dest_row);
	}

	if (g_strcmp0 (icon_name, "folder") == 0) {
		if (is_selected) {
			g_free (icon_name);
			icon_name = g_strdup ("folder-open");
		} else if (is_drag_dest) {
			g_free (icon_name);
			icon_name = g_strdup ("folder-drag-accept");
		}
	}

	icon = g_themed_icon_new (icon_name);

	/* Show an emblem if there's new mail. */
	if (!is_selected && unread > old_unread && !is_drafts) {
		GIcon *temp_icon;
		GEmblem *emblem;

		temp_icon = g_themed_icon_new ("emblem-new");
		emblem = g_emblem_new (temp_icon);
		g_object_unref (temp_icon);

		temp_icon = g_emblemed_icon_new (icon, emblem);
		g_object_unref (emblem);
		g_object_unref (icon);

		icon = temp_icon;
	}

	g_object_set (renderer, "gicon", icon, NULL);

	g_object_unref (icon);
	g_free (icon_name);
}

static void
folder_tree_selection_changed_cb (EMFolderTree *folder_tree,
                                  GtkTreeSelection *selection)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	GList *list;
	CamelStore *store = NULL;
	CamelFolderInfoFlags flags = 0;
	guint unread = 0;
	guint old_unread = 0;
	gchar *folder_name = NULL;

	list = gtk_tree_selection_get_selected_rows (selection, &model);

	if (list == NULL)
		goto exit;

	gtk_tree_model_get_iter (model, &iter, list->data);

	gtk_tree_model_get (
		model, &iter,
		COL_POINTER_CAMEL_STORE, &store,
		COL_STRING_FULL_NAME, &folder_name,
		COL_UINT_FLAGS, &flags,
		COL_UINT_UNREAD, &unread,
		COL_UINT_UNREAD_LAST_SEL, &old_unread, -1);

	/* Sync unread counts to distinguish new incoming mail. */
	if (unread != old_unread)
		gtk_tree_store_set (
			GTK_TREE_STORE (model), &iter,
			COL_UINT_UNREAD_LAST_SEL, unread, -1);

exit:
	g_signal_emit (
		folder_tree, signals[FOLDER_SELECTED], 0,
		store, folder_name, flags);

	g_free (folder_name);

	g_list_foreach (list, (GFunc) gtk_tree_path_free, NULL);
	g_list_free (list);
}

static void
folder_tree_copy_expanded_cb (GtkTreeView *unused,
                              GtkTreePath *path,
                              GtkTreeView *tree_view)
{
	gtk_tree_view_expand_row (tree_view, path, FALSE);
}

static void
folder_tree_copy_selection_cb (GtkTreeModel *model,
                               GtkTreePath *path,
                               GtkTreeIter *iter,
                               GtkTreeView *tree_view)
{
	GtkTreeSelection *selection;

	selection = gtk_tree_view_get_selection (tree_view);
	gtk_tree_selection_select_path (selection, path);

	/* Center the tree view on the selected path. */
	gtk_tree_view_scroll_to_cell (tree_view, path, NULL, TRUE, 0.5, 0.0);
}

static void
folder_tree_copy_state (EMFolderTree *folder_tree)
{
	GtkTreeSelection *selection;
	GtkTreeView *tree_view;
	GtkTreeModel *model;

	tree_view = GTK_TREE_VIEW (folder_tree);
	model = gtk_tree_view_get_model (tree_view);

	selection = em_folder_tree_model_get_selection (
		EM_FOLDER_TREE_MODEL (model));
	if (selection == NULL)
		return;

	gtk_tree_view_map_expanded_rows (
		tree_view, (GtkTreeViewMappingFunc)
		folder_tree_copy_expanded_cb, folder_tree);

	gtk_tree_selection_selected_foreach (
		selection, (GtkTreeSelectionForeachFunc)
		folder_tree_copy_selection_cb, folder_tree);
}

static void
folder_tree_set_alert_sink (EMFolderTree *folder_tree,
                            EAlertSink *alert_sink)
{
	g_return_if_fail (E_IS_ALERT_SINK (alert_sink));
	g_return_if_fail (folder_tree->priv->alert_sink == NULL);

	folder_tree->priv->alert_sink = g_object_ref (alert_sink);
}

static void
folder_tree_set_backend (EMFolderTree *folder_tree,
                         EMailBackend *backend)
{
	g_return_if_fail (E_IS_MAIL_BACKEND (backend));
	g_return_if_fail (folder_tree->priv->backend == NULL);

	folder_tree->priv->backend = g_object_ref (backend);
}

static GtkTargetList *
folder_tree_get_copy_target_list (EMFolderTree *folder_tree)
{
	GtkTargetList *target_list = NULL;

	if (E_IS_SELECTABLE (folder_tree->priv->selectable)) {
		ESelectable *selectable;

		selectable = E_SELECTABLE (folder_tree->priv->selectable);
		target_list = e_selectable_get_copy_target_list (selectable);
	}

	return target_list;
}

static GtkTargetList *
folder_tree_get_paste_target_list (EMFolderTree *folder_tree)
{
	GtkTargetList *target_list = NULL;

	if (E_IS_SELECTABLE (folder_tree->priv->selectable)) {
		ESelectable *selectable;

		selectable = E_SELECTABLE (folder_tree->priv->selectable);
		target_list = e_selectable_get_paste_target_list (selectable);
	}

	return target_list;
}

static void
folder_tree_set_property (GObject *object,
                          guint property_id,
                          const GValue *value,
                          GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ALERT_SINK:
			folder_tree_set_alert_sink (
				EM_FOLDER_TREE (object),
				g_value_get_object (value));
			return;

		case PROP_BACKEND:
			folder_tree_set_backend (
				EM_FOLDER_TREE (object),
				g_value_get_object (value));
			return;

		case PROP_ELLIPSIZE:
			em_folder_tree_set_ellipsize (
				EM_FOLDER_TREE (object),
				g_value_get_enum (value));
			return;

		case PROP_MODEL:
			gtk_tree_view_set_model (
				GTK_TREE_VIEW (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
folder_tree_get_property (GObject *object,
                          guint property_id,
                          GValue *value,
                          GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ALERT_SINK:
			g_value_set_object (
				value,
				em_folder_tree_get_alert_sink (
				EM_FOLDER_TREE (object)));
			return;

		case PROP_BACKEND:
			g_value_set_object (
				value,
				em_folder_tree_get_backend (
				EM_FOLDER_TREE (object)));
			return;

		case PROP_COPY_TARGET_LIST:
			g_value_set_boxed (
				value,
				folder_tree_get_copy_target_list (
				EM_FOLDER_TREE (object)));
			return;

		case PROP_ELLIPSIZE:
			g_value_set_enum (
				value,
				em_folder_tree_get_ellipsize (
				EM_FOLDER_TREE (object)));
			return;

		case PROP_MODEL:
			g_value_set_object (
				value,
				gtk_tree_view_get_model (
				GTK_TREE_VIEW (object)));
			return;

		case PROP_PASTE_TARGET_LIST:
			g_value_set_boxed (
				value,
				folder_tree_get_paste_target_list (
				EM_FOLDER_TREE (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
folder_tree_dispose (GObject *object)
{
	EMFolderTreePrivate *priv;
	GtkTreeModel *model;

	priv = EM_FOLDER_TREE_GET_PRIVATE (object);
	model = gtk_tree_view_get_model (GTK_TREE_VIEW (object));

	if (priv->loaded_row_id != 0) {
		g_signal_handler_disconnect (model, priv->loaded_row_id);
		priv->loaded_row_id = 0;
	}

	if (priv->autoscroll_id != 0) {
		g_source_remove (priv->autoscroll_id);
		priv->autoscroll_id = 0;
	}

	if (priv->autoexpand_id != 0) {
		gtk_tree_row_reference_free (priv->autoexpand_row);
		priv->autoexpand_row = NULL;

		g_source_remove (priv->autoexpand_id);
		priv->autoexpand_id = 0;
	}

	if (priv->alert_sink != NULL) {
		g_object_unref (priv->alert_sink);
		priv->alert_sink = NULL;
	}

	if (priv->backend != NULL) {
		g_object_unref (priv->backend);
		priv->backend = NULL;
	}

	if (priv->text_renderer != NULL) {
		g_object_unref (priv->text_renderer);
		priv->text_renderer = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (em_folder_tree_parent_class)->dispose (object);
}

static void
folder_tree_finalize (GObject *object)
{
	EMFolderTreePrivate *priv;

	priv = EM_FOLDER_TREE_GET_PRIVATE (object);

	if (priv->select_uris != NULL) {
		g_slist_foreach (
			priv->select_uris,
			(GFunc) folder_tree_free_select_uri, NULL);
		g_slist_free (priv->select_uris);
		priv->select_uris = NULL;
	}

	if (priv->select_uris_table) {
		g_hash_table_destroy (priv->select_uris_table);
		priv->select_uris_table = NULL;
	}

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (em_folder_tree_parent_class)->finalize (object);
}

static void
folder_tree_constructed (GObject *object)
{
	EMFolderTreePrivate *priv;
	GtkTreeSelection *selection;
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	gulong handler_id;

	priv = EM_FOLDER_TREE_GET_PRIVATE (object);

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (em_folder_tree_parent_class)->constructed (object);

	tree_view = GTK_TREE_VIEW (object);
	model = gtk_tree_view_get_model (tree_view);
	selection = gtk_tree_view_get_selection (tree_view);

	handler_id = g_signal_connect (
		model, "loading-row",
		G_CALLBACK (folder_tree_maybe_expand_row), object);
	priv->loading_row_id = handler_id;

	handler_id = g_signal_connect (
		model, "loaded-row",
		G_CALLBACK (folder_tree_maybe_expand_row), object);
	priv->loaded_row_id = handler_id;

	handler_id = g_signal_connect_swapped (
		selection, "changed",
		G_CALLBACK (folder_tree_selection_changed_cb), object);
	priv->selection_changed_handler_id = handler_id;

	column = gtk_tree_view_column_new ();
	gtk_tree_view_append_column (tree_view, column);

	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (
		column, renderer, "visible", COL_BOOL_IS_FOLDER);
	gtk_tree_view_column_set_cell_data_func (
		column, renderer, (GtkTreeCellDataFunc)
		folder_tree_render_icon, NULL, NULL);

	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_set_cell_data_func (
		column, renderer, (GtkTreeCellDataFunc)
		folder_tree_render_display_name, NULL, NULL);
	priv->text_renderer = g_object_ref (renderer);

	g_object_bind_property (
		object, "ellipsize",
		renderer, "ellipsize",
		G_BINDING_SYNC_CREATE);

	g_signal_connect_swapped (
		renderer, "edited",
		G_CALLBACK (folder_tree_cell_edited_cb), object);

	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);
	gtk_tree_selection_set_select_function (
		selection, (GtkTreeSelectionFunc)
		folder_tree_select_func, NULL, NULL);

	gtk_tree_view_set_headers_visible (tree_view, FALSE);
	gtk_tree_view_set_search_column (tree_view, COL_STRING_DISPLAY_NAME);

	folder_tree_copy_state (EM_FOLDER_TREE (object));
	gtk_widget_show (GTK_WIDGET (object));
}

static gboolean
folder_tree_button_press_event (GtkWidget *widget,
                                GdkEventButton *event)
{
	EMFolderTreePrivate *priv;
	GtkWidgetClass *widget_class;
	GtkTreeSelection *selection;
	GtkTreeView *tree_view;
	GtkTreePath *path;
	gulong handler_id;

	priv = EM_FOLDER_TREE_GET_PRIVATE (widget);

	tree_view = GTK_TREE_VIEW (widget);
	selection = gtk_tree_view_get_selection (tree_view);

	if (gtk_tree_selection_get_mode (selection) == GTK_SELECTION_SINGLE)
		folder_tree_clear_selected_list (EM_FOLDER_TREE (widget));

	priv->cursor_set = TRUE;

	if (event->button != 3)
		goto chainup;

	if (!gtk_tree_view_get_path_at_pos (
		tree_view, event->x, event->y,
		&path, NULL, NULL, NULL))
		goto chainup;

	/* Select and focus the row that was right-clicked, but prevent
	 * a "folder-selected" signal emission since this does not count
	 * as a folder selection in the sense we mean. */
	handler_id = priv->selection_changed_handler_id;
	g_signal_handler_block (selection, handler_id);
	gtk_tree_selection_select_path (selection, path);
	gtk_tree_view_set_cursor (tree_view, path, NULL, FALSE);
	g_signal_handler_unblock (selection, handler_id);

	gtk_tree_path_free (path);

	folder_tree_emit_popup_event (
		EM_FOLDER_TREE (tree_view), (GdkEvent *) event);

chainup:

	/* Chain up to parent's button_press_event() method. */
	widget_class = GTK_WIDGET_CLASS (em_folder_tree_parent_class);
	return widget_class->button_press_event (widget, event);
}

static gboolean
folder_tree_key_press_event (GtkWidget *widget,
                             GdkEventKey *event)
{
	EMFolderTreePrivate *priv;
	GtkWidgetClass *widget_class;
	GtkTreeSelection *selection;
	GtkTreeView *tree_view;

	if (event && event->type == GDK_KEY_PRESS &&
		(event->keyval == GDK_KEY_space ||
		 event->keyval == '.' ||
		 event->keyval == ',' ||
		 event->keyval == '[' ||
		 event->keyval == ']')) {
		g_signal_emit (widget, signals[HIDDEN_KEY_EVENT], 0, event);

		return TRUE;
	}

	priv = EM_FOLDER_TREE_GET_PRIVATE (widget);

	tree_view = GTK_TREE_VIEW (widget);
	selection = gtk_tree_view_get_selection (tree_view);

	if (gtk_tree_selection_get_mode (selection) == GTK_SELECTION_SINGLE)
		folder_tree_clear_selected_list (EM_FOLDER_TREE (widget));

	priv->cursor_set = TRUE;

	/* Chain up to parent's key_press_event() method. */
	widget_class = GTK_WIDGET_CLASS (em_folder_tree_parent_class);
	return widget_class->key_press_event (widget, event);
}

static gboolean
folder_tree_popup_menu (GtkWidget *widget)
{
	folder_tree_emit_popup_event (EM_FOLDER_TREE (widget), NULL);

	return TRUE;
}

static void
folder_tree_row_activated (GtkTreeView *tree_view,
                           GtkTreePath *path,
                           GtkTreeViewColumn *column)
{
	EMFolderTreePrivate *priv;
	GtkTreeModel *model;
	gchar *folder_name;
	GtkTreeIter iter;
	CamelStore *store;
	CamelFolderInfoFlags flags;

	priv = EM_FOLDER_TREE_GET_PRIVATE (tree_view);

	model = gtk_tree_view_get_model (tree_view);

	if (priv->skip_double_click)
		return;

	if (!gtk_tree_model_get_iter (model, &iter, path))
		return;

	gtk_tree_model_get (
		model, &iter,
		COL_POINTER_CAMEL_STORE, &store,
		COL_STRING_FULL_NAME, &folder_name,
		COL_UINT_FLAGS, &flags, -1);

	folder_tree_clear_selected_list (EM_FOLDER_TREE (tree_view));

	g_signal_emit (
		tree_view, signals[FOLDER_SELECTED], 0,
		store, folder_name, flags);

	g_signal_emit (
		tree_view, signals[FOLDER_ACTIVATED], 0,
		store, folder_name);

	g_free (folder_name);
}

static gboolean
folder_tree_test_collapse_row (GtkTreeView *tree_view,
                               GtkTreeIter *iter,
                               GtkTreePath *path)
{
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter cursor;

	selection = gtk_tree_view_get_selection (tree_view);

	if (!gtk_tree_selection_get_selected (selection, &model, &cursor))
		goto exit;

	/* Select the collapsed node IFF it is a
	 * parent of the currently selected folder. */
	if (gtk_tree_store_is_ancestor (GTK_TREE_STORE (model), iter, &cursor))
		gtk_tree_view_set_cursor (tree_view, path, NULL, FALSE);

exit:
	return FALSE;
}

static void
folder_tree_row_expanded (GtkTreeView *tree_view,
                          GtkTreeIter *iter,
                          GtkTreePath *path)
{
	EActivity *activity;
	GCancellable *cancellable;
	EMFolderTree *folder_tree;
	AsyncContext *context;
	GtkTreeModel *model;
	CamelStore *store;
	gchar *full_name;
	gboolean load;

	folder_tree = EM_FOLDER_TREE (tree_view);
	model = gtk_tree_view_get_model (tree_view);

	gtk_tree_model_get (
		model, iter,
		COL_STRING_FULL_NAME, &full_name,
		COL_POINTER_CAMEL_STORE, &store,
		COL_BOOL_LOAD_SUBDIRS, &load, -1);

	if (!load) {
		g_free (full_name);
		return;
	}

	gtk_tree_store_set (
		GTK_TREE_STORE (model), iter,
		COL_BOOL_LOAD_SUBDIRS, FALSE, -1);

	/* Retrieve folder info asynchronously. */

	activity = em_folder_tree_new_activity (folder_tree);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->folder_tree = g_object_ref (folder_tree);
	context->root = gtk_tree_row_reference_new (model, path);
	context->full_name = g_strdup (full_name);

	camel_store_get_folder_info (
		store, full_name,
		CAMEL_STORE_FOLDER_INFO_FAST |
		CAMEL_STORE_FOLDER_INFO_RECURSIVE |
		CAMEL_STORE_FOLDER_INFO_SUBSCRIBED,
		G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) folder_tree_get_folder_info_cb,
		context);

	g_free (full_name);
}

static void
em_folder_tree_class_init (EMFolderTreeClass *class)
{
	GObjectClass *object_class;
	GtkWidgetClass *widget_class;
	GtkTreeViewClass *tree_view_class;

	g_type_class_add_private (class, sizeof (EMFolderTreePrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = folder_tree_set_property;
	object_class->get_property = folder_tree_get_property;
	object_class->dispose = folder_tree_dispose;
	object_class->finalize = folder_tree_finalize;
	object_class->constructed = folder_tree_constructed;

	widget_class = GTK_WIDGET_CLASS (class);
	widget_class->button_press_event = folder_tree_button_press_event;
	widget_class->key_press_event = folder_tree_key_press_event;
	widget_class->popup_menu = folder_tree_popup_menu;

	tree_view_class = GTK_TREE_VIEW_CLASS (class);
	tree_view_class->row_activated = folder_tree_row_activated;
	tree_view_class->test_collapse_row = folder_tree_test_collapse_row;
	tree_view_class->row_expanded = folder_tree_row_expanded;

	g_object_class_install_property (
		object_class,
		PROP_ALERT_SINK,
		g_param_spec_object (
			"alert-sink",
			NULL,
			NULL,
			E_TYPE_ALERT_SINK,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY |
			G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (
		object_class,
		PROP_BACKEND,
		g_param_spec_object (
			"backend",
			NULL,
			NULL,
			E_TYPE_MAIL_BACKEND,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY |
			G_PARAM_STATIC_STRINGS));

	/* Inherited from ESelectableInterface */
	g_object_class_override_property (
		object_class,
		PROP_COPY_TARGET_LIST,
		"copy-target-list");

	g_object_class_install_property (
		object_class,
		PROP_ELLIPSIZE,
		g_param_spec_enum (
			"ellipsize",
			NULL,
			NULL,
			PANGO_TYPE_ELLIPSIZE_MODE,
			PANGO_ELLIPSIZE_NONE,
			G_PARAM_READWRITE));

	/* XXX We override the GtkTreeView:model property to add
	 *     G_PARAM_CONSTRUCT_ONLY so the model is set by the
	 *     time we get to folder_tree_constructed(). */
	g_object_class_install_property (
		object_class,
		PROP_MODEL,
		g_param_spec_object (
			"model",
			"TreeView Model",
			"The model for the tree view",
			GTK_TYPE_TREE_MODEL,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY));

	/* Inherited from ESelectableInterface */
	g_object_class_override_property (
		object_class,
		PROP_PASTE_TARGET_LIST,
		"paste-target-list");

	signals[FOLDER_SELECTED] = g_signal_new (
		"folder-selected",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET (EMFolderTreeClass, folder_selected),
		NULL, NULL,
		e_marshal_VOID__OBJECT_STRING_UINT,
		G_TYPE_NONE, 3,
		CAMEL_TYPE_STORE,
		G_TYPE_STRING,
		G_TYPE_UINT);

	signals[FOLDER_ACTIVATED] = g_signal_new (
		"folder-activated",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET (EMFolderTreeClass, folder_activated),
		NULL, NULL,
		e_marshal_VOID__OBJECT_STRING,
		G_TYPE_NONE, 2,
		CAMEL_TYPE_STORE,
		G_TYPE_STRING);

	signals[POPUP_EVENT] = g_signal_new (
		"popup-event",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (EMFolderTreeClass, popup_event),
		NULL, NULL,
		g_cclosure_marshal_VOID__BOXED,
		G_TYPE_NONE, 1,
		GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);

	signals[HIDDEN_KEY_EVENT] = g_signal_new (
		"hidden-key-event",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EMFolderTreeClass, hidden_key_event),
		NULL, NULL,
		g_cclosure_marshal_VOID__BOXED,
		G_TYPE_NONE, 1,
		GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);
}

static void
em_folder_tree_init (EMFolderTree *folder_tree)
{
	GHashTable *select_uris_table;
	AtkObject *a11y;

	select_uris_table = g_hash_table_new (g_str_hash, g_str_equal);

	folder_tree->priv = EM_FOLDER_TREE_GET_PRIVATE (folder_tree);
	folder_tree->priv->select_uris_table = select_uris_table;

	/* FIXME Gross hack. */
	gtk_widget_set_can_focus (GTK_WIDGET (folder_tree), TRUE);

	a11y = gtk_widget_get_accessible (GTK_WIDGET (folder_tree));
	atk_object_set_name (a11y, _("Mail Folder Tree"));
}

/* Sets a selectable widget, which will be used for update-actions and
 * select-all selectable interface functions. This can be NULL, then nothing
 * can be selected and calling selectable function does nothing. */
void
em_folder_tree_set_selectable_widget (EMFolderTree *folder_tree,
                                      GtkWidget *selectable)
{
	g_return_if_fail (EM_IS_FOLDER_TREE (folder_tree));

	if (selectable != NULL)
		g_return_if_fail (E_IS_SELECTABLE (selectable));

	folder_tree->priv->selectable = selectable;
}

static void
folder_tree_selectable_update_actions (ESelectable *selectable,
                                        EFocusTracker *focus_tracker,
                                        GdkAtom *clipboard_targets,
                                        gint n_clipboard_targets)
{
	EMFolderTree *folder_tree;

	folder_tree = EM_FOLDER_TREE (selectable);
	g_return_if_fail (folder_tree != NULL);

	if (folder_tree->priv->selectable != NULL) {
		ESelectableInterface *interface;
		ESelectable *selectable;

		selectable = E_SELECTABLE (folder_tree->priv->selectable);
		interface = E_SELECTABLE_GET_INTERFACE (selectable);
		g_return_if_fail (interface->update_actions != NULL);

		interface->update_actions (
			selectable, focus_tracker,
			clipboard_targets, n_clipboard_targets);
	}
}

static void
folder_tree_selectable_cut_clipboard (ESelectable *selectable)
{
	ESelectableInterface *interface;
	EMFolderTree *folder_tree;
	GtkWidget *proxy;

	folder_tree = EM_FOLDER_TREE (selectable);
	proxy = folder_tree->priv->selectable;

	if (!E_IS_SELECTABLE (proxy))
		return;

	interface = E_SELECTABLE_GET_INTERFACE (proxy);

	if (interface->cut_clipboard == NULL)
		return;

	if (gtk_widget_get_can_focus (proxy))
		gtk_widget_grab_focus (proxy);

	interface->cut_clipboard (E_SELECTABLE (proxy));
}

static void
folder_tree_selectable_copy_clipboard (ESelectable *selectable)
{
	ESelectableInterface *interface;
	EMFolderTree *folder_tree;
	GtkWidget *proxy;

	folder_tree = EM_FOLDER_TREE (selectable);
	proxy = folder_tree->priv->selectable;

	if (!E_IS_SELECTABLE (proxy))
		return;

	interface = E_SELECTABLE_GET_INTERFACE (proxy);

	if (interface->copy_clipboard == NULL)
		return;

	if (gtk_widget_get_can_focus (proxy))
		gtk_widget_grab_focus (proxy);

	interface->copy_clipboard (E_SELECTABLE (proxy));
}

static void
folder_tree_selectable_paste_clipboard (ESelectable *selectable)
{
	ESelectableInterface *interface;
	EMFolderTree *folder_tree;
	GtkWidget *proxy;

	folder_tree = EM_FOLDER_TREE (selectable);
	proxy = folder_tree->priv->selectable;

	if (!E_IS_SELECTABLE (proxy))
		return;

	interface = E_SELECTABLE_GET_INTERFACE (proxy);

	if (interface->paste_clipboard == NULL)
		return;

	if (gtk_widget_get_can_focus (proxy))
		gtk_widget_grab_focus (proxy);

	interface->paste_clipboard (E_SELECTABLE (proxy));
}

static void
folder_tree_selectable_delete_selection (ESelectable *selectable)
{
	ESelectableInterface *interface;
	EMFolderTree *folder_tree;
	GtkWidget *proxy;

	folder_tree = EM_FOLDER_TREE (selectable);
	proxy = folder_tree->priv->selectable;

	if (!E_IS_SELECTABLE (proxy))
		return;

	interface = E_SELECTABLE_GET_INTERFACE (proxy);

	if (interface->delete_selection == NULL)
		return;

	if (gtk_widget_get_can_focus (proxy))
		gtk_widget_grab_focus (proxy);

	interface->delete_selection (E_SELECTABLE (proxy));
}

static void
folder_tree_selectable_select_all (ESelectable *selectable)
{
	ESelectableInterface *interface;
	EMFolderTree *folder_tree;
	GtkWidget *proxy;

	folder_tree = EM_FOLDER_TREE (selectable);
	proxy = folder_tree->priv->selectable;

	if (!E_IS_SELECTABLE (proxy))
		return;

	interface = E_SELECTABLE_GET_INTERFACE (proxy);

	if (interface->select_all == NULL)
		return;

	if (gtk_widget_get_can_focus (proxy))
		gtk_widget_grab_focus (proxy);

	interface->select_all (E_SELECTABLE (proxy));
}

static void
em_folder_tree_selectable_init (ESelectableInterface *interface)
{
	interface->update_actions = folder_tree_selectable_update_actions;
	interface->cut_clipboard = folder_tree_selectable_cut_clipboard;
	interface->copy_clipboard = folder_tree_selectable_copy_clipboard;
	interface->paste_clipboard = folder_tree_selectable_paste_clipboard;
	interface->delete_selection = folder_tree_selectable_delete_selection;
	interface->select_all = folder_tree_selectable_select_all;
}

GtkWidget *
em_folder_tree_new (EMailBackend *backend,
                    EAlertSink *alert_sink)
{
	EMFolderTreeModel *model;

	g_return_val_if_fail (E_IS_MAIL_BACKEND (backend), NULL);
	g_return_val_if_fail (E_IS_ALERT_SINK (alert_sink), NULL);

	model = em_folder_tree_model_get_default ();

	return em_folder_tree_new_with_model (backend, alert_sink, model);
}

GtkWidget *
em_folder_tree_new_with_model (EMailBackend *backend,
                               EAlertSink *alert_sink,
                               EMFolderTreeModel *model)
{
	const gchar *data_dir;

	g_return_val_if_fail (E_IS_MAIL_BACKEND (backend), NULL);
	g_return_val_if_fail (E_IS_ALERT_SINK (alert_sink), NULL);
	g_return_val_if_fail (EM_IS_FOLDER_TREE_MODEL (model), NULL);

	data_dir = e_shell_backend_get_data_dir (E_SHELL_BACKEND (backend));

	e_mail_store_init (backend, data_dir);

	return g_object_new (
		EM_TYPE_FOLDER_TREE,
		"alert-sink", alert_sink,
		"backend", backend,
		"model", model, NULL);
}

EActivity *
em_folder_tree_new_activity (EMFolderTree *folder_tree)
{
	EActivity *activity;
	EMailBackend *backend;
	EAlertSink *alert_sink;
	GCancellable *cancellable;

	g_return_val_if_fail (EM_IS_FOLDER_TREE (folder_tree), NULL);

	activity = e_activity_new ();

	alert_sink = em_folder_tree_get_alert_sink (folder_tree);
	e_activity_set_alert_sink (activity, alert_sink);

	cancellable = camel_operation_new ();
	e_activity_set_cancellable (activity, cancellable);
	g_object_unref (cancellable);

	backend = em_folder_tree_get_backend (folder_tree);
	e_shell_backend_add_activity (E_SHELL_BACKEND (backend), activity);

	return activity;
}

PangoEllipsizeMode
em_folder_tree_get_ellipsize (EMFolderTree *folder_tree)
{
	g_return_val_if_fail (EM_IS_FOLDER_TREE (folder_tree), 0);

	return folder_tree->priv->ellipsize;
}

void
em_folder_tree_set_ellipsize (EMFolderTree *folder_tree,
                              PangoEllipsizeMode ellipsize)
{
	g_return_if_fail (EM_IS_FOLDER_TREE (folder_tree));

	if (ellipsize == folder_tree->priv->ellipsize)
		return;

	folder_tree->priv->ellipsize = ellipsize;

	g_object_notify (G_OBJECT (folder_tree), "ellipsize");
}

EAlertSink *
em_folder_tree_get_alert_sink (EMFolderTree *folder_tree)
{
	g_return_val_if_fail (EM_IS_FOLDER_TREE (folder_tree), NULL);

	return folder_tree->priv->alert_sink;
}

EMailBackend *
em_folder_tree_get_backend (EMFolderTree *folder_tree)
{
	g_return_val_if_fail (EM_IS_FOLDER_TREE (folder_tree), NULL);

	return folder_tree->priv->backend;
}

static void
tree_drag_begin (GtkWidget *widget,
                 GdkDragContext *context,
                 EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	GtkTreeSelection *selection;
	GtkTreeView *tree_view;
	cairo_surface_t *s;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkTreeIter iter;

	tree_view = GTK_TREE_VIEW (widget);
	selection = gtk_tree_view_get_selection (tree_view);
	if (!gtk_tree_selection_get_selected (selection, &model, &iter))
		return;

	path = gtk_tree_model_get_path (model, &iter);
	priv->drag_row = gtk_tree_row_reference_new (model, path);

	s = gtk_tree_view_create_row_drag_icon (tree_view, path);
	gtk_drag_set_icon_surface (context, s);

	gtk_tree_path_free (path);
}

static void
tree_drag_data_delete (GtkWidget *widget,
                       GdkDragContext *context,
                       EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	gchar *full_name = NULL;
	GtkTreeModel *model;
	GtkTreePath *src_path;
	gboolean is_store;
	CamelStore *store;
	GtkTreeIter iter;

	if (!priv->drag_row)
		return;

	src_path = gtk_tree_row_reference_get_path (priv->drag_row);
	if (src_path == NULL)
		return;

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (folder_tree));

	if (!gtk_tree_model_get_iter (model, &iter, src_path))
		goto fail;

	gtk_tree_model_get (
		model, &iter,
		COL_POINTER_CAMEL_STORE, &store,
		COL_STRING_FULL_NAME, &full_name,
		COL_BOOL_IS_STORE, &is_store, -1);

	if (is_store)
		goto fail;

	/* FIXME camel_store_delete_folder_sync() may block. */
	camel_store_delete_folder_sync (store, full_name, NULL, NULL);

fail:
	gtk_tree_path_free (src_path);
	g_free (full_name);
}

static void
tree_drag_data_get (GtkWidget *widget,
                    GdkDragContext *context,
                    GtkSelectionData *selection,
                    guint info,
                    guint time,
                    EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	gchar *full_name = NULL, *uri = NULL;
	GtkTreeModel *model;
	GtkTreePath *src_path;
	CamelFolder *folder;
	CamelStore *store;
	GtkTreeIter iter;

	if (!priv->drag_row || !(src_path =
		gtk_tree_row_reference_get_path (priv->drag_row)))
		return;

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (folder_tree));

	if (!gtk_tree_model_get_iter (model, &iter, src_path))
		goto fail;

	gtk_tree_model_get (
		model, &iter,
		COL_POINTER_CAMEL_STORE, &store,
		COL_STRING_FULL_NAME, &full_name,
		COL_STRING_URI, &uri, -1);

	/* make sure user isn't trying to drag on a placeholder row */
	if (full_name == NULL)
		goto fail;

	switch (info) {
	case DND_DRAG_TYPE_FOLDER:
		/* dragging to a new location in the folder tree */
		gtk_selection_data_set (
			selection, drag_atoms[info], 8,
			(guchar *) uri, strlen (uri) + 1);
		break;
	case DND_DRAG_TYPE_TEXT_URI_LIST:
		/* dragging to nautilus or something, probably */
		/* FIXME camel_store_get_folder_sync() may block. */
		if ((folder = camel_store_get_folder_sync (
			store, full_name, 0, NULL, NULL))) {

			GPtrArray *uids = camel_folder_get_uids (folder);

			em_utils_selection_set_urilist (selection, folder, uids);
			camel_folder_free_uids (folder, uids);
			g_object_unref (folder);
		}
		break;
	default:
		abort ();
	}

fail:
	gtk_tree_path_free (src_path);
	g_free (full_name);
	g_free (uri);
}

/* Drop handling */
struct _DragDataReceivedAsync {
	MailMsg base;

	/* input data */
	GdkDragContext *context;

	/* Only selection->data and selection->length are valid */
	GtkSelectionData *selection;

	EMailSession *session;
	CamelStore *store;
	gchar *full_name;
	guint32 action;
	guint info;

	guint move : 1;
	guint moved : 1;
	guint aborted : 1;
};

static void
folder_tree_drop_folder (struct _DragDataReceivedAsync *m)
{
	CamelFolder *folder;
	CamelStore *parent_store;
	GCancellable *cancellable;
	const gchar *full_name;
	const guchar *data;

	data = gtk_selection_data_get_data (m->selection);

	d(printf(" * Drop folder '%s' onto '%s'\n", data, m->full_name));

	cancellable = e_activity_get_cancellable (m->base.activity);

	folder = e_mail_session_uri_to_folder_sync (
		m->session, (gchar *) data, 0,
		cancellable, &m->base.error);
	if (folder == NULL)
		return;

	full_name = camel_folder_get_full_name (folder);
	parent_store = camel_folder_get_parent_store (folder);

	em_folder_utils_copy_folders (
		parent_store, full_name, m->store,
		m->full_name ? m->full_name : "", m->move);

	g_object_unref (folder);
}

static gchar *
folder_tree_drop_async__desc (struct _DragDataReceivedAsync *m)
{
	const guchar *data;

	data = gtk_selection_data_get_data (m->selection);

	if (m->info == DND_DROP_TYPE_FOLDER) {
		gchar *folder_name = NULL;
		gchar *res;

		e_mail_folder_uri_parse (
			CAMEL_SESSION (m->session),
			(gchar *) data, NULL, &folder_name, NULL);
		g_return_val_if_fail (folder_name != NULL, NULL);

		if (m->move)
			res = g_strdup_printf (
				_("Moving folder %s"), folder_name);
		else
			res = g_strdup_printf (
				_("Copying folder %s"), folder_name);
		g_free (folder_name);

		return res;
	} else {
		if (m->move)
			return g_strdup_printf (
				_("Moving messages into folder %s"),
				m->full_name);
		else
			return g_strdup_printf (
				_("Copying messages into folder %s"),
				m->full_name);
	}
}

static void
folder_tree_drop_async__exec (struct _DragDataReceivedAsync *m,
                              GCancellable *cancellable,
                              GError **error)
{
	CamelFolder *folder;

	/* for types other than folder, we can't drop to the root path */
	if (m->info == DND_DROP_TYPE_FOLDER) {
		/* copy or move (aka rename) a folder */
		folder_tree_drop_folder (m);
	} else if (m->full_name == NULL) {
		g_set_error (
			error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
			_("Cannot drop message(s) into toplevel store"));
	} else if ((folder = camel_store_get_folder_sync (
		m->store, m->full_name, 0, cancellable, error))) {

		switch (m->info) {
		case DND_DROP_TYPE_UID_LIST:
			/* import a list of uids from another evo folder */
			em_utils_selection_get_uidlist (
				m->selection, m->session, folder, m->move,
				cancellable, error);
			m->moved = m->move && (!error || !*error);
			break;
		case DND_DROP_TYPE_MESSAGE_RFC822:
			/* import a message/rfc822 stream */
			em_utils_selection_get_message (m->selection, folder);
			break;
		case DND_DROP_TYPE_TEXT_URI_LIST:
			/* import an mbox, maildir, or mh folder? */
			em_utils_selection_get_urilist (m->selection, folder);
			break;
		default:
			abort ();
		}
		g_object_unref (folder);
	}
}

static void
folder_tree_drop_async__free (struct _DragDataReceivedAsync *m)
{
	g_object_unref (m->session);
	g_object_unref (m->context);
	g_object_unref (m->store);
	g_free (m->full_name);
	gtk_selection_data_free (m->selection);
}

static MailMsgInfo folder_tree_drop_async_info = {
	sizeof (struct _DragDataReceivedAsync),
	(MailMsgDescFunc) folder_tree_drop_async__desc,
	(MailMsgExecFunc) folder_tree_drop_async__exec,
	(MailMsgDoneFunc) NULL,
	(MailMsgFreeFunc) folder_tree_drop_async__free
};

static void
tree_drag_data_action (struct _DragDataReceivedAsync *m)
{
	m->move = m->action == GDK_ACTION_MOVE;
	mail_msg_unordered_push (m);
}

static void
tree_drag_data_received (GtkWidget *widget,
                         GdkDragContext *context,
                         gint x,
                         gint y,
                         GtkSelectionData *selection,
                         guint info,
                         guint time,
                         EMFolderTree *folder_tree)
{
	GtkTreeViewDropPosition pos;
	GtkTreeModel *model;
	GtkTreeView *tree_view;
	GtkTreePath *dest_path = NULL;
	EMailBackend *backend;
	EMailSession *session;
	struct _DragDataReceivedAsync *m;
	gboolean is_store;
	CamelStore *store;
	GtkTreeIter iter;
	gchar *full_name;

	tree_view = GTK_TREE_VIEW (folder_tree);
	model = gtk_tree_view_get_model (tree_view);

	backend = em_folder_tree_get_backend (folder_tree);
	session = e_mail_backend_get_session (backend);

	if (!gtk_tree_view_get_dest_row_at_pos (tree_view, x, y, &dest_path, &pos))
		return;

	/* this means we are receiving no data */
	if (gtk_selection_data_get_data (selection) == NULL) {
		gtk_drag_finish (context, FALSE, FALSE, GDK_CURRENT_TIME);
		gtk_tree_path_free (dest_path);
		return;
	}

	if (gtk_selection_data_get_length (selection) == -1) {
		gtk_drag_finish (context, FALSE, FALSE, GDK_CURRENT_TIME);
		gtk_tree_path_free (dest_path);
		return;
	}

	if (!gtk_tree_model_get_iter (model, &iter, dest_path)) {
		gtk_drag_finish (context, FALSE, FALSE, GDK_CURRENT_TIME);
		gtk_tree_path_free (dest_path);
		return;
	}

	gtk_tree_model_get (
		model, &iter,
		COL_POINTER_CAMEL_STORE, &store,
		COL_BOOL_IS_STORE, &is_store,
		COL_STRING_FULL_NAME, &full_name, -1);

	/* make sure user isn't try to drop on a placeholder row */
	if (full_name == NULL && !is_store) {
		gtk_drag_finish (context, FALSE, FALSE, GDK_CURRENT_TIME);
		gtk_tree_path_free (dest_path);
		return;
	}

	m = mail_msg_new (&folder_tree_drop_async_info);
	m->session = g_object_ref (session);
	m->context = g_object_ref (context);
	m->store = g_object_ref (store);
	m->full_name = full_name;
	m->action = gdk_drag_context_get_selected_action (context);
	m->info = info;

	/* need to copy, goes away once we exit */
	m->selection = gtk_selection_data_copy (selection);

	tree_drag_data_action (m);
	gtk_tree_path_free (dest_path);
}

static gboolean
is_special_local_folder (const gchar *name)
{
	return strcmp (name, "Drafts") == 0
		|| strcmp (name, "Inbox") == 0
		|| strcmp (name, "Outbox") == 0
		|| strcmp (name, "Sent") == 0
		|| strcmp (name, "Templates") == 0;
}

static GdkAtom
folder_tree_drop_target (EMFolderTree *folder_tree,
                         GdkDragContext *context,
                         GtkTreePath *path,
                         GdkDragAction *actions,
                         GdkDragAction *suggested_action)
{
	EMFolderTreePrivate *p = folder_tree->priv;
	gchar *dst_full_name = NULL;
	gchar *src_full_name = NULL;
	CamelStore *local;
	CamelStore *dst_store;
	CamelStore *src_store = NULL;
	GdkAtom atom = GDK_NONE;
	gboolean is_store;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GList *targets;
	const gchar *uid;
	gboolean src_is_vfolder;
	gboolean dst_is_vfolder;
	guint32 flags = 0;

	/* This is a bit of a mess, but should handle all the cases properly */

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (folder_tree));

	if (!gtk_tree_model_get_iter (model, &iter, path))
		return GDK_NONE;

	/* We may override these further down. */
	*actions = gdk_drag_context_get_actions (context);
	*suggested_action = gdk_drag_context_get_suggested_action (context);

	gtk_tree_model_get (
		model, &iter,
		COL_BOOL_IS_STORE, &is_store,
		COL_POINTER_CAMEL_STORE, &dst_store,
		COL_STRING_FULL_NAME, &dst_full_name,
		COL_UINT_FLAGS, &flags, -1);

	local = e_mail_local_get_store ();

	uid = camel_service_get_uid (CAMEL_SERVICE (dst_store));
	dst_is_vfolder = (g_strcmp0 (uid, "vfolder") == 0);

	targets = gdk_drag_context_list_targets (context);

	/* Check for special destinations */

	/* Don't allow copying/moving into the UNMATCHED vfolder. */
	if (dst_is_vfolder)
		if (g_strcmp0 (dst_full_name, CAMEL_UNMATCHED_NAME) == 0)
			goto done;

	/* Don't allow copying/moving into a vTrash folder. */
	if (g_strcmp0 (dst_full_name, CAMEL_VTRASH_NAME) == 0)
		goto done;

	/* Don't allow copying/moving into a vJunk folder. */
	if (g_strcmp0 (dst_full_name, CAMEL_VJUNK_NAME) == 0)
		goto done;

	if (flags & CAMEL_FOLDER_NOSELECT)
		goto done;

	if (p->drag_row) {
		GtkTreePath *src_path = gtk_tree_row_reference_get_path (p->drag_row);

		if (src_path) {
			guint32 src_flags = 0;

			if (gtk_tree_model_get_iter (model, &iter, src_path))
				gtk_tree_model_get (
					model, &iter,
					COL_POINTER_CAMEL_STORE, &src_store,
					COL_STRING_FULL_NAME, &src_full_name,
					COL_UINT_FLAGS, &src_flags, -1);

			/* can't dnd onto itself or below itself - bad things happen,
			 * no point dragging to where we were either */
			if (gtk_tree_path_compare (path, src_path) == 0
			    || gtk_tree_path_is_descendant (path, src_path)
			    || (gtk_tree_path_is_ancestor (path, src_path)
				&& gtk_tree_path_get_depth (path) ==
				   gtk_tree_path_get_depth (src_path) - 1)) {
				gtk_tree_path_free (src_path);
				goto done;
			}

			gtk_tree_path_free (src_path);

			if ((src_flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_INBOX ||
			    (src_flags & CAMEL_FOLDER_SYSTEM) != 0) {
				/* allow only copy of the Inbox and other system folders */
				GdkAtom xfolder;

				/* force copy for special local folders */
				*suggested_action = GDK_ACTION_COPY;
				*actions = GDK_ACTION_COPY;
				xfolder = drop_atoms[DND_DROP_TYPE_FOLDER];
				while (targets != NULL) {
					if (targets->data == (gpointer) xfolder) {
						atom = xfolder;
						goto done;
					}

					targets = targets->next;
				}

				goto done;
			}
		}
	}

	/* Check for special sources, and vfolder stuff */
	if (src_store != NULL && src_full_name != NULL) {

		uid = camel_service_get_uid (CAMEL_SERVICE (src_store));
		src_is_vfolder = (g_strcmp0 (uid, "vfolder") == 0);

		/* FIXME: this is a total hack, but i think all we can do at present */
		/* Check for dragging from special folders which can't be moved/copied */

		/* Don't allow moving any of the the special local folders. */
		if (src_store == local && is_special_local_folder (src_full_name)) {
			GdkAtom xfolder;

			/* force copy for special local folders */
			*suggested_action = GDK_ACTION_COPY;
			*actions = GDK_ACTION_COPY;
			xfolder = drop_atoms[DND_DROP_TYPE_FOLDER];
			while (targets != NULL) {
				if (targets->data == (gpointer) xfolder) {
					atom = xfolder;
					goto done;
				}

				targets = targets->next;
			}

			goto done;
		}

		/* Don't allow copying/moving the UNMATCHED vfolder. */
		if (src_is_vfolder)
			if (g_strcmp0 (src_full_name, CAMEL_UNMATCHED_NAME) == 0)
				goto done;

		/* Don't allow copying/moving any vTrash folder. */
		if (g_strcmp0 (src_full_name, CAMEL_VTRASH_NAME) == 0)
			goto done;

		/* Don't allow copying/moving any vJunk folder. */
		if (g_strcmp0 (src_full_name, CAMEL_VJUNK_NAME) == 0)
			goto done;

		/* Don't allow copying/moving any maildir 'inbox'. */
		if (g_strcmp0 (src_full_name, ".") == 0)
			goto done;

		/* Search Folders can only be dropped into other
		 * Search Folders. */
		if (src_is_vfolder) {
			/* force move only for vfolders */
			*suggested_action = GDK_ACTION_MOVE;

			if (dst_is_vfolder) {
				GdkAtom xfolder;

				xfolder = drop_atoms[DND_DROP_TYPE_FOLDER];
				while (targets != NULL) {
					if (targets->data == (gpointer) xfolder) {
						atom = xfolder;
						goto done;
					}

					targets = targets->next;
				}
			}

			goto done;
		}
	}

	/* Can't drag anything but a Search Folder into a Search Folder. */
	if (dst_is_vfolder)
		goto done;

	/* Now we either have a store or a normal folder. */

	if (is_store) {
		GdkAtom xfolder;

		xfolder = drop_atoms[DND_DROP_TYPE_FOLDER];
		while (targets != NULL) {
			if (targets->data == (gpointer) xfolder) {
				atom = xfolder;
				goto done;
			}

			targets = targets->next;
		}
	} else {
		gint i;

		while (targets != NULL) {
			for (i = 0; i < NUM_DROP_TYPES; i++) {
				if (targets->data == (gpointer) drop_atoms[i]) {
					atom = drop_atoms[i];
					goto done;
				}
			}

			targets = targets->next;
		}
	}

 done:

	g_free (dst_full_name);
	g_free (src_full_name);

	return atom;
}

static gboolean
tree_drag_drop (GtkWidget *widget,
                GdkDragContext *context,
                gint x,
                gint y,
                guint time,
                EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	GtkTreeViewColumn *column;
	GtkTreeView *tree_view;
	gint cell_x, cell_y;
	GdkDragAction actions;
	GdkDragAction suggested_action;
	GtkTreePath *path;
	GdkAtom target;

	tree_view = GTK_TREE_VIEW (folder_tree);

	if (priv->autoscroll_id != 0) {
		g_source_remove (priv->autoscroll_id);
		priv->autoscroll_id = 0;
	}

	if (priv->autoexpand_id != 0) {
		gtk_tree_row_reference_free (priv->autoexpand_row);
		priv->autoexpand_row = NULL;

		g_source_remove (priv->autoexpand_id);
		priv->autoexpand_id = 0;
	}

	if (!gtk_tree_view_get_path_at_pos (
		tree_view, x, y, &path, &column, &cell_x, &cell_y))
		return FALSE;

	target = folder_tree_drop_target (
		folder_tree, context, path,
		&actions, &suggested_action);

	gtk_tree_path_free (path);

	return (target != GDK_NONE);
}

static void
tree_drag_end (GtkWidget *widget,
               GdkDragContext *context,
               EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;

	if (priv->drag_row != NULL) {
		gtk_tree_row_reference_free (priv->drag_row);
		priv->drag_row = NULL;
	}

	/* FIXME: undo anything done in drag-begin */
}

static void
tree_drag_leave (GtkWidget *widget,
                 GdkDragContext *context,
                 guint time,
                 EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	GtkTreeView *tree_view;

	tree_view = GTK_TREE_VIEW (folder_tree);

	if (priv->autoscroll_id != 0) {
		g_source_remove (priv->autoscroll_id);
		priv->autoscroll_id = 0;
	}

	if (priv->autoexpand_id != 0) {
		gtk_tree_row_reference_free (priv->autoexpand_row);
		priv->autoexpand_row = NULL;

		g_source_remove (priv->autoexpand_id);
		priv->autoexpand_id = 0;
	}

	gtk_tree_view_set_drag_dest_row (
		tree_view, NULL, GTK_TREE_VIEW_DROP_BEFORE);
}

#define SCROLL_EDGE_SIZE 15

static gboolean
tree_autoscroll (EMFolderTree *folder_tree)
{
	GtkAdjustment *adjustment;
	GtkTreeView *tree_view;
	GdkRectangle rect;
	GdkWindow *window;
	gdouble value;
	gint offset, y;

	/* Get the y pointer position relative to the treeview. */
	tree_view = GTK_TREE_VIEW (folder_tree);
	window = gtk_tree_view_get_bin_window (tree_view);
	gdk_window_get_pointer (window, NULL, &y, NULL);

	/* Rect is in coorinates relative to the scrolled window,
	 * relative to the treeview. */
	gtk_tree_view_get_visible_rect (tree_view, &rect);

	/* Move y into the same coordinate system as rect. */
	y += rect.y;

	/* See if we are near the top edge. */
	offset = y - (rect.y + 2 * SCROLL_EDGE_SIZE);
	if (offset > 0) {
		/* See if we are near the bottom edge. */
		offset = y - (rect.y + rect.height - 2 * SCROLL_EDGE_SIZE);
		if (offset < 0)
			return TRUE;
	}

	adjustment = gtk_tree_view_get_vadjustment (tree_view);
	value = gtk_adjustment_get_value (adjustment);
	gtk_adjustment_set_value (adjustment, MAX (value + offset, 0.0));

	return TRUE;
}

static gboolean
tree_autoexpand (EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	GtkTreeView *tree_view;
	GtkTreePath *path;

	tree_view = GTK_TREE_VIEW (folder_tree);
	path = gtk_tree_row_reference_get_path (priv->autoexpand_row);
	gtk_tree_view_expand_row (tree_view, path, FALSE);
	gtk_tree_path_free (path);

	return TRUE;
}

static gboolean
tree_drag_motion (GtkWidget *widget,
                  GdkDragContext *context,
                  gint x,
                  gint y,
                  guint time,
                  EMFolderTree *folder_tree)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	GtkTreeViewDropPosition pos;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	GdkDragAction actions;
	GdkDragAction suggested_action;
	GdkDragAction chosen_action = 0;
	GtkTreePath *path = NULL;
	GtkTreeIter iter;
	GdkAtom target;
	gint i;

	tree_view = GTK_TREE_VIEW (folder_tree);
	model = gtk_tree_view_get_model (tree_view);

	if (!gtk_tree_view_get_dest_row_at_pos (tree_view, x, y, &path, &pos))
		return FALSE;

	if (priv->autoscroll_id == 0)
		priv->autoscroll_id = g_timeout_add (
			150, (GSourceFunc) tree_autoscroll, folder_tree);

	gtk_tree_model_get_iter (model, &iter, path);

	if (gtk_tree_model_iter_has_child (model, &iter) &&
		!gtk_tree_view_row_expanded (tree_view, path)) {

		if (priv->autoexpand_id != 0) {
			GtkTreePath *autoexpand_path;

			autoexpand_path = gtk_tree_row_reference_get_path (priv->autoexpand_row);
			if (gtk_tree_path_compare (autoexpand_path, path) != 0) {
				/* row changed, restart timer */
				gtk_tree_row_reference_free (priv->autoexpand_row);
				priv->autoexpand_row = gtk_tree_row_reference_new (model, path);
				g_source_remove (priv->autoexpand_id);
				priv->autoexpand_id = g_timeout_add (
					600, (GSourceFunc)
					tree_autoexpand, folder_tree);
			}

			gtk_tree_path_free (autoexpand_path);
		} else {
			priv->autoexpand_id = g_timeout_add (
				600, (GSourceFunc)
				tree_autoexpand, folder_tree);
			priv->autoexpand_row = gtk_tree_row_reference_new (model, path);
		}
	} else if (priv->autoexpand_id != 0) {
		gtk_tree_row_reference_free (priv->autoexpand_row);
		priv->autoexpand_row = NULL;

		g_source_remove (priv->autoexpand_id);
		priv->autoexpand_id = 0;
	}

	target = folder_tree_drop_target (
		folder_tree, context, path,
		&actions, &suggested_action);
	for (i = 0; target != GDK_NONE && i < NUM_DROP_TYPES; i++) {
		if (drop_atoms[i] != target)
			continue;
		switch (i) {
			case DND_DROP_TYPE_UID_LIST:
			case DND_DROP_TYPE_FOLDER:
				chosen_action = suggested_action;
				if (chosen_action == GDK_ACTION_COPY && (actions & GDK_ACTION_MOVE))
					chosen_action = GDK_ACTION_MOVE;
				gtk_tree_view_set_drag_dest_row (
					tree_view, path,
					GTK_TREE_VIEW_DROP_INTO_OR_AFTER);
				break;
			default:
				gtk_tree_view_set_drag_dest_row (
					tree_view, path,
					GTK_TREE_VIEW_DROP_INTO_OR_AFTER);
				chosen_action = suggested_action;
				break;
		}

		break;
	}

	gdk_drag_status (context, chosen_action, time);
	gtk_tree_path_free (path);

	return chosen_action != 0;
}

void
em_folder_tree_enable_drag_and_drop (EMFolderTree *folder_tree)
{
	GtkTreeView *tree_view;
	static gint setup = 0;
	gint i;

	g_return_if_fail (EM_IS_FOLDER_TREE (folder_tree));

	tree_view = GTK_TREE_VIEW (folder_tree);

	if (!setup) {
		for (i = 0; i < NUM_DRAG_TYPES; i++)
			drag_atoms[i] = gdk_atom_intern (drag_types[i].target, FALSE);

		for (i = 0; i < NUM_DROP_TYPES; i++)
			drop_atoms[i] = gdk_atom_intern (drop_types[i].target, FALSE);

		setup = 1;
	}

	gtk_drag_source_set (
		GTK_WIDGET (tree_view), GDK_BUTTON1_MASK,drag_types,
		NUM_DRAG_TYPES, GDK_ACTION_COPY | GDK_ACTION_MOVE);
	gtk_drag_dest_set (
		GTK_WIDGET (tree_view), GTK_DEST_DEFAULT_ALL, drop_types,
		NUM_DROP_TYPES, GDK_ACTION_COPY | GDK_ACTION_MOVE);

	g_signal_connect (
		tree_view, "drag-begin",
		G_CALLBACK (tree_drag_begin), folder_tree);
	g_signal_connect (
		tree_view, "drag-data-delete",
		G_CALLBACK (tree_drag_data_delete), folder_tree);
	g_signal_connect (
		tree_view, "drag-data-get",
		G_CALLBACK (tree_drag_data_get), folder_tree);
	g_signal_connect (
		tree_view, "drag-data-received",
		G_CALLBACK (tree_drag_data_received), folder_tree);
	g_signal_connect (
		tree_view, "drag-drop",
		G_CALLBACK (tree_drag_drop), folder_tree);
	g_signal_connect (
		tree_view, "drag-end",
		G_CALLBACK (tree_drag_end), folder_tree);
	g_signal_connect (
		tree_view, "drag-leave",
		G_CALLBACK (tree_drag_leave), folder_tree);
	g_signal_connect (
		tree_view, "drag-motion",
		G_CALLBACK (tree_drag_motion), folder_tree);
}

void
em_folder_tree_set_excluded (EMFolderTree *folder_tree,
                             guint32 flags)
{
	g_return_if_fail (EM_IS_FOLDER_TREE (folder_tree));

	folder_tree->priv->excluded = flags;
}

void
em_folder_tree_set_excluded_func (EMFolderTree *folder_tree,
                                  EMFTExcludeFunc exclude,
                                  gpointer data)
{
	g_return_if_fail (EM_IS_FOLDER_TREE (folder_tree));
	g_return_if_fail (exclude != NULL);

	folder_tree->priv->excluded_func = exclude;
	folder_tree->priv->excluded_data = data;
}

GList *
em_folder_tree_get_selected_uris (EMFolderTree *folder_tree)
{
	GtkTreeSelection *selection;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	GList *list = NULL, *rows, *l;
	GSList *sl;

	tree_view = GTK_TREE_VIEW (folder_tree);
	selection = gtk_tree_view_get_selection (tree_view);

	/* at first, add lost uris */
	for (sl = folder_tree->priv->select_uris; sl; sl = g_slist_next (sl)) {
		const gchar *uri;
		uri = ((struct _selected_uri *) sl->data)->uri;
		list = g_list_append (list, g_strdup (uri));
	}

	rows = gtk_tree_selection_get_selected_rows (selection, &model);
	for (l = rows; l; l = g_list_next (l)) {
		GtkTreeIter iter;
		GtkTreePath *path = l->data;

		if (gtk_tree_model_get_iter (model, &iter, path)) {
			gchar *uri;

			gtk_tree_model_get (model, &iter, COL_STRING_URI, &uri, -1);
			list = g_list_prepend (list, uri);
		}
		gtk_tree_path_free (path);
	}
	g_list_free (rows);

	return g_list_reverse (list);
}

static void
get_selected_uris_path_iterate (GtkTreeModel *model,
                                GtkTreePath *treepath,
                                GtkTreeIter *iter,
                                gpointer data)
{
	GList **list = (GList **) data;
	gchar *full_name;

	gtk_tree_model_get (model, iter, COL_STRING_FULL_NAME, &full_name, -1);
	*list = g_list_append (*list, full_name);
}

GList *
em_folder_tree_get_selected_paths (EMFolderTree *folder_tree)
{
	GtkTreeSelection *selection;
	GtkTreeView *tree_view;
	GList *list = NULL;

	tree_view = GTK_TREE_VIEW (folder_tree);
	selection = gtk_tree_view_get_selection (tree_view);

	gtk_tree_selection_selected_foreach (
		selection, get_selected_uris_path_iterate, &list);

	return list;
}

void
em_folder_tree_set_selected_list (EMFolderTree *folder_tree,
                                  GList *list,
                                  gboolean expand_only)
{
	EMFolderTreePrivate *priv = folder_tree->priv;
	EMailBackend *backend;
	EMailSession *session;

	backend = em_folder_tree_get_backend (folder_tree);
	session = e_mail_backend_get_session (backend);

	/* FIXME: need to remove any currently selected stuff? */
	if (!expand_only)
		folder_tree_clear_selected_list (folder_tree);

	for (; list; list = list->next) {
		CamelStore *store;
		struct _selected_uri *u;
		const gchar *folder_uri;
		const gchar *uid;
		gchar *folder_name;
		gchar *expand_key;
		gchar *end;
		gboolean success;

		/* This makes sure all our parents up to the root are
		 * expanded. */

		folder_uri = list->data;

		success = e_mail_folder_uri_parse (
			CAMEL_SESSION (session), folder_uri,
			&store, &folder_name, NULL);

		if (!success)
			continue;

		uid = camel_service_get_uid (CAMEL_SERVICE (store));
		expand_key = g_strdup_printf ("%s/%s", uid, folder_name);
		g_free (folder_name);

		u = g_malloc0 (sizeof (*u));
		u->uri = g_strdup (folder_uri);
		u->service = CAMEL_SERVICE (store);  /* takes ownership */
		u->key = g_strdup (expand_key);

		if (!expand_only) {
			g_hash_table_insert (
				priv->select_uris_table, u->key, u);
			priv->select_uris =
				g_slist_append (priv->select_uris, u);
		}

		end = strrchr (expand_key, '/');
		do {
			folder_tree_expand_node (expand_key, folder_tree);
			*end = 0;
			end = strrchr (expand_key, '/');
		} while (end);

		g_free (expand_key);
	}
}

#if 0
static void
dump_fi (CamelFolderInfo *fi,
         gint depth)
{
	gint i;

	while (fi != NULL) {
		for (i = 0; i < depth; i++)
			fputs ("  ", stdout);

		printf ("path='%s'; full_name='%s'\n", fi->path, fi->full_name);

		if (fi->child)
			dump_fi (fi->child, depth + 1);

		fi = fi->sibling;
	}
}
#endif

void
em_folder_tree_set_selected (EMFolderTree *folder_tree,
                             const gchar *uri,
                             gboolean expand_only)
{
	GList *l = NULL;

	g_return_if_fail (EM_IS_FOLDER_TREE (folder_tree));

	if (uri && uri[0])
		l = g_list_append (l, (gpointer) uri);

	em_folder_tree_set_selected_list (folder_tree, l, expand_only);
	g_list_free (l);
}

void
em_folder_tree_select_next_path (EMFolderTree *folder_tree,
                                 gboolean skip_read_folders)
{
	GtkTreeView *tree_view;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter, parent, child;
	GtkTreePath *current_path, *path = NULL;
	guint unread = 0;
	EMFolderTreePrivate *priv = folder_tree->priv;

	g_return_if_fail (EM_IS_FOLDER_TREE (folder_tree));

	tree_view = GTK_TREE_VIEW (folder_tree);
	selection = gtk_tree_view_get_selection (tree_view);
	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {

		current_path = gtk_tree_model_get_path (model, &iter);

		do {
		if (gtk_tree_model_iter_has_child (model, &iter)) {
			gtk_tree_model_iter_children (model, &child, &iter);
			path = gtk_tree_model_get_path (model, &child);
			iter = child;
		} else {
			while (1) {
				gboolean has_parent;

				has_parent = gtk_tree_model_iter_parent (
					model, &parent, &iter);

				if (gtk_tree_model_iter_next (model, &iter)) {
					path = gtk_tree_model_get_path (model, &iter);
					break;
				} else {
					if (has_parent) {
						iter =  parent;
					} else {
						/* Reached end. Wrapup*/
						gtk_tree_model_get_iter_first (model, &iter);
						path = gtk_tree_model_get_path (model, &iter);
						break;
					}
				}
			}
		}
		gtk_tree_model_get (model, &iter, COL_UINT_UNREAD, &unread, -1);

		/* TODO : Flags here for better options */
		} while (skip_read_folders && unread <=0 &&
			gtk_tree_path_compare (current_path, path));
	}

	if (path) {
		if (!gtk_tree_view_row_expanded (tree_view, path))
			gtk_tree_view_expand_to_path (tree_view, path);

		gtk_tree_selection_select_path (selection, path);

		if (!priv->cursor_set) {
			gtk_tree_view_set_cursor (tree_view, path, NULL, FALSE);
			priv->cursor_set = TRUE;
		}
		gtk_tree_view_scroll_to_cell (tree_view, path, NULL, TRUE, 0.5f, 0.0f);
	}
	return;
}

static gboolean
folder_tree_descend (GtkTreeModel *model,
                     GtkTreeIter *iter,
                     GtkTreeIter *root)
{
	GtkTreeIter parent;
	gint n_children;

	/* Finds the rightmost descendant of the given root. */

	if (root == NULL) {
		n_children = gtk_tree_model_iter_n_children (model, NULL);

		/* This will invalidate the iterator and return FALSE. */
		if (n_children == 0)
			return gtk_tree_model_get_iter_first (model, iter);

		gtk_tree_model_iter_nth_child (
			model, &parent, NULL, n_children - 1);
	} else
		parent = *root;

	n_children = gtk_tree_model_iter_n_children (model, &parent);

	while (n_children > 0) {
		GtkTreeIter child;

		gtk_tree_model_iter_nth_child (
			model, &child, &parent, n_children - 1);

		parent = child;

		n_children = gtk_tree_model_iter_n_children (model, &parent);
	}

	*iter = parent;

	return TRUE;
}

void
em_folder_tree_select_prev_path (EMFolderTree *folder_tree,
                                 gboolean skip_read_folders)
{
	GtkTreeView *tree_view;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreePath *path = NULL;
	GtkTreePath *sentinel;
	GtkTreeIter iter;
	guint unread = 0;
	EMFolderTreePrivate *priv = folder_tree->priv;

	g_return_if_fail (EM_IS_FOLDER_TREE (folder_tree));

	tree_view = GTK_TREE_VIEW (folder_tree);
	selection = gtk_tree_view_get_selection (tree_view);

	/* Nothing selected means nothing to do. */
	if (!gtk_tree_selection_get_selected (selection, &model, &iter))
		return;

	/* This prevents us from looping over the model indefinitely,
	 * looking for unread messages when there are none. */
	sentinel = gtk_tree_model_get_path (model, &iter);

	do {
		GtkTreeIter descendant;

		if (path != NULL)
			gtk_tree_path_free (path);

		path = gtk_tree_model_get_path (model, &iter);

		if (gtk_tree_path_prev (path)) {
			gtk_tree_model_get_iter (model, &iter, path);
			folder_tree_descend (model, &descendant, &iter);

			gtk_tree_path_free (path);
			path = gtk_tree_model_get_path (model, &descendant);

		} else if (gtk_tree_path_get_depth (path) > 1) {
			gtk_tree_path_up (path);

		} else {
			folder_tree_descend (model, &descendant, NULL);

			gtk_tree_path_free (path);
			path = gtk_tree_model_get_path (model, &descendant);
		}

		gtk_tree_model_get_iter (model, &iter, path);
		gtk_tree_model_get (model, &iter, COL_UINT_UNREAD, &unread, -1);

	} while (skip_read_folders && unread <= 0 &&
		gtk_tree_path_compare (path, sentinel) != 0);

	if (!gtk_tree_view_row_expanded (tree_view, path))
		gtk_tree_view_expand_to_path (tree_view, path);

	gtk_tree_selection_select_path (selection, path);

	if (!priv->cursor_set) {
		gtk_tree_view_set_cursor (tree_view, path, NULL, FALSE);
		priv->cursor_set = TRUE;
	}

	gtk_tree_view_scroll_to_cell (
		tree_view, path, NULL, TRUE, 0.5f, 0.0f);

	gtk_tree_path_free (sentinel);
	gtk_tree_path_free (path);
}

void
em_folder_tree_edit_selected (EMFolderTree *folder_tree)
{
	GtkTreeSelection *selection;
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	GtkTreePath *path = NULL;
	GtkTreeIter iter;

	g_return_if_fail (EM_IS_FOLDER_TREE (folder_tree));

	tree_view = GTK_TREE_VIEW (folder_tree);
	column = gtk_tree_view_get_column (tree_view, 0);
	selection = gtk_tree_view_get_selection (tree_view);
	renderer = folder_tree->priv->text_renderer;

	if (gtk_tree_selection_get_selected (selection, &model, &iter))
		path = gtk_tree_model_get_path (model, &iter);

	if (path == NULL)
		return;

	/* Make the text cell renderer editable, but only temporarily.
	 * We don't want editing to be activated by simply clicking on
	 * the folder name.  Too easy for accidental edits to occur. */
	g_object_set (renderer, "editable", TRUE, NULL);
	gtk_tree_view_expand_to_path (tree_view, path);
	gtk_tree_view_set_cursor_on_cell (
		tree_view, path, column, renderer, TRUE);
	g_object_set (renderer, "editable", FALSE, NULL);

	gtk_tree_path_free (path);
}

gboolean
em_folder_tree_get_selected (EMFolderTree *folder_tree,
                             CamelStore **out_store,
                             gchar **out_folder_name)
{
	GtkTreeView *tree_view;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;
	CamelStore *store = NULL;
	gchar *folder_name = NULL;

	g_return_val_if_fail (EM_IS_FOLDER_TREE (folder_tree), FALSE);

	tree_view = GTK_TREE_VIEW (folder_tree);
	selection = gtk_tree_view_get_selection (tree_view);

	if (!gtk_tree_selection_get_selected (selection, &model, &iter))
		return FALSE;

	gtk_tree_model_get (
		model, &iter,
		COL_POINTER_CAMEL_STORE, &store,
		COL_STRING_FULL_NAME, &folder_name, -1);

	/* We should always get a valid store. */
	g_return_val_if_fail (CAMEL_IS_STORE (store), FALSE);

	/* If a store is selected, the folder name will be NULL.
	 * Treat this as though nothing is selected, so that callers
	 * can assume a TRUE return value means a folder is selected. */
	if (folder_name == NULL)
		return FALSE;

	/* FIXME We really should be storing the CamelStore as a GObject
	 *       so it gets referenced.  The pointer type is a relic of
	 *       days before Camel used GObject. */
	if (out_store != NULL)
		*out_store = g_object_ref (store);

	if (out_folder_name != NULL)
		*out_folder_name = folder_name;
	else
		g_free (folder_name);

	return TRUE;
}

gboolean
em_folder_tree_store_root_selected (EMFolderTree *folder_tree,
                                    CamelStore **out_store)
{
	GtkTreeView *tree_view;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;
	CamelStore *store = NULL;
	gboolean is_store = FALSE;

	g_return_val_if_fail (folder_tree != NULL, FALSE);
	g_return_val_if_fail (EM_IS_FOLDER_TREE (folder_tree), FALSE);

	tree_view = GTK_TREE_VIEW (folder_tree);
	selection = gtk_tree_view_get_selection (tree_view);

	if (!gtk_tree_selection_get_selected (selection, &model, &iter))
		return FALSE;

	gtk_tree_model_get (
		model, &iter,
		COL_POINTER_CAMEL_STORE, &store,
		COL_BOOL_IS_STORE, &is_store, -1);

	/* We should always get a valid store. */
	g_return_val_if_fail (CAMEL_IS_STORE (store), FALSE);

	if (!is_store)
		return FALSE;

	if (out_store != NULL)
		*out_store = g_object_ref (store);

	return TRUE;
}

gchar *
em_folder_tree_get_selected_uri (EMFolderTree *folder_tree)
{
	GtkTreeView *tree_view;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;
	gchar *uri = NULL;

	g_return_val_if_fail (EM_IS_FOLDER_TREE (folder_tree), NULL);

	tree_view = GTK_TREE_VIEW (folder_tree);
	selection = gtk_tree_view_get_selection (tree_view);

	if (!gtk_tree_selection_get_selected (selection, &model, &iter))
		return NULL;

	gtk_tree_model_get (model, &iter, COL_STRING_URI, &uri, -1);

	return uri;
}

CamelFolder *
em_folder_tree_get_selected_folder (EMFolderTree *folder_tree)
{
	CamelFolder *folder;
	CamelStore *store;
	gchar *full_name;

	g_return_val_if_fail (EM_IS_FOLDER_TREE (folder_tree), NULL);

	if (!em_folder_tree_get_selected (folder_tree, &store, &full_name))
		return NULL;

	/* FIXME camel_store_get_folder_sync() may block. */
	folder = camel_store_get_folder_sync (
		store, full_name, CAMEL_STORE_FOLDER_INFO_FAST, NULL, NULL);

	g_object_unref (store);
	g_free (full_name);

	return folder;
}

CamelStore *
em_folder_tree_get_selected_store (EMFolderTree *folder_tree)
{
	GtkTreeView *tree_view;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;
	CamelStore *store = NULL;

	g_return_val_if_fail (EM_IS_FOLDER_TREE (folder_tree), NULL);

	/* Don't use em_folder_tree_get_selected() here because we
	 * want this to work whether a folder or store is selected. */

	tree_view = GTK_TREE_VIEW (folder_tree);
	selection = gtk_tree_view_get_selection (tree_view);

	if (gtk_tree_selection_get_selected (selection, &model, &iter))
		gtk_tree_model_get (
			model, &iter,
			COL_POINTER_CAMEL_STORE, &store, -1);

	return CAMEL_IS_STORE (store) ? store : NULL;
}

void
em_folder_tree_set_skip_double_click (EMFolderTree *folder_tree,
                                      gboolean skip)
{
	folder_tree->priv->skip_double_click = skip;
}

/* stores come first, then by uri */
static gint
sort_by_store_and_uri (gconstpointer name1,
                       gconstpointer name2)
{
	const gchar *n1 = name1, *n2 = name2;
	gboolean is_store1, is_store2;

	if (n1 == NULL || n2 == NULL) {
		if (n1 == n2)
			return 0;
		else
			return n1 ? -1 : 1;
	}

	is_store1 = g_str_has_prefix (n1, "Store ");
	is_store2 = g_str_has_prefix (n2, "Store ");

	if ((is_store1 || is_store2) && (!is_store1 || !is_store2)) {
		return is_store1 ? -1 : 1;
	}

	return strcmp (n1, n2);
}

/* restores state of a tree (collapsed/expanded) as stores in the given key_file */
void
em_folder_tree_restore_state (EMFolderTree *folder_tree,
                              GKeyFile *key_file)
{
	EShell *shell;
	GtkTreeModel *tree_model;
	GtkTreeView *tree_view;
	GtkTreeIter iter;
	gboolean valid;
	gchar **groups_arr;
	GSList *groups, *group;
	gint ii;

	/* Make sure we have a key file to restore state from. */
	if (key_file == NULL)
		return;

	/* XXX Pass this in. */
	shell = e_shell_get_default ();

	tree_view = GTK_TREE_VIEW (folder_tree);
	tree_model = gtk_tree_view_get_model (tree_view);

	/* Set the initial folder tree expanded state in two stages:
	 *
	 * 1) Iterate over the "Store" and "Folder" state file groups
	 *    and apply the "Expanded" keys where possible.
	 *
	 * 2) Iterate over the top-level nodes in the folder tree
	 *    (these are all stores) and expand those that have no
	 *    corresponding "Expanded" key in the state file.  This
	 *    ensures that new stores are expanded by default.
	 */

	/* Stage 1 */

	/* Collapse all so we have a clean slate. */
	gtk_tree_view_collapse_all (tree_view);

	groups_arr = g_key_file_get_groups (key_file, NULL);
	groups = NULL;

	for (ii = 0; groups_arr[ii] != NULL; ii++) {
		groups = g_slist_prepend (groups, groups_arr[ii]);
	}

	groups = g_slist_sort (groups, sort_by_store_and_uri);

	for (group = groups; group != NULL; group = group->next) {
		GtkTreeRowReference *reference;
		GtkTreePath *path;
		GtkTreeIter iter;
		const gchar *group_name = group->data;
		const gchar *key = STATE_KEY_EXPANDED;
		const gchar *uri;
		gboolean expanded;

		if (g_str_has_prefix (group_name, "Store ")) {
			uri = group_name + 6;
			expanded = TRUE;
		} else if (g_str_has_prefix (group_name, "Folder ")) {
			uri = group_name + 7;
			expanded = FALSE;
		} else
			continue;

		if (g_key_file_has_key (key_file, group_name, key, NULL))
			expanded = g_key_file_get_boolean (
				key_file, group_name, key, NULL);

		if (!expanded)
			continue;

		reference = em_folder_tree_model_lookup_uri (
			EM_FOLDER_TREE_MODEL (tree_model), uri);
		if (reference == NULL)
			continue;

		path = gtk_tree_row_reference_get_path (reference);
		gtk_tree_model_get_iter (tree_model, &iter, path);
		gtk_tree_view_expand_row (tree_view, path, FALSE);
		gtk_tree_path_free (path);
	}

	g_slist_free (groups);
	g_strfreev (groups_arr);

	/* Stage 2 */

	valid = gtk_tree_model_get_iter_first (tree_model, &iter);

	while (valid) {
		const gchar *key = STATE_KEY_EXPANDED;
		gboolean expand_row;
		gchar *group_name;
		gchar *uri;

		gtk_tree_model_get (
			tree_model, &iter, COL_STRING_URI, &uri, -1);

		if (uri == NULL)
			goto next;

		group_name = g_strdup_printf ("Store %s", uri);

		/* Expand stores that have no "Expanded" key. */
		expand_row = !g_key_file_has_key (
			key_file, group_name, key, NULL);

		/* Do not expand local stores in Express mode. */
		if (e_shell_get_express_mode (shell)) {
			expand_row &= (strncmp (uri, "vfolder", 7) != 0);
			expand_row &= (strncmp (uri, "maildir", 7) != 0);
		}

		if (expand_row) {
			GtkTreePath *path;

			path = gtk_tree_model_get_path (tree_model, &iter);
			gtk_tree_view_expand_row (tree_view, path, FALSE);
			gtk_tree_path_free (path);
		}

		g_free (group_name);
		g_free (uri);

	next:
		valid = gtk_tree_model_iter_next (tree_model, &iter);
	}
}

/*
 * em-subscription-editor.c
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "em-subscription-editor.h"

#include <string.h>
#include <glib/gi18n-lib.h>

#include "mail-tools.h"
#include "mail-ops.h"
#include "mail-mt.h"

#include <e-util/e-util.h>
#include <e-util/e-account-utils.h>
#include <e-util/e-util-private.h>
#include <e-util/gconf-bridge.h>

#include "em-folder-utils.h"

#define FOLDER_CAN_SELECT(folder_info) \
	((folder_info) != NULL && \
	((folder_info)->flags & CAMEL_FOLDER_NOSELECT) == 0)
#define FOLDER_SUBSCRIBED(folder_info) \
	((folder_info) != NULL && \
	((folder_info)->flags & CAMEL_FOLDER_SUBSCRIBED) != 0)

#define EM_SUBSCRIPTION_EDITOR_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), EM_TYPE_SUBSCRIPTION_EDITOR, EMSubscriptionEditorPrivate))

typedef struct _AsyncContext AsyncContext;
typedef struct _StoreData StoreData;

struct _EMSubscriptionEditorPrivate {
	EMailBackend *backend;
	CamelStore *initial_store;

	GtkWidget *combo_box;		/* not referenced */
	GtkWidget *entry;		/* not referenced */
	GtkWidget *notebook;		/* not referenced */
	GtkWidget *subscribe_button;	/* not referenced */
	GtkWidget *unsubscribe_button;	/* not referenced */
	GtkWidget *collapse_all_button;	/* not referenced */
	GtkWidget *expand_all_button;	/* not referenced */
	GtkWidget *refresh_button;	/* not referenced */
	GtkWidget *stop_button;		/* not referenced */

	/* Indicies coincide with the combo box. */
	GPtrArray *stores;

	/* Points at an item in the stores array. */
	StoreData *active;

	/* Casefolded search string. */
	gchar *search_string;

	guint timeout_id;
};

struct _AsyncContext {
	EMSubscriptionEditor *editor;
	CamelFolderInfo *folder_info;
	GtkTreeRowReference *reference;
};

struct _StoreData {
	CamelStore *store;
	GtkTreeView *tree_view;
	GtkTreeModel *list_store;
	GtkTreeModel *tree_store;
	GCancellable *cancellable;
	CamelFolderInfo *folder_info;
	gboolean filtered_view;
	gboolean needs_refresh;
};

enum {
	PROP_0,
	PROP_BACKEND,
	PROP_STORE
};

enum {
	COL_CASEFOLDED,		/* G_TYPE_STRING  */
	COL_FOLDER_NAME,	/* G_TYPE_STRING  */
	COL_FOLDER_ICON,	/* G_TYPE_STRING  */
	COL_FOLDER_INFO,	/* G_TYPE_POINTER */
	N_COLUMNS
};

G_DEFINE_TYPE (EMSubscriptionEditor, em_subscription_editor, GTK_TYPE_DIALOG)

static void
async_context_free (AsyncContext *context)
{
	g_object_unref (context->editor);
	gtk_tree_row_reference_free (context->reference);

	g_slice_free (AsyncContext, context);
}

static void
store_data_free (StoreData *data)
{
	if (data->store != NULL)
		g_object_unref (data->store);

	if (data->tree_view != NULL)
		g_object_unref (data->tree_view);

	if (data->list_store != NULL)
		g_object_unref (data->list_store);

	if (data->tree_store != NULL)
		g_object_unref (data->tree_store);

	if (data->cancellable != NULL) {
		g_cancellable_cancel (data->cancellable);
		g_object_unref (data->cancellable);
	}

	camel_folder_info_free (data->folder_info);

	g_slice_free (StoreData, data);
}

static void
subscription_editor_populate (EMSubscriptionEditor *editor,
                              CamelFolderInfo *folder_info,
                              GtkTreeIter *parent,
                              GSList **expand_paths)
{
	GtkListStore *list_store;
	GtkTreeStore *tree_store;

	list_store = GTK_LIST_STORE (editor->priv->active->list_store);
	tree_store = GTK_TREE_STORE (editor->priv->active->tree_store);

	while (folder_info != NULL) {
		GtkTreeIter iter;
		const gchar *icon_name;
		gchar *casefolded;

		icon_name =
			em_folder_utils_get_icon_name (folder_info->flags);

		casefolded = g_utf8_casefold (folder_info->full_name, -1);

		gtk_list_store_append (list_store, &iter);

		gtk_list_store_set (
			list_store, &iter,
			COL_CASEFOLDED, casefolded,
			COL_FOLDER_ICON, icon_name,
			COL_FOLDER_NAME, folder_info->full_name,
			COL_FOLDER_INFO, folder_info, -1);

		gtk_tree_store_append (tree_store, &iter, parent);

		gtk_tree_store_set (
			tree_store, &iter,
			COL_CASEFOLDED, NULL,  /* not needed */
			COL_FOLDER_ICON, icon_name,
			COL_FOLDER_NAME, folder_info->display_name,
			COL_FOLDER_INFO, folder_info, -1);

		if (FOLDER_SUBSCRIBED (folder_info)) {
			GtkTreePath *path;

			path = gtk_tree_model_get_path (
				GTK_TREE_MODEL (tree_store), &iter);
			*expand_paths = g_slist_prepend (*expand_paths, path);
		}

		g_free (casefolded);

		if (folder_info->child != NULL)
			subscription_editor_populate (
				editor, folder_info->child, &iter, expand_paths);

		folder_info = folder_info->next;
	}
}

static void
expand_paths_cb (gpointer path,
                 gpointer tree_view)
{
	gtk_tree_view_expand_to_path (tree_view, path);
}

static void
subscription_editor_get_folder_info_done (CamelStore *store,
                                          GAsyncResult *result,
                                          EMSubscriptionEditor *editor)
{
	GtkTreePath *path;
	GtkTreeView *tree_view;
	GtkTreeModel *list_store;
	GtkTreeModel *tree_store;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	CamelFolderInfo *folder_info;
	GdkWindow *window;
	GSList *expand_paths = NULL;
	GError *error = NULL;

	folder_info = camel_store_get_folder_info_finish (
		store, result, &error);

	/* Just return quietly if we were cancelled. */
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_warn_if_fail (folder_info == NULL);
		g_error_free (error);
		goto exit;
	}

	gtk_widget_set_sensitive (editor->priv->notebook, TRUE);
	gtk_widget_set_sensitive (editor->priv->refresh_button, TRUE);
	gtk_widget_set_sensitive (editor->priv->stop_button, FALSE);

	window = gtk_widget_get_window (GTK_WIDGET (editor));
	gdk_window_set_cursor (window, NULL);

	/* XXX Do something smarter with errors. */
	if (error != NULL) {
		g_warn_if_fail (folder_info == NULL);
		g_warning ("%s", error->message);
		g_error_free (error);
		goto exit;
	}

	g_return_if_fail (folder_info != NULL);

	camel_folder_info_free (editor->priv->active->folder_info);
	editor->priv->active->folder_info = folder_info;

	tree_view = editor->priv->active->tree_view;
	list_store = editor->priv->active->list_store;
	tree_store = editor->priv->active->tree_store;

	gtk_list_store_clear (GTK_LIST_STORE (list_store));
	gtk_tree_store_clear (GTK_TREE_STORE (tree_store));

	model = gtk_tree_view_get_model (tree_view);
	gtk_tree_view_set_model (tree_view, NULL);
	subscription_editor_populate (editor, folder_info, NULL, &expand_paths);
	gtk_tree_view_set_model (tree_view, model);

	g_slist_foreach (expand_paths, expand_paths_cb, tree_view);
	g_slist_foreach (expand_paths, (GFunc) gtk_tree_path_free, NULL);
	g_slist_free (expand_paths);

	path = gtk_tree_path_new_first ();
	selection = gtk_tree_view_get_selection (tree_view);
	gtk_tree_selection_select_path (selection, path);
	gtk_tree_path_free (path);

	gtk_widget_grab_focus (GTK_WIDGET (tree_view));

exit:
	g_object_unref (editor);
}

static void
subscription_editor_subscribe_folder_done (CamelSubscribable *subscribable,
                                           GAsyncResult *result,
                                           AsyncContext *context)
{
	GtkTreeView *tree_view;
	GtkTreeModel *tree_model;
	GtkTreeSelection *selection;
	GtkTreePath *path;
	GtkTreeIter iter;
	GdkWindow *window;
	GError *error = NULL;

	camel_subscribable_subscribe_folder_finish (
		subscribable, result, &error);

	/* Just return quietly if we were cancelled. */
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_error_free (error);
		goto exit;
	}

	/* XXX Do something smarter with errors. */
	if (error == NULL)
		context->folder_info->flags |= CAMEL_FOLDER_SUBSCRIBED;
	else {
		g_warning ("%s", error->message);
		g_error_free (error);
	}

	gtk_widget_set_sensitive (context->editor->priv->notebook, TRUE);
	gtk_widget_set_sensitive (context->editor->priv->refresh_button, TRUE);
	gtk_widget_set_sensitive (context->editor->priv->stop_button, FALSE);

	window = gtk_widget_get_window (GTK_WIDGET (context->editor));
	gdk_window_set_cursor (window, NULL);

	/* Update the Subscription/Unsubscription buttons. */
	tree_view = context->editor->priv->active->tree_view;
	selection = gtk_tree_view_get_selection (tree_view);
	g_signal_emit_by_name (selection, "changed");

	/* Update the toggle renderer in the selected row. */
	tree_model = gtk_tree_row_reference_get_model (context->reference);
	path = gtk_tree_row_reference_get_path (context->reference);
	gtk_tree_model_get_iter (tree_model, &iter, path);
	gtk_tree_model_row_changed (tree_model, path, &iter);
	gtk_tree_path_free (path);

exit:
	async_context_free (context);
}

static void
subscription_editor_unsubscribe_folder_done (CamelSubscribable *subscribable,
                                             GAsyncResult *result,
                                             AsyncContext *context)
{
	GtkTreeView *tree_view;
	GtkTreeModel *tree_model;
	GtkTreeSelection *selection;
	GtkTreePath *path;
	GtkTreeIter iter;
	GdkWindow *window;
	GError *error = NULL;

	camel_subscribable_unsubscribe_folder_finish (
		subscribable, result, &error);

	/* Just return quietly if we were cancelled. */
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_error_free (error);
		goto exit;
	}

	/* XXX Do something smarter with errors. */
	if (error == NULL)
		context->folder_info->flags &= ~CAMEL_FOLDER_SUBSCRIBED;
	else {
		g_warning ("%s", error->message);
		g_error_free (error);
	}

	gtk_widget_set_sensitive (context->editor->priv->notebook, TRUE);
	gtk_widget_set_sensitive (context->editor->priv->refresh_button, TRUE);
	gtk_widget_set_sensitive (context->editor->priv->stop_button, FALSE);

	window = gtk_widget_get_window (GTK_WIDGET (context->editor));
	gdk_window_set_cursor (window, NULL);

	/* Update the Subscription/Unsubscription buttons. */
	tree_view = context->editor->priv->active->tree_view;
	selection = gtk_tree_view_get_selection (tree_view);
	g_signal_emit_by_name (selection, "changed");

	/* Update the toggle renderer in the selected row. */
	tree_model = gtk_tree_row_reference_get_model (context->reference);
	path = gtk_tree_row_reference_get_path (context->reference);
	gtk_tree_model_get_iter (tree_model, &iter, path);
	gtk_tree_model_row_changed (tree_model, path, &iter);
	gtk_tree_path_free (path);

exit:
	async_context_free (context);
}

static void
subscription_editor_subscribe (EMSubscriptionEditor *editor)
{
	CamelStore *active_store;
	CamelFolderInfo *folder_info;
	GtkTreeRowReference *reference;
	GtkTreeSelection *selection;
	GtkTreeModel *tree_model;
	GtkTreeView *tree_view;
	GtkTreePath *path;
	GtkTreeIter iter;
	GdkCursor *cursor;
	GdkWindow *window;
	AsyncContext *context;
	gboolean have_selection;

	tree_view = editor->priv->active->tree_view;
	selection = gtk_tree_view_get_selection (tree_view);

	have_selection = gtk_tree_selection_get_selected (
		selection, &tree_model, &iter);
	g_return_if_fail (have_selection);

	/* Cancel any operation on this store still in progress. */
	gtk_button_clicked (GTK_BUTTON (editor->priv->stop_button));

	/* Start a new 'subscription' operation. */
	editor->priv->active->cancellable = g_cancellable_new ();

	gtk_widget_set_sensitive (editor->priv->notebook, FALSE);
	gtk_widget_set_sensitive (editor->priv->subscribe_button, FALSE);
	gtk_widget_set_sensitive (editor->priv->unsubscribe_button, FALSE);
	gtk_widget_set_sensitive (editor->priv->refresh_button, FALSE);
	gtk_widget_set_sensitive (editor->priv->stop_button, TRUE);

	cursor = gdk_cursor_new (GDK_WATCH);
	window = gtk_widget_get_window (GTK_WIDGET (editor));
	gdk_window_set_cursor (window, cursor);
	g_object_unref (cursor);

	gtk_tree_model_get (
		tree_model, &iter, COL_FOLDER_INFO, &folder_info, -1);

	path = gtk_tree_model_get_path (tree_model, &iter);
	reference = gtk_tree_row_reference_new (tree_model, path);
	gtk_tree_path_free (path);

	context = g_slice_new0 (AsyncContext);
	context->editor = g_object_ref (editor);
	context->folder_info = folder_info;
	context->reference = reference;

	active_store = editor->priv->active->store;

	camel_subscribable_subscribe_folder (
		CAMEL_SUBSCRIBABLE (active_store),
		folder_info->full_name, G_PRIORITY_DEFAULT,
		editor->priv->active->cancellable, (GAsyncReadyCallback)
		subscription_editor_subscribe_folder_done, context);
}

static void
subscription_editor_unsubscribe (EMSubscriptionEditor *editor)
{
	CamelStore *active_store;
	CamelFolderInfo *folder_info;
	GtkTreeRowReference *reference;
	GtkTreeSelection *selection;
	GtkTreeModel *tree_model;
	GtkTreeView *tree_view;
	GtkTreePath *path;
	GtkTreeIter iter;
	GdkCursor *cursor;
	GdkWindow *window;
	AsyncContext *context;
	gboolean have_selection;

	tree_view = editor->priv->active->tree_view;
	selection = gtk_tree_view_get_selection (tree_view);

	have_selection = gtk_tree_selection_get_selected (
		selection, &tree_model, &iter);
	g_return_if_fail (have_selection);

	/* Cancel any operation on this store still in progress. */
	gtk_button_clicked (GTK_BUTTON (editor->priv->stop_button));

	/* Start a new 'unsubscription' operation. */
	editor->priv->active->cancellable = g_cancellable_new ();

	gtk_widget_set_sensitive (editor->priv->notebook, FALSE);
	gtk_widget_set_sensitive (editor->priv->subscribe_button, FALSE);
	gtk_widget_set_sensitive (editor->priv->unsubscribe_button, FALSE);
	gtk_widget_set_sensitive (editor->priv->refresh_button, FALSE);
	gtk_widget_set_sensitive (editor->priv->stop_button, TRUE);

	cursor = gdk_cursor_new (GDK_WATCH);
	window = gtk_widget_get_window (GTK_WIDGET (editor));
	gdk_window_set_cursor (window, cursor);
	g_object_unref (cursor);

	gtk_tree_model_get (
		tree_model, &iter, COL_FOLDER_INFO, &folder_info, -1);

	path = gtk_tree_model_get_path (tree_model, &iter);
	reference = gtk_tree_row_reference_new (tree_model, path);
	gtk_tree_path_free (path);

	context = g_slice_new0 (AsyncContext);
	context->editor = g_object_ref (editor);
	context->folder_info = folder_info;
	context->reference = reference;

	active_store = editor->priv->active->store;

	camel_subscribable_unsubscribe_folder (
		CAMEL_SUBSCRIBABLE (active_store),
		folder_info->full_name, G_PRIORITY_DEFAULT,
		editor->priv->active->cancellable, (GAsyncReadyCallback)
		subscription_editor_unsubscribe_folder_done, context);
}

static void
subscription_editor_collapse_all (EMSubscriptionEditor *editor)
{
	gtk_tree_view_collapse_all (editor->priv->active->tree_view);
}

static void
subscription_editor_expand_all (EMSubscriptionEditor *editor)
{
	gtk_tree_view_expand_all (editor->priv->active->tree_view);
}

static void
subscription_editor_refresh (EMSubscriptionEditor *editor)
{
	GdkCursor *cursor;
	GdkWindow *window;

	/* Cancel any operation on this store still in progress. */
	gtk_button_clicked (GTK_BUTTON (editor->priv->stop_button));

	/* Start a new 'refresh' operation. */
	editor->priv->active->cancellable = g_cancellable_new ();

	gtk_widget_set_sensitive (editor->priv->notebook, FALSE);
	gtk_widget_set_sensitive (editor->priv->subscribe_button, FALSE);
	gtk_widget_set_sensitive (editor->priv->unsubscribe_button, FALSE);
	gtk_widget_set_sensitive (editor->priv->refresh_button, FALSE);
	gtk_widget_set_sensitive (editor->priv->stop_button, TRUE);

	cursor = gdk_cursor_new (GDK_WATCH);
	window = gtk_widget_get_window (GTK_WIDGET (editor));
	gdk_window_set_cursor (window, cursor);
	g_object_unref (cursor);

	camel_store_get_folder_info (
		editor->priv->active->store, NULL,
		CAMEL_STORE_FOLDER_INFO_RECURSIVE |
		CAMEL_STORE_FOLDER_INFO_NO_VIRTUAL |
		CAMEL_STORE_FOLDER_INFO_SUBSCRIPTION_LIST,
		G_PRIORITY_DEFAULT, editor->priv->active->cancellable,
		(GAsyncReadyCallback) subscription_editor_get_folder_info_done,
		g_object_ref (editor));
}

static void
subscription_editor_stop (EMSubscriptionEditor *editor)
{
	GdkWindow *window;

	if (editor->priv->active->cancellable != NULL) {
		g_cancellable_cancel (editor->priv->active->cancellable);
		g_object_unref (editor->priv->active->cancellable);
		editor->priv->active->cancellable = NULL;
	}

	gtk_widget_set_sensitive (editor->priv->notebook, TRUE);
	gtk_widget_set_sensitive (editor->priv->subscribe_button, TRUE);
	gtk_widget_set_sensitive (editor->priv->unsubscribe_button, TRUE);
	gtk_widget_set_sensitive (editor->priv->refresh_button, TRUE);
	gtk_widget_set_sensitive (editor->priv->stop_button, FALSE);

	window = gtk_widget_get_window (GTK_WIDGET (editor));
	gdk_window_set_cursor (window, NULL);
}

static gboolean
subscription_editor_filter_cb (GtkTreeModel *tree_model,
                               GtkTreeIter *iter,
                               EMSubscriptionEditor *editor)
{
	CamelFolderInfo *folder_info;
	gchar *casefolded;
	gboolean match;

	/* If there's no search string let everything through. */
	if (editor->priv->search_string == NULL)
		return TRUE;

	gtk_tree_model_get (
		tree_model, iter,
		COL_CASEFOLDED, &casefolded,
		COL_FOLDER_INFO, &folder_info, -1);

	match = FOLDER_CAN_SELECT (folder_info) &&
		(casefolded != NULL) && (*casefolded != '\0') &&
		(strstr (casefolded, editor->priv->search_string) != NULL);

	g_free (casefolded);

	return match;
}

static void
subscription_editor_update_view (EMSubscriptionEditor *editor)
{
	GtkEntry *entry;
	GtkTreeView *tree_view;
	GtkTreeModel *tree_model;
	const gchar *text;

	entry = GTK_ENTRY (editor->priv->entry);
	tree_view = editor->priv->active->tree_view;

	editor->priv->timeout_id = 0;

	text = gtk_entry_get_text (entry);

	if (text != NULL && *text != '\0') {
		g_free (editor->priv->search_string);
		editor->priv->search_string = g_utf8_casefold (text, -1);

		/* Install the list store in the tree view if needed. */
		if (!editor->priv->active->filtered_view) {
			GtkTreeSelection *selection;
			GtkTreePath *path;

			tree_model = gtk_tree_model_filter_new (
				editor->priv->active->list_store, NULL);
			gtk_tree_model_filter_set_visible_func (
				GTK_TREE_MODEL_FILTER (tree_model),
				(GtkTreeModelFilterVisibleFunc)
				subscription_editor_filter_cb, editor,
				(GDestroyNotify) NULL);
			gtk_tree_view_set_model (tree_view, tree_model);
			g_object_unref (tree_model);

			path = gtk_tree_path_new_first ();
			selection = gtk_tree_view_get_selection (tree_view);
			gtk_tree_selection_select_path (selection, path);
			gtk_tree_path_free (path);

			editor->priv->active->filtered_view = TRUE;
		}

		tree_model = gtk_tree_view_get_model (tree_view);
		gtk_tree_model_filter_refilter (
			GTK_TREE_MODEL_FILTER (tree_model));

		gtk_entry_set_icon_sensitive (
			entry, GTK_ENTRY_ICON_SECONDARY, TRUE);

		gtk_widget_set_sensitive (
			editor->priv->collapse_all_button, FALSE);
		gtk_widget_set_sensitive (
			editor->priv->expand_all_button, FALSE);

	} else {
		/* Install the tree store in the tree view if needed. */
		if (editor->priv->active->filtered_view) {
			GtkTreeSelection *selection;
			GtkTreePath *path;

			tree_model = editor->priv->active->tree_store;
			gtk_tree_view_set_model (tree_view, tree_model);

			path = gtk_tree_path_new_first ();
			selection = gtk_tree_view_get_selection (tree_view);
			gtk_tree_selection_select_path (selection, path);
			gtk_tree_path_free (path);

			editor->priv->active->filtered_view = FALSE;
		}

		gtk_entry_set_icon_sensitive (
			entry, GTK_ENTRY_ICON_SECONDARY, FALSE);

		gtk_widget_set_sensitive (
			editor->priv->collapse_all_button, TRUE);
		gtk_widget_set_sensitive (
			editor->priv->expand_all_button, TRUE);
	}
}

static gboolean
subscription_editor_timeout_cb (EMSubscriptionEditor *editor)
{
	subscription_editor_update_view (editor);
	editor->priv->timeout_id = 0;

	return FALSE;
}

static void
subscription_editor_combo_box_changed_cb (GtkComboBox *combo_box,
                                          EMSubscriptionEditor *editor)
{
	StoreData *data;
	gint index;

	index = gtk_combo_box_get_active (combo_box);
	g_return_if_fail (index < editor->priv->stores->len);

	data = g_ptr_array_index (editor->priv->stores, index);
	g_return_if_fail (data != NULL);

	editor->priv->active = data;

	subscription_editor_stop (editor);
	subscription_editor_update_view (editor);

	g_object_notify (G_OBJECT (editor), "store");

	if (data->needs_refresh) {
		subscription_editor_refresh (editor);
		data->needs_refresh = FALSE;
	}
}

static void
subscription_editor_entry_changed_cb (GtkEntry *entry,
                                      EMSubscriptionEditor *editor)
{
	const gchar *text;

	if (editor->priv->timeout_id > 0) {
		g_source_remove (editor->priv->timeout_id);
		editor->priv->timeout_id = 0;
	}

	text = gtk_entry_get_text (entry);

	if (text != NULL && *text != '\0')
		editor->priv->timeout_id = g_timeout_add_seconds (
			1, (GSourceFunc) subscription_editor_timeout_cb, editor);
	else
		subscription_editor_update_view (editor);
}

static void
subscription_editor_icon_release_cb (GtkEntry *entry,
                                     GtkEntryIconPosition icon_pos,
                                     GdkEvent *event,
                                     EMSubscriptionEditor *editor)
{
	if (icon_pos == GTK_ENTRY_ICON_SECONDARY)
		gtk_entry_set_text (entry, "");
}

static void
subscription_editor_renderer_toggled_cb (GtkCellRendererToggle *renderer,
                                         const gchar *path_string,
                                         EMSubscriptionEditor *editor)
{
	GtkTreeSelection *selection;
	GtkTreeView *tree_view;
	GtkTreePath *path;

	tree_view = editor->priv->active->tree_view;
	selection = gtk_tree_view_get_selection (tree_view);

	path = gtk_tree_path_new_from_string (path_string);
	gtk_tree_selection_select_path (selection, path);
	gtk_tree_path_free (path);

	if (gtk_cell_renderer_toggle_get_active (renderer))
		subscription_editor_unsubscribe (editor);
	else
		subscription_editor_subscribe (editor);
}

static void
subscription_editor_render_toggle_cb (GtkCellLayout *cell_layout,
                                      GtkCellRenderer *renderer,
                                      GtkTreeModel *tree_model,
                                      GtkTreeIter *iter)
{
	CamelFolderInfo *folder_info;

	gtk_tree_model_get (
		tree_model, iter, COL_FOLDER_INFO, &folder_info, -1);

	g_object_set (
		renderer, "active", FOLDER_SUBSCRIBED (folder_info),
		"visible", FOLDER_CAN_SELECT (folder_info), NULL);
}

static void
subscription_editor_selection_changed_cb (GtkTreeSelection *selection,
                                          EMSubscriptionEditor *editor)
{
	GtkTreeModel *tree_model;
	GtkTreeIter iter;

	if (gtk_tree_selection_get_selected (selection, &tree_model, &iter)) {
		CamelFolderInfo *folder_info;

		gtk_tree_model_get (
			tree_model, &iter,
			COL_FOLDER_INFO, &folder_info, -1);
		gtk_widget_set_sensitive (
			editor->priv->subscribe_button,
			FOLDER_CAN_SELECT (folder_info) &&
			!FOLDER_SUBSCRIBED (folder_info));
		gtk_widget_set_sensitive (
			editor->priv->unsubscribe_button,
			FOLDER_CAN_SELECT (folder_info) &&
			FOLDER_SUBSCRIBED (folder_info));
	} else {
		gtk_widget_set_sensitive (
			editor->priv->subscribe_button, FALSE);
		gtk_widget_set_sensitive (
			editor->priv->subscribe_button, FALSE);
	}
}

static void
subscription_editor_add_store (EMSubscriptionEditor *editor,
                               CamelStore *store)
{
	StoreData *data;
	CamelService *service;
	GtkListStore *list_store;
	GtkTreeStore *tree_store;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;
	GtkCellRenderer *renderer;
	GtkComboBoxText *combo_box;
	GtkWidget *container;
	GtkWidget *widget;
	const gchar *display_name;

	service = CAMEL_SERVICE (store);
	display_name = camel_service_get_display_name (service);

	combo_box = GTK_COMBO_BOX_TEXT (editor->priv->combo_box);
	gtk_combo_box_text_append_text (combo_box, display_name);

	tree_store = gtk_tree_store_new (
		N_COLUMNS,
		/* COL_CASEFOLDED */	G_TYPE_STRING,
		/* COL_FOLDER_ICON */	G_TYPE_STRING,
		/* COL_FOLDER_NAME */	G_TYPE_STRING,
		/* COL_FOLDER_INFO */	G_TYPE_POINTER);

	list_store = gtk_list_store_new (
		N_COLUMNS,
		/* COL_CASEFOLDED */	G_TYPE_STRING,
		/* COL_FOLDER_ICON */	G_TYPE_STRING,
		/* COL_FOLDER_NAME */	G_TYPE_STRING,
		/* COL_FOLDER_INFO */	G_TYPE_POINTER);

	container = editor->priv->notebook;

	widget = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (
		GTK_SCROLLED_WINDOW (widget),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (
		GTK_SCROLLED_WINDOW (widget), GTK_SHADOW_IN);
	gtk_notebook_append_page (GTK_NOTEBOOK (container), widget, NULL);
	gtk_container_child_set (
		GTK_CONTAINER (container), widget,
		"tab-fill", FALSE, "tab-expand", FALSE, NULL);
	gtk_widget_show (widget);

	container = widget;

	widget = gtk_tree_view_new_with_model (GTK_TREE_MODEL (tree_store));
	gtk_tree_view_set_enable_search (GTK_TREE_VIEW (widget), TRUE);
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (widget), FALSE);
	gtk_tree_view_set_search_column (
		GTK_TREE_VIEW (widget), COL_FOLDER_NAME);
	gtk_container_add (GTK_CONTAINER (container), widget);
	gtk_widget_show (widget);

	column = gtk_tree_view_column_new ();
	gtk_tree_view_append_column (GTK_TREE_VIEW (widget), column);

	renderer = gtk_cell_renderer_toggle_new ();
	g_object_set (renderer, "activatable", TRUE, NULL);
	gtk_tree_view_column_pack_start (column, renderer, FALSE);

	gtk_cell_layout_set_cell_data_func (
		GTK_CELL_LAYOUT (column), renderer,
		(GtkCellLayoutDataFunc) subscription_editor_render_toggle_cb,
		NULL, (GDestroyNotify) NULL);

	g_signal_connect (
		renderer, "toggled",
		G_CALLBACK (subscription_editor_renderer_toggled_cb), editor);

	column = gtk_tree_view_column_new ();
	gtk_tree_view_append_column (GTK_TREE_VIEW (widget), column);
	gtk_tree_view_set_expander_column (GTK_TREE_VIEW (widget), column);

	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (
		column, renderer, "icon-name", COL_FOLDER_ICON);

	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_add_attribute (
		column, renderer, "text", COL_FOLDER_NAME);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));

	g_signal_connect (
		selection, "changed",
		G_CALLBACK (subscription_editor_selection_changed_cb), editor);

	data = g_slice_new0 (StoreData);
	data->store = g_object_ref (store);
	data->tree_view = g_object_ref (widget);
	data->list_store = GTK_TREE_MODEL (list_store);
	data->tree_store = GTK_TREE_MODEL (tree_store);
	data->needs_refresh = TRUE;

	g_ptr_array_add (editor->priv->stores, data);
}

static void
subscription_editor_set_store (EMSubscriptionEditor *editor,
                               CamelStore *store)
{
	g_return_if_fail (editor->priv->initial_store == NULL);

	if (CAMEL_IS_SUBSCRIBABLE (store))
		editor->priv->initial_store = g_object_ref (store);
}

static void
subscription_editor_set_backend (EMSubscriptionEditor *editor,
                                 EMailBackend *backend)
{
	g_return_if_fail (E_IS_MAIL_BACKEND (backend));
	g_return_if_fail (editor->priv->backend == NULL);

	editor->priv->backend = g_object_ref (backend);
}

static void
subscription_editor_set_property (GObject *object,
                                  guint property_id,
                                  const GValue *value,
                                  GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_BACKEND:
			subscription_editor_set_backend (
				EM_SUBSCRIPTION_EDITOR (object),
				g_value_get_object (value));
			return;

		case PROP_STORE:
			subscription_editor_set_store (
				EM_SUBSCRIPTION_EDITOR (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
subscription_editor_get_property (GObject *object,
                                  guint property_id,
                                  GValue *value,
                                  GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_BACKEND:
			g_value_set_object (
				value,
				em_subscription_editor_get_backend (
				EM_SUBSCRIPTION_EDITOR (object)));
			return;

		case PROP_STORE:
			g_value_set_object (
				value,
				em_subscription_editor_get_store (
				EM_SUBSCRIPTION_EDITOR (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
subscription_editor_dispose (GObject *object)
{
	EMSubscriptionEditorPrivate *priv;

	priv = EM_SUBSCRIPTION_EDITOR_GET_PRIVATE (object);

	if (priv->backend != NULL) {
		g_object_unref (priv->backend);
		priv->backend = NULL;
	}

	if (priv->initial_store != NULL) {
		g_object_unref (priv->initial_store);
		priv->initial_store = NULL;
	}

	if (priv->timeout_id > 0) {
		g_source_remove (priv->timeout_id);
		priv->timeout_id = 0;
	}

	g_ptr_array_set_size (priv->stores, 0);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (em_subscription_editor_parent_class)->dispose (object);
}

static void
subscription_editor_finalize (GObject *object)
{
	EMSubscriptionEditorPrivate *priv;

	priv = EM_SUBSCRIPTION_EDITOR_GET_PRIVATE (object);

	g_ptr_array_free (priv->stores, TRUE);

	g_free (priv->search_string);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (em_subscription_editor_parent_class)->finalize (object);
}

static void
subscription_editor_constructed (GObject *object)
{
	EMSubscriptionEditor *editor;

	editor = EM_SUBSCRIPTION_EDITOR (object);

	/* Pick an initial store based on the default mail account, if
	 * one wasn't already given in em_subscription_editor_new(). */
	if (editor->priv->initial_store == NULL) {
		EAccount *account;
		CamelService *service;
		EMailBackend *backend;
		EMailSession *session;

		account = e_get_default_account ();

		backend = em_subscription_editor_get_backend (editor);
		session = e_mail_backend_get_session (backend);

		service = camel_session_get_service (
			CAMEL_SESSION (session),
			account->uid);

		if (CAMEL_IS_SUBSCRIBABLE (service))
			editor->priv->initial_store = g_object_ref (service);
	}

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (em_subscription_editor_parent_class)->constructed (object);
}

static void
subscription_editor_realize (GtkWidget *widget)
{
	EMSubscriptionEditor *editor;
	EMFolderTreeModel *model;
	GtkComboBox *combo_box;
	GList *list, *link;
	gint initial_index = 0;

	editor = EM_SUBSCRIPTION_EDITOR (widget);

	/* Chain up to parent's realize() method. */
	GTK_WIDGET_CLASS (em_subscription_editor_parent_class)->realize (widget);

	/* Find stores to display, and watch for the initial store. */

	model = em_folder_tree_model_get_default ();
	list = em_folder_tree_model_list_stores (model);

	for (link = list; link != NULL; link = g_list_next (link)) {
		CamelStore *store = CAMEL_STORE (link->data);

		if (!CAMEL_IS_SUBSCRIBABLE (store))
			continue;

		if (store == editor->priv->initial_store)
			initial_index = editor->priv->stores->len;

		subscription_editor_add_store (editor, store);
	}

	g_list_free (list);

	/* The subscription editor should only be accessible if
	 * at least one enabled store supports subscriptions. */
	g_return_if_fail (editor->priv->stores->len > 0);

	combo_box = GTK_COMBO_BOX (editor->priv->combo_box);
	gtk_combo_box_set_active (combo_box, initial_index);
}

static void
em_subscription_editor_class_init (EMSubscriptionEditorClass *class)
{
	GObjectClass *object_class;
	GtkWidgetClass *widget_class;

	g_type_class_add_private (class, sizeof (EMSubscriptionEditorPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = subscription_editor_set_property;
	object_class->get_property = subscription_editor_get_property;
	object_class->dispose = subscription_editor_dispose;
	object_class->finalize = subscription_editor_finalize;
	object_class->constructed = subscription_editor_constructed;

	widget_class = GTK_WIDGET_CLASS (class);
	widget_class->realize = subscription_editor_realize;

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

	g_object_class_install_property (
		object_class,
		PROP_STORE,
		g_param_spec_object (
			"store",
			NULL,
			NULL,
			CAMEL_TYPE_STORE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY |
			G_PARAM_STATIC_STRINGS));
}

static void
em_subscription_editor_init (EMSubscriptionEditor *editor)
{
	GtkWidget *container;
	GtkWidget *widget;
	GtkWidget *box;
	const gchar *tooltip;

	editor->priv = EM_SUBSCRIPTION_EDITOR_GET_PRIVATE (editor);

	editor->priv->stores = g_ptr_array_new_with_free_func (
		(GDestroyNotify) store_data_free);

	gtk_container_set_border_width (GTK_CONTAINER (editor), 5);
	gtk_window_set_title (GTK_WINDOW (editor), _("Folder Subscriptions"));

	gconf_bridge_bind_window_size (
		gconf_bridge_get (),
		"/apps/evolution/mail/subscription_editor",
		GTK_WINDOW (editor));

	gtk_dialog_add_button (
		GTK_DIALOG (editor),
		GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE);

	container = gtk_dialog_get_content_area (GTK_DIALOG (editor));

	widget = gtk_vbox_new (FALSE, 12);
	gtk_container_set_border_width (GTK_CONTAINER (widget), 5);
	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);
	gtk_widget_show (widget);

	container = box = widget;

	widget = gtk_table_new (2, 3, FALSE);
	gtk_table_set_col_spacings (GTK_TABLE (widget), 6);
	gtk_table_set_row_spacings (GTK_TABLE (widget), 6);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	container = widget;

	widget = gtk_combo_box_text_new ();
	gtk_table_attach (
		GTK_TABLE (container), widget,
		1, 2, 0, 1, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
	editor->priv->combo_box = widget;
	gtk_widget_show (widget);

	g_signal_connect (
		widget, "changed",
		G_CALLBACK (subscription_editor_combo_box_changed_cb), editor);

	widget = gtk_label_new_with_mnemonic (_("_Account:"));
	gtk_label_set_mnemonic_widget (
		GTK_LABEL (widget), editor->priv->combo_box);
	gtk_misc_set_alignment (GTK_MISC (widget), 1.0, 0.5);
	gtk_table_attach (
		GTK_TABLE (container), widget,
		0, 1, 0, 1, GTK_FILL, GTK_FILL, 0, 0);
	gtk_widget_show (widget);

	widget = gtk_entry_new ();
	gtk_entry_set_icon_from_stock (
		GTK_ENTRY (widget),
		GTK_ENTRY_ICON_SECONDARY, GTK_STOCK_CLEAR);
	gtk_entry_set_icon_tooltip_text (
		GTK_ENTRY (widget),
		GTK_ENTRY_ICON_SECONDARY, _("Clear Search"));
	gtk_entry_set_icon_sensitive (
		GTK_ENTRY (widget),
		GTK_ENTRY_ICON_SECONDARY, FALSE);
	gtk_table_attach (
		GTK_TABLE (container), widget,
		1, 2, 1, 2, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
	editor->priv->entry = widget;
	gtk_widget_show (widget);

	g_signal_connect (
		widget, "changed",
		G_CALLBACK (subscription_editor_entry_changed_cb), editor);

	g_signal_connect (
		widget, "icon-release",
		G_CALLBACK (subscription_editor_icon_release_cb), editor);

	widget = gtk_label_new_with_mnemonic (_("Sho_w items that contain:"));
	gtk_label_set_mnemonic_widget (
		GTK_LABEL (widget), editor->priv->entry);
	gtk_misc_set_alignment (GTK_MISC (widget), 1.0, 0.5);
	gtk_table_attach (
		GTK_TABLE (container), widget,
		0, 1, 1, 2, GTK_FILL, GTK_FILL, 0, 0);
	gtk_widget_show (widget);

	container = box;

	widget = gtk_hbox_new (FALSE, 6);
	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);
	gtk_widget_show (widget);

	container = widget;

	widget = gtk_notebook_new ();
	gtk_notebook_set_show_tabs (GTK_NOTEBOOK (widget), FALSE);
	gtk_notebook_set_show_border (GTK_NOTEBOOK (widget), FALSE);
	gtk_container_add (GTK_CONTAINER (container), widget);
	editor->priv->notebook = widget;
	gtk_widget_show (widget);

	g_object_bind_property (
		editor->priv->combo_box, "active",
		editor->priv->notebook, "page",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE);

	widget = gtk_vbutton_box_new ();
	gtk_box_set_spacing (GTK_BOX (widget), 6);
	gtk_button_box_set_layout (
		GTK_BUTTON_BOX (widget), GTK_BUTTONBOX_START);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, TRUE, 0);
	gtk_widget_show (widget);

	container = widget;

	tooltip = _("Subscribe to the selected folder");
	widget = gtk_button_new_with_mnemonic (_("Su_bscribe"));
	gtk_widget_set_sensitive (widget, FALSE);
	gtk_widget_set_tooltip_text (widget, tooltip);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	editor->priv->subscribe_button = widget;
	gtk_widget_show (widget);

	g_signal_connect_swapped (
		widget, "clicked",
		G_CALLBACK (subscription_editor_subscribe), editor);

	tooltip = _("Unsubscribe from the selected folder");
	widget = gtk_button_new_with_mnemonic (_("_Unsubscribe"));
	gtk_widget_set_sensitive (widget, FALSE);
	gtk_widget_set_tooltip_text (widget, tooltip);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	editor->priv->unsubscribe_button = widget;
	gtk_widget_show (widget);

	g_signal_connect_swapped (
		widget, "clicked",
		G_CALLBACK (subscription_editor_unsubscribe), editor);

	tooltip = _("Collapse all folders");
	widget = gtk_button_new_with_mnemonic (_("C_ollapse All"));
	gtk_widget_set_tooltip_text (widget, tooltip);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	editor->priv->collapse_all_button = widget;
	gtk_widget_show (widget);

	g_signal_connect_swapped (
		widget, "clicked",
		G_CALLBACK (subscription_editor_collapse_all), editor);

	tooltip = _("Expand all folders");
	widget = gtk_button_new_with_mnemonic (_("E_xpand All"));
	gtk_widget_set_tooltip_text (widget, tooltip);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	editor->priv->expand_all_button = widget;
	gtk_widget_show (widget);

	g_signal_connect_swapped (
		widget, "clicked",
		G_CALLBACK (subscription_editor_expand_all), editor);

	tooltip = _("Refresh the folder list");
	widget = gtk_button_new_from_stock (GTK_STOCK_REFRESH);
	gtk_widget_set_tooltip_text (widget, tooltip);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_set_sensitive (widget, FALSE);
	editor->priv->refresh_button = widget;
	gtk_widget_show (widget);

	g_signal_connect_swapped (
		widget, "clicked",
		G_CALLBACK (subscription_editor_refresh), editor);

	tooltip = _("Stop the current operation");
	widget = gtk_button_new_from_stock (GTK_STOCK_STOP);
	gtk_widget_set_tooltip_text (widget, tooltip);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_set_sensitive (widget, FALSE);
	editor->priv->stop_button = widget;
	gtk_widget_show (widget);

	g_signal_connect_swapped (
		widget, "clicked",
		G_CALLBACK (subscription_editor_stop), editor);
}

GtkWidget *
em_subscription_editor_new (GtkWindow *parent,
                            EMailBackend *backend,
                            CamelStore *initial_store)
{
	g_return_val_if_fail (GTK_IS_WINDOW (parent), NULL);
	g_return_val_if_fail (E_IS_MAIL_BACKEND (backend), NULL);

	return g_object_new (
		EM_TYPE_SUBSCRIPTION_EDITOR,
		"backend", backend,
		"store", initial_store,
		"transient-for", parent,
		NULL);
}

EMailBackend *
em_subscription_editor_get_backend (EMSubscriptionEditor *editor)
{
	g_return_val_if_fail (EM_IS_SUBSCRIPTION_EDITOR (editor), NULL);

	return editor->priv->backend;
}

CamelStore *
em_subscription_editor_get_store (EMSubscriptionEditor *editor)
{
	g_return_val_if_fail (EM_IS_SUBSCRIPTION_EDITOR (editor), NULL);

	if (editor->priv->active == NULL)
		return NULL;

	return editor->priv->active->store;
}

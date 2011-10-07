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
 *		Srinivasa Ragavan <sragavan@gnome.org>
 *
 * Copyright (C) 2010 Intel corporation. (www.intel.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib/gi18n.h>
#include "e-mail-paned-view.h"

#include <libedataserver/e-data-server-util.h>

#include "e-util/e-util-private.h"
#include "e-util/gconf-bridge.h"
#include "widgets/menus/gal-view-etable.h"
#include "widgets/menus/gal-view-instance.h"
#include "widgets/misc/e-paned.h"
#include "widgets/misc/e-preview-pane.h"
#include "widgets/misc/e-search-bar.h"

#include <shell/e-shell-window-actions.h>

#include "em-utils.h"
#include "mail-config.h"
#include "mail-ops.h"
#include "message-list.h"
#include "e-mail-folder-utils.h"
#include "e-mail-reader-utils.h"

#define E_SHELL_WINDOW_ACTION_GROUP_MAIL(window) \
	E_SHELL_WINDOW_ACTION_GROUP ((window), "mail")

struct _EMailPanedViewPrivate {
	GtkWidget *paned;
	GtkWidget *scrolled_window;
	GtkWidget *message_list;
	GtkWidget *search_bar;

	EMailDisplay *display;
	GalViewInstance *view_instance;

	/* ETable scrolling hack */
	gdouble default_scrollbar_position;

	guint paned_binding_id;

	/* Signal handler IDs */
	guint message_list_built_id;
	guint enable_show_folder : 1;
};

enum {
	PROP_0,
	PROP_FORWARD_STYLE,
	PROP_GROUP_BY_THREADS,
	PROP_REPLY_STYLE
};

#define STATE_KEY_GROUP_BY_THREADS	"GroupByThreads"
#define STATE_KEY_SELECTED_MESSAGE	"SelectedMessage"
#define STATE_KEY_PREVIEW_VISIBLE	"PreviewVisible"

/* Forward Declarations */
static void e_mail_paned_view_reader_init (EMailReaderInterface *interface);

G_DEFINE_TYPE_WITH_CODE (
	EMailPanedView, e_mail_paned_view, E_TYPE_MAIL_VIEW,
	G_IMPLEMENT_INTERFACE (
		E_TYPE_MAIL_READER, e_mail_paned_view_reader_init))

static void
mail_paned_view_save_boolean (EMailView *view,
                              const gchar *key,
                              gboolean value)
{
	EShellView *shell_view;
	EMailReader *reader;
	CamelFolder *folder;
	GKeyFile *key_file;
	gchar *folder_uri;
	gchar *group_name;

	shell_view = e_mail_view_get_shell_view (view);
	key_file = e_shell_view_get_state_key_file (shell_view);

	reader = E_MAIL_READER (view);
	folder = e_mail_reader_get_folder (reader);

	if (folder == NULL)
		return;

	folder_uri = e_mail_folder_uri_from_folder (folder);
	group_name = g_strdup_printf ("Folder %s", folder_uri);
	g_key_file_set_boolean (key_file, group_name, key, value);
	g_free (group_name);
	g_free (folder_uri);

	e_shell_view_set_state_dirty (shell_view);
}

static void
mail_paned_view_message_list_built_cb (EMailView *view,
                                       MessageList *message_list)
{
	EMailPanedViewPrivate *priv;
	EShellView *shell_view;
	EShellWindow *shell_window;
	GKeyFile *key_file;

	priv = E_MAIL_PANED_VIEW (view)->priv;

	g_signal_handler_disconnect (
		message_list, priv->message_list_built_id);
	priv->message_list_built_id = 0;

	shell_view = e_mail_view_get_shell_view (view);
	shell_window = e_shell_view_get_shell_window (shell_view);

	key_file = e_shell_view_get_state_key_file (shell_view);

	if (message_list->cursor_uid != NULL)
		;  /* do nothing */

	else if (message_list->folder == NULL)
		;  /* do nothing */

	else if (e_shell_window_get_safe_mode (shell_window))
		e_shell_window_set_safe_mode (shell_window, FALSE);

	else {
		CamelFolder *folder;
		const gchar *key;
		gchar *folder_uri;
		gchar *group_name;
		gchar *uid;

		folder = message_list->folder;
		folder_uri = e_mail_folder_uri_from_folder (folder);

		key = STATE_KEY_SELECTED_MESSAGE;
		group_name = g_strdup_printf ("Folder %s", folder_uri);
		uid = g_key_file_get_string (key_file, group_name, key, NULL);
		g_free (group_name);

		g_free (folder_uri);

		/* Use selection fallbacks if UID is not found. */
		message_list_select_uid (message_list, uid, TRUE);

		g_free (uid);
	}
}

static void
mail_paned_view_message_selected_cb (EMailView *view,
                                     const gchar *message_uid,
                                     MessageList *message_list)
{
	EShellView *shell_view;
	CamelFolder *folder;
	GKeyFile *key_file;
	const gchar *key;
	gchar *folder_uri;
	gchar *group_name;

	folder = message_list->folder;

	/* This also gets triggered when selecting a store name on
	 * the sidebar such as "On This Computer", in which case
	 * 'folder' will be NULL. */
	if (folder == NULL)
		return;

	shell_view = e_mail_view_get_shell_view (view);
	key_file = e_shell_view_get_state_key_file (shell_view);

	folder_uri = e_mail_folder_uri_from_folder (folder);

	key = STATE_KEY_SELECTED_MESSAGE;
	group_name = g_strdup_printf ("Folder %s", folder_uri);

	if (message_uid != NULL)
		g_key_file_set_string (key_file, group_name, key, message_uid);
	else
		g_key_file_remove_key (key_file, group_name, key, NULL);
	e_shell_view_set_state_dirty (shell_view);

	g_free (group_name);
	g_free (folder_uri);
}

static void
mail_paned_view_restore_state_cb (EShellWindow *shell_window,
                                  EShellView *shell_view,
                                  EMailPanedView *view)
{
	EMailPanedViewPrivate *priv;
	GConfBridge *bridge;
	GObject *object;
	const gchar *key;

	priv = E_MAIL_PANED_VIEW (view)->priv;

	/* Bind GObject properties to GConf keys. */

	bridge = gconf_bridge_get ();

	object = G_OBJECT (priv->paned);
	key = "/apps/evolution/mail/display/hpaned_size";
	gconf_bridge_bind_property (bridge, key, object, "hposition");

	object = G_OBJECT (priv->paned);
	key = "/apps/evolution/mail/display/paned_size";
	gconf_bridge_bind_property (bridge, key, object, "vposition");
}

static void
mail_paned_display_view_cb (EMailView *view,
                            GalView *gal_view)
{
	EMailReader *reader;
	GtkWidget *message_list;

	reader = E_MAIL_READER (view);
	message_list = e_mail_reader_get_message_list (reader);

	if (GAL_IS_VIEW_ETABLE (gal_view))
		gal_view_etable_attach_tree (
			GAL_VIEW_ETABLE (gal_view),
			E_TREE (message_list));
}

static void
mail_paned_view_notify_group_by_threads_cb (EMailReader *reader)
{
	gboolean group_by_threads;

	group_by_threads = e_mail_reader_get_group_by_threads (reader);

	mail_paned_view_save_boolean (
		E_MAIL_VIEW (reader),
		STATE_KEY_GROUP_BY_THREADS, group_by_threads);
}

static void
mail_paned_view_set_property (GObject *object,
                              guint property_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_FORWARD_STYLE:
			e_mail_reader_set_forward_style (
				E_MAIL_READER (object),
				g_value_get_enum (value));
			return;

		case PROP_GROUP_BY_THREADS:
			e_mail_reader_set_group_by_threads (
				E_MAIL_READER (object),
				g_value_get_boolean (value));
			return;

		case PROP_REPLY_STYLE:
			e_mail_reader_set_reply_style (
				E_MAIL_READER (object),
				g_value_get_enum (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_paned_view_get_property (GObject *object,
                              guint property_id,
                              GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_FORWARD_STYLE:
			g_value_set_enum (
				value,
				e_mail_reader_get_forward_style (
				E_MAIL_READER (object)));
			return;

		case PROP_GROUP_BY_THREADS:
			g_value_set_boolean (
				value,
				e_mail_reader_get_group_by_threads (
				E_MAIL_READER (object)));
			return;

		case PROP_REPLY_STYLE:
			g_value_set_enum (
				value,
				e_mail_reader_get_reply_style (
				E_MAIL_READER (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_paned_view_dispose (GObject *object)
{
	EMailPanedViewPrivate *priv;

	priv = E_MAIL_PANED_VIEW (object)->priv;

	if (priv->paned != NULL) {
		g_object_unref (priv->paned);
		priv->paned = NULL;
	}

	if (priv->scrolled_window != NULL) {
		g_object_unref (priv->scrolled_window);
		priv->scrolled_window = NULL;
	}

	if (priv->message_list != NULL) {
		g_object_unref (priv->message_list);
		priv->message_list = NULL;
	}

	if (priv->search_bar != NULL) {
		g_object_unref (priv->search_bar);
		priv->search_bar = NULL;
	}

	if (priv->display != NULL) {
		g_object_unref (priv->display);
		priv->display = NULL;
	}

	if (priv->view_instance != NULL) {
		g_object_unref (priv->view_instance);
		priv->view_instance = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_mail_paned_view_parent_class)->dispose (object);
}

static GtkActionGroup *
mail_paned_view_get_action_group (EMailReader *reader,
                                  EMailReaderActionGroup group)
{
	EMailView *view;
	EShellView *shell_view;
	EShellWindow *shell_window;
	const gchar *group_name;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);
	shell_window = e_shell_view_get_shell_window (shell_view);

	switch (group) {
		case E_MAIL_READER_ACTION_GROUP_STANDARD:
			group_name = "mail";
			break;
		case E_MAIL_READER_ACTION_GROUP_SEARCH_FOLDERS:
			group_name = "search-folders";
			break;
		default:
			g_return_val_if_reached (NULL);
	}

	return e_shell_window_get_action_group (shell_window, group_name);
}

static EAlertSink *
mail_paned_view_get_alert_sink (EMailReader *reader)
{
	EMailView *view;
	EShellView *shell_view;
	EShellContent *shell_content;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);
	shell_content = e_shell_view_get_shell_content (shell_view);

	return E_ALERT_SINK (shell_content);
}

static EMailBackend *
mail_paned_view_get_backend (EMailReader *reader)
{
	EMailView *view;
	EShellView *shell_view;
	EShellBackend *shell_backend;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);
	shell_backend = e_shell_view_get_shell_backend (shell_view);

	return E_MAIL_BACKEND (shell_backend);
}

static EMailDisplay *
mail_paned_view_get_mail_display (EMailReader *reader)
{
	EMailPanedViewPrivate *priv;

	priv = E_MAIL_PANED_VIEW (reader)->priv;

	return priv->display;
}

static gboolean
mail_paned_view_get_hide_deleted (EMailReader *reader)
{
	return !e_mail_view_get_show_deleted (E_MAIL_VIEW (reader));
}

static GtkWidget *
mail_paned_view_get_message_list (EMailReader *reader)
{
	EMailPanedViewPrivate *priv;

	priv = E_MAIL_PANED_VIEW (reader)->priv;

	return priv->message_list;
}

static GtkMenu *
mail_paned_view_get_popup_menu (EMailReader *reader)
{
	EMailView *view;
	EShellView *shell_view;
	EShellWindow *shell_window;
	GtkUIManager *ui_manager;
	GtkWidget *widget;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);
	shell_window = e_shell_view_get_shell_window (shell_view);

	ui_manager = e_shell_window_get_ui_manager (shell_window);
	widget = gtk_ui_manager_get_widget (ui_manager, "/mail-preview-popup");

	return GTK_MENU (widget);
}

static GtkWindow *
mail_paned_view_get_window (EMailReader *reader)
{
	EMailView *view;
	EShellView *shell_view;
	EShellWindow *shell_window;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);
	shell_window = e_shell_view_get_shell_window (shell_view);

	return GTK_WINDOW (shell_window);
}

static void
mail_paned_view_set_folder (EMailReader *reader,
                            CamelFolder *folder)
{
	EMailPanedViewPrivate *priv;
	EMailView *view;
	EShell *shell;
	EShellView *shell_view;
	EShellWindow *shell_window;
	EShellSettings *shell_settings;
	EMailReaderInterface *default_interface;
	GtkWidget *message_list;
	GKeyFile *key_file;
	gchar *folder_uri;
	gchar *group_name;
	const gchar *key;
	gboolean value;
	GError *error = NULL;

	priv = E_MAIL_PANED_VIEW (reader)->priv;

	view = E_MAIL_VIEW (reader);
	shell_view = e_mail_view_get_shell_view (view);
	shell_window = e_shell_view_get_shell_window (shell_view);

	shell = e_shell_window_get_shell (shell_window);
	shell_settings = e_shell_get_shell_settings (shell);

	message_list = e_mail_reader_get_message_list (reader);

	message_list_freeze (MESSAGE_LIST (message_list));

	/* Chain up to interface's default set_folder() method. */
	default_interface = g_type_default_interface_peek (E_TYPE_MAIL_READER);
	default_interface->set_folder (reader, folder);

	if (folder == NULL)
		goto exit;

	/* Only refresh the folder if we're online. */
	if (e_shell_get_online (shell))
		mail_refresh_folder (folder, NULL, NULL);

	/* This is a one-time-only callback. */
	if (MESSAGE_LIST (message_list)->cursor_uid == NULL &&
		priv->message_list_built_id == 0)
		priv->message_list_built_id = g_signal_connect_swapped (
			message_list, "message-list-built",
			G_CALLBACK (mail_paned_view_message_list_built_cb),
			reader);

	/* Restore the folder's preview and threaded state. */

	folder_uri = e_mail_folder_uri_from_folder (folder);
	key_file = e_shell_view_get_state_key_file (shell_view);
	group_name = g_strdup_printf ("Folder %s", folder_uri);
	g_free (folder_uri);

	key = STATE_KEY_GROUP_BY_THREADS;
	value = g_key_file_get_boolean (key_file, group_name, key, &error);
	if (error != NULL) {
		value = TRUE;
		g_clear_error (&error);
	}

	e_mail_reader_set_group_by_threads (reader, value);

	key = STATE_KEY_PREVIEW_VISIBLE;
	value = g_key_file_get_boolean (key_file, group_name, key, &error);
	if (error != NULL) {
		value = TRUE;
		g_clear_error (&error);
	}

	/* XXX This is a little confusing and needs rethought.  The
	 *     EShellWindow:safe-mode property blocks automatic message
	 *     selection, but the "mail-safe-list" shell setting blocks
	 *     both the preview pane and automatic message selection. */
	if (e_shell_settings_get_boolean (shell_settings, "mail-safe-list")) {
		e_shell_settings_set_boolean (
			shell_settings, "mail-safe-list", FALSE);
		e_shell_window_set_safe_mode (shell_window, TRUE);
		value = FALSE;
	}

	e_mail_view_set_preview_visible (E_MAIL_VIEW (reader), value);

	g_free (group_name);

exit:
	message_list_thaw (MESSAGE_LIST (message_list));
}

static void
mail_paned_view_show_search_bar (EMailReader *reader)
{
	EMailPanedViewPrivate *priv;

	priv = E_MAIL_PANED_VIEW (reader)->priv;

	gtk_widget_show (priv->search_bar);
}

static guint
mail_paned_view_reader_open_selected_mail (EMailReader *reader)
{
	return E_MAIL_PANED_VIEW_GET_CLASS (reader)->
		open_selected_mail (E_MAIL_PANED_VIEW (reader));
}

static gboolean
mail_paned_view_enable_show_folder (EMailReader *reader)
{
	EMailPanedViewPrivate *priv;

	priv = E_MAIL_PANED_VIEW (reader)->priv;

	return priv->enable_show_folder ? TRUE : FALSE;
}

static void
mail_paned_view_constructed (GObject *object)
{
	EMailPanedViewPrivate *priv;
	EShellBackend *shell_backend;
	EShellWindow *shell_window;
	EShellView *shell_view;
	ESearchBar *search_bar;
	EMailReader *reader;
	EMailView *view;
	GtkWidget *message_list;
	GtkWidget *container;
	GtkWidget *widget;

	priv = E_MAIL_PANED_VIEW (object)->priv;

	priv->display = g_object_new (E_TYPE_MAIL_DISPLAY, NULL);

	view = E_MAIL_VIEW (object);
	shell_view = e_mail_view_get_shell_view (view);
	shell_window = e_shell_view_get_shell_window (shell_view);
	shell_backend = e_shell_view_get_shell_backend (shell_view);

	/* Make headers collapsable and store state of headers in config file */
	/* FIXME WEBKIT */
	/*em_format_html_set_headers_collapsable (EM_FORMAT_HTML (priv->formatter), TRUE);
	g_object_bind_property (shell_settings, "paned-view-headers-state",
				EM_FORMAT_HTML (priv->formatter), "headers-state",
				G_BINDING_BIDIRECTIONAL |
				G_BINDING_SYNC_CREATE);
	*/

	/* Build content widgets. */

	container = GTK_WIDGET (object);

	widget = e_paned_new (GTK_ORIENTATION_VERTICAL);
	gtk_container_add (GTK_CONTAINER (container), widget);
	priv->paned = g_object_ref (widget);
	gtk_widget_show (widget);

	g_object_bind_property (
		object, "orientation",
		widget, "orientation",
		G_BINDING_SYNC_CREATE);

	container = priv->paned;

	widget = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (
		GTK_SCROLLED_WINDOW (widget),
		GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	gtk_scrolled_window_set_shadow_type (
		GTK_SCROLLED_WINDOW (widget), GTK_SHADOW_IN);
	priv->scrolled_window = g_object_ref (widget);
	gtk_paned_pack1 (GTK_PANED (container), widget, TRUE, FALSE);
	gtk_widget_show (widget);

	container = widget;

	widget = message_list_new (E_MAIL_BACKEND (shell_backend));
	gtk_container_add (GTK_CONTAINER (container), widget);
	priv->message_list = g_object_ref (widget);
	gtk_widget_show (widget);

	container = priv->paned;

	gtk_widget_show (GTK_WIDGET (priv->display));

	widget = GTK_WIDGET (priv->display);
	gtk_paned_pack2 (GTK_PANED (container), widget, FALSE, FALSE);
	gtk_widget_show (widget);

	g_object_bind_property (
		object, "preview-visible",
		widget, "visible",
		G_BINDING_SYNC_CREATE);

	/* FIXME WEBKIT: Searchbar!!! */
	search_bar = e_mail_display_get_search_bar (priv->display);
	if (search_bar) {
		priv->search_bar = g_object_ref (search_bar);

		g_signal_connect_swapped (
				search_bar, "changed",
				G_CALLBACK (e_mail_display_reload), priv->display);
	}

	/* Load the view instance. */

	e_mail_view_update_view_instance (E_MAIL_VIEW (object));

	/* Message list customizations. */

	e_mail_reader_init (E_MAIL_READER (object), FALSE, TRUE);

	reader = E_MAIL_READER (object);
	message_list = e_mail_reader_get_message_list (reader);

	g_signal_connect_swapped (
		message_list, "message-selected",
		G_CALLBACK (mail_paned_view_message_selected_cb),
		object);

	/* Restore pane positions from the last session once
	 * the shell view is fully initialized and visible. */
	g_signal_connect (
		shell_window, "shell-view-created::mail",
		G_CALLBACK (mail_paned_view_restore_state_cb),
		object);

	e_mail_reader_connect_headers (reader);

	/* Do this after creating the message list.  Our
	 * set_preview_visible() method relies on it. */
	e_mail_view_set_preview_visible (view, TRUE);

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_mail_paned_view_parent_class)->constructed (object);
}

static void
mail_paned_view_set_search_strings (EMailView *view,
                                    GSList *search_strings)
{
#if 0  /* WEBKIT */
	EMailPanedViewPrivate *priv;
	ESearchBar *search_bar;
	ESearchingTokenizer *tokenizer;

	priv = E_MAIL_PANED_VIEW (view)->priv;

	search_bar = E_SEARCH_BAR (priv->search_bar);
	tokenizer = e_search_bar_get_tokenizer (search_bar);

	e_searching_tokenizer_set_secondary_case_sensitivity (tokenizer, FALSE);
	e_searching_tokenizer_set_secondary_search_string (tokenizer, NULL);

	while (search_strings != NULL) {
		e_searching_tokenizer_add_secondary_search_string (
			tokenizer, search_strings->data);
		search_strings = g_slist_next (search_strings);
	}

	e_search_bar_changed (search_bar);
#endif
}

static GalViewInstance *
mail_paned_view_get_view_instance (EMailView *view)
{
	EMailPanedViewPrivate *priv;

	priv = E_MAIL_PANED_VIEW (view)->priv;

	return priv->view_instance;
}

static void
mail_paned_view_update_view_instance (EMailView *view)
{
	EMailPanedViewPrivate *priv;
	EMailReader *reader;
	EShell *shell;
	EShellView *shell_view;
	EShellWindow *shell_window;
	EShellViewClass *shell_view_class;
	EShellSettings *shell_settings;
	GalViewCollection *view_collection;
	GalViewInstance *view_instance;
	CamelFolder *folder;
	GtkOrientable *orientable;
	GtkOrientation orientation;
	gboolean outgoing_folder;
	gboolean show_vertical_view;
	gchar *view_id;

	priv = E_MAIL_PANED_VIEW (view)->priv;

	shell_view = e_mail_view_get_shell_view (view);
	shell_view_class = E_SHELL_VIEW_GET_CLASS (shell_view);
	view_collection = shell_view_class->view_collection;

	shell_window = e_shell_view_get_shell_window (shell_view);
	shell = e_shell_window_get_shell (shell_window);
	shell_settings = e_shell_get_shell_settings (shell);

	reader = E_MAIL_READER (view);
	folder = e_mail_reader_get_folder (reader);

	/* If no folder is selected, return silently. */
	if (folder == NULL)
		return;

	if (priv->view_instance != NULL) {
		g_object_unref (priv->view_instance);
		priv->view_instance = NULL;
	}

	view_id = e_mail_folder_uri_from_folder (folder);
	e_filename_make_safe (view_id);

	outgoing_folder =
		em_utils_folder_is_drafts (folder) ||
		em_utils_folder_is_outbox (folder) ||
		em_utils_folder_is_sent (folder);

	if (e_shell_settings_get_boolean (shell_settings, "mail-global-view-setting"))
		view_instance = e_shell_view_new_view_instance (
			shell_view, outgoing_folder ?
			"global_view_sent_setting" : "global_view_setting");
	else
		view_instance = e_shell_view_new_view_instance (shell_view, view_id);

	priv->view_instance = view_instance;

	orientable = GTK_ORIENTABLE (view);
	orientation = gtk_orientable_get_orientation (orientable);
	show_vertical_view =
		(orientation == GTK_ORIENTATION_HORIZONTAL) &&
		!e_shell_settings_get_boolean (
		shell_settings, "mail-global-view-setting");

	if (show_vertical_view) {
		gchar *filename;
		gchar *safe_view_id;

		/* Force the view instance into vertical view. */

		g_free (view_instance->custom_filename);
		g_free (view_instance->current_view_filename);

		safe_view_id = g_strdup (view_id);
		e_filename_make_safe (safe_view_id);

		filename = g_strdup_printf (
			"custom_wide_view-%s.xml", safe_view_id);
		view_instance->custom_filename = g_build_filename (
			view_collection->local_dir, filename, NULL);
		g_free (filename);

		filename = g_strdup_printf (
			"current_wide_view-%s.xml", safe_view_id);
		view_instance->current_view_filename = g_build_filename (
			view_collection->local_dir, filename, NULL);
		g_free (filename);

		g_free (safe_view_id);
	}

	g_free (view_id);

	if (outgoing_folder) {
		if (show_vertical_view)
			gal_view_instance_set_default_view (
				view_instance, "Wide_View_Sent");
		else
			gal_view_instance_set_default_view (
				view_instance, "As_Sent_Folder");
	} else if (show_vertical_view) {
		gal_view_instance_set_default_view (
			view_instance, "Wide_View_Normal");
	}

	gal_view_instance_load (view_instance);

	if (!gal_view_instance_exists (view_instance)) {
		gchar *state_filename;

		state_filename = mail_config_folder_to_cachename (
			folder, "et-header-");

		if (g_file_test (state_filename, G_FILE_TEST_IS_REGULAR)) {
			ETableSpecification *spec;
			ETableState *state;
			GalView *view;
			gchar *spec_filename;

			spec = e_table_specification_new ();
			spec_filename = g_build_filename (
				EVOLUTION_ETSPECDIR,
				"message-list.etspec",
				NULL);
			e_table_specification_load_from_file (
				spec, spec_filename);
			g_free (spec_filename);

			state = e_table_state_new ();
			view = gal_view_etable_new (spec, "");

			e_table_state_load_from_file (
				state, state_filename);
			gal_view_etable_set_state (
				GAL_VIEW_ETABLE (view), state);
			gal_view_instance_set_custom_view (
				view_instance, view);

			g_object_unref (state);
			g_object_unref (view);
			g_object_unref (spec);
		}

		g_free (state_filename);
	}

	g_signal_connect_swapped (
		view_instance, "display-view",
		G_CALLBACK (mail_paned_display_view_cb), view);

	mail_paned_display_view_cb (
		view, gal_view_instance_get_current_view (view_instance));
}

static void
mail_paned_view_set_preview_visible (EMailView *view,
                                     gboolean preview_visible)
{
	EMailViewClass *parent_class;

	/* If we're showing the preview, tell EMailReader to reload the
	 * selected message.  This should force it to download the full
	 * message if necessary, so we don't get an empty preview. */
	if (preview_visible) {
		EMailReader *reader;
		GtkWidget *message_list;
		const gchar *cursor_uid;

		reader = E_MAIL_READER (view);
		message_list = e_mail_reader_get_message_list (reader);
		cursor_uid = MESSAGE_LIST (message_list)->cursor_uid;

		if (cursor_uid != NULL)
			e_mail_reader_set_message (reader, cursor_uid);
	}

	mail_paned_view_save_boolean (
		E_MAIL_VIEW (view),
		STATE_KEY_PREVIEW_VISIBLE, preview_visible);

	/* Chain up to parent's set_preview_visible() method. */
	parent_class = E_MAIL_VIEW_CLASS (e_mail_paned_view_parent_class);
	parent_class->set_preview_visible (view, preview_visible);
}

static guint
mail_paned_view_open_selected_mail (EMailPanedView *view)
{
	return e_mail_reader_open_selected (E_MAIL_READER (view));
}

static void
e_mail_paned_view_class_init (EMailPanedViewClass *class)
{
	GObjectClass *object_class;
	EMailViewClass *mail_view_class;

	g_type_class_add_private (class, sizeof (EMailPanedViewPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = mail_paned_view_dispose;
	object_class->constructed = mail_paned_view_constructed;
	object_class->set_property = mail_paned_view_set_property;
	object_class->get_property = mail_paned_view_get_property;

	mail_view_class = E_MAIL_VIEW_CLASS (class);
	mail_view_class->set_search_strings = mail_paned_view_set_search_strings;
	mail_view_class->get_view_instance = mail_paned_view_get_view_instance;
	mail_view_class->update_view_instance = mail_paned_view_update_view_instance;

	mail_view_class->set_preview_visible = mail_paned_view_set_preview_visible;

	class->open_selected_mail = mail_paned_view_open_selected_mail;

	/* Inherited from EMailReader */
	g_object_class_override_property (
		object_class,
		PROP_FORWARD_STYLE,
		"forward-style");

	/* Inherited from EMailReader */
	g_object_class_override_property (
		object_class,
		PROP_GROUP_BY_THREADS,
		"group-by-threads");

	/* Inherited from EMailReader */
	g_object_class_override_property (
		object_class,
		PROP_REPLY_STYLE,
		"reply-style");
}

static void
e_mail_paned_view_reader_init (EMailReaderInterface *interface)
{
	interface->get_action_group = mail_paned_view_get_action_group;
	interface->get_alert_sink = mail_paned_view_get_alert_sink;
	interface->get_backend = mail_paned_view_get_backend;
	interface->get_mail_display = mail_paned_view_get_mail_display;
	interface->get_hide_deleted = mail_paned_view_get_hide_deleted;
	interface->get_message_list = mail_paned_view_get_message_list;
	interface->get_popup_menu = mail_paned_view_get_popup_menu;
	interface->get_window = mail_paned_view_get_window;
	interface->set_folder = mail_paned_view_set_folder;
	interface->show_search_bar = mail_paned_view_show_search_bar;
	interface->open_selected_mail = mail_paned_view_reader_open_selected_mail;
	interface->enable_show_folder = mail_paned_view_enable_show_folder;
}

static void
e_mail_paned_view_init (EMailPanedView *view)
{
	view->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		view, E_TYPE_MAIL_PANED_VIEW, EMailPanedViewPrivate);
	view->priv->enable_show_folder = FALSE;

	g_signal_connect (
		view, "notify::group-by-threads",
		G_CALLBACK (mail_paned_view_notify_group_by_threads_cb),
		NULL);
}

GtkWidget *
e_mail_paned_view_new (EShellView *shell_view)
{
	g_return_val_if_fail (E_IS_SHELL_VIEW (shell_view), NULL);

	return g_object_new (
		E_TYPE_MAIL_PANED_VIEW,
		"shell-view", shell_view, NULL);
}

void
e_mail_paned_view_hide_message_list_pane (EMailPanedView *view,
                                          gboolean visible)
{
	g_return_if_fail (E_IS_MAIL_PANED_VIEW (view));

	if (visible)
		gtk_widget_show (view->priv->scrolled_window);
	else
		gtk_widget_hide (view->priv->scrolled_window);
}

GtkWidget *
e_mail_paned_view_get_preview (EMailPanedView *view)
{
	g_return_val_if_fail (E_IS_MAIL_PANED_VIEW (view), NULL);

	return GTK_WIDGET (mail_paned_view_get_mail_display (E_MAIL_READER (view)));
}

void
e_mail_paned_view_set_enable_show_folder (EMailPanedView *view,
                                          gboolean set)
{
	view->priv->enable_show_folder = set ? 1 : 0;
}

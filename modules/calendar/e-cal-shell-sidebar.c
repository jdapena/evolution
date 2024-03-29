/*
 * e-cal-shell-sidebar.c
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

#include "e-cal-shell-sidebar.h"

#include <string.h>
#include <glib/gi18n.h>
#include <libedataserverui/e-client-utils.h>

#include "e-util/e-alert-dialog.h"
#include "e-util/gconf-bridge.h"
#include "widgets/misc/e-paned.h"

#include "calendar/gui/e-calendar-selector.h"
#include "calendar/gui/misc.h"
#include "calendar/gui/dialogs/calendar-setup.h"

#include "e-cal-shell-view.h"
#include "e-cal-shell-backend.h"
#include "e-cal-shell-content.h"

struct _ECalShellSidebarPrivate {
	GtkWidget *paned;
	GtkWidget *selector;
	GtkWidget *date_navigator;

	/* UID -> Client */
	GHashTable *client_table;

	/* The default client is for ECalModel.  It follows the
	 * sidebar's primary selection, even if the highlighted
	 * source is not selected.  The tricky part is we don't
	 * update the property until the client is successfully
	 * opened.  So the user first highlights a source, then
	 * sometime later we update our default-client property
	 * which is bound by an EBinding to ECalModel. */
	ECalClient *default_client;

	GCancellable *loading_default_client;
	GCancellable *loading_clients;
};

enum {
	PROP_0,
	PROP_DATE_NAVIGATOR,
	PROP_DEFAULT_CLIENT,
	PROP_SELECTOR
};

enum {
	CLIENT_ADDED,
	CLIENT_REMOVED,
	STATUS_MESSAGE,
	LAST_SIGNAL
};

static gpointer parent_class;
static guint signals[LAST_SIGNAL];
static GType cal_shell_sidebar_type;

static void
cal_shell_sidebar_emit_client_added (ECalShellSidebar *cal_shell_sidebar,
                                     ECalClient *client)
{
	guint signal_id = signals[CLIENT_ADDED];

	g_signal_emit (cal_shell_sidebar, signal_id, 0, client);
}

static void
cal_shell_sidebar_emit_client_removed (ECalShellSidebar *cal_shell_sidebar,
                                       ECalClient *client)
{
	guint signal_id = signals[CLIENT_REMOVED];

	g_signal_emit (cal_shell_sidebar, signal_id, 0, client);
}

static void
cal_shell_sidebar_emit_status_message (ECalShellSidebar *cal_shell_sidebar,
                                       const gchar *status_message)
{
	guint signal_id = signals[STATUS_MESSAGE];

	g_signal_emit (cal_shell_sidebar, signal_id, 0, status_message);
}

static void
cal_shell_sidebar_backend_died_cb (ECalShellSidebar *cal_shell_sidebar,
                                   ECalClient *client)
{
	EShellView *shell_view;
	EShellContent *shell_content;
	EShellSidebar *shell_sidebar;
	GHashTable *client_table;
	ESource *source;
	const gchar *uid;

	client_table = cal_shell_sidebar->priv->client_table;

	shell_sidebar = E_SHELL_SIDEBAR (cal_shell_sidebar);
	shell_view = e_shell_sidebar_get_shell_view (shell_sidebar);
	shell_content = e_shell_view_get_shell_content (shell_view);

	source = e_client_get_source (E_CLIENT (client));
	uid = e_source_peek_uid (source);

	g_object_ref (source);

	g_hash_table_remove (client_table, uid);
	cal_shell_sidebar_emit_status_message (cal_shell_sidebar, NULL);

	e_alert_submit (
		E_ALERT_SINK (shell_content),
		"calendar:calendar-crashed", NULL);

	g_object_unref (source);
}

static void
cal_shell_sidebar_backend_error_cb (ECalShellSidebar *cal_shell_sidebar,
                                    const gchar *message,
                                    ECalClient *client)
{
	EShellView *shell_view;
	EShellContent *shell_content;
	EShellSidebar *shell_sidebar;
	ESourceGroup *source_group;
	ESource *source;

	shell_sidebar = E_SHELL_SIDEBAR (cal_shell_sidebar);
	shell_view = e_shell_sidebar_get_shell_view (shell_sidebar);
	shell_content = e_shell_view_get_shell_content (shell_view);

	source = e_client_get_source (E_CLIENT (client));
	source_group = e_source_peek_group (source);

	e_alert_submit (
		E_ALERT_SINK (shell_content),
		"calendar:backend-error",
		e_source_group_peek_name (source_group),
		e_source_peek_name (source), message, NULL);
}

static void
cal_shell_sidebar_retrieve_capabilies_cb (GObject *source_object,
                                          GAsyncResult *result,
                                          gpointer user_data)
{
	ECalClient *client = E_CAL_CLIENT (source_object);
	ECalShellSidebar *cal_shell_sidebar = user_data;
	gchar *capabilities = NULL;

	g_return_if_fail (client != NULL);
	g_return_if_fail (cal_shell_sidebar != NULL);

	e_client_retrieve_capabilities_finish (
		E_CLIENT (client), result, &capabilities, NULL);
	g_free (capabilities);

	cal_shell_sidebar_emit_status_message (
		cal_shell_sidebar, _("Loading calendars"));
	cal_shell_sidebar_emit_client_added (cal_shell_sidebar, client);
	cal_shell_sidebar_emit_status_message (cal_shell_sidebar, NULL);
}

static gboolean cal_shell_sidebar_retry_open_timeout_cb (gpointer user_data);

struct RetryOpenData
{
	EClient *client;
	ECalShellSidebar *cal_shell_sidebar;
	GCancellable *cancellable;
};

static void
free_retry_open_data (gpointer data)
{
	struct RetryOpenData *rod = data;

	if (!rod)
		return;

	g_object_unref (rod->client);
	g_object_unref (rod->cancellable);
	g_free (rod);
}

static void
cal_shell_sidebar_client_opened_cb (GObject *source_object,
                                    GAsyncResult *result,
                                    gpointer user_data)
{
	ECalClient *client = E_CAL_CLIENT (source_object);
	ECalShellSidebar *cal_shell_sidebar = user_data;
	ESource *source;
	EShellView *shell_view;
	EShellContent *shell_content;
	EShellSidebar *shell_sidebar;
	GError *error = NULL;

	source = e_client_get_source (E_CLIENT (client));

	e_client_open_finish (E_CLIENT (client), result, &error);

	if (g_error_matches (error, E_CLIENT_ERROR, E_CLIENT_ERROR_CANCELLED) ||
	    g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_clear_error (&error);
		return;
	}

	if (g_error_matches (error, E_CLIENT_ERROR, E_CLIENT_ERROR_AUTHENTICATION_FAILED) ||
	    g_error_matches (error, E_CLIENT_ERROR, E_CLIENT_ERROR_AUTHENTICATION_REQUIRED))
		e_client_utils_forget_password (E_CLIENT (client));

	if (g_error_matches (error, E_CLIENT_ERROR, E_CLIENT_ERROR_AUTHENTICATION_FAILED)) {
		e_client_open (
			E_CLIENT (client), FALSE,
			cal_shell_sidebar->priv->loading_clients,
			cal_shell_sidebar_client_opened_cb, user_data);

		g_clear_error (&error);
		return;
	}

	if (g_error_matches (error, E_CLIENT_ERROR, E_CLIENT_ERROR_BUSY)) {
		struct RetryOpenData *rod;

		rod = g_new0 (struct RetryOpenData, 1);
		rod->client = g_object_ref (client);
		rod->cal_shell_sidebar = cal_shell_sidebar;
		rod->cancellable = g_object_ref (cal_shell_sidebar->priv->loading_clients);

		/* postpone for 1/2 of a second, backend is busy now */
		g_timeout_add_full (
			G_PRIORITY_DEFAULT, 500,
			cal_shell_sidebar_retry_open_timeout_cb,
			rod, free_retry_open_data);

		g_clear_error (&error);
		return;
	}

	shell_sidebar = E_SHELL_SIDEBAR (cal_shell_sidebar);
	shell_view = e_shell_sidebar_get_shell_view (shell_sidebar);
	shell_content = e_shell_view_get_shell_content (shell_view);

	/* Handle errors. */
	switch ((error && error->domain == E_CLIENT_ERROR) ? error->code : -1) {
		case -1:
			break;

		case E_CLIENT_ERROR_BUSY:
			g_warning (
				"%s: Cannot open '%s', it's busy (%s)",
				G_STRFUNC, e_source_peek_name (source),
				error->message);
			g_clear_error (&error);
			return;

		case E_CLIENT_ERROR_REPOSITORY_OFFLINE:
			e_alert_submit (
				E_ALERT_SINK (shell_content),
				"calendar:prompt-no-contents-offline-calendar",
				NULL);
			/* fall through */

		default:
			if (error->code != E_CLIENT_ERROR_REPOSITORY_OFFLINE) {
				e_alert_submit (
					E_ALERT_SINK (shell_content),
					"calendar:failed-open-calendar",
					error->message, NULL);
			}

			e_cal_shell_sidebar_remove_source (
				cal_shell_sidebar,
				e_client_get_source (E_CLIENT (client)));
			g_clear_error (&error);
			return;
	}

	g_clear_error (&error);

	/* to have them ready for later use */
	e_client_retrieve_capabilities (
		E_CLIENT (client), NULL,
		cal_shell_sidebar_retrieve_capabilies_cb,
		cal_shell_sidebar);
}

static gboolean
cal_shell_sidebar_retry_open_timeout_cb (gpointer user_data)
{
	struct RetryOpenData *rod = user_data;

	g_return_val_if_fail (rod != NULL, FALSE);
	g_return_val_if_fail (rod->client != NULL, FALSE);
	g_return_val_if_fail (rod->cal_shell_sidebar != NULL, FALSE);
	g_return_val_if_fail (rod->cancellable != NULL, FALSE);

	if (g_cancellable_is_cancelled (rod->cancellable))
		return FALSE;

	e_client_open (
		rod->client, FALSE,
		rod->cal_shell_sidebar->priv->loading_clients,
		cal_shell_sidebar_client_opened_cb,
		rod->cal_shell_sidebar);

	return FALSE;
}

static void
cal_shell_sidebar_default_loaded_cb (GObject *source_object,
                                     GAsyncResult *result,
                                     gpointer user_data)
{
	ESource *source = E_SOURCE (source_object);
	EShellSidebar *shell_sidebar = user_data;
	ECalShellSidebarPrivate *priv;
	EShellContent *shell_content;
	EShellView *shell_view;
	ECalShellContent *cal_shell_content;
	ECalModel *model;
	EClient *client = NULL;
	GError *error = NULL;

	priv = E_CAL_SHELL_SIDEBAR (shell_sidebar)->priv;

	shell_view = e_shell_sidebar_get_shell_view (shell_sidebar);
	shell_content = e_shell_view_get_shell_content (shell_view);
	cal_shell_content = E_CAL_SHELL_CONTENT (shell_content);
	model = e_cal_shell_content_get_model (cal_shell_content);

	e_client_utils_open_new_finish (source, result, &client, &error);

	/* Ignore cancellations. */
	if (g_error_matches (error, E_CLIENT_ERROR, E_CLIENT_ERROR_CANCELLED) ||
	    g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_warn_if_fail (client == NULL);
		g_error_free (error);
		goto exit;

	} else if (error != NULL) {
		g_warn_if_fail (client == NULL);
		e_alert_submit (
			E_ALERT_SINK (shell_content),
			"calendar:failed-open-calendar",
			error->message, NULL);
		g_error_free (error);
		goto exit;
	}

	g_return_if_fail (E_IS_CAL_CLIENT (client));

	if (priv->default_client != NULL)
		g_object_unref (priv->default_client);

	priv->default_client = E_CAL_CLIENT (client);

	e_cal_client_set_default_timezone (
		priv->default_client, e_cal_model_get_timezone (model));

	g_object_notify (G_OBJECT (shell_sidebar), "default-client");

 exit:
	g_object_unref (shell_sidebar);
}

static void
cal_shell_sidebar_set_default (ECalShellSidebar *cal_shell_sidebar,
                               ESource *source)
{
	ECalShellSidebarPrivate *priv;
	EShellView *shell_view;
	EShellWindow *shell_window;
	EShellSidebar *shell_sidebar;
	ECalClient *client;
	const gchar *uid;

	priv = cal_shell_sidebar->priv;

	/* FIXME Sidebar should not be accessing the EShellContent.
	 *       This probably needs to be moved to ECalShellView. */
	shell_sidebar = E_SHELL_SIDEBAR (cal_shell_sidebar);
	shell_view = e_shell_sidebar_get_shell_view (shell_sidebar);
	shell_window = e_shell_view_get_shell_window (shell_view);

	/* Cancel any unfinished previous request. */
	if (priv->loading_default_client != NULL) {
		g_cancellable_cancel (priv->loading_default_client);
		g_object_unref (priv->loading_default_client);
		priv->loading_default_client = NULL;
	}

	uid = e_source_peek_uid (source);
	client = g_hash_table_lookup (priv->client_table, uid);

	/* If we already have an open connection for
	 * this UID, we can finish immediately. */
	if (client != NULL) {
		if (priv->default_client != NULL)
			g_object_unref (priv->default_client);
		priv->default_client = g_object_ref (client);
		g_object_notify (G_OBJECT (shell_sidebar), "default-client");
		return;
	}

	priv->loading_default_client = g_cancellable_new ();

	e_client_utils_open_new (
		source, E_CLIENT_SOURCE_TYPE_EVENTS,
		FALSE, priv->loading_default_client,
		e_client_utils_authenticate_handler,
		GTK_WINDOW (shell_window),
		cal_shell_sidebar_default_loaded_cb,
		g_object_ref (shell_sidebar));
}

static void
cal_shell_sidebar_row_changed_cb (ECalShellSidebar *cal_shell_sidebar,
                                  GtkTreePath *tree_path,
                                  GtkTreeIter *tree_iter,
                                  GtkTreeModel *tree_model)
{
	ESourceSelector *selector;
	ESource *source;

	selector = e_cal_shell_sidebar_get_selector (cal_shell_sidebar);
	source = e_source_selector_get_source_by_path (selector, tree_path);

	/* XXX This signal gets emitted a lot while the model is being
	 *     rebuilt, during which time we won't get a valid ESource.
	 *     ESourceSelector should probably block this signal while
	 *     rebuilding the model, but we'll be forgiving and not
	 *     emit a warning. */
	if (!E_IS_SOURCE (source))
		return;

	if (e_source_selector_source_is_selected (selector, source))
		e_cal_shell_sidebar_add_source (cal_shell_sidebar, source);
	else
		e_cal_shell_sidebar_remove_source (cal_shell_sidebar, source);
}

static void
cal_shell_sidebar_selection_changed_cb (ECalShellSidebar *cal_shell_sidebar,
                                        ESourceSelector *selector)
{
	EShellView *shell_view;
	EShellBackend *shell_backend;
	EShellSidebar *shell_sidebar;
	GSList *list, *iter;

	/* This signal is emitted less frequently than "row-changed",
	 * especially when the model is being rebuilt.  So we'll take
	 * it easy on poor GConf. */

	shell_sidebar = E_SHELL_SIDEBAR (cal_shell_sidebar);
	shell_view = e_shell_sidebar_get_shell_view (shell_sidebar);
	shell_backend = e_shell_view_get_shell_backend (shell_view);

	list = e_source_selector_get_selection (selector);

	for (iter = list; iter != NULL; iter = iter->next) {
		ESource *source = iter->data;

		iter->data = (gpointer) e_source_peek_uid (source);
		g_object_unref (source);
	}

	e_cal_shell_backend_set_selected_calendars (
		E_CAL_SHELL_BACKEND (shell_backend), list);

	g_slist_free (list);
}

static void
cal_shell_sidebar_primary_selection_changed_cb (ECalShellSidebar *cal_shell_sidebar,
                                                ESourceSelector *selector)
{
	ESource *source;

	source = e_source_selector_get_primary_selection (selector);
	if (source == NULL)
		return;

	cal_shell_sidebar_set_default (cal_shell_sidebar, source);
}

static void
cal_shell_sidebar_restore_state_cb (EShellWindow *shell_window,
                                    EShellView *shell_view,
                                    EShellSidebar *shell_sidebar)
{
	ECalShellSidebarPrivate *priv;
	EShell *shell;
	EShellBackend *shell_backend;
	EShellSettings *shell_settings;
	ESourceSelector *selector;
	ESourceList *source_list;
	ESource *source;
	GConfBridge *bridge;
	GtkTreeModel *model;
	GSList *list, *iter;
	GObject *object;
	const gchar *key;

	priv = E_CAL_SHELL_SIDEBAR (shell_sidebar)->priv;

	shell = e_shell_window_get_shell (shell_window);
	shell_settings = e_shell_get_shell_settings (shell);

	shell_backend = e_shell_view_get_shell_backend (shell_view);
	g_return_if_fail (E_IS_CAL_SHELL_BACKEND (shell_backend));

	selector = E_SOURCE_SELECTOR (priv->selector);
	model = gtk_tree_view_get_model (GTK_TREE_VIEW (selector));

	source_list = e_cal_shell_backend_get_source_list (
		E_CAL_SHELL_BACKEND (shell_backend));

	g_signal_connect_swapped (
		model, "row-changed",
		G_CALLBACK (cal_shell_sidebar_row_changed_cb),
		shell_sidebar);

	g_signal_connect_swapped (
		selector, "primary-selection-changed",
		G_CALLBACK (cal_shell_sidebar_primary_selection_changed_cb),
		shell_sidebar);

	g_object_bind_property_full (
		shell_settings, "cal-primary-calendar",
		selector, "primary-selection",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE,
		(GBindingTransformFunc) e_binding_transform_uid_to_source,
		(GBindingTransformFunc) e_binding_transform_source_to_uid,
		g_object_ref (source_list),
		(GDestroyNotify) g_object_unref);

	list = e_cal_shell_backend_get_selected_calendars (
		E_CAL_SHELL_BACKEND (shell_backend));

	for (iter = list; iter != NULL; iter = iter->next) {
		const gchar *uid = iter->data;

		source = e_source_list_peek_source_by_uid (source_list, uid);

		if (source != NULL)
			e_source_selector_select_source (selector, source);
	}

	g_slist_foreach (list, (GFunc) g_free, NULL);
	g_slist_free (list);

	/* Listen for subsequent changes to the selector. */

	g_signal_connect_swapped (
		selector, "selection-changed",
		G_CALLBACK (cal_shell_sidebar_selection_changed_cb),
		shell_sidebar);

	/* Bind GObject properties to GConf keys. */

	bridge = gconf_bridge_get ();

	object = G_OBJECT (priv->paned);
	key = "/apps/evolution/calendar/display/date_navigator_pane_position";
	gconf_bridge_bind_property_delayed (bridge, key, object, "vposition");
}

static void
cal_shell_sidebar_get_property (GObject *object,
                                guint property_id,
                                GValue *value,
                                GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_DATE_NAVIGATOR:
			g_value_set_object (
				value,
				e_cal_shell_sidebar_get_date_navigator (
				E_CAL_SHELL_SIDEBAR (object)));
			return;

		case PROP_DEFAULT_CLIENT:
			g_value_set_object (
				value,
				e_cal_shell_sidebar_get_default_client (
				E_CAL_SHELL_SIDEBAR (object)));
			return;

		case PROP_SELECTOR:
			g_value_set_object (
				value,
				e_cal_shell_sidebar_get_selector (
				E_CAL_SHELL_SIDEBAR (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
cal_shell_sidebar_dispose (GObject *object)
{
	ECalShellSidebarPrivate *priv;

	priv = E_CAL_SHELL_SIDEBAR (object)->priv;

	if (priv->paned != NULL) {
		g_object_unref (priv->paned);
		priv->paned = NULL;
	}

	if (priv->selector != NULL) {
		g_object_unref (priv->selector);
		priv->selector = NULL;
	}

	if (priv->date_navigator != NULL) {
		g_object_unref (priv->date_navigator);
		priv->date_navigator = NULL;
	}

	if (priv->default_client != NULL) {
		g_object_unref (priv->default_client);
		priv->default_client = NULL;
	}

	if (priv->loading_default_client != NULL) {
		g_cancellable_cancel (priv->loading_default_client);
		g_object_unref (priv->loading_default_client);
		priv->loading_default_client = NULL;
	}

	if (priv->loading_clients != NULL) {
		g_cancellable_cancel (priv->loading_clients);
		g_object_unref (priv->loading_clients);
		priv->loading_clients = NULL;
	}

	g_hash_table_remove_all (priv->client_table);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
cal_shell_sidebar_finalize (GObject *object)
{
	ECalShellSidebarPrivate *priv;

	priv = E_CAL_SHELL_SIDEBAR (object)->priv;

	g_hash_table_destroy (priv->client_table);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
new_calendar_clicked (GtkButton *button,
                      EShellSidebar *shell_sidebar)
{
	EShellView *shell_view;
	EShellWindow *shell_window;

	shell_view = e_shell_sidebar_get_shell_view (shell_sidebar);
	shell_window = e_shell_view_get_shell_window (shell_view);

	calendar_setup_new_calendar (GTK_WINDOW (shell_window));
}

static void
cal_shell_sidebar_constructed (GObject *object)
{
	ECalShellSidebarPrivate *priv;
	EShell *shell;
	EShellView *shell_view;
	EShellWindow *shell_window;
	EShellBackend *shell_backend;
	EShellSidebar *shell_sidebar;
	EShellSettings *shell_settings;
	ESourceList *source_list;
	ECalendarItem *calitem;
	GtkWidget *container;
	GtkWidget *widget;
	AtkObject *a11y;

	priv = E_CAL_SHELL_SIDEBAR (object)->priv;

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (parent_class)->constructed (object);

	shell_sidebar = E_SHELL_SIDEBAR (object);
	shell_view = e_shell_sidebar_get_shell_view (shell_sidebar);
	shell_backend = e_shell_view_get_shell_backend (shell_view);
	shell_window = e_shell_view_get_shell_window (shell_view);

	shell = e_shell_backend_get_shell (shell_backend);
	shell_settings = e_shell_get_shell_settings (shell);

	source_list = e_cal_shell_backend_get_source_list (
		E_CAL_SHELL_BACKEND (shell_backend));

	container = GTK_WIDGET (shell_sidebar);

	widget = e_paned_new (GTK_ORIENTATION_VERTICAL);
	gtk_container_add (GTK_CONTAINER (container), widget);
	priv->paned = g_object_ref (widget);
	gtk_widget_show (widget);

	container = widget;

	widget = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (
		GTK_SCROLLED_WINDOW (widget),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (
		GTK_SCROLLED_WINDOW (widget), GTK_SHADOW_IN);
	if (!e_shell_get_express_mode (shell)) {
		gtk_paned_pack1 (GTK_PANED (container), widget, TRUE, TRUE);
	} else {
		GtkWidget *button;

		container = gtk_vbox_new (FALSE, 6);
		gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);

		button = gtk_button_new_with_mnemonic (_("_New Calendar..."));
		gtk_box_pack_start (GTK_BOX (container), button, FALSE, FALSE, 0);
		g_signal_connect (
			button, "clicked",
			G_CALLBACK (new_calendar_clicked), shell_sidebar);

		gtk_paned_pack1 (GTK_PANED (priv->paned), container, TRUE, TRUE);
		gtk_widget_show_all (container);
	}
	gtk_widget_show (widget);

	container = widget;

	widget = e_calendar_selector_new (source_list);
	e_source_selector_set_select_new (E_SOURCE_SELECTOR (widget), TRUE);
	gtk_container_add (GTK_CONTAINER (container), widget);
	a11y = gtk_widget_get_accessible (widget);
	atk_object_set_name (a11y, _("Calendar Selector"));
	priv->selector = g_object_ref (widget);
	gtk_widget_show (widget);

	container = priv->paned;

	widget = e_calendar_new ();
	calitem = E_CALENDAR (widget)->calitem;
	e_calendar_item_set_days_start_week_sel (calitem, 9);
	e_calendar_item_set_max_days_sel (calitem, 42);
	gtk_paned_pack2 (GTK_PANED (container), widget, FALSE, FALSE);
	priv->date_navigator = g_object_ref (widget);
	gtk_widget_show (widget);

	g_object_bind_property (
		shell_settings, "cal-show-week-numbers",
		calitem, "show-week-numbers",
		G_BINDING_SYNC_CREATE);

	g_object_bind_property (
		shell_settings, "cal-week-start-day",
		calitem, "week-start-day",
		G_BINDING_SYNC_CREATE);

	/* Restore widget state from the last session once
	 * the shell view is fully initialized and visible. */
	g_signal_connect (
		shell_window, "shell-view-created::calendar",
		G_CALLBACK (cal_shell_sidebar_restore_state_cb),
		shell_sidebar);
}

static guint32
cal_shell_sidebar_check_state (EShellSidebar *shell_sidebar)
{
	ECalShellSidebar *cal_shell_sidebar;
	ESourceSelector *selector;
	ESource *source;
	gboolean can_delete = FALSE;
	gboolean is_system = FALSE;
	gboolean refresh_supported = FALSE;
	guint32 state = 0;

	cal_shell_sidebar = E_CAL_SHELL_SIDEBAR (shell_sidebar);
	selector = e_cal_shell_sidebar_get_selector (cal_shell_sidebar);
	source = e_source_selector_get_primary_selection (selector);

	if (source != NULL) {
		ECalClient *client;
		const gchar *uri;
		const gchar *delete;

		uri = e_source_peek_relative_uri (source);
		is_system = (uri == NULL || strcmp (uri, "system") == 0);

		can_delete = !is_system;
		delete = e_source_get_property (source, "delete");
		can_delete &= (delete == NULL || strcmp (delete, "no") != 0);

		client = g_hash_table_lookup (
			cal_shell_sidebar->priv->client_table,
			e_source_peek_uid (source));
		refresh_supported =
			client && e_client_check_refresh_supported (E_CLIENT (client));
	}

	if (source != NULL)
		state |= E_CAL_SHELL_SIDEBAR_HAS_PRIMARY_SOURCE;
	if (can_delete)
		state |= E_CAL_SHELL_SIDEBAR_CAN_DELETE_PRIMARY_SOURCE;
	if (is_system)
		state |= E_CAL_SHELL_SIDEBAR_PRIMARY_SOURCE_IS_SYSTEM;
	if (refresh_supported)
		state |= E_CAL_SHELL_SIDEBAR_SOURCE_SUPPORTS_REFRESH;

	return state;
}

static void
cal_shell_sidebar_client_removed (ECalShellSidebar *cal_shell_sidebar,
                                  ECalClient *client)
{
	ESourceSelector *selector;
	GHashTable *client_table;
	ESource *source;
	const gchar *uid;

	client_table = cal_shell_sidebar->priv->client_table;
	selector = e_cal_shell_sidebar_get_selector (cal_shell_sidebar);

	g_signal_handlers_disconnect_matched (
		client, G_SIGNAL_MATCH_DATA, 0, 0,
		NULL, NULL, cal_shell_sidebar);

	source = e_client_get_source (E_CLIENT (client));
	uid = e_source_peek_uid (source);
	g_return_if_fail (uid != NULL);

	g_hash_table_remove (client_table, uid);
	e_source_selector_unselect_source (selector, source);

	cal_shell_sidebar_emit_status_message (cal_shell_sidebar, NULL);
}

static void
cal_shell_sidebar_class_init (ECalShellSidebarClass *class)
{
	GObjectClass *object_class;
	EShellSidebarClass *shell_sidebar_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (ECalShellSidebarPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->get_property = cal_shell_sidebar_get_property;
	object_class->dispose = cal_shell_sidebar_dispose;
	object_class->finalize = cal_shell_sidebar_finalize;
	object_class->constructed = cal_shell_sidebar_constructed;

	shell_sidebar_class = E_SHELL_SIDEBAR_CLASS (class);
	shell_sidebar_class->check_state = cal_shell_sidebar_check_state;

	class->client_removed = cal_shell_sidebar_client_removed;

	g_object_class_install_property (
		object_class,
		PROP_DATE_NAVIGATOR,
		g_param_spec_object (
			"date-navigator",
			"Date Navigator Widget",
			"This widget displays a miniature calendar",
			E_TYPE_CALENDAR,
			G_PARAM_READABLE));

	g_object_class_install_property (
		object_class,
		PROP_DEFAULT_CLIENT,
		g_param_spec_object (
			"default-client",
			"Default Calendar ECalClient",
			"Default client for calendar operations",
			E_TYPE_CAL_CLIENT,
			G_PARAM_READABLE));

	g_object_class_install_property (
		object_class,
		PROP_SELECTOR,
		g_param_spec_object (
			"selector",
			"Source Selector Widget",
			"This widget displays groups of calendars",
			E_TYPE_SOURCE_SELECTOR,
			G_PARAM_READABLE));

	signals[CLIENT_ADDED] = g_signal_new (
		"client-added",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (ECalShellSidebarClass, client_added),
		NULL, NULL,
		g_cclosure_marshal_VOID__OBJECT,
		G_TYPE_NONE, 1,
		E_TYPE_CAL_CLIENT);

	signals[CLIENT_REMOVED] = g_signal_new (
		"client-removed",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (ECalShellSidebarClass, client_removed),
		NULL, NULL,
		g_cclosure_marshal_VOID__OBJECT,
		G_TYPE_NONE, 1,
		E_TYPE_CAL_CLIENT);

	signals[STATUS_MESSAGE] = g_signal_new (
		"status-message",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (ECalShellSidebarClass, status_message),
		NULL, NULL,
		g_cclosure_marshal_VOID__STRING,
		G_TYPE_NONE, 1,
		G_TYPE_STRING);
}

static void
cal_shell_sidebar_init (ECalShellSidebar *cal_shell_sidebar)
{
	GHashTable *client_table;

	client_table = g_hash_table_new_full (
		g_str_hash, g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) g_object_unref);

	cal_shell_sidebar->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		cal_shell_sidebar, E_TYPE_CAL_SHELL_SIDEBAR,
		ECalShellSidebarPrivate);

	cal_shell_sidebar->priv->client_table = client_table;
	cal_shell_sidebar->priv->loading_clients = g_cancellable_new ();

	/* Postpone widget construction until we have a shell view. */
}

GType
e_cal_shell_sidebar_get_type (void)
{
	return cal_shell_sidebar_type;
}

void
e_cal_shell_sidebar_register_type (GTypeModule *type_module)
{
	static const GTypeInfo type_info = {
		sizeof (ECalShellSidebarClass),
		(GBaseInitFunc) NULL,
		(GBaseFinalizeFunc) NULL,
		(GClassInitFunc) cal_shell_sidebar_class_init,
		(GClassFinalizeFunc) NULL,
		NULL,  /* class_data */
		sizeof (ECalShellSidebar),
		0,     /* n_preallocs */
		(GInstanceInitFunc) cal_shell_sidebar_init,
		NULL   /* value_table */
	};

	cal_shell_sidebar_type = g_type_module_register_type (
		type_module, E_TYPE_SHELL_SIDEBAR,
		"ECalShellSidebar", &type_info, 0);
}

GtkWidget *
e_cal_shell_sidebar_new (EShellView *shell_view)
{
	g_return_val_if_fail (E_IS_SHELL_VIEW (shell_view), NULL);

	return g_object_new (
		E_TYPE_CAL_SHELL_SIDEBAR,
		"shell-view", shell_view, NULL);
}

GList *
e_cal_shell_sidebar_get_clients (ECalShellSidebar *cal_shell_sidebar)
{
	GHashTable *client_table;

	g_return_val_if_fail (
		E_IS_CAL_SHELL_SIDEBAR (cal_shell_sidebar), NULL);

	client_table = cal_shell_sidebar->priv->client_table;

	return g_hash_table_get_values (client_table);
}

ECalendar *
e_cal_shell_sidebar_get_date_navigator (ECalShellSidebar *cal_shell_sidebar)
{
	g_return_val_if_fail (
		E_IS_CAL_SHELL_SIDEBAR (cal_shell_sidebar), NULL);

	return E_CALENDAR (cal_shell_sidebar->priv->date_navigator);
}

ECalClient *
e_cal_shell_sidebar_get_default_client (ECalShellSidebar *cal_shell_sidebar)
{
	g_return_val_if_fail (
		E_IS_CAL_SHELL_SIDEBAR (cal_shell_sidebar), NULL);

	return cal_shell_sidebar->priv->default_client;
}

ESourceSelector *
e_cal_shell_sidebar_get_selector (ECalShellSidebar *cal_shell_sidebar)
{
	g_return_val_if_fail (
		E_IS_CAL_SHELL_SIDEBAR (cal_shell_sidebar), NULL);

	return E_SOURCE_SELECTOR (cal_shell_sidebar->priv->selector);
}

void
e_cal_shell_sidebar_add_source (ECalShellSidebar *cal_shell_sidebar,
                                ESource *source)
{
	EShellView *shell_view;
	EShellContent *shell_content;
	EShellSidebar *shell_sidebar;
	ECalShellContent *cal_shell_content;
	ECalClientSourceType source_type;
	ESourceSelector *selector;
	GHashTable *client_table;
	ECalModel *model;
	ECalClient *default_client;
	ECalClient *client;
	icaltimezone *timezone;
	const gchar *uid;
	const gchar *uri;
	gchar *message;

	g_return_if_fail (E_IS_CAL_SHELL_SIDEBAR (cal_shell_sidebar));
	g_return_if_fail (E_IS_SOURCE (source));

	source_type = E_CAL_CLIENT_SOURCE_TYPE_EVENTS;
	client_table = cal_shell_sidebar->priv->client_table;
	default_client = cal_shell_sidebar->priv->default_client;
	selector = e_cal_shell_sidebar_get_selector (cal_shell_sidebar);

	uid = e_source_peek_uid (source);
	client = g_hash_table_lookup (client_table, uid);

	if (client != NULL)
		return;

	if (default_client != NULL) {
		ESource *default_source;
		const gchar *default_uid;

		default_source = e_client_get_source (E_CLIENT (default_client));
		default_uid = e_source_peek_uid (default_source);

		if (g_strcmp0 (uid, default_uid) == 0)
			client = g_object_ref (default_client);
	}

	if (client == NULL) {
		client = e_cal_client_new (source, source_type, NULL);
		if (client)
			g_signal_connect (
				client, "authenticate",
				G_CALLBACK (e_client_utils_authenticate_handler), NULL);
	}

	g_return_if_fail (client != NULL);

	g_signal_connect_swapped (
		client, "backend-died",
		G_CALLBACK (cal_shell_sidebar_backend_died_cb),
		cal_shell_sidebar);

	g_signal_connect_swapped (
		client, "backend-error",
		G_CALLBACK (cal_shell_sidebar_backend_error_cb),
		cal_shell_sidebar);

	g_hash_table_insert (client_table, g_strdup (uid), client);
	e_source_selector_select_source (selector, source);

	uri = e_client_get_uri (E_CLIENT (client));
	/* Translators: The string field is a URI. */
	message = g_strdup_printf (_("Opening calendar at %s"), uri);
	cal_shell_sidebar_emit_status_message (cal_shell_sidebar, message);
	g_free (message);

	/* FIXME Sidebar should not be accessing the EShellContent.
	 *       This probably needs to be moved to ECalShellView. */
	shell_sidebar = E_SHELL_SIDEBAR (cal_shell_sidebar);
	shell_view = e_shell_sidebar_get_shell_view (shell_sidebar);
	shell_content = e_shell_view_get_shell_content (shell_view);

	cal_shell_content = E_CAL_SHELL_CONTENT (shell_content);
	model = e_cal_shell_content_get_model (cal_shell_content);
	timezone = e_cal_model_get_timezone (model);

	e_cal_client_set_default_timezone (client, timezone);

	e_client_open (
		E_CLIENT (client), FALSE,
		cal_shell_sidebar->priv->loading_clients,
		cal_shell_sidebar_client_opened_cb, cal_shell_sidebar);
}

void
e_cal_shell_sidebar_remove_source (ECalShellSidebar *cal_shell_sidebar,
                                   ESource *source)
{
	GHashTable *client_table;
	ECalClient *client;
	const gchar *uid;

	g_return_if_fail (E_IS_CAL_SHELL_SIDEBAR (cal_shell_sidebar));
	g_return_if_fail (E_IS_SOURCE (source));

	client_table = cal_shell_sidebar->priv->client_table;

	uid = e_source_peek_uid (source);
	client = g_hash_table_lookup (client_table, uid);

	if (client == NULL)
		return;

	cal_shell_sidebar_emit_client_removed (cal_shell_sidebar, client);
}

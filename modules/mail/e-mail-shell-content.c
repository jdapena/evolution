/*
 * e-mail-shell-content.c
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

#include "e-mail-shell-content.h"

#include <glib/gi18n.h>
#include <libedataserver/e-data-server-util.h>

#include "e-util/e-util-private.h"
#include "e-util/gconf-bridge.h"
#include "widgets/menus/gal-view-etable.h"
#include "widgets/menus/gal-view-instance.h"
#include "widgets/misc/e-paned.h"
#include "widgets/misc/e-preview-pane.h"
#include "widgets/misc/e-search-bar.h"

#include "em-utils.h"
#include "mail-ops.h"
#include "message-list.h"

#include "e-mail-paned-view.h"
#include "e-mail-notebook-view.h"
#include "e-mail-reader.h"
#include "e-mail-reader-utils.h"
#include "e-mail-shell-backend.h"
#include "e-mail-shell-view-actions.h"

struct _EMailShellContentPrivate {
	EMailView *mail_view;
};

enum {
	PROP_0,
	PROP_FORWARD_STYLE,
	PROP_GROUP_BY_THREADS,
	PROP_MAIL_VIEW,
	PROP_REPLY_STYLE
};

static gpointer parent_class;
static GType mail_shell_content_type;

static void
reconnect_changed_event (EMailReader *child,
                         EMailReader *parent)
{
	g_signal_emit_by_name (parent, "changed");
}

static void
reconnect_folder_loaded_event (EMailReader *child,
                               EMailReader *parent)
{
	g_signal_emit_by_name (parent, "folder-loaded");
}

static void
mail_shell_content_view_changed_cb (EMailView *view,
                                    EMailShellContent *content)
{
	g_object_freeze_notify (G_OBJECT (content));
	g_object_notify (G_OBJECT (content), "group-by-threads");
	g_object_thaw_notify (G_OBJECT (content));
}

static void
mail_shell_content_set_property (GObject *object,
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
mail_shell_content_get_property (GObject *object,
                                 guint property_id,
                                 GValue *value,
                                 GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_FORWARD_STYLE:
			g_value_set_enum (
				value, e_mail_reader_get_forward_style (
				E_MAIL_READER (object)));
			return;

		case PROP_GROUP_BY_THREADS:
			g_value_set_boolean (
				value, e_mail_reader_get_group_by_threads (
				E_MAIL_READER (object)));
			return;

		case PROP_MAIL_VIEW:
			g_value_set_object (
				value, e_mail_shell_content_get_mail_view (
				E_MAIL_SHELL_CONTENT (object)));
			return;

		case PROP_REPLY_STYLE:
			g_value_set_enum (
				value, e_mail_reader_get_reply_style (
				E_MAIL_READER (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_shell_content_dispose (GObject *object)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (object)->priv;

	if (priv->mail_view != NULL) {
		g_object_unref (priv->mail_view);
		priv->mail_view = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
mail_shell_content_constructed (GObject *object)
{
	EMailShellContentPrivate *priv;
	EShellContent *shell_content;
	EShellView *shell_view;
	GtkWidget *container;
	GtkWidget *widget;

	priv = E_MAIL_SHELL_CONTENT (object)->priv;

	/* Chain up to parent's constructed () method. */
	G_OBJECT_CLASS (parent_class)->constructed (object);

	shell_content = E_SHELL_CONTENT (object);
	shell_view = e_shell_content_get_shell_view (shell_content);

	/* Build content widgets. */

	container = GTK_WIDGET (object);

	if (e_shell_get_express_mode (e_shell_get_default ())) {
		widget = e_mail_notebook_view_new (shell_view);
		g_signal_connect (
			widget, "view-changed",
			G_CALLBACK (mail_shell_content_view_changed_cb),
			object);
	} else
		widget = e_mail_paned_view_new (shell_view);

	gtk_container_add (GTK_CONTAINER (container), widget);
	priv->mail_view = g_object_ref (widget);
	gtk_widget_show (widget);

	g_signal_connect (
		widget, "changed",
		G_CALLBACK (reconnect_changed_event), object);
	g_signal_connect (
		widget, "folder-loaded",
		G_CALLBACK (reconnect_folder_loaded_event), object);
}

static guint32
mail_shell_content_check_state (EShellContent *shell_content)
{
	EMailShellContentPrivate *priv;
	EMailReader *reader;

	priv = E_MAIL_SHELL_CONTENT (shell_content)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	return e_mail_reader_check_state (reader);
}

static void
mail_shell_content_focus_search_results (EShellContent *shell_content)
{
	EMailShellContentPrivate *priv;
	GtkWidget *message_list;
	EMailReader *reader;

	priv = E_MAIL_SHELL_CONTENT (shell_content)->priv;

	reader = E_MAIL_READER (priv->mail_view);
	message_list = e_mail_reader_get_message_list (reader);

	gtk_widget_grab_focus (message_list);
}

static guint
mail_shell_content_open_selected_mail (EMailReader *reader)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	return e_mail_reader_open_selected_mail (reader);
}

static GtkActionGroup *
mail_shell_content_get_action_group (EMailReader *reader,
                                     EMailReaderActionGroup group)
{
	EShellView *shell_view;
	EShellWindow *shell_window;
	EShellContent *shell_content;
	const gchar *group_name;

	shell_content = E_SHELL_CONTENT (reader);
	shell_view = e_shell_content_get_shell_view (shell_content);
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
mail_shell_content_get_alert_sink (EMailReader *reader)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	return e_mail_reader_get_alert_sink (reader);
}

static EMailBackend *
mail_shell_content_get_backend (EMailReader *reader)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	return e_mail_reader_get_backend (reader);
}

static EMailDisplay *
mail_shell_content_get_mail_display(EMailReader *reader)
{
	EMailShellContentPrivate *priv;
	EMailDisplay *display;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);
	return e_mail_reader_get_mail_display (reader);
}

static gboolean
mail_shell_content_get_hide_deleted (EMailReader *reader)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	return e_mail_reader_get_hide_deleted (reader);
}

static GtkWidget *
mail_shell_content_get_message_list (EMailReader *reader)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	return e_mail_reader_get_message_list (reader);
}

static GtkMenu *
mail_shell_content_get_popup_menu (EMailReader *reader)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	return e_mail_reader_get_popup_menu (reader);
}

static GtkWindow *
mail_shell_content_get_window (EMailReader *reader)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	return e_mail_reader_get_window (reader);
}

static void
mail_shell_content_set_folder (EMailReader *reader,
                               CamelFolder *folder)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	return e_mail_reader_set_folder (reader, folder);
}

static void
mail_shell_content_show_search_bar (EMailReader *reader)
{
	EMailShellContentPrivate *priv;

	priv = E_MAIL_SHELL_CONTENT (reader)->priv;

	/* Forward this to our internal EMailView, which
	 * also implements the EMailReader interface. */
	reader = E_MAIL_READER (priv->mail_view);

	e_mail_reader_show_search_bar (reader);
}

static void
mail_shell_content_class_init (EMailShellContentClass *class)
{
	GObjectClass *object_class;
	EShellContentClass *shell_content_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EMailShellContentPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = mail_shell_content_set_property;
	object_class->get_property = mail_shell_content_get_property;
	object_class->dispose = mail_shell_content_dispose;
	object_class->constructed = mail_shell_content_constructed;

	shell_content_class = E_SHELL_CONTENT_CLASS (class);
	shell_content_class->check_state = mail_shell_content_check_state;
	shell_content_class->focus_search_results =
		mail_shell_content_focus_search_results;

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

	g_object_class_install_property (
		object_class,
		PROP_MAIL_VIEW,
		g_param_spec_object (
			"mail-view",
			"Mail View",
			NULL,
			E_TYPE_MAIL_VIEW,
			G_PARAM_READABLE));

	/* Inherited from EMailReader */
	g_object_class_override_property (
		object_class,
		PROP_REPLY_STYLE,
		"reply-style");
}

static void
mail_shell_content_init (EMailShellContent *mail_shell_content)
{
	mail_shell_content->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		mail_shell_content, E_TYPE_MAIL_SHELL_CONTENT,
		EMailShellContentPrivate);

	/* Postpone widget construction until we have a shell view. */
}

GType
e_mail_shell_content_get_type (void)
{
	return mail_shell_content_type;
}

static void
mail_shell_content_reader_init (EMailReaderInterface *interface)
{
	interface->get_action_group = mail_shell_content_get_action_group;
	interface->get_alert_sink = mail_shell_content_get_alert_sink;
	interface->get_backend = mail_shell_content_get_backend;
	interface->get_mail_display = mail_shell_content_get_mail_display;
	interface->get_hide_deleted = mail_shell_content_get_hide_deleted;
	interface->get_message_list = mail_shell_content_get_message_list;
	interface->get_popup_menu = mail_shell_content_get_popup_menu;
	interface->get_window = mail_shell_content_get_window;
	interface->set_folder = mail_shell_content_set_folder;
	interface->show_search_bar = mail_shell_content_show_search_bar;
	interface->open_selected_mail = mail_shell_content_open_selected_mail;
}

void
e_mail_shell_content_register_type (GTypeModule *type_module)
{
	static const GTypeInfo type_info = {
		sizeof (EMailShellContentClass),
		(GBaseInitFunc) NULL,
		(GBaseFinalizeFunc) NULL,
		(GClassInitFunc) mail_shell_content_class_init,
		(GClassFinalizeFunc) NULL,
		NULL,  /* class_data */
		sizeof (EMailShellContent),
		0,     /* n_preallocs */
		(GInstanceInitFunc) mail_shell_content_init,
		NULL   /* value_table */
	};

	static const GInterfaceInfo reader_info = {
		(GInterfaceInitFunc) mail_shell_content_reader_init,
		(GInterfaceFinalizeFunc) NULL,
		NULL  /* interface_data */
	};

	mail_shell_content_type = g_type_module_register_type (
		type_module, E_TYPE_SHELL_CONTENT,
		"EMailShellContent", &type_info, 0);

	g_type_module_add_interface (
		type_module, mail_shell_content_type,
		E_TYPE_MAIL_READER, &reader_info);
}

GtkWidget *
e_mail_shell_content_new (EShellView *shell_view)
{
	g_return_val_if_fail (E_IS_SHELL_VIEW (shell_view), NULL);

	return g_object_new (
		E_TYPE_MAIL_SHELL_CONTENT,
		"shell-view", shell_view, NULL);
}

EMailView *
e_mail_shell_content_get_mail_view (EMailShellContent *mail_shell_content)
{
	g_return_val_if_fail (
		E_IS_MAIL_SHELL_CONTENT (mail_shell_content), NULL);

	return mail_shell_content->priv->mail_view;
}

EShellSearchbar *
e_mail_shell_content_get_searchbar (EMailShellContent *mail_shell_content)
{
	GtkWidget *searchbar;
	EShellView *shell_view;
	EShellContent *shell_content;

	g_return_val_if_fail (
		E_IS_MAIL_SHELL_CONTENT (mail_shell_content), NULL);

	shell_content = E_SHELL_CONTENT (mail_shell_content);
	shell_view = e_shell_content_get_shell_view (shell_content);
	searchbar = e_shell_view_get_searchbar (shell_view);

	return E_SHELL_SEARCHBAR (searchbar);
}

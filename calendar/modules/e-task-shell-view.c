/*
 * e-task-shell-view.c
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

#include "e-task-shell-view-private.h"

enum {
	PROP_0,
	PROP_SOURCE_LIST
};

GType e_task_shell_view_type = 0;
static gpointer parent_class;

static void
task_shell_view_get_property (GObject *object,
                              guint property_id,
                              GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_SOURCE_LIST:
			g_value_set_object (
				value, e_task_shell_view_get_source_list (
				E_TASK_SHELL_VIEW (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
task_shell_view_dispose (GObject *object)
{
	e_task_shell_view_private_dispose (E_TASK_SHELL_VIEW (object));

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
task_shell_view_finalize (GObject *object)
{
	e_task_shell_view_private_finalize (E_TASK_SHELL_VIEW (object));

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
task_shell_view_constructed (GObject *object)
{
	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (parent_class)->constructed (object);

	e_task_shell_view_private_constructed (E_TASK_SHELL_VIEW (object));
}

static void
task_shell_view_update_actions (EShellView *shell_view)
{
	ETaskShellViewPrivate *priv;
	EShellContent *shell_content;
	EShellSidebar *shell_sidebar;
	EShellWindow *shell_window;
	GtkAction *action;
	const gchar *label;
	gboolean sensitive;
	guint32 state;

	/* Be descriptive. */
	gboolean any_tasks_selected;
	gboolean has_primary_source;
	gboolean multiple_tasks_selected;
	gboolean primary_source_is_system;
	gboolean selection_has_url;
	gboolean selection_is_assignable;
	gboolean single_task_selected;
	gboolean some_tasks_complete;
	gboolean some_tasks_incomplete;
	gboolean sources_are_editable;

	priv = E_TASK_SHELL_VIEW_GET_PRIVATE (shell_view);

	shell_window = e_shell_view_get_shell_window (shell_view);

	shell_content = e_shell_view_get_shell_content (shell_view);
	state = e_shell_content_check_state (shell_content);

	single_task_selected =
		(state & E_TASK_SHELL_CONTENT_SELECTION_SINGLE);
	multiple_tasks_selected =
		(state & E_TASK_SHELL_CONTENT_SELECTION_MULTIPLE);
	selection_is_assignable =
		(state & E_TASK_SHELL_CONTENT_SELECTION_CAN_ASSIGN);
	sources_are_editable =
		(state & E_TASK_SHELL_CONTENT_SELECTION_CAN_EDIT);
	some_tasks_complete =
		(state & E_TASK_SHELL_CONTENT_SELECTION_HAS_COMPLETE);
	some_tasks_incomplete =
		(state & E_TASK_SHELL_CONTENT_SELECTION_HAS_INCOMPLETE);
	selection_has_url =
		(state & E_TASK_SHELL_CONTENT_SELECTION_HAS_URL);

	shell_sidebar = e_shell_view_get_shell_sidebar (shell_view);
	state = e_shell_sidebar_check_state (shell_sidebar);

	has_primary_source =
		(state & E_TASK_SHELL_SIDEBAR_HAS_PRIMARY_SOURCE);
	primary_source_is_system =
		(state & E_TASK_SHELL_SIDEBAR_PRIMARY_SOURCE_IS_SYSTEM);

	any_tasks_selected =
		(single_task_selected || multiple_tasks_selected);

	action = ACTION (TASK_ASSIGN);
	sensitive =
		single_task_selected && sources_are_editable &&
		selection_is_assignable;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_CLIPBOARD_COPY);
	sensitive = any_tasks_selected;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_CLIPBOARD_CUT);
	sensitive = any_tasks_selected && sources_are_editable;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_CLIPBOARD_PASTE);
	sensitive = sources_are_editable;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_DELETE);
	sensitive = any_tasks_selected && sources_are_editable;
	gtk_action_set_sensitive (action, sensitive);
	if (multiple_tasks_selected)
		label = _("Delete Tasks");
	else
		label = _("Delete Task");
	g_object_set (action, "label", label, NULL);

	action = ACTION (TASK_FORWARD);
	sensitive = single_task_selected;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_LIST_COPY);
	sensitive = has_primary_source;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_LIST_DELETE);
	sensitive = has_primary_source && !primary_source_is_system;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_LIST_PROPERTIES);
	sensitive = has_primary_source;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_MARK_COMPLETE);
	sensitive =
		any_tasks_selected &&
		sources_are_editable &&
		some_tasks_incomplete;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_MARK_INCOMPLETE);
	sensitive =
		any_tasks_selected &&
		sources_are_editable &&
		some_tasks_complete;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_OPEN);
	sensitive = single_task_selected;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_OPEN_URL);
	sensitive = single_task_selected && selection_has_url;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_PRINT);
	sensitive = single_task_selected;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_PURGE);
	sensitive = sources_are_editable;
	gtk_action_set_sensitive (action, sensitive);

	action = ACTION (TASK_SAVE_AS);
	sensitive = single_task_selected;
	gtk_action_set_sensitive (action, sensitive);
}

static void
task_shell_view_class_init (ETaskShellViewClass *class,
                            GTypeModule *type_module)
{
	GObjectClass *object_class;
	EShellViewClass *shell_view_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (ETaskShellViewPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->get_property = task_shell_view_get_property;
	object_class->dispose = task_shell_view_dispose;
	object_class->finalize = task_shell_view_finalize;
	object_class->constructed = task_shell_view_constructed;

	shell_view_class = E_SHELL_VIEW_CLASS (class);
	shell_view_class->label = _("Tasks");
	shell_view_class->icon_name = "evolution-tasks";
	shell_view_class->ui_definition = "evolution-tasks.ui";
	shell_view_class->search_options = "/task-search-options";
	shell_view_class->search_rules = "tasktypes.xml";
	shell_view_class->type_module = type_module;
	shell_view_class->new_shell_content = e_task_shell_content_new;
	shell_view_class->new_shell_sidebar = e_task_shell_sidebar_new;
	shell_view_class->update_actions = task_shell_view_update_actions;

	g_object_class_install_property (
		object_class,
		PROP_SOURCE_LIST,
		g_param_spec_object (
			"source-list",
			_("Source List"),
			_("The registry of task lists"),
			E_TYPE_SOURCE_LIST,
			G_PARAM_READABLE));
}

static void
task_shell_view_init (ETaskShellView *task_shell_view,
                      EShellViewClass *shell_view_class)
{
	task_shell_view->priv =
		E_TASK_SHELL_VIEW_GET_PRIVATE (task_shell_view);

	e_task_shell_view_private_init (task_shell_view, shell_view_class);
}

GType
e_task_shell_view_get_type (GTypeModule *type_module)
{
	if (e_task_shell_view_type == 0) {
		const GTypeInfo type_info = {
			sizeof (ETaskShellViewClass),
			(GBaseInitFunc) NULL,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) task_shell_view_class_init,
			(GClassFinalizeFunc) NULL,
			type_module,
			sizeof (ETaskShellView),
			0,    /* n_preallocs */
			(GInstanceInitFunc) task_shell_view_init,
			NULL  /* value_table */
		};

		e_task_shell_view_type =
			g_type_module_register_type (
				type_module, E_TYPE_SHELL_VIEW,
				"ETaskShellView", &type_info, 0);
	}

	return e_task_shell_view_type;
}

ESourceList *
e_task_shell_view_get_source_list (ETaskShellView *task_shell_view)
{
	g_return_val_if_fail (E_IS_TASK_SHELL_VIEW (task_shell_view), NULL);

	return task_shell_view->priv->source_list;
}

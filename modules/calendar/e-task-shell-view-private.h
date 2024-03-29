/*
 * e-task-shell-view-private.h
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

#ifndef E_TASK_SHELL_VIEW_PRIVATE_H
#define E_TASK_SHELL_VIEW_PRIVATE_H

#include "e-task-shell-view.h"

#include <string.h>
#include <glib/gi18n.h>
#include <libecal/e-cal-time-util.h>
#include <libedataserver/e-categories.h>
#include <libedataserver/e-sexp.h>
#include <libedataserverui/e-client-utils.h>

#include "e-util/e-dialog-utils.h"
#include "e-util/e-file-utils.h"
#include "e-util/e-util.h"
#include "e-util/gconf-bridge.h"
#include "shell/e-shell-utils.h"
#include "misc/e-popup-action.h"
#include "misc/e-selectable.h"

#include "calendar/gui/calendar-config.h"
#include "calendar/gui/comp-util.h"
#include "calendar/gui/e-cal-component-preview.h"
#include "calendar/gui/e-cal-model-tasks.h"
#include "calendar/gui/e-calendar-selector.h"
#include "calendar/gui/print.h"
#include "calendar/gui/dialogs/calendar-setup.h"
#include "calendar/gui/dialogs/copy-source-dialog.h"
#include "calendar/gui/dialogs/task-editor.h"

#include "e-task-shell-backend.h"
#include "e-task-shell-content.h"
#include "e-task-shell-sidebar.h"
#include "e-task-shell-view-actions.h"

/* Shorthand, requires a variable named "shell_window". */
#define ACTION(name) \
	(E_SHELL_WINDOW_ACTION_##name (shell_window))
#define ACTION_GROUP(name) \
	(E_SHELL_WINDOW_ACTION_GROUP_##name (shell_window))

/* For use in dispose() methods. */
#define DISPOSE(obj) \
	G_STMT_START { \
	if ((obj) != NULL) { g_object_unref (obj); (obj) = NULL; } \
	} G_STMT_END

/* ETable Specifications */
#define ETSPEC_FILENAME		"e-calendar-table.etspec"

G_BEGIN_DECLS

/* Filter items are displayed in ascending order.
 * Non-negative values are reserved for categories. */
enum {
	TASK_FILTER_ANY_CATEGORY		= -7,
	TASK_FILTER_UNMATCHED			= -6,
	TASK_FILTER_NEXT_7_DAYS_TASKS		= -5,
	TASK_FILTER_ACTIVE_TASKS		= -4,
	TASK_FILTER_OVERDUE_TASKS		= -3,
	TASK_FILTER_COMPLETED_TASKS		= -2,
	TASK_FILTER_TASKS_WITH_ATTACHMENTS	= -1
};

/* Search items are displayed in ascending order. */
enum {
	TASK_SEARCH_ADVANCED = -1,
	TASK_SEARCH_SUMMARY_CONTAINS,
	TASK_SEARCH_DESCRIPTION_CONTAINS,
	TASK_SEARCH_ANY_FIELD_CONTAINS
};

struct _ETaskShellViewPrivate {

	/* These are just for convenience. */
	ETaskShellBackend *task_shell_backend;
	ETaskShellContent *task_shell_content;
	ETaskShellSidebar *task_shell_sidebar;

	EActivity *activity;
	guint update_timeout;

	guint confirm_purge : 1;
};

void		e_task_shell_view_private_init
					(ETaskShellView *task_shell_view,
					 EShellViewClass *shell_view_class);
void		e_task_shell_view_private_constructed
					(ETaskShellView *task_shell_view);
void		e_task_shell_view_private_dispose
					(ETaskShellView *task_shell_view);
void		e_task_shell_view_private_finalize
					(ETaskShellView *task_shell_view);

/* Private Utilities */

void		e_task_shell_view_actions_init
					(ETaskShellView *task_shell_view);
void		e_task_shell_view_open_task
					(ETaskShellView *task_shell_view,
					 ECalModelComponent *comp_data);
void		e_task_shell_view_delete_completed
					(ETaskShellView *task_shell_view);
void		e_task_shell_view_set_status_message
					(ETaskShellView *task_shell_view,
					 const gchar *status_message,
					 gdouble percent);
void		e_task_shell_view_update_sidebar
					(ETaskShellView *task_shell_view);
void		e_task_shell_view_update_search_filter
					(ETaskShellView *task_shell_view);
void		e_task_shell_view_update_timezone
					(ETaskShellView *task_shell_view);

G_END_DECLS

#endif /* E_TASK_SHELL_VIEW_PRIVATE_H */

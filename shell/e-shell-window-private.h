/*
 * e-shell-window-private.h
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

#ifndef E_SHELL_WINDOW_PRIVATE_H
#define E_SHELL_WINDOW_PRIVATE_H

#include "e-shell-window.h"

#include <string.h>
#include <glib/gi18n.h>

#include <gconf/gconf-client.h>
#include <libebackend/e-extensible.h>

#include <e-util/e-util.h>
#include <e-util/e-util-private.h>
#include <e-util/e-alert-dialog.h>
#include <e-util/e-alert-sink.h>
#include <e-util/e-plugin-ui.h>
#include <e-util/gconf-bridge.h>
#include <widgets/misc/e-alert-bar.h>
#include <widgets/misc/e-import-assistant.h>
#include <widgets/misc/e-menu-tool-button.h>
#include <widgets/misc/e-online-button.h>
#include <widgets/misc/e-popup-action.h>

#include <e-shell.h>
#include <e-shell-content.h>
#include <e-shell-view.h>
#include <e-shell-switcher.h>
#include <e-shell-window-actions.h>
#include <e-shell-utils.h>

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

/* Format for switcher action names.
 * The last part is the shell view name.
 * (e.g. switch-to-mail, switch-to-calendar) */
#define E_SHELL_SWITCHER_FORMAT   "switch-to-%s"
#define E_SHELL_NEW_WINDOW_FORMAT "new-%s-window"

G_BEGIN_DECLS

struct _EShellWindowPrivate {

	gpointer shell;  /* weak pointer */

	/*** UI Management ***/

	EFocusTracker *focus_tracker;
	GtkUIManager *ui_manager;
	guint custom_rule_merge_id;
	guint gal_view_merge_id;

	/*** Shell Views ***/

	GHashTable *loaded_views;
	const gchar *active_view;

	/*** Widgetry ***/

	GtkWidget *alert_bar;
	GtkWidget *content_pane;
	GtkWidget *content_notebook;
	GtkWidget *sidebar_notebook;
	GtkWidget *switcher;
	GtkWidget *tooltip_label;
	GtkWidget *status_notebook;

	/* Miscellaneous */
	GtkWidget *menubar_box;

	/* Shell signal handlers. */
	GArray *signal_handler_ids;

	gchar *geometry;

	guint destroyed        : 1;  /* XXX Do we still need this? */
	guint safe_mode        : 1;
	guint sidebar_visible  : 1;
	guint switcher_visible : 1;
	guint taskbar_visible  : 1;
	guint toolbar_visible  : 1;
};

void		e_shell_window_private_init	(EShellWindow *shell_window);
void		e_shell_window_private_constructed
						(EShellWindow *shell_window);
void		e_shell_window_private_dispose	(EShellWindow *shell_window);
void		e_shell_window_private_finalize	(EShellWindow *shell_window);

/* Private Utilities */

void		e_shell_window_actions_init	(EShellWindow *shell_window);
void		e_shell_window_switch_to_view	(EShellWindow *shell_window,
						 const gchar *view_name);
GtkWidget *	e_shell_window_create_new_menu	(EShellWindow *shell_window);
void		e_shell_window_create_switcher_actions
						(EShellWindow *shell_window);
void		e_shell_window_update_gal_view	(EShellWindow *shell_window);
void		e_shell_window_update_icon	(EShellWindow *shell_window);
void		e_shell_window_update_title	(EShellWindow *shell_window);
void		e_shell_window_update_new_menu	(EShellWindow *shell_window);
void		e_shell_window_update_view_menu	(EShellWindow *shell_window);
void		e_shell_window_update_search_menu
						(EShellWindow *shell_window);

G_END_DECLS

#endif /* E_SHELL_WINDOW_PRIVATE_H */

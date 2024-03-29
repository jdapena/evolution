/*
 * e-shell-taskbar.c
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

/**
 * SECTION: e-shell-taskbar
 * @short_description: the bottom of the main window
 * @include: shell/e-shell-taskbar.h
 **/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "e-shell-taskbar.h"

#include <libebackend/e-extensible.h>

#include <e-shell-view.h>
#include <misc/e-activity-proxy.h>

struct _EShellTaskbarPrivate {

	gpointer shell_view;  /* weak pointer */

	/* Keep a reference to the shell backend since
	 * we connect to its "activity-added" signal. */
	EShellBackend *shell_backend;

	GtkWidget *label;
	GtkWidget *hbox;

	GHashTable *proxy_table;

	gint fixed_height;
};

enum {
	PROP_0,
	PROP_MESSAGE,
	PROP_SHELL_VIEW
};

G_DEFINE_TYPE_WITH_CODE (
	EShellTaskbar,
	e_shell_taskbar,
	GTK_TYPE_HBOX,
	G_IMPLEMENT_INTERFACE (
		E_TYPE_EXTENSIBLE, NULL))

static void
shell_taskbar_weak_notify_cb (EShellTaskbar *shell_taskbar,
                              GObject *where_the_activity_was)
{
	GtkWidget *proxy;
	GtkContainer *container;
	GHashTable *proxy_table;
	GList *children;

	proxy_table = shell_taskbar->priv->proxy_table;
	proxy = g_hash_table_lookup (proxy_table, where_the_activity_was);
	g_hash_table_remove (proxy_table, where_the_activity_was);
	g_return_if_fail (proxy != NULL);

	container = GTK_CONTAINER (shell_taskbar->priv->hbox);
	gtk_container_remove (container, proxy);

	children = gtk_container_get_children (container);

	if (children == NULL)
		gtk_widget_hide (GTK_WIDGET (container));

	g_list_free (children);
}

static void
shell_taskbar_activity_add (EShellTaskbar *shell_taskbar,
                            EActivity *activity)
{
	GtkBox *box;
	GtkWidget *proxy;
	EActivityState state;
	GHashTable *proxy_table;

	/* Sanity check the activity state. */
	state = e_activity_get_state (activity);
	g_return_if_fail (state == E_ACTIVITY_RUNNING);

	/* Make sure it hasn't already been added. */
	proxy_table = shell_taskbar->priv->proxy_table;
	proxy = g_hash_table_lookup (proxy_table, activity);
	g_return_if_fail (proxy == NULL);

	/* Proxy widgets manage their own visibility.
	 * Don't call gtk_widget_show() on it here. */
	proxy = e_activity_proxy_new (activity);
	box = GTK_BOX (shell_taskbar->priv->hbox);
	gtk_box_pack_start (box, proxy, TRUE, TRUE, 0);
	gtk_box_reorder_child (box, proxy, 0);
	gtk_widget_show (GTK_WIDGET (box));

	/* The proxy widget also holds a weak reference to the activity,
	 * so the activity should get finalized in the normal course of
	 * operation.  When that happens we remove the corresponding
	 * proxy widget from the taskbar. */

	g_object_weak_ref (
		G_OBJECT (activity), (GWeakNotify)
		shell_taskbar_weak_notify_cb, shell_taskbar);

	g_hash_table_insert (proxy_table, activity, proxy);
}

static gboolean
shell_taskbar_weak_unref (EActivity *activity,
                          EActivityProxy *proxy,
                          EShellTaskbar *shell_taskbar)
{
	g_object_weak_unref (
		G_OBJECT (activity), (GWeakNotify)
		shell_taskbar_weak_notify_cb, shell_taskbar);

	return TRUE;
}

static void
shell_taskbar_set_shell_view (EShellTaskbar *shell_taskbar,
                              EShellView *shell_view)
{
	g_return_if_fail (shell_taskbar->priv->shell_view == NULL);

	shell_taskbar->priv->shell_view = shell_view;

	g_object_add_weak_pointer (
		G_OBJECT (shell_view),
		&shell_taskbar->priv->shell_view);
}

static void
shell_taskbar_set_property (GObject *object,
                            guint property_id,
                            const GValue *value,
                            GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_MESSAGE:
			e_shell_taskbar_set_message (
				E_SHELL_TASKBAR (object),
				g_value_get_string (value));
			return;

		case PROP_SHELL_VIEW:
			shell_taskbar_set_shell_view (
				E_SHELL_TASKBAR (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
shell_taskbar_get_property (GObject *object,
                            guint property_id,
                            GValue *value,
                            GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_MESSAGE:
			g_value_set_string (
				value, e_shell_taskbar_get_message (
				E_SHELL_TASKBAR (object)));
			return;

		case PROP_SHELL_VIEW:
			g_value_set_object (
				value, e_shell_taskbar_get_shell_view (
				E_SHELL_TASKBAR (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
shell_taskbar_dispose (GObject *object)
{
	EShellTaskbarPrivate *priv;

	priv = E_SHELL_TASKBAR (object)->priv;

	g_hash_table_foreach_remove (
		priv->proxy_table, (GHRFunc)
		shell_taskbar_weak_unref, object);

	if (priv->shell_view != NULL) {
		g_object_remove_weak_pointer (
			G_OBJECT (priv->shell_view), &priv->shell_view);
		priv->shell_view = NULL;
	}

	if (priv->shell_backend != NULL) {
		g_signal_handlers_disconnect_matched (
			priv->shell_backend, G_SIGNAL_MATCH_DATA,
			0, 0, NULL, NULL, object);
		g_object_unref (priv->shell_backend);
		priv->shell_backend = NULL;
	}

	if (priv->label != NULL) {
		g_object_unref (priv->label);
		priv->label = NULL;
	}

	if (priv->hbox != NULL) {
		g_object_unref (priv->hbox);
		priv->hbox = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_shell_taskbar_parent_class)->dispose (object);
}

static void
shell_taskbar_finalize (GObject *object)
{
	EShellTaskbarPrivate *priv;

	priv = E_SHELL_TASKBAR (object)->priv;

	g_hash_table_destroy (priv->proxy_table);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_shell_taskbar_parent_class)->finalize (object);
}

static void
shell_taskbar_constructed (GObject *object)
{
	EShellView *shell_view;
	EShellBackend *shell_backend;
	EShellTaskbar *shell_taskbar;

	shell_taskbar = E_SHELL_TASKBAR (object);
	shell_view = e_shell_taskbar_get_shell_view (shell_taskbar);
	shell_backend = e_shell_view_get_shell_backend (shell_view);

	/* Keep a reference to the shell backend so we can
	 * disconnect the signal handler during dispose(). */
	shell_taskbar->priv->shell_backend = g_object_ref (shell_backend);

	g_signal_connect_swapped (
		shell_backend, "activity-added",
		G_CALLBACK (shell_taskbar_activity_add), shell_taskbar);

	/* Do not enlarge window width on new activities. */
	gtk_widget_set_size_request (GTK_WIDGET (shell_taskbar), 0, -1);

	e_extensible_load_extensions (E_EXTENSIBLE (object));

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_shell_taskbar_parent_class)->constructed (object);
}

static void
shell_taskbar_size_allocate (GtkWidget *widget,
                             GtkAllocation *allocation)
{
	EShellTaskbar *shell_taskbar;
	gint fixed_height;

	shell_taskbar = E_SHELL_TASKBAR (widget);

	/* Maximum height allocation sticks. */
	fixed_height = shell_taskbar->priv->fixed_height;
	fixed_height = MAX (fixed_height, allocation->height);
	shell_taskbar->priv->fixed_height = fixed_height;

	/* Chain up to parent's size_allocate() method. */
	GTK_WIDGET_CLASS (e_shell_taskbar_parent_class)->
		size_allocate (widget, allocation);
}

static void
shell_taskbar_get_preferred_height (GtkWidget *widget,
                                    gint *minimum_height,
                                    gint *natural_height)
{
	EShellTaskbar *shell_taskbar;

	shell_taskbar = E_SHELL_TASKBAR (widget);

	if (minimum_height != NULL)
		*minimum_height = shell_taskbar->priv->fixed_height;

	if (natural_height != NULL)
		*natural_height = shell_taskbar->priv->fixed_height;
}

static void
e_shell_taskbar_class_init (EShellTaskbarClass *class)
{
	GObjectClass *object_class;
	GtkWidgetClass *widget_class;

	g_type_class_add_private (class, sizeof (EShellTaskbarPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = shell_taskbar_set_property;
	object_class->get_property = shell_taskbar_get_property;
	object_class->dispose = shell_taskbar_dispose;
	object_class->finalize = shell_taskbar_finalize;
	object_class->constructed = shell_taskbar_constructed;

	widget_class = GTK_WIDGET_CLASS (class);
	widget_class->size_allocate = shell_taskbar_size_allocate;
	widget_class->get_preferred_height = shell_taskbar_get_preferred_height;

	/**
	 * EShellTaskbar:message
	 *
	 * The message to display in the taskbar.
	 **/
	g_object_class_install_property (
		object_class,
		PROP_MESSAGE,
		g_param_spec_string (
			"message",
			NULL,
			NULL,
			NULL,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	/**
	 * EShellTaskbar:shell-view
	 *
	 * The #EShellView to which the taskbar widget belongs.
	 **/
	g_object_class_install_property (
		object_class,
		PROP_SHELL_VIEW,
		g_param_spec_object (
			"shell-view",
			NULL,
			NULL,
			E_TYPE_SHELL_VIEW,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY));
}

static void
e_shell_taskbar_init (EShellTaskbar *shell_taskbar)
{
	GtkWidget *widget;

	shell_taskbar->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		shell_taskbar, E_TYPE_SHELL_TASKBAR, EShellTaskbarPrivate);
	shell_taskbar->priv->proxy_table = g_hash_table_new (NULL, NULL);

	gtk_box_set_spacing (GTK_BOX (shell_taskbar), 12);

	widget = gtk_label_new (NULL);
	gtk_label_set_ellipsize (GTK_LABEL (widget), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start (GTK_BOX (shell_taskbar), widget, TRUE, TRUE, 0);
	gtk_misc_set_alignment (GTK_MISC (widget), 0.0, 0.5);
	shell_taskbar->priv->label = g_object_ref (widget);
	gtk_widget_hide (widget);

	widget = gtk_hbox_new (FALSE, 3);
	gtk_box_pack_start (GTK_BOX (shell_taskbar), widget, TRUE, TRUE, 0);
	shell_taskbar->priv->hbox = g_object_ref (widget);
	gtk_widget_hide (widget);
}

/**
 * e_shell_taskbar_new:
 * @shell_view: an #EShellView
 *
 * Creates a new #EShellTaskbar instance belonging to @shell_view.
 *
 * Returns: a new #EShellTaskbar instance
 **/
GtkWidget *
e_shell_taskbar_new (EShellView *shell_view)
{
	g_return_val_if_fail (E_IS_SHELL_VIEW (shell_view), NULL);

	return g_object_new (
		E_TYPE_SHELL_TASKBAR, "shell-view", shell_view, NULL);
}

/**
 * e_shell_taskbar_get_shell_view:
 * @shell_taskbar: an #EShellTaskbar
 *
 * Returns the #EShellView that was passed to e_shell_taskbar_new().
 *
 * Returns: the #EShellView to which @shell_taskbar belongs
 **/
EShellView *
e_shell_taskbar_get_shell_view (EShellTaskbar *shell_taskbar)
{
	g_return_val_if_fail (E_IS_SHELL_TASKBAR (shell_taskbar), NULL);

	return shell_taskbar->priv->shell_view;
}

/**
 * e_shell_taskbar_get_message:
 * @shell_taskbar: an #EShellTaskbar
 *
 * Returns the message currently shown in the taskbar, or an empty string
 * if no message is shown.  Taskbar messages are used primarily for menu
 * tooltips.
 *
 * Returns: the current taskbar message
 **/
const gchar *
e_shell_taskbar_get_message (EShellTaskbar *shell_taskbar)
{
	GtkWidget *label;

	g_return_val_if_fail (E_IS_SHELL_TASKBAR (shell_taskbar), NULL);

	label = shell_taskbar->priv->label;

	return gtk_label_get_text (GTK_LABEL (label));
}

/**
 * e_shell_taskbar_set_message:
 * @shell_taskbar: an #EShellTaskbar
 * @message: the message to show
 *
 * Shows a message in the taskbar.  If @message is %NULL or an empty string,
 * the taskbar message is cleared.  Taskbar messages are used primarily for
 * menu tooltips.
 **/
void
e_shell_taskbar_set_message (EShellTaskbar *shell_taskbar,
                             const gchar *message)
{
	GtkWidget *label;

	g_return_if_fail (E_IS_SHELL_TASKBAR (shell_taskbar));

	label = shell_taskbar->priv->label;
	gtk_label_set_text (GTK_LABEL (label), message);

	if (message != NULL && *message != '\0')
		gtk_widget_show (label);
	else
		gtk_widget_hide (label);

	g_object_notify (G_OBJECT (shell_taskbar), "message");
}

/**
 * e_shell_taskbar_unset_message:
 * @shell_taskbar: an #EShellTaskbar
 *
 * This is equivalent to passing a %NULL message to
 * e_shell_taskbar_set_message().
 **/
void
e_shell_taskbar_unset_message (EShellTaskbar *shell_taskbar)
{
	g_return_if_fail (E_IS_SHELL_TASKBAR (shell_taskbar));

	e_shell_taskbar_set_message (shell_taskbar, NULL);
}

/**
 * e_shell_taskbar_get_activity_count:
 * @shell_taskbar: an #EShellTaskbar
 *
 * Returns the number of active #EActivity instances being tracked.
 *
 * Returns: the number of #EActivity instances
 **/
guint
e_shell_taskbar_get_activity_count (EShellTaskbar *shell_taskbar)
{
	g_return_val_if_fail (E_IS_SHELL_TASKBAR (shell_taskbar), 0);

	return g_hash_table_size (shell_taskbar->priv->proxy_table);
}

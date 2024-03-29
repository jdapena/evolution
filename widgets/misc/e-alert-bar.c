/*
 * e-alert-bar.c
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

#include "e-alert-bar.h"

#include <glib/gi18n-lib.h>

/* GTK_ICON_SIZE_DIALOG is a tad too big. */
#define ICON_SIZE GTK_ICON_SIZE_DND

/* Dismiss warnings automatically after 5 minutes. */
#define WARNING_TIMEOUT_SECONDS (5 * 60)

struct _EAlertBarPrivate {
	GQueue alerts;
	GtkWidget *image;		/* not referenced */
	GtkWidget *primary_label;	/* not referenced */
	GtkWidget *secondary_label;	/* not referenced */
};

G_DEFINE_TYPE (
	EAlertBar,
	e_alert_bar,
	GTK_TYPE_INFO_BAR)

static void
alert_bar_show_alert (EAlertBar *alert_bar)
{
	GtkImage *image;
	GtkLabel *label;
	GtkInfoBar *info_bar;
	GtkWidget *action_area;
	EAlert *alert;
	GList *actions;
	GList *children;
	GtkMessageType message_type;
	const gchar *stock_id;
	const gchar *text;
	gint response_id;

	info_bar = GTK_INFO_BAR (alert_bar);
	action_area = gtk_info_bar_get_action_area (info_bar);

	alert = g_queue_peek_head (&alert_bar->priv->alerts);
	g_return_if_fail (E_IS_ALERT (alert));

	/* Remove all buttons from the previous alert. */
	children = gtk_container_get_children (GTK_CONTAINER (action_area));
	while (children != NULL) {
		GtkWidget *child = GTK_WIDGET (children->data);
		gtk_container_remove (GTK_CONTAINER (action_area), child);
		children = g_list_delete_link (children, children);
	}

	/* Add new buttons. */
	actions = e_alert_peek_actions (alert);
	while (actions != NULL) {
		GtkWidget *button;

		/* These actions are already wired to trigger an
		 * EAlert::response signal when activated, which
		 * will in turn call gtk_info_bar_response(), so
		 * we can add buttons directly to the action
		 * area without knowning their response IDs. */

		button = gtk_button_new ();

		gtk_activatable_set_related_action (
			GTK_ACTIVATABLE (button),
			GTK_ACTION (actions->data));

		gtk_box_pack_end (
			GTK_BOX (action_area),
			button, FALSE, FALSE, 0);

		actions = g_list_next (actions);
	}

	response_id = e_alert_get_default_response (alert);
	gtk_info_bar_set_default_response (info_bar, response_id);

	message_type = e_alert_get_message_type (alert);
	gtk_info_bar_set_message_type (info_bar, message_type);

	text = e_alert_get_primary_text (alert);
	label = GTK_LABEL (alert_bar->priv->primary_label);
	gtk_label_set_text (label, text);

	text = e_alert_get_secondary_text (alert);
	label = GTK_LABEL (alert_bar->priv->secondary_label);
	gtk_label_set_text (label, text);

	stock_id = e_alert_get_stock_id (alert);
	image = GTK_IMAGE (alert_bar->priv->image);
	gtk_image_set_from_stock (image, stock_id, ICON_SIZE);

	gtk_widget_show (GTK_WIDGET (alert_bar));

	/* Warnings are generally meant for transient errors.
	 * No need to leave them up indefinitely.  Close them
	 * automatically if the user hasn't responded after a
	 * reasonable period of time has elapsed. */
	if (message_type == GTK_MESSAGE_WARNING)
		e_alert_start_timer (alert, WARNING_TIMEOUT_SECONDS);
}

static void
alert_bar_response_cb (EAlert *alert,
                       gint response_id,
                       EAlertBar *alert_bar)
{
	GQueue *queue;
	EAlert *head;
	GList *link;
	gboolean was_head;

	queue = &alert_bar->priv->alerts;
	head = g_queue_peek_head (queue);
	was_head = (alert == head);

	g_signal_handlers_disconnect_by_func (
		alert, alert_bar_response_cb, alert_bar);

	/* XXX Would be easier if g_queue_remove() returned a boolean. */
	link = g_queue_find (queue, alert);
	if (link != NULL) {
		g_queue_delete_link (queue, link);
		g_object_unref (alert);
	}

	if (g_queue_is_empty (queue))
		gtk_widget_hide (GTK_WIDGET (alert_bar));
	else if (was_head) {
		GtkInfoBar *info_bar = GTK_INFO_BAR (alert_bar);
		gtk_info_bar_response (info_bar, response_id);
		alert_bar_show_alert (alert_bar);
	}
}

static void
alert_bar_dispose (GObject *object)
{
	EAlertBarPrivate *priv;

	priv = E_ALERT_BAR (object)->priv;

	while (!g_queue_is_empty (&priv->alerts)) {
		EAlert *alert = g_queue_pop_head (&priv->alerts);
		g_signal_handlers_disconnect_by_func (
			alert, alert_bar_response_cb, object);
		g_object_unref (alert);
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_alert_bar_parent_class)->dispose (object);
}

static GtkSizeRequestMode
alert_bar_get_request_mode (GtkWidget *widget)
{
	/* GtkHBox does width-for-height by default.  But we
	 * want the alert bar to be as short as possible. */
	return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
e_alert_bar_class_init (EAlertBarClass *class)
{
	GObjectClass *object_class;
	GtkWidgetClass *widget_class;

	g_type_class_add_private (class, sizeof (EAlertBarPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = alert_bar_dispose;

	widget_class = GTK_WIDGET_CLASS (class);
	widget_class->get_request_mode = alert_bar_get_request_mode;
}

static void
e_alert_bar_init (EAlertBar *alert_bar)
{
	GtkWidget *container;
	GtkWidget *widget;
	PangoAttribute *attr;
	PangoAttrList *attr_list;

	alert_bar->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		alert_bar, E_TYPE_ALERT_BAR, EAlertBarPrivate);

	g_queue_init (&alert_bar->priv->alerts);

	container = gtk_info_bar_get_content_area (GTK_INFO_BAR (alert_bar));

	widget = gtk_image_new ();
	gtk_misc_set_alignment (GTK_MISC (widget), 0.5, 0.0);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	alert_bar->priv->image = widget;
	gtk_widget_show (widget);

	widget = gtk_vbox_new (FALSE, 12);
	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);
	gtk_widget_show (widget);

	container = widget;

	attr_list = pango_attr_list_new ();
	attr = pango_attr_weight_new (PANGO_WEIGHT_BOLD);
	pango_attr_list_insert (attr_list, attr);

	widget = gtk_label_new (NULL);
	gtk_label_set_attributes (GTK_LABEL (widget), attr_list);
	gtk_label_set_line_wrap (GTK_LABEL (widget), TRUE);
	gtk_label_set_selectable (GTK_LABEL (widget), TRUE);
	gtk_misc_set_alignment (GTK_MISC (widget), 0.0, 0.5);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	alert_bar->priv->primary_label = widget;
	gtk_widget_show (widget);

	pango_attr_list_unref (attr_list);

	attr_list = pango_attr_list_new ();
	attr = pango_attr_scale_new  (PANGO_SCALE_SMALL);
	pango_attr_list_insert (attr_list, attr);

	widget = gtk_label_new (NULL);
	gtk_label_set_attributes (GTK_LABEL (widget), attr_list);
	gtk_label_set_line_wrap (GTK_LABEL (widget), TRUE);
	gtk_label_set_selectable (GTK_LABEL (widget), TRUE);
	gtk_misc_set_alignment (GTK_MISC (widget), 0.0, 0.5);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	alert_bar->priv->secondary_label = widget;
	gtk_widget_show (widget);

	pango_attr_list_unref (attr_list);
}

GtkWidget *
e_alert_bar_new (void)
{
	return g_object_new (E_TYPE_ALERT_BAR, NULL);
}

typedef struct {
	gboolean found;
	EAlert *looking_for;
} DuplicateData;

static void
alert_bar_find_duplicate_cb (EAlert *alert,
                             DuplicateData *dd)
{
	g_return_if_fail (dd->looking_for != NULL);

	dd->found |= (
		e_alert_get_message_type (alert) ==
		e_alert_get_message_type (dd->looking_for) &&
		g_strcmp0 (e_alert_get_primary_text (alert),
		e_alert_get_primary_text (dd->looking_for)) == 0 &&
		g_strcmp0 (e_alert_get_secondary_text (alert),
		e_alert_get_secondary_text (dd->looking_for)) == 0);
}

void
e_alert_bar_add_alert (EAlertBar *alert_bar,
                       EAlert *alert)
{
	DuplicateData dd;

	g_return_if_fail (E_IS_ALERT_BAR (alert_bar));
	g_return_if_fail (E_IS_ALERT (alert));

	dd.found = FALSE;
	dd.looking_for = alert;

	g_queue_foreach (
		&alert_bar->priv->alerts,
		(GFunc) alert_bar_find_duplicate_cb, &dd);

	if (dd.found)
		return;

	g_signal_connect (
		alert, "response",
		G_CALLBACK (alert_bar_response_cb), alert_bar);

	g_queue_push_head (&alert_bar->priv->alerts, g_object_ref (alert));

	alert_bar_show_alert (alert_bar);
}

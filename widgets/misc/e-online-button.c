/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* e-offline-button.c
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "e-online-button.h"

#include <glib/gi18n.h>

#define ONLINE_IMAGE	"online.png"
#define OFFLINE_IMAGE	"offline.png"

#define E_ONLINE_BUTTON_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_ONLINE_BUTTON, EOnlineButtonPrivate))

struct _EOnlineButtonPrivate {
	GtkWidget *image;
	gboolean online;
};

enum {
	PROP_0,
	PROP_ONLINE
};

static gpointer parent_class;

static void
online_button_set_property (GObject *object,
                             guint property_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ONLINE:
			e_online_button_set_online (
				E_ONLINE_BUTTON (object),
				g_value_get_boolean (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
online_button_get_property (GObject *object,
                             guint property_id,
                             GValue *value,
                             GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ONLINE:
			g_value_set_boolean (
				value, e_online_button_get_online (
				E_ONLINE_BUTTON (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
online_button_dispose (GObject *object)
{
	EOnlineButtonPrivate *priv;

	priv = E_ONLINE_BUTTON_GET_PRIVATE (object);

	if (priv->image != NULL) {
		g_object_unref (priv->image);
		priv->image = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
online_button_class_init (EOnlineButtonClass *class)
{
	GObjectClass *object_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EOnlineButtonPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = online_button_set_property;
	object_class->get_property = online_button_get_property;
	object_class->dispose = online_button_dispose;

	g_object_class_install_property (
		object_class,
		PROP_ONLINE,
		g_param_spec_boolean (
			"online",
			_("Online"),
			_("The button state is online"),
			TRUE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));
}

static void
online_button_init (EOnlineButton *button)
{
	GtkWidget *widget;

	button->priv = E_ONLINE_BUTTON_GET_PRIVATE (button);

	GTK_WIDGET_UNSET_FLAGS (button, GTK_CAN_FOCUS);
	gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);

	widget = gtk_image_new ();
	gtk_container_add (GTK_CONTAINER (button), widget);
	button->priv->image = g_object_ref (widget);
	gtk_widget_show (widget);
}

GType
e_online_button_get_type (void)
{
	static GType type = 0;

	if (G_UNLIKELY (type == 0)) {
		const GTypeInfo type_info = {
			sizeof (EOnlineButtonClass),
			(GBaseInitFunc) NULL,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) online_button_class_init,
			(GClassFinalizeFunc) NULL,
			NULL,  /* class_data */
			sizeof (EOnlineButton),
			0,     /* n_preallocs */
			(GInstanceInitFunc) online_button_init,
			NULL   /* value_table */
		};

		type = g_type_register_static (
			GTK_TYPE_BUTTON, "EOnlineButton", &type_info, 0);
	}

	return type;
}

GtkWidget *
e_online_button_new (void)
{
	return g_object_new (E_TYPE_ONLINE_BUTTON, NULL);
}

gboolean
e_online_button_get_online (EOnlineButton *button)
{
	g_return_val_if_fail (E_IS_ONLINE_BUTTON (button), FALSE);

	return button->priv->online;
}

void
e_online_button_set_online (EOnlineButton *button,
                            gboolean online)
{
	GtkImage *image;
	gchar *filename;
	const gchar *image_name;

	g_return_if_fail (E_IS_ONLINE_BUTTON (button));

	button->priv->online = online;
	image_name = online ? ONLINE_IMAGE : OFFLINE_IMAGE;

	image = GTK_IMAGE (button->priv->image);
	filename = g_build_filename (EVOLUTION_IMAGES, image_name, NULL);
	gtk_image_set_from_file (image, filename);
	g_free (filename);

	g_object_notify (G_OBJECT (button), "online");
}
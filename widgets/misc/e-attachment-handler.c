/*
 * e-attachment-handler.c
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

#include "e-attachment-handler.h"

struct _EAttachmentHandlerPrivate {
	gpointer placeholder;
};

G_DEFINE_TYPE (
	EAttachmentHandler,
	e_attachment_handler,
	E_TYPE_EXTENSION)

static void
attachment_handler_constructed (GObject *object)
{
	EAttachmentView *view;
	EAttachmentHandler *handler;
	GdkDragAction drag_actions;
	GtkTargetList *target_list;
	const GtkTargetEntry *targets;
	guint n_targets;

	handler = E_ATTACHMENT_HANDLER (object);
	drag_actions = e_attachment_handler_get_drag_actions (handler);
	targets = e_attachment_handler_get_target_table (handler, &n_targets);

	view = e_attachment_handler_get_view (handler);

	target_list = e_attachment_view_get_target_list (view);
	gtk_target_list_add_table (target_list, targets, n_targets);

	e_attachment_view_add_drag_actions (view, drag_actions);

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_attachment_handler_parent_class)->constructed (object);
}

static void
e_attachment_handler_class_init (EAttachmentHandlerClass *class)
{
	GObjectClass *object_class;
	EExtensionClass *extension_class;

	g_type_class_add_private (class, sizeof (EAttachmentHandlerPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->constructed = attachment_handler_constructed;

	extension_class = E_EXTENSION_CLASS (class);
	extension_class->extensible_type = E_TYPE_ATTACHMENT_VIEW;
}

static void
e_attachment_handler_init (EAttachmentHandler *handler)
{
	handler->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		handler, E_TYPE_ATTACHMENT_HANDLER, EAttachmentHandlerPrivate);
}

EAttachmentView *
e_attachment_handler_get_view (EAttachmentHandler *handler)
{
	EExtensible *extensible;

	/* This is purely a convenience function. */

	g_return_val_if_fail (E_IS_ATTACHMENT_HANDLER (handler), NULL);

	extensible = e_extension_get_extensible (E_EXTENSION (handler));

	return E_ATTACHMENT_VIEW (extensible);
}

GdkDragAction
e_attachment_handler_get_drag_actions (EAttachmentHandler *handler)
{
	EAttachmentHandlerClass *class;

	g_return_val_if_fail (E_IS_ATTACHMENT_HANDLER (handler), 0);

	class = E_ATTACHMENT_HANDLER_GET_CLASS (handler);

	if (class->get_drag_actions != NULL)
		return class->get_drag_actions (handler);

	return 0;
}

const GtkTargetEntry *
e_attachment_handler_get_target_table (EAttachmentHandler *handler,
                                       guint *n_targets)
{
	EAttachmentHandlerClass *class;

	g_return_val_if_fail (E_IS_ATTACHMENT_HANDLER (handler), NULL);

	class = E_ATTACHMENT_HANDLER_GET_CLASS (handler);

	if (class->get_target_table != NULL)
		return class->get_target_table (handler, n_targets);

	if (n_targets != NULL)
		*n_targets = 0;

	return NULL;
}

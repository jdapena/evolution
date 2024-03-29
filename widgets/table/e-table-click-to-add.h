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
 *		Chris Lahey <clahey@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef _E_TABLE_CLICK_TO_ADD_H_
#define _E_TABLE_CLICK_TO_ADD_H_

#include <libxml/tree.h>
#include <libgnomecanvas/libgnomecanvas.h>
#include <table/e-table-header.h>
#include <table/e-table-sort-info.h>
#include <table/e-table-item.h>
#include <table/e-table-selection-model.h>

/* Standard GObject macros */
#define E_TYPE_TABLE_CLICK_TO_ADD \
	(e_table_click_to_add_get_type ())
#define E_TABLE_CLICK_TO_ADD(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_TABLE_CLICK_TO_ADD, ETableClickToAdd))
#define E_TABLE_CLICK_TO_ADD_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_TABLE_CLICK_TO_ADD, ETableClickToAddClass))
#define E_IS_TABLE_CLICK_TO_ADD(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_TABLE_CLICK_TO_ADD))
#define E_IS_TABLE_CLICK_TO_ADD_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_TABLE_CLICK_TO_ADD))
#define E_TABLE_CLICK_TO_ADD_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_TABLE_CLICK_TO_ADD, ETableClickToAddClass))

G_BEGIN_DECLS

typedef struct _ETableClickToAdd ETableClickToAdd;
typedef struct _ETableClickToAddClass ETableClickToAddClass;

struct _ETableClickToAdd {
	GnomeCanvasGroup  parent;

	ETableModel      *one;    /* The ETableOne. */

	ETableModel      *model;  /* The backend model. */
	ETableHeader     *eth;    /* This is just to give to the ETableItem. */

	gchar             *message;

	GnomeCanvasItem  *row;    /* If row is NULL, we're sitting with
				   * no data and a "Click here" message. */
	GnomeCanvasItem  *text;   /* If text is NULL, row shouldn't be. */
	GnomeCanvasItem  *rect;   /* What the heck.  Why not. */

	gdouble           width;
	gdouble           height;

	ETableSelectionModel *selection;
};

struct _ETableClickToAddClass {
	GnomeCanvasGroupClass parent_class;

	/* Signals */
	void		(*cursor_change)	(ETableClickToAdd *etcta,
						 gint row,
						 gint col);
	void		(*style_set)		(ETableClickToAdd *etcta,
						 GtkStyle *previous_style);
};

GType		e_table_click_to_add_get_type	(void) G_GNUC_CONST;
void		e_table_click_to_add_commit	(ETableClickToAdd *etcta);

G_END_DECLS

#endif /* _E_TABLE_CLICK_TO_ADD_H_ */

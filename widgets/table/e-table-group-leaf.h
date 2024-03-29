/*
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
 * Authors:
 *		Chris Lahey <clahey@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef _E_TABLE_GROUP_LEAF_H_
#define _E_TABLE_GROUP_LEAF_H_

#include <libgnomecanvas/libgnomecanvas.h>
#include <table/e-table-group.h>
#include <table/e-table-subset.h>
#include <table/e-table-item.h>

/* Standard GObject macros */
#define E_TYPE_TABLE_GROUP_LEAF \
	(e_table_group_leaf_get_type ())
#define E_TABLE_GROUP_LEAF(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_TABLE_GROUP_LEAF, ETableGroupLeaf))
#define E_TABLE_GROUP_LEAF_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_TABLE_GROUP_LEAF, ETableGroupLeafClass))
#define E_IS_TABLE_GROUP_LEAF(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_TABLE_GROUP_LEAF))
#define E_IS_TABLE_GROUP_LEAF_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_TABLE_GROUP_LEAF))
#define E_TABLE_GROUP_LEAF_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_TABLE_GROUP_LEAF, ETableGroupLeafClass))

G_BEGIN_DECLS

typedef struct _ETableGroupLeaf ETableGroupLeaf;
typedef struct _ETableGroupLeafClass ETableGroupLeafClass;

struct _ETableGroupLeaf {
	ETableGroup group;

	/*
	 * Item.
	 */
	ETableItem *item;

	gdouble height;
	gdouble width;
	gdouble minimum_width;

	gint length_threshold;

	ETableSubset *ets;
	guint is_grouped : 1;

	guint alternating_row_colors : 1;
	guint horizontal_draw_grid : 1;
	guint vertical_draw_grid : 1;
	guint draw_focus : 1;
	guint uniform_row_height : 1;
	ECursorMode cursor_mode;

	gint etgl_cursor_change_id;
	gint etgl_cursor_activated_id;
	gint etgl_double_click_id;
	gint etgl_right_click_id;
	gint etgl_click_id;
	gint etgl_key_press_id;
	gint etgl_start_drag_id;

	ESelectionModel *selection_model;
};

struct _ETableGroupLeafClass {
	ETableGroupClass parent_class;
};

GType		e_table_group_leaf_get_type	(void) G_GNUC_CONST;
ETableGroup *	e_table_group_leaf_new		(GnomeCanvasGroup *parent,
						 ETableHeader *full_header,
						 ETableHeader *header,
						 ETableModel *model,
						 ETableSortInfo *sort_info);

G_END_DECLS

#endif /* _E_TABLE_GROUP_LEAF_H_ */


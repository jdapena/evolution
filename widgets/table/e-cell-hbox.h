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
 *		Srinivasa Ragavan <sragavan@novell.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef _E_CELL_HBOX_H_
#define _E_CELL_HBOX_H_

#include <libgnomecanvas/libgnomecanvas.h>
#include <table/e-cell.h>

/* Standard GObject macros */
#define E_TYPE_CELL_HBOX \
	(e_cell_hbox_get_type ())
#define E_CELL_HBOX(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_CELL_HBOX, ECellHbox))
#define E_CELL_HBOX_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_CELL_HBOX, ECellHboxClass))
#define E_IS_CELL_HBOX(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_CELL_HBOX))
#define E_IS_CELL_HBOX_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_CELL_HBOX))
#define E_CELL_HBOX_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_CELL_HBOX, ECellHboxClass))

G_BEGIN_DECLS

typedef struct _ECellHbox ECellHbox;
typedef struct _ECellHboxView ECellHboxView;
typedef struct _ECellHboxClass ECellHboxClass;

struct _ECellHbox {
	ECell parent;

	gint subcell_count;
	ECell **subcells;
	gint *model_cols;
	gint *def_size_cols;
};

struct _ECellHboxView {
	ECellView cell_view;

	gint subcell_view_count;
	ECellView **subcell_views;
	gint *model_cols;
	gint *def_size_cols;
};

struct _ECellHboxClass {
	ECellClass parent_class;
};

GType		e_cell_hbox_get_type		(void) G_GNUC_CONST;
ECell *		e_cell_hbox_new			(void);
void		e_cell_hbox_append		(ECellHbox *vbox,
						 ECell *subcell,
						 gint model_col,
						 gint size);

G_END_DECLS

#endif /* _E_CELL_HBOX_H_ */

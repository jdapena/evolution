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
 *		Damon Chaplin <damon@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

/*
 * ECellPercent - a subclass of ECellText used to show an integer percentage
 * in an ETable.
 */

#ifndef E_CELL_PERCENT_H
#define E_CELL_PERCENT_H

#include <table/e-cell-text.h>

/* Standard GObject macros */
#define E_TYPE_CELL_PERCENT \
	(e_cell_percent_get_type ())
#define E_CELL_PERCENT(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_CELL_PERCENT, ECellPercent))
#define E_CELL_PERCENT_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_CELL_PERCENT, ECellPercentClass))
#define E_IS_CELL_NUMBER(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_CELL_PERCENT))
#define E_IS_CELL_NUMBER_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_CELL_PERCENT))
#define E_CELL_NUMBER_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_CELL_PERCENT, ECellPercentClass))

G_BEGIN_DECLS

typedef struct _ECellPercent ECellPercent;
typedef struct _ECellPercentClass ECellPercentClass;

struct _ECellPercent {
	ECellText parent;
};

struct _ECellPercentClass {
	ECellTextClass parent_class;
};

GType		e_cell_percent_get_type		(void) G_GNUC_CONST;
ECell *		e_cell_percent_new		(const gchar *fontname,
						 GtkJustification justify);

G_END_DECLS

#endif /* E_CELL_PERCENT_H */

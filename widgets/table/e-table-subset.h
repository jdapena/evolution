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
 *		Miguel de Icaza <miguel@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef _E_TABLE_SUBSET_H_
#define _E_TABLE_SUBSET_H_

#include <table/e-table-model.h>

/* Standard GObject macros */
#define E_TYPE_TABLE_SUBSET \
	(e_table_subset_get_type ())
#define E_TABLE_SUBSET(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_TABLE_SUBSET, ETableSubset))
#define E_TABLE_SUBSET_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_TABLE_SUBSET, ETableSubsetClass))
#define E_IS_TABLE_SUBSET(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_TABLE_SUBSET))
#define E_IS_TABLE_SUBSET_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_TABLE_SUBSET))
#define E_TABLE_SUBSET_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_TABLE_SUBSET, ETableSubsetClass))

G_BEGIN_DECLS

typedef struct _ETableSubset ETableSubset;
typedef struct _ETableSubsetClass ETableSubsetClass;

struct _ETableSubset {
	ETableModel parent;

	ETableModel *source;
	gint n_map;
	gint *map_table;

	gint last_access;

	gint table_model_pre_change_id;
	gint table_model_no_change_id;
	gint table_model_changed_id;
	gint table_model_row_changed_id;
	gint table_model_cell_changed_id;
	gint table_model_rows_inserted_id;
	gint table_model_rows_deleted_id;
};

struct _ETableSubsetClass {
	ETableModelClass parent_class;

	void		(*proxy_model_pre_change)	(ETableSubset *etss,
							 ETableModel *etm);
	void		(*proxy_model_no_change)	(ETableSubset *etss,
							 ETableModel *etm);
	void		(*proxy_model_changed)		(ETableSubset *etss,
							 ETableModel *etm);
	void		(*proxy_model_row_changed)	(ETableSubset *etss,
							 ETableModel *etm,
							 gint row);
	void		(*proxy_model_cell_changed)	(ETableSubset *etss,
							 ETableModel *etm,
							 gint col,
							 gint row);
	void		(*proxy_model_rows_inserted)	(ETableSubset *etss,
							 ETableModel *etm,
							 gint row,
							 gint count);
	void		(*proxy_model_rows_deleted)	(ETableSubset *etss,
							 ETableModel *etm,
							 gint row,
							 gint count);
};

GType		e_table_subset_get_type		(void) G_GNUC_CONST;
ETableModel *	e_table_subset_new		(ETableModel  *etm,
						 gint n_vals);
ETableModel *	e_table_subset_construct	(ETableSubset *ets,
						 ETableModel *source,
						 gint nvals);
gint		e_table_subset_model_to_view_row
						(ETableSubset *ets,
						 gint model_row);
gint		e_table_subset_view_to_model_row
						(ETableSubset *ets,
						 gint view_row);
ETableModel *	e_table_subset_get_toplevel	(ETableSubset *table_model);
void		e_table_subset_print_debugging	(ETableSubset *table_model);

G_END_DECLS

#endif /* _E_TABLE_SUBSET_H_ */


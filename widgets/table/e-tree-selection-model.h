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

#ifndef _E_TREE_SELECTION_MODEL_H_
#define _E_TREE_SELECTION_MODEL_H_

#include <e-util/e-sorter.h>
#include <misc/e-selection-model.h>
#include <table/e-tree-model.h>

/* Standard GObject macros */
#define E_TYPE_TREE_SELECTION_MODEL \
	(e_tree_selection_model_get_type ())
#define E_TREE_SELECTION_MODEL(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_TREE_SELECTION_MODEL, ETreeSelectionModel))
#define E_TREE_SELECTION_MODEL_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_TREE_SELECTION_MODEL, ETreeSelectionModelClass))
#define E_IS_TREE_SELECTION_MODEL(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_TREE_SELECTION_MODEL))
#define E_IS_TREE_SELECTION_MODEL_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_TREE_SELECTION_MODEL))
#define E_TREE_SELECTION_MODEL_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_TREE_SELECTION_MODEL, ETreeSelectionModelClass))

G_BEGIN_DECLS

typedef void	(*ETreeForeachFunc)		(ETreePath path,
						 gpointer closure);

typedef struct _ETreeSelectionModel ETreeSelectionModel;
typedef struct _ETreeSelectionModelClass ETreeSelectionModelClass;
typedef struct _ETreeSelectionModelPrivate ETreeSelectionModelPrivate;

struct _ETreeSelectionModel {
	ESelectionModel parent;
	ETreeSelectionModelPrivate *priv;
};

struct _ETreeSelectionModelClass {
	ESelectionModelClass parent_class;
};

GType		e_tree_selection_model_get_type	(void) G_GNUC_CONST;
ESelectionModel *
		e_tree_selection_model_new	(void);
void		e_tree_selection_model_foreach	(ETreeSelectionModel *etsm,
						 ETreeForeachFunc callback,
						 gpointer closure);
void		e_tree_selection_model_select_single_path
						(ETreeSelectionModel *etsm,
						 ETreePath path);
void		e_tree_selection_model_select_paths
						(ETreeSelectionModel *etsm,
						 GPtrArray *paths);

void		e_tree_selection_model_add_to_selection
						(ETreeSelectionModel *etsm,
						 ETreePath path);
void		e_tree_selection_model_change_cursor
						(ETreeSelectionModel *etsm,
						 ETreePath path);
ETreePath	e_tree_selection_model_get_cursor
						(ETreeSelectionModel *etsm);

G_END_DECLS

#endif /* _E_TREE_SELECTION_MODEL_H_ */

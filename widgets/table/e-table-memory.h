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

#ifndef _E_TABLE_MEMORY_H_
#define _E_TABLE_MEMORY_H_

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <table/e-table-model.h>

/* Standard GObject macros */
#define E_TYPE_TABLE_MEMORY \
	(e_table_memory_get_type ())
#define E_TABLE_MEMORY(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_TABLE_MEMORY, ETableMemory))
#define E_TABLE_MEMORY_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_TABLE_MEMORY, ETableMemoryClass))
#define E_IS_TABLE_MEMORY(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_TABLE_MEMORY))
#define E_IS_TABLE_MEMORY_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_TABLE_MEMORY))
#define E_TABLE_MEMORY_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_TABLE_MEMORY, ETableMemoryClass))

G_BEGIN_DECLS

typedef struct _ETableMemory ETableMemory;
typedef struct _ETableMemoryClass ETableMemoryClass;
typedef struct _ETableMemoryPrivate ETableMemoryPrivate;

struct _ETableMemory {
	ETableModel parent;
	ETableMemoryPrivate *priv;
};

struct _ETableMemoryClass {
	ETableModelClass parent_class;
};

GType		e_table_memory_get_type		(void) G_GNUC_CONST;
ETableMemory *	e_table_memory_new		(void);
void		e_table_memory_construct	(ETableMemory *etable);

/* row operations */
void		e_table_memory_insert		(ETableMemory *etable,
						 gint row,
						 gpointer data);
gpointer	e_table_memory_remove		(ETableMemory *etable,
						 gint row);
void		e_table_memory_clear		(ETableMemory *etable);

/* Freeze and thaw */
void		e_table_memory_freeze		(ETableMemory *etable);
void		e_table_memory_thaw		(ETableMemory *etable);
gpointer	e_table_memory_get_data		(ETableMemory *etm,
						 gint row);
void		e_table_memory_set_data		(ETableMemory *etm,
						 gint row,
						 gpointer data);

G_END_DECLS

#endif /* _E_TABLE_MEMORY_H */

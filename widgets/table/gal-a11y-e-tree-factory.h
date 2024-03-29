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
 *		Yuedong Du <yuedong.du@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef __GAL_A11Y_E_TREE_FACTORY_H__
#define __GAL_A11Y_E_TREE_FACTORY_H__

#include <atk/atkobjectfactory.h>

#define GAL_A11Y_TYPE_E_TREE_FACTORY            (gal_a11y_e_table_factory_get_type ())
#define GAL_A11Y_E_TREE_FACTORY(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GAL_A11Y_TYPE_E_TREE_FACTORY, GalA11yETreeFactory))
#define GAL_A11Y_E_TREE_FACTORY_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GAL_A11Y_TYPE_E_TREE_FACTORY, GalA11yETreeFactoryClass))
#define GAL_A11Y_IS_E_TREE_FACTORY(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GAL_A11Y_TYPE_E_TREE_FACTORY))
#define GAL_A11Y_IS_E_TREE_FACTORY_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GAL_A11Y_TYPE_E_TREE_FACTORY))

typedef struct _GalA11yETreeFactory GalA11yETreeFactory;
typedef struct _GalA11yETreeFactoryClass GalA11yETreeFactoryClass;

struct _GalA11yETreeFactory {
	AtkObject object;
};

struct _GalA11yETreeFactoryClass {
	AtkObjectClass parent_class;
};

/* Standard Glib function */
GType              gal_a11y_e_tree_factory_get_type         (void);

#endif /* __GAL_A11Y_E_TREE_FACTORY_H__ */

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
 * ECellCombo - a subclass of ECellPopup used to support popup lists like a
 * GtkCombo widget. It only supports a basic popup list of strings at present,
 * with no auto-completion. The child ECell of the ECellPopup must be an
 * ECellText or subclass.
 */

#ifndef _E_CELL_COMBO_H_
#define _E_CELL_COMBO_H_

#include <table/e-cell-popup.h>

/* Standard GObject macros */
#define E_TYPE_CELL_COMBO \
	(e_cell_combo_get_type ())
#define E_CELL_COMBO(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_CELL_COMBO, ECellCombo))
#define E_CELL_COMBO_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_CELL_COMBO, ECellComboClass))
#define E_IS_CELL_COMBO(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_CELL_COMBO))
#define E_IS_CELL_COMBO_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_CELL_COMBO))
#define E_CELL_COMBO_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_CELL_COMBO, ECellComboClass))

G_BEGIN_DECLS

typedef struct _ECellCombo ECellCombo;
typedef struct _ECellComboClass ECellComboClass;

struct _ECellCombo {
	ECellPopup parent;

	GtkWidget *popup_window;
	GtkWidget *popup_scrolled_window;
	GtkWidget *popup_tree_view;
};

struct _ECellComboClass {
	ECellPopupClass parent_class;
};

GType		e_cell_combo_get_type		(void) G_GNUC_CONST;
ECell *		e_cell_combo_new		(void);

/* These must be UTF-8. */
void		e_cell_combo_set_popdown_strings
						(ECellCombo *ecc,
						 GList *strings);

G_END_DECLS

#endif /* _E_CELL_COMBO_H_ */

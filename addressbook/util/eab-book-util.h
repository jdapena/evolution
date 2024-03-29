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
 *		Jon Trowbridge <trow@ximian.com>
 *      Chris Toshok <toshok@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef EAB_BOOK_UTIL_H
#define EAB_BOOK_UTIL_H

#include <libebook/e-book-client.h>

G_BEGIN_DECLS

GSList *	eab_contact_list_from_string	(const gchar *str);
gchar *		eab_contact_list_to_string	(const GSList *contacts);

gboolean	eab_book_and_contact_list_from_string
						(const gchar *str,
						 EBookClient **book_client,
						 GSList **contacts);
gchar *		eab_book_and_contact_list_to_string
						(EBookClient *book_client,
						 const GSList *contacts);
gint		e_utf8_casefold_collate_len	(const gchar *str1,
						 const gchar *str2,
						 gint len);
gint		e_utf8_casefold_collate		(const gchar *str1,
						 const gchar *str2);

G_END_DECLS

#endif /* EAB_BOOK_UTIL_H */

/*
 *
 * Evolution calendar - Copy source dialog
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
 *		Rodrigo Moya <rodrigo@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef COPY_SOURCE_DIALOG_H
#define COPY_SOURCE_DIALOG_H

#include <gtk/gtk.h>
#include <libecal/e-cal-util.h>
#include <libecal/e-cal-client.h>
#include <libedataserver/e-source.h>

void		copy_source_dialog		(GtkWindow *parent,
						 ESource *source,
						 ECalClientSourceType type);

#endif /* COPY_SOURCE_DIALOG_H */

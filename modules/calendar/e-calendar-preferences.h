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
 *		David Trowbridge <trowbrds cs colorado edu>
 *		Damon Chaplin <damon@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef CAL_PREFERENCES_H
#define CAL_PREFERENCES_H

#include <gtk/gtk.h>
#include <shell/e-shell.h>
#include <libedataserverui/e-source-selector.h>
#include <widgets/misc/e-preferences-window.h>

/* Standard GObject macros */
#define E_TYPE_CALENDAR_PREFERENCES \
	(e_calendar_preferences_get_type ())
#define E_CALENDAR_PREFERENCES(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_CALENDAR_PREFERENCES, ECalendarPreferences))
#define E_CALENDAR_PREFERENCES_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_CALENDAR_PREFERENCES, ECalendarPreferencesClass))
#define E_CALENDAR_IS_PREFERENCES(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_CALENDAR_PREFERENCES))
#define E_CALENDAR_IS_PREFERENCES_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_CALENDAR_PREFERENCES))
#define E_CALENDAR_PREFERENCES_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_CALENDAR_PREFERENCES, ECalendarPreferencesClass))

G_BEGIN_DECLS

typedef struct _ECalendarPreferences ECalendarPreferences;
typedef struct _ECalendarPreferencesClass ECalendarPreferencesClass;

struct _ECalendarPreferences {
	GtkVBox parent;

	GtkBuilder *builder;

	EShellSettings *shell_settings;

	/* General tab */
	GtkWidget *day_second_zone;
	GtkWidget *start_of_day;
	GtkWidget *end_of_day;
	GtkWidget *ba_reminder;
	GtkWidget *ba_reminder_interval;
	GtkWidget *ba_reminder_units;

	/* Alarms tab */
	GtkWidget *notify_with_tray;
	GtkWidget *scrolled_window;
	ESourceList *alarms_list;
	GtkWidget *alarm_list_widget;
};

struct _ECalendarPreferencesClass {
	GtkVBoxClass parent;
};

GType		e_calendar_preferences_get_type (void);
void		e_calendar_preferences_type_register
						(GTypeModule *type_module);
GtkWidget *	e_calendar_preferences_new	(EPreferencesWindow *window);

G_END_DECLS

#endif /* CAL_PREFERENCES_H */

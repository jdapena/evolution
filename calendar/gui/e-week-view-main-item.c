/*
 * EWeekViewMainItem - displays the background grid and dates for the Week and
 * Month calendar views.
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
 * Authors:
 *		Damon Chaplin <damon@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <glib/gi18n.h>

#include "e-week-view-main-item.h"
#include "ea-calendar.h"
#include "calendar-config.h"

struct _EWeekViewMainItemPrivate {
	EWeekView *week_view;
};

enum {
	PROP_0,
	PROP_WEEK_VIEW
};

static gpointer parent_class;

static gint
gdate_to_cal_weekdays (GDateWeekday wd)
{
	switch (wd) {
	case G_DATE_MONDAY: return CAL_MONDAY;
	case G_DATE_TUESDAY: return CAL_TUESDAY;
	case G_DATE_WEDNESDAY: return CAL_WEDNESDAY;
	case G_DATE_THURSDAY: return CAL_THURSDAY;
	case G_DATE_FRIDAY: return CAL_FRIDAY;
	case G_DATE_SATURDAY: return CAL_SATURDAY;
	case G_DATE_SUNDAY: return CAL_SUNDAY;
	default: break;
	}

	return 0;
}

static void
week_view_main_item_draw_day (EWeekViewMainItem *main_item,
                              gint day,
                              GDate *date,
                              cairo_t *cr,
                              gint x,
                              gint y,
                              gint width,
                              gint height)
{
	EWeekView *week_view;
	GtkStyle *style;
	gint right_edge, bottom_edge, date_width, date_x, line_y;
	gboolean show_day_name, show_month_name, selected;
	gchar buffer[128], *format_string;
	gint day_of_week, month, day_of_month, max_width;
	GdkColor *bg_color;
	PangoFontDescription *font_desc;
	PangoContext *pango_context;
	PangoFontMetrics *font_metrics;
	PangoLayout *layout;
	gboolean today = FALSE;
	CalWeekdays working_days;

	week_view = e_week_view_main_item_get_week_view (main_item);
	style = gtk_widget_get_style (GTK_WIDGET (week_view));

	/* Set up Pango prerequisites */
	font_desc = pango_font_description_copy (style->font_desc);
	pango_context = gtk_widget_get_pango_context (GTK_WIDGET (week_view));
	font_metrics = pango_context_get_metrics (
		pango_context, font_desc,
		pango_context_get_language (pango_context));

	day_of_week = gdate_to_cal_weekdays (g_date_get_weekday (date));
	month = g_date_get_month (date);
	day_of_month = g_date_get_day (date);
	line_y = y + E_WEEK_VIEW_DATE_T_PAD +
		PANGO_PIXELS (pango_font_metrics_get_ascent (font_metrics)) +
		PANGO_PIXELS (pango_font_metrics_get_descent (font_metrics)) +
		E_WEEK_VIEW_DATE_LINE_T_PAD;

	if (!today) {
		ECalendarView *view;
		struct icaltimetype tt;
		const icaltimezone *zone;

		view = E_CALENDAR_VIEW (week_view);
		zone = e_calendar_view_get_timezone (view);

		/* Check if we are drawing today */
		tt = icaltime_from_timet_with_zone (
			time (NULL), FALSE, zone);
		today = g_date_get_year (date) == tt.year
			&& g_date_get_month (date) == tt.month
			&& g_date_get_day (date) == tt.day;
	}

	working_days = calendar_config_get_working_days ();

	/* Draw the background of the day. In the month view odd months are
	 * one color and even months another, so you can easily see when each
	 * month starts (defaults are white for odd - January, March, ... and
	 * light gray for even). In the week view the background is always the
	 * same color, the color used for the odd months in the month view. */
	if (today)
		bg_color = &week_view->colors[E_WEEK_VIEW_COLOR_TODAY_BACKGROUND];
	else if ((working_days & day_of_week) == 0)
		bg_color = &week_view->colors[E_WEEK_VIEW_COLOR_MONTH_NONWORKING_DAY];
	else if (week_view->multi_week_view && (month % 2 == 0))
		bg_color = &week_view->colors[E_WEEK_VIEW_COLOR_EVEN_MONTHS];
	else
		bg_color = &week_view->colors[E_WEEK_VIEW_COLOR_ODD_MONTHS];

	cairo_save (cr);
	gdk_cairo_set_source_color (cr, bg_color);
	cairo_rectangle (cr, x, y, width, height);
	cairo_fill (cr);
	cairo_restore (cr);

	/* Draw the lines on the right and bottom of the cell. The canvas is
	 * sized so that the lines on the right & bottom edges will be off the
	 * edge of the canvas, so we don't have to worry about them. */
	right_edge = x + width - 1;
	bottom_edge = y + height - 1;

	cairo_save (cr);
	gdk_cairo_set_source_color (cr,  &week_view->colors[E_WEEK_VIEW_COLOR_GRID]);
	cairo_set_line_width (cr, 0.7);
	cairo_move_to (cr, right_edge, y);
	cairo_line_to (cr, right_edge, bottom_edge);
	cairo_move_to (cr, x, bottom_edge);
	cairo_line_to (cr, right_edge, bottom_edge);
	cairo_stroke (cr);
	cairo_restore (cr);

	/* If the day is selected, draw the blue background. */
	cairo_save (cr);
	selected = TRUE;
	if (week_view->selection_start_day == -1
	    || week_view->selection_start_day > day
	    || week_view->selection_end_day < day)
		selected = FALSE;
	if (selected) {
		if (gtk_widget_has_focus (GTK_WIDGET (week_view))) {
			gdk_cairo_set_source_color (
				cr, &week_view->colors[E_WEEK_VIEW_COLOR_SELECTED]);
		} else {
			gdk_cairo_set_source_color (
				cr, &week_view->colors[E_WEEK_VIEW_COLOR_SELECTED]);
		}

		if (week_view->multi_week_view) {
			cairo_rectangle (cr, x + 2, y + 1,
					    width - 5,
					    E_WEEK_VIEW_DATE_T_PAD - 1 +
				PANGO_PIXELS (pango_font_metrics_get_ascent (font_metrics)) +
				PANGO_PIXELS (pango_font_metrics_get_descent (font_metrics)));
			cairo_fill (cr);
		} else {
			cairo_rectangle (cr, x + 2, y + 1,
				    width - 5, line_y - y);
			cairo_fill (cr);
		}
	}
	cairo_restore (cr);

	/* Display the date in the top of the cell.
	 * In the week view, display the long format "10 January" in all cells,
	 * or abbreviate it to "10 Jan" or "10" if that doesn't fit.
	 * In the month view, only use the long format for the first cell and
	 * the 1st of each month, otherwise use "10". */
	show_day_name = FALSE;
	show_month_name = FALSE;
	if (!week_view->multi_week_view) {
		show_day_name = TRUE;
		show_month_name = TRUE;
	} else if (day == 0 || day_of_month == 1) {
		show_month_name = TRUE;
	}

	/* Now find the longest form of the date that will fit. */
	max_width = width - 4;
	format_string = NULL;
	if (show_day_name) {
		if (week_view->max_day_width + week_view->digit_width * 2
		    + week_view->space_width * 2
		    + week_view->month_widths[month - 1] < max_width)
			/* strftime format %A = full weekday name, %d = day of
			 * month, %B = full month name. You can change the
			 * order but don't change the specifiers or add
			 * anything. */
			format_string = _("%A %d %B");
		else if (week_view->max_abbr_day_width
			 + week_view->digit_width * 2
			 + week_view->space_width * 2
			 + week_view->abbr_month_widths[month - 1] < max_width)
			/* strftime format %a = abbreviated weekday name,
			 * %d = day of month, %b = abbreviated month name.
			 * You can change the order but don't change the
			 * specifiers or add anything. */
			format_string = _("%a %d %b");
	}
	if (!format_string && show_month_name) {
		if (week_view->digit_width * 2 + week_view->space_width
		    + week_view->month_widths[month - 1] < max_width)
			/* strftime format %d = day of month, %B = full
			 * month name. You can change the order but don't
			 * change the specifiers or add anything. */
			format_string = _("%d %B");
		else if (week_view->digit_width * 2 + week_view->space_width
		    + week_view->abbr_month_widths[month - 1] < max_width)
			/* strftime format %d = day of month, %b = abbreviated
			 * month name. You can change the order but don't
			 * change the specifiers or add anything. */
			format_string = _("%d %b");
	}

	cairo_save (cr);
	if (selected) {
		gdk_cairo_set_source_color (
			cr, &week_view->colors[E_WEEK_VIEW_COLOR_DATES_SELECTED]);
	} else if (week_view->multi_week_view) {
		if (today) {
			gdk_cairo_set_source_color (
				cr, &week_view->colors[E_WEEK_VIEW_COLOR_TODAY]);
		} else {
			gdk_cairo_set_source_color (
				cr, &week_view->colors[E_WEEK_VIEW_COLOR_DATES]);
		}
	} else {
		gdk_cairo_set_source_color (
			cr, &week_view->colors[E_WEEK_VIEW_COLOR_DATES]);
	}

	if (today) {
		g_date_strftime (
			buffer, sizeof (buffer),
			 format_string ? format_string : "<b>%d</b>", date);
		pango_cairo_update_context (cr, pango_context);
		layout = pango_cairo_create_layout (cr);
		pango_layout_set_font_description (layout, font_desc);
		pango_layout_set_text (layout, buffer, -1);
		pango_layout_set_markup (layout, buffer, strlen (buffer));
	} else {
		g_date_strftime (buffer, sizeof (buffer),
				 format_string ? format_string : "%d", date);
		pango_cairo_update_context (cr, pango_context);
		layout = pango_cairo_create_layout (cr);
		pango_layout_set_font_description (layout, font_desc);
		pango_layout_set_text (layout, buffer, -1);
	}

	pango_layout_get_pixel_size (layout, &date_width, NULL);
	date_x = x + width - date_width - E_WEEK_VIEW_DATE_R_PAD;
	date_x = MAX (date_x, x + 1);

	cairo_translate (cr, date_x, y + E_WEEK_VIEW_DATE_T_PAD);
	pango_cairo_update_layout (cr, layout);
	pango_cairo_show_layout (cr, layout);
	cairo_restore (cr);
	g_object_unref (layout);

	/* Draw the line under the date. */
	if (!week_view->multi_week_view) {
		cairo_save (cr);
		gdk_cairo_set_source_color (
			cr, &week_view->colors[E_WEEK_VIEW_COLOR_GRID]);
		cairo_set_line_width (cr, 0.7);
		cairo_move_to (cr, x + E_WEEK_VIEW_DATE_LINE_L_PAD, line_y);
		cairo_line_to (cr, right_edge, line_y);
		cairo_stroke (cr);
		cairo_restore (cr);
	}
	pango_font_metrics_unref (font_metrics);
	pango_font_description_free (font_desc);
}

static void
week_view_main_item_set_property (GObject *object,
                                  guint property_id,
                                  const GValue *value,
                                  GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_WEEK_VIEW:
			e_week_view_main_item_set_week_view (
				E_WEEK_VIEW_MAIN_ITEM (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
week_view_main_item_get_property (GObject *object,
                                  guint property_id,
                                  GValue *value,
                                  GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_WEEK_VIEW:
			g_value_set_object (
				value, e_week_view_main_item_get_week_view (
				E_WEEK_VIEW_MAIN_ITEM (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
week_view_main_item_dispose (GObject *object)
{
	EWeekViewMainItemPrivate *priv;

	priv = E_WEEK_VIEW_MAIN_ITEM (object)->priv;

	if (priv->week_view != NULL) {
		g_object_unref (priv->week_view);
		priv->week_view = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
week_view_main_item_update (GnomeCanvasItem *item,
                            const cairo_matrix_t *i2c,
                            gint flags)
{
	GnomeCanvasItemClass *canvas_item_class;

	/* Chain up to parent's update() method. */
	canvas_item_class = GNOME_CANVAS_ITEM_CLASS (parent_class);
	canvas_item_class->update (item, i2c, flags);

	/* The item covers the entire canvas area. */
	item->x1 = 0;
	item->y1 = 0;
	item->x2 = INT_MAX;
	item->y2 = INT_MAX;
}

static void
week_view_main_item_draw (GnomeCanvasItem *canvas_item,
                          cairo_t *cr,
                          gint x,
                          gint y,
                          gint width,
                          gint height)
{
	EWeekViewMainItem *main_item;
	EWeekView *week_view;
	GDate date;
	gint num_days, day, day_x, day_y, day_w, day_h;

	main_item = E_WEEK_VIEW_MAIN_ITEM (canvas_item);
	week_view = e_week_view_main_item_get_week_view (main_item);
	g_return_if_fail (week_view != NULL);

	/* Step through each of the days. */
	date = week_view->first_day_shown;

	/* If no date has been set, we just use Dec 1999/January 2000. */
	if (!g_date_valid (&date))
		g_date_set_dmy (&date, 27, 12, 1999);

	num_days = week_view->multi_week_view ? week_view->weeks_shown * 7 : 7;
	for (day = 0; day < num_days; day++) {
		e_week_view_get_day_position (week_view, day,
					      &day_x, &day_y,
					      &day_w, &day_h);
		/* Skip any days which are outside the area. */
		if (day_x < x + width && day_x + day_w >= x
		    && day_y < y + height && day_y + day_h >= y) {
			week_view_main_item_draw_day (
				main_item, day, &date, cr,
				day_x - x, day_y - y, day_w, day_h);
		}
		g_date_add_days (&date, 1);
	}
}

static GnomeCanvasItem *
week_view_main_item_point (GnomeCanvasItem *item,
                           gdouble x,
                           gdouble y,
                           gint cx,
                           gint cy)
{
	return item;
}

static void
week_view_main_item_class_init (EWeekViewMainItemClass *class)
{
	GObjectClass  *object_class;
	GnomeCanvasItemClass *item_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EWeekViewMainItemPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = week_view_main_item_set_property;
	object_class->get_property = week_view_main_item_get_property;
	object_class->dispose = week_view_main_item_dispose;

	item_class = GNOME_CANVAS_ITEM_CLASS (class);
	item_class->update = week_view_main_item_update;
	item_class->draw = week_view_main_item_draw;
	item_class->point = week_view_main_item_point;

	g_object_class_install_property (
		object_class,
		PROP_WEEK_VIEW,
		g_param_spec_object (
			"week-view",
			"Week View",
			NULL,
			E_TYPE_WEEK_VIEW,
			G_PARAM_READWRITE));

	/* init the accessibility support for e_week_view_main_item */
	e_week_view_main_item_a11y_init ();
}

static void
week_view_main_item_init (EWeekViewMainItem *main_item)
{
	main_item->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		main_item, E_TYPE_WEEK_VIEW_MAIN_ITEM,
		EWeekViewMainItemPrivate);
}

GType
e_week_view_main_item_get_type (void)
{
	static GType type = 0;

	if (G_UNLIKELY (type == 0)) {
		const GTypeInfo type_info = {
			sizeof (EWeekViewMainItemClass),
			(GBaseInitFunc) NULL,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) week_view_main_item_class_init,
			(GClassFinalizeFunc) NULL,
			NULL,  /* class_data */
			sizeof (EWeekViewMainItem),
			0,     /* n_preallocs */
			(GInstanceInitFunc) week_view_main_item_init,
			NULL   /* value_table */
		};

		type = g_type_register_static (
			GNOME_TYPE_CANVAS_ITEM, "EWeekViewMainItem",
			&type_info, 0);
	}

	return type;
}

EWeekView *
e_week_view_main_item_get_week_view (EWeekViewMainItem *main_item)
{
	g_return_val_if_fail (E_IS_WEEK_VIEW_MAIN_ITEM (main_item), NULL);

	return main_item->priv->week_view;
}

void
e_week_view_main_item_set_week_view (EWeekViewMainItem *main_item,
                                     EWeekView *week_view)
{
	g_return_if_fail (E_IS_WEEK_VIEW_MAIN_ITEM (main_item));
	g_return_if_fail (E_IS_WEEK_VIEW (week_view));

	if (main_item->priv->week_view != NULL)
		g_object_unref (main_item->priv->week_view);

	main_item->priv->week_view = g_object_ref (week_view);

	g_object_notify (G_OBJECT (main_item), "week-view");
}

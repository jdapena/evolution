/*
 * color.c: Color allocation on the Gnumeric spreadsheet
 *
 * Author:
 *  Miguel de Icaza (miguel@kernel.org)
 *
 * We keep our own color context, as the color allocation might take place
 * before any of our Canvases are realized.
 */
#include <config.h>
#include <gnome.h>
#include "color.h"

static int color_inited;
GdkColorContext *gnumeric_color_context;

int 
color_alloc (gushort red, gushort green, gushort blue)
{
	int failed;
	
	if (!color_inited)
		color_init ();
	
	return gdk_color_context_get_pixel (gnumeric_color_context,
					    red, green, blue, &failed);
}

void
color_alloc_gdk (GdkColor *c)
{
	int failed;
	
	g_return_if_fail (c != NULL);
	
	c->pixel = gdk_color_context_get_pixel (gnumeric_color_context, c->red, c->green, c->blue, &failed);
}

void
color_alloc_name (char *name, GdkColor *c)
{
	int failed;
	
	g_return_if_fail (name != NULL);
	g_return_if_fail (c != NULL);

	gdk_color_parse (name, c);
	c->pixel = 0;
	c->pixel = gdk_color_context_get_pixel (gnumeric_color_context, c->red, c->green, c->blue, &failed);
}

void
color_init (void)
{
	GdkColormap *colormap = gtk_widget_get_default_colormap ();
	
	/* Initialize the color context */
	gnumeric_color_context = gdk_color_context_new (
		gtk_widget_get_default_visual (), colormap);

	/* Allocate the default colors */
	gdk_color_white (colormap, &gs_white);
	gdk_color_black (colormap, &gs_black);

	color_alloc_name ("gray60", &gs_light_gray);
	color_alloc_name ("gray20", &gs_dark_gray);

	color_inited = 1;
}

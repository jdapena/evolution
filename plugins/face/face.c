/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  Author: Sankar P <psankar@novell.com>
 *   
 *  Copyright 2004 Novell, Inc. (www.novell.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "composer/e-msg-composer.h"
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <mail/em-menu.h>
#include <e-util/e-error.h>

#define d(x) x 

void org_gnome_composer_face (EPlugin * ep, EMMenuTargetWidget * target);
int e_plugin_lib_configure (EPlugin * ep);

void org_gnome_composer_face (EPlugin * ep, EMMenuTargetWidget * t)
{
	EMsgComposer *composer;
	gchar *filename, *file_contents;
	GError *error = NULL;

	composer = (EMsgComposer *) t->target.widget;
	filename = g_build_filename (g_get_home_dir (), ".evolution", "faces", NULL);
	g_file_get_contents (filename, &file_contents, NULL, &error);

	if (error) {

		GtkWidget *filesel;
		const char *image_filename;
		gsize length;

#ifdef USE_GTKFILECHOOSER
		GtkFileFilter *filter;

		filesel = gtk_file_chooser_dialog_new (_
					("Select a (48*48) png of size < 700bytes"),
					NULL,
					GTK_FILE_CHOOSER_ACTION_OPEN,
					GTK_STOCK_CANCEL,
					GTK_RESPONSE_CANCEL,
					GTK_STOCK_OPEN, GTK_RESPONSE_OK, NULL);

		gtk_dialog_set_default_response (GTK_DIALOG (filesel), GTK_RESPONSE_OK);

		filter = gtk_file_filter_new ();
		gtk_file_filter_set_name (filter, _("PNG files"));
		gtk_file_filter_add_mime_type (filter, "image/png");
		gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (filesel), filter);

#else
		filesel = gtk_file_selection_new (_("Select a (48*48) png of size < 720bytes"));
#endif

		if (GTK_RESPONSE_OK == gtk_dialog_run (GTK_DIALOG (filesel))) {
#ifdef USE_GTKFILECHOOSER
			image_filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filesel));
#else
			image_filename = gtk_file_selection_get_filename (GTK_FILE_SELECTION (filesel));
#endif

			error = NULL;
			file_contents = NULL;
			g_file_get_contents (image_filename, &file_contents, &length, &error);

			if (!error) {
				error = NULL;
				if (length < 720) {

					GdkPixbuf *pixbuf;
					GdkPixbufLoader *loader = gdk_pixbuf_loader_new();

					gdk_pixbuf_loader_write (loader, (guchar *)file_contents, length, NULL);
					gdk_pixbuf_loader_close (loader, NULL);

					pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
					if (pixbuf) {
						int width, height;

						g_object_ref (pixbuf);

						height = gdk_pixbuf_get_height (pixbuf);
						width = gdk_pixbuf_get_width (pixbuf);

						if (height != 48 || width != 48) {
							d (printf ("\n\a Invalid Image Size. Please choose a 48*48 image\n\a"));
							e_error_run (NULL, "org.gnome.evolution.plugins.face:invalid-image-size", NULL, NULL);
						} else {
							file_contents = camel_base64_encode_simple (file_contents, length);
							g_file_set_contents (filename, file_contents, -1, &error);
						}
					}
				} else {
					d (printf ("File too big"));
					e_error_run (NULL, "org.gnome.evolution.plugins.face:invalid-file-size", NULL, NULL);
				}

			} else {
				d (printf ("\n\a File cannot be read\n\a"));
				e_error_run (NULL, "org.gnome.evolution.plugins.face:file-not-found", NULL, NULL);
			}
		}
		gtk_widget_destroy (filesel);
	}
	e_msg_composer_modify_header (composer, "Face", file_contents);
}
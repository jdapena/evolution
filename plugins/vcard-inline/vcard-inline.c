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
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtk/gtk.h>
#include <glib/gi18n-lib.h>
#include <libebook/e-book-client.h>
#include <libebook/e-contact.h>
#include <gtkhtml/gtkhtml-embedded.h>
#include <libedataserverui/e-client-utils.h>
#include <libedataserverui/e-source-selector-dialog.h>

#include "addressbook/gui/merging/eab-contact-merging.h"
#include "addressbook/gui/widgets/eab-contact-display.h"
#include "addressbook/util/eab-book-util.h"
#include "mail/em-format-hook.h"
#include "mail/em-format-html.h"

#define d(x)

typedef struct _VCardInlinePURI VCardInlinePURI;

struct _VCardInlinePURI {
	EMFormatPURI puri;

	GSList *contact_list;
	ESourceList *source_list;
	GtkWidget *contact_display;
	GtkWidget *message_label;
};

static gint org_gnome_vcard_inline_classid;

/* Forward Declarations */
void org_gnome_vcard_inline_format (gpointer ep, EMFormatHookTarget *target);
gint e_plugin_lib_enable (EPlugin *ep, gint enable);

gint
e_plugin_lib_enable (EPlugin *ep,
                     gint enable)
{
	return 0;
}

static void
org_gnome_vcard_inline_pobject_free (EMFormatPURI *object)
{
	VCardInlinePURI *vcard_object;

	vcard_object = (VCardInlinePURI *) object;

	e_client_util_free_object_slist (vcard_object->contact_list);
	vcard_object->contact_list = NULL;

	if (vcard_object->source_list != NULL) {
		g_object_unref (vcard_object->source_list);
		vcard_object->source_list = NULL;
	}

	if (vcard_object->contact_display != NULL) {
		g_object_unref (vcard_object->contact_display);
		vcard_object->contact_display = NULL;
	}

	if (vcard_object->message_label != NULL) {
		g_object_unref (vcard_object->message_label);
		vcard_object->message_label = NULL;
	}
}

static void
org_gnome_vcard_inline_decode (VCardInlinePURI *vcard_object,
                               CamelMimePart *mime_part)
{
	CamelDataWrapper *data_wrapper;
	CamelMedium *medium;
	CamelStream *stream;
	GSList *contact_list;
	GByteArray *array;
	const gchar *string;
	const guint8 padding[2] = {0};

	array = g_byte_array_new ();
	medium = CAMEL_MEDIUM (mime_part);

	/* Stream takes ownership of the byte array. */
	stream = camel_stream_mem_new_with_byte_array (array);
	data_wrapper = camel_medium_get_content (medium);
	camel_data_wrapper_decode_to_stream_sync (
		data_wrapper, stream, NULL, NULL);

	/* because the result is not NULL-terminated */
	g_byte_array_append (array, padding, 2);

	string = (gchar *) array->data;
	contact_list = eab_contact_list_from_string (string);
	vcard_object->contact_list = contact_list;

	g_object_unref (mime_part);
	g_object_unref (stream);
}

static void
org_gnome_vcard_inline_client_loaded_cb (ESource *source,
                                         GAsyncResult *result,
                                         GSList *contact_list)
{
	EClient *client = NULL;
	EBookClient *book_client;
	GSList *iter;
	GError *error = NULL;

	e_client_utils_open_new_finish (source, result, &client, &error);

	if (error != NULL) {
		g_warn_if_fail (client == NULL);
		g_warning (
			"%s: Failed to open book client: %s",
			G_STRFUNC, error->message);
		g_error_free (error);
		goto exit;
	}

	g_return_if_fail (E_IS_BOOK_CLIENT (client));

	book_client = E_BOOK_CLIENT (client);

	for (iter = contact_list; iter != NULL; iter = iter->next) {
		EContact *contact;

		contact = E_CONTACT (iter->data);
		eab_merging_book_add_contact (book_client, contact, NULL, NULL);
	}

	g_object_unref (client);

 exit:
	e_client_util_free_object_slist (contact_list);
}

static void
org_gnome_vcard_inline_save_cb (VCardInlinePURI *vcard_object)
{
	ESource *source;
	GSList *contact_list;
	GtkWidget *dialog;

	g_return_if_fail (vcard_object->source_list != NULL);

	dialog = e_source_selector_dialog_new (NULL, vcard_object->source_list);

	e_source_selector_dialog_select_default_source (
		E_SOURCE_SELECTOR_DIALOG (dialog));

	if (gtk_dialog_run (GTK_DIALOG (dialog)) != GTK_RESPONSE_OK) {
		gtk_widget_destroy (dialog);
		return;
	}

	source = e_source_selector_dialog_peek_primary_selection (
		E_SOURCE_SELECTOR_DIALOG (dialog));

	gtk_widget_destroy (dialog);

	g_return_if_fail (source != NULL);

	contact_list = e_client_util_copy_object_slist (NULL, vcard_object->contact_list);

	e_client_utils_open_new (
		source, E_CLIENT_SOURCE_TYPE_CONTACTS, FALSE,
		NULL, e_client_utils_authenticate_handler, NULL,
		(GAsyncReadyCallback) org_gnome_vcard_inline_client_loaded_cb,
		contact_list);
}

static void
org_gnome_vcard_inline_toggle_cb (VCardInlinePURI *vcard_object,
                                  GtkButton *button)
{
	EABContactDisplay *contact_display;
	EABContactDisplayMode mode;
	const gchar *label;

	contact_display = EAB_CONTACT_DISPLAY (vcard_object->contact_display);
	mode = eab_contact_display_get_mode (contact_display);

	/* Toggle between "full" and "compact" modes. */
	if (mode == EAB_CONTACT_DISPLAY_RENDER_NORMAL) {
		mode = EAB_CONTACT_DISPLAY_RENDER_COMPACT;
		label = _("Show Full vCard");
	} else {
		mode = EAB_CONTACT_DISPLAY_RENDER_NORMAL;
		label = _("Show Compact vCard");
	}

	eab_contact_display_set_mode (contact_display, mode);
	gtk_button_set_label (button, label);
}

static GtkWidget*
org_gnome_vcard_inline_embed (EMFormat *emf,
                              EMFormatPURI *object,
                              GCancellable *cancellable)
{
	VCardInlinePURI *vcard_object;
	GtkWidget *button_box;
	GtkWidget *container;
	GtkWidget *widget;
	GtkWidget *layout;
	EContact *contact;
	guint length;

	vcard_object = (VCardInlinePURI *) object;
	length = g_slist_length (vcard_object->contact_list);

	if (vcard_object->contact_list != NULL)
		contact = E_CONTACT (vcard_object->contact_list->data);
	else
		contact = NULL;

	layout = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (layout);

	container = layout;

	widget = gtk_hbutton_box_new ();
	gtk_button_box_set_layout (
		GTK_BUTTON_BOX (widget), GTK_BUTTONBOX_START);
	gtk_box_set_spacing (GTK_BOX (widget), 12);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, TRUE, 0);
	gtk_widget_show (widget);

	button_box = widget;

	widget = eab_contact_display_new ();
	eab_contact_display_set_contact (
		EAB_CONTACT_DISPLAY (widget), contact);
	eab_contact_display_set_mode (
		EAB_CONTACT_DISPLAY (widget),
		EAB_CONTACT_DISPLAY_RENDER_COMPACT);
	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);
	vcard_object->contact_display = g_object_ref (widget);
	gtk_widget_show (widget);

	widget = gtk_label_new (NULL);
	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);
	vcard_object->message_label = g_object_ref (widget);

	if (length == 2) {
		const gchar *text;

		text = _("There is one other contact.");
		gtk_label_set_text (GTK_LABEL (widget), text);
		gtk_widget_show (widget);

	} else if (length > 2) {
		gchar *text;

		/* Translators: This will always be two or more. */
		text = g_strdup_printf (ngettext (
			"There is %d other contact.",
			"There are %d other contacts.",
			length - 1), length - 1);
		gtk_label_set_text (GTK_LABEL (widget), text);
		gtk_widget_show (widget);
		g_free (text);

	} else
		gtk_widget_hide (widget);

	container = button_box;

	widget = gtk_button_new_with_label (_("Show Full vCard"));
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	gtk_widget_show (widget);

	g_signal_connect_swapped (
		widget, "clicked",
		G_CALLBACK (org_gnome_vcard_inline_toggle_cb),
		vcard_object);

	widget = gtk_button_new_with_label (_("Save in Address Book"));
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);

	/* This depends on having a source list. */
	if (vcard_object->source_list != NULL)
		gtk_widget_show (widget);
	else
		gtk_widget_hide (widget);

	g_signal_connect_swapped (
		widget, "clicked",
		G_CALLBACK (org_gnome_vcard_inline_save_cb),
		vcard_object);

	return layout;
}

void
org_gnome_vcard_inline_format (gpointer ep,
                               EMFormatHookTarget *target)
{
	VCardInlinePURI *vcard_object;
	gchar *classid;
	gchar *content;

	classid = g_strdup_printf (
		"org-gnome-vcard-inline-display-%d",
		org_gnome_vcard_inline_classid++);

	vcard_object = (VCardInlinePURI *) em_format_puri_new (
			target->format, sizeof(VCardInlinePURI), target->part, classid);
	vcard_object->puri.widget_func = org_gnome_vcard_inline_embed;
	vcard_object->puri.free = org_gnome_vcard_inline_pobject_free;

	em_format_add_puri (target->format, (EMFormatPURI *) vcard_object);

	g_object_ref (target->part);
	org_gnome_vcard_inline_decode (vcard_object, target->part);

	e_book_client_get_sources (&vcard_object->source_list, NULL);

	/* FIXME WEBKIT: No streams, right?
	content = g_strdup_printf ("<object classid=%s></object>", classid);
	camel_stream_write_string (target->stream, content, NULL, NULL);
	g_free (content);
	*/

	g_free (classid);
}

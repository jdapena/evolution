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
 *		Michael Zucchi <notzed@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <gdk/gdkkeysyms.h>

#ifdef G_OS_WIN32
/* Work around 'DATADIR' and 'interface' lossage in <windows.h> */
#define DATADIR crap_DATADIR
#include <windows.h>
#undef DATADIR
#undef interface
#endif

#include <glib/gi18n.h>

#include <e-util/e-util.h>
#include <e-util/e-util-private.h>

#include "e-util/e-datetime-format.h"
#include <e-util/e-dialog-utils.h>
#include <e-util/e-icon-factory.h>

#include <shell/e-shell.h>
#include <shell/e-shell-utils.h>

#if defined (HAVE_NSS) && defined (ENABLE_SMIME)
#include "certificate-viewer.h"
#include "e-cert-db.h"
#endif

#include "e-mail-display.h"
#include "e-mail-attachment-bar.h"
#include "em-format-html-display.h"
#include "em-utils.h"
#include "widgets/misc/e-attachment.h"
#include "widgets/misc/e-attachment-button.h"
#include "widgets/misc/e-attachment-view.h"
#include "shell/e-shell.h"
#include "shell/e-shell-window.h"

#define d(x)

struct _EMFormatHTMLDisplayPrivate {
	EAttachmentView *attachment_view;
};

/* TODO: move the dialogue elsehwere */
/* FIXME: also in em-format-html.c */
static const struct {
	const gchar *icon, *shortdesc, *description;
} smime_sign_table[5] = {
	{ "stock_signature-bad", N_("Unsigned"), N_("This message is not signed. There is no guarantee that this message is authentic.") },
	{ "stock_signature-ok", N_("Valid signature"), N_("This message is signed and is valid meaning that it is very likely that this message is authentic.") },
	{ "stock_signature-bad", N_("Invalid signature"), N_("The signature of this message cannot be verified, it may have been altered in transit.") },
	{ "stock_signature", N_("Valid signature, but cannot verify sender"), N_("This message is signed with a valid signature, but the sender of the message cannot be verified.") },
	{ "stock_signature-bad", N_("Signature exists, but need public key"), N_("This message is signed with a signature, but there is no corresponding public key.") },

};

static const struct {
	const gchar *icon, *shortdesc, *description;
} smime_encrypt_table[4] = {
	{ "stock_lock-broken", N_("Unencrypted"), N_("This message is not encrypted. Its content may be viewed in transit across the Internet.") },
	{ "stock_lock-ok", N_("Encrypted, weak"), N_("This message is encrypted, but with a weak encryption algorithm. It would be difficult, but not impossible for an outsider to view the content of this message in a practical amount of time.") },
	{ "stock_lock-ok", N_("Encrypted"), N_("This message is encrypted.  It would be difficult for an outsider to view the content of this message.") },
	{ "stock_lock-ok", N_("Encrypted, strong"), N_("This message is encrypted, with a strong encryption algorithm. It would be very difficult for an outsider to view the content of this message in a practical amount of time.") },
};

static const gchar *smime_sign_colour[5] = {
	"", " bgcolor=\"#88bb88\"", " bgcolor=\"#bb8888\"", " bgcolor=\"#e8d122\"",""
};

static void efhd_message_prefix 	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void efhd_message_add_bar	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void efhd_parse_attachment	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);

static GtkWidget* efhd_attachment_bar		(EMFormat *emf, EMFormatPURI *puri, GCancellable *cancellable);
static GtkWidget* efhd_attachment_button	(EMFormat *emf, EMFormatPURI *puri, GCancellable *cancellable);
static GtkWidget* efhd_attachment_optional	(EMFormat *emf, EMFormatPURI *puri, GCancellable *cancellable);

static void efhd_free_attach_puri_data (EMFormatPURI *puri);
static void efhd_builtin_init (EMFormatHTMLDisplayClass *efhc);

static gpointer parent_class;

static void
efhd_xpkcs7mime_free (EMFormatPURI *puri)
{
	EMFormatSMIMEPURI *sp = (EMFormatSMIMEPURI *) puri;

	if (sp->widget)
		gtk_widget_destroy (sp->widget);

	if (sp->description)
		g_free (sp->description);

	camel_cipher_validity_free (sp->valid);
}

static void
efhd_xpkcs7mime_info_response (GtkWidget *widget,
                               guint button,
                               EMFormatSMIMEPURI *po)
{
	gtk_widget_destroy (widget);
	po->widget = NULL;
}

#if defined (HAVE_NSS) && defined (ENABLE_SMIME)
static void
efhd_xpkcs7mime_viewcert_clicked (GtkWidget *button,
                                  EMFormatSMIMEPURI *po)
{
	CamelCipherCertInfo *info = g_object_get_data((GObject *)button, "e-cert-info");
	ECert *ec = NULL;

	if (info->cert_data)
		ec = e_cert_new (CERT_DupCertificate (info->cert_data));

	if (ec != NULL) {
		GtkWidget *w = certificate_viewer_show (ec);

		/* oddly enough certificate_viewer_show doesn't ... */
		gtk_widget_show (w);
		g_signal_connect (w, "response", G_CALLBACK(gtk_widget_destroy), NULL);

		if (w && po->widget)
			gtk_window_set_transient_for ((GtkWindow *) w, (GtkWindow *) po->widget);

		g_object_unref (ec);
	} else {
		g_warning("can't find certificate for %s <%s>", info->name?info->name:"", info->email?info->email:"");
	}
}
#endif

static void
efhd_xpkcs7mime_add_cert_table (GtkWidget *vbox,
                                CamelDList *certlist,
                                EMFormatSMIMEPURI *po)
{
	CamelCipherCertInfo *info = (CamelCipherCertInfo *) certlist->head;
	GtkTable *table = (GtkTable *) gtk_table_new (camel_dlist_length (certlist), 2, FALSE);
	gint n = 0;

	while (info->next) {
		gchar *la = NULL;
		const gchar *l = NULL;

		if (info->name) {
			if (info->email && strcmp (info->name, info->email) != 0)
				l = la = g_strdup_printf("%s <%s>", info->name, info->email);
			else
				l = info->name;
		} else {
			if (info->email)
				l = info->email;
		}

		if (l) {
			GtkWidget *w;
#if defined (HAVE_NSS) && defined (ENABLE_SMIME)
			ECert *ec = NULL;
#endif
			w = gtk_label_new (l);
			gtk_misc_set_alignment ((GtkMisc *) w, 0.0, 0.5);
			g_free (la);
			gtk_table_attach (table, w, 0, 1, n, n + 1, GTK_FILL, GTK_FILL, 3, 3);
#if defined (HAVE_NSS) && defined (ENABLE_SMIME)
			w = gtk_button_new_with_mnemonic(_("_View Certificate"));
			gtk_table_attach (table, w, 1, 2, n, n + 1, 0, 0, 3, 3);
			g_object_set_data((GObject *)w, "e-cert-info", info);
			g_signal_connect (w, "clicked", G_CALLBACK(efhd_xpkcs7mime_viewcert_clicked), po);

			if (info->cert_data)
				ec = e_cert_new (CERT_DupCertificate (info->cert_data));

			if (ec == NULL)
				gtk_widget_set_sensitive (w, FALSE);
			else
				g_object_unref (ec);
#else
			w = gtk_label_new (_("This certificate is not viewable"));
			gtk_table_attach (table, w, 1, 2, n, n + 1, 0, 0, 3, 3);
#endif
			n++;
		}

		info = info->next;
	}

	gtk_box_pack_start ((GtkBox *) vbox, (GtkWidget *) table, TRUE, TRUE, 6);
}

static void
efhd_xpkcs7mime_validity_clicked (GtkWidget *button,
                                  EMFormatPURI *puri)
{
	EMFormatSMIMEPURI *po = (EMFormatSMIMEPURI *) puri;
	GtkBuilder *builder;
	GtkWidget *vbox, *w;

	if (po->widget)
		/* FIXME: window raise? */
		return;

	builder = gtk_builder_new ();
	e_load_ui_builder_definition (builder, "mail-dialogs.ui");

	po->widget = e_builder_get_widget(builder, "message_security_dialog");

	vbox = e_builder_get_widget(builder, "signature_vbox");
	w = gtk_label_new (_(smime_sign_table[po->valid->sign.status].description));
	gtk_misc_set_alignment ((GtkMisc *) w, 0.0, 0.5);
	gtk_label_set_line_wrap ((GtkLabel *) w, TRUE);
	gtk_box_pack_start ((GtkBox *) vbox, w, TRUE, TRUE, 6);
	if (po->valid->sign.description) {
		GtkTextBuffer *buffer;

		buffer = gtk_text_buffer_new (NULL);
		gtk_text_buffer_set_text (buffer, po->valid->sign.description, strlen (po->valid->sign.description));
		w = g_object_new (gtk_scrolled_window_get_type (),
				 "hscrollbar_policy", GTK_POLICY_AUTOMATIC,
				 "vscrollbar_policy", GTK_POLICY_AUTOMATIC,
				 "shadow_type", GTK_SHADOW_IN,
				 "child", g_object_new(gtk_text_view_get_type(),
						       "buffer", buffer,
						       "cursor_visible", FALSE,
						       "editable", FALSE,
						       "width_request", 500,
						       "height_request", 160,
						       NULL),
				 NULL);
		g_object_unref (buffer);

		gtk_box_pack_start ((GtkBox *) vbox, w, TRUE, TRUE, 6);
	}

	if (!camel_dlist_empty (&po->valid->sign.signers))
		efhd_xpkcs7mime_add_cert_table (vbox, &po->valid->sign.signers, po);

	gtk_widget_show_all (vbox);

	vbox = e_builder_get_widget(builder, "encryption_vbox");
	w = gtk_label_new (_(smime_encrypt_table[po->valid->encrypt.status].description));
	gtk_misc_set_alignment ((GtkMisc *) w, 0.0, 0.5);
	gtk_label_set_line_wrap ((GtkLabel *) w, TRUE);
	gtk_box_pack_start ((GtkBox *) vbox, w, TRUE, TRUE, 6);
	if (po->valid->encrypt.description) {
		GtkTextBuffer *buffer;

		buffer = gtk_text_buffer_new (NULL);
		gtk_text_buffer_set_text (buffer, po->valid->encrypt.description, strlen (po->valid->encrypt.description));
		w = g_object_new (gtk_scrolled_window_get_type (),
				 "hscrollbar_policy", GTK_POLICY_AUTOMATIC,
				 "vscrollbar_policy", GTK_POLICY_AUTOMATIC,
				 "shadow_type", GTK_SHADOW_IN,
				 "child", g_object_new(gtk_text_view_get_type(),
						       "buffer", buffer,
						       "cursor_visible", FALSE,
						       "editable", FALSE,
						       "width_request", 500,
						       "height_request", 160,
						       NULL),
				 NULL);
		g_object_unref (buffer);

		gtk_box_pack_start ((GtkBox *) vbox, w, TRUE, TRUE, 6);
	}

	if (!camel_dlist_empty (&po->valid->encrypt.encrypters))
		efhd_xpkcs7mime_add_cert_table (vbox, &po->valid->encrypt.encrypters, po);

	gtk_widget_show_all (vbox);

	g_object_unref (builder);

	g_signal_connect (po->widget, "response", G_CALLBACK(efhd_xpkcs7mime_info_response), po);
	gtk_widget_show (po->widget);
}

static GtkWidget*
efhd_xpkcs7mime_button (EMFormat *emf,
                        EMFormatPURI *puri,
                        GCancellable *cancellable)
{
	GtkWidget *widget;
	EMFormatSMIMEPURI *po = (EMFormatSMIMEPURI *) puri;
	const gchar *icon_name;

	/* FIXME: need to have it based on encryption and signing too */
	if (po->valid->sign.status != 0)
		icon_name = smime_sign_table[po->valid->sign.status].icon;
	else
		icon_name = smime_encrypt_table[po->valid->encrypt.status].icon;

	widget = gtk_button_new ();
	g_signal_connect (
		widget, "clicked",
		G_CALLBACK (efhd_xpkcs7mime_validity_clicked), puri);
	gtk_widget_show (widget);

	widget = gtk_image_new_from_icon_name (
		icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
	gtk_widget_show (widget);

	return widget;
}

static void
efhd_parse (EMFormat *emf,
	    CamelMimeMessage *msg,
	    CamelFolder *folder,
	    GCancellable *cancellable)
{
	EMFormatHTMLDisplay *efhd;

	efhd = EM_FORMAT_HTML_DISPLAY (emf);
	g_return_if_fail (efhd != NULL);

	/* FIXME WEBKIT Duh?
	if (emf != src)
		EM_FORMAT_HTML (emf)->header_wrap_flags = 0;
	 */

	/* Chain up to parent's format_clone() method. */
	EM_FORMAT_CLASS (parent_class)->
		parse (emf, msg, folder, cancellable);
}

static void
efhd_parse_attachment (EMFormat *emf,
                       CamelMimePart *part,
                       GString *part_id,
                       EMFormatParserInfo *info,
                       GCancellable *cancellable)
{
	gchar *text, *html;
	EMFormatAttachmentPURI *puri;
	const EMFormatHandler *handler;
	CamelContentType *ct;
	gchar *mime_type;
	gint len;
	const gchar *cid;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	len = part_id->len;
	g_string_append (part_id, ".attachment");

	ct = camel_mime_part_get_content_type (part);
	if (ct) {
		mime_type = camel_content_type_simple (ct);

		handler = em_format_find_handler (emf, mime_type);
	}

	/* FIXME: should we look up mime_type from object again? */
	text = em_format_describe_part (part, mime_type);
	html = camel_text_to_html (
		text, EM_FORMAT_HTML (emf)->text_html_flags &
		CAMEL_MIME_FILTER_TOHTML_CONVERT_URLS, 0);
	g_free (text);
	g_free (mime_type);

	puri = (EMFormatAttachmentPURI*) em_format_puri_new (
			emf, sizeof (EMFormatAttachmentPURI), part, part_id->str);
	puri->puri.free = efhd_free_attach_puri_data;
	puri->puri.widget_func = efhd_attachment_button;
	puri->shown = em_format_is_inline (emf, part_id->str, part, info->handler);
	puri->snoop_mime_type = em_format_snoop_type (part);
	puri->attachment = e_attachment_new ();
	puri->attachment_view_part_id = g_strdup (part_id->str);
	puri->description = html;
	puri->handle = handler;

	cid = camel_mime_part_get_content_id (part);
	if (cid)
		puri->puri.cid = g_strdup_printf ("cid:%s", cid);

	if (handler)
		puri->puri.write_func = handler->write_func;

	em_format_add_puri (emf, (EMFormatPURI *) puri);

	e_attachment_set_mime_part (puri->attachment, part);

	if (info->validity) {
		puri->sign = info->validity->sign.status;
		puri->encrypt = info->validity->encrypt.status;
	}

	g_string_truncate (part_id, len);
}

static void
efhd_format_optional (EMFormat *emf,
                      EMFormatPURI *puri,
                      CamelStream *mstream,
                      GCancellable *cancellable)
{
	gchar *classid;
	EMFormatAttachmentPURI *info;

	classid = g_strdup_printf ("optional%s", puri->uri);

	info = (EMFormatAttachmentPURI *) em_format_puri_new (
			emf, sizeof (EMFormatAttachmentPURI), puri->part, classid);
	info->puri.free = efhd_free_attach_puri_data;
	info->attachment_view_part_id = g_strdup (puri->uri);
	info->handle = em_format_find_handler (emf, "text/plain");
	info->shown = FALSE;
	info->snoop_mime_type = "text/plain";
	info->attachment = e_attachment_new ();
	e_attachment_set_mime_part (info->attachment, info->puri.part);
	info->description = g_strdup(_("Evolution cannot render this email as it is too "
			  	  	  	  	  	   "large to process. You can view it unformatted or "
			  	  	  	  	  	   "with an external text editor."));

	/* MStream holds content of the 'attachment' to be displayed */
	info->mstream = (CamelStreamMem *) g_object_ref (mstream);

	if (puri->validity) {
		info->sign = puri->validity->sign.status;
		info->encrypt = puri->validity->encrypt.status;
	}

	em_format_add_puri (emf, (EMFormatPURI *) info);

	g_free (classid);
}

static void
efhd_format_secure (EMFormat *emf,
		    EMFormatPURI *puri,
		    GCancellable *cancellable)
{
	EMFormatClass *format_class;

	format_class = g_type_class_peek (EM_TYPE_FORMAT);
	format_class->format_secure (emf, puri, cancellable);

	if (puri->validity
	    && (puri->validity->encrypt.status != CAMEL_CIPHER_VALIDITY_ENCRYPT_NONE
		|| puri->validity->sign.status != CAMEL_CIPHER_VALIDITY_SIGN_NONE)) {
		GString *buffer;
		EMFormatSMIMEPURI *pobj;

		pobj = (EMFormatSMIMEPURI *) em_format_puri_new (
				emf, sizeof (EMFormatSMIMEPURI), puri->part, puri->uri);
		pobj->puri.free = efhd_xpkcs7mime_free;
		pobj->valid = camel_cipher_validity_clone (puri->validity);
		pobj->puri.widget_func = efhd_xpkcs7mime_button;

		em_format_add_puri (emf, (EMFormatPURI*) pobj);

		buffer = g_string_new ("");

		if (puri->validity->sign.status != CAMEL_CIPHER_VALIDITY_SIGN_NONE) {
			gchar *signers;
			const gchar *desc;
			gint status;

			status = puri->validity->sign.status;
			desc = smime_sign_table[status].shortdesc;

			g_string_append (buffer, gettext (desc));

			signers = em_format_html_format_cert_infos (
				(CamelCipherCertInfo *) puri->validity->sign.signers.head);
			if (signers && *signers) {
				g_string_append_printf (
					buffer, " (%s)", signers);
			}
			g_free (signers);
		}

		if (puri->validity->encrypt.status != CAMEL_CIPHER_VALIDITY_ENCRYPT_NONE) {
			const gchar *desc;
			gint status;

			if (puri->validity->sign.status != CAMEL_CIPHER_VALIDITY_SIGN_NONE)
				g_string_append (buffer, "\n");

			status = puri->validity->encrypt.status;
			desc = smime_encrypt_table[status].shortdesc;
			g_string_append (buffer, gettext (desc));
		}

		pobj->description = g_string_free (buffer, FALSE);
	}
}

static void
attachment_load_finish (EAttachment *attachment,
                        GAsyncResult *result,
                        GFile *file)
{
	EShell *shell;
	GtkWindow *parent;

	e_attachment_load_finish (attachment, result, NULL);

	shell = e_shell_get_default ();
	parent = e_shell_get_active_window (shell);

	e_attachment_save_async (
		attachment, file, (GAsyncReadyCallback)
		e_attachment_save_handle_error, parent);

	g_object_unref (file);
}

static void
action_image_save_cb (GtkAction *action,
                      EMFormatHTMLDisplay *efhd)
{
	EWebView *web_view;
	EMFormat *emf;
	const gchar *image_src;
	CamelMimePart *part;
	EAttachment *attachment;
	GFile *file;

	web_view = em_format_html_get_web_view (EM_FORMAT_HTML (efhd));
	g_return_if_fail (web_view != NULL);

	image_src = e_web_view_get_cursor_image_src (web_view);
	if (!image_src)
		return;

	emf = EM_FORMAT (efhd);
	g_return_if_fail (emf != NULL);
	g_return_if_fail (emf->message != NULL);

	if (g_str_has_prefix (image_src, "cid:")) {
		part = camel_mime_message_get_part_by_content_id (
			emf->message, image_src + 4);
		g_return_if_fail (part != NULL);

		g_object_ref (part);
	} else {
		CamelStream *image_stream;
		CamelDataWrapper *dw;
		const gchar *filename;

		image_stream = em_format_html_get_cached_image (
			EM_FORMAT_HTML (efhd), image_src);
		if (!image_stream)
			return;

		filename = strrchr (image_src, '/');
		if (filename && strchr (filename, '?'))
			filename = NULL;
		else if (filename)
			filename = filename + 1;

		part = camel_mime_part_new ();
		if (filename)
			camel_mime_part_set_filename (part, filename);

		dw = camel_data_wrapper_new ();
		camel_data_wrapper_set_mime_type (
			dw, "application/octet-stream");
		camel_data_wrapper_construct_from_stream_sync (
			dw, image_stream, NULL, NULL);
		camel_medium_set_content (CAMEL_MEDIUM (part), dw);
		g_object_unref (dw);

		camel_mime_part_set_encoding (
			part, CAMEL_TRANSFER_ENCODING_BASE64);

		g_object_unref (image_stream);
	}

	file = e_shell_run_save_dialog (
		e_shell_get_default (),
		_("Save Image"), camel_mime_part_get_filename (part),
		NULL, NULL, NULL);
	if (file == NULL) {
		g_object_unref (part);
		return;
	}

	attachment = e_attachment_new ();
	e_attachment_set_mime_part (attachment, part);

	e_attachment_load_async (
		attachment, (GAsyncReadyCallback)
		attachment_load_finish, file);

	g_object_unref (part);
}

static void
efhd_web_view_update_actions_cb (EWebView *web_view,
                                 EMFormatHTMLDisplay *efhd)
{
	const gchar *image_src;
	gboolean visible;
	GtkAction *action;

	g_return_if_fail (web_view != NULL);

	image_src = e_web_view_get_cursor_image_src (web_view);
	visible = image_src && g_str_has_prefix (image_src, "cid:");
	if (!visible && image_src) {
		CamelStream *image_stream;

		image_stream = em_format_html_get_cached_image (
			EM_FORMAT_HTML (efhd), image_src);
		visible = image_stream != NULL;

		if (image_stream)
			g_object_unref (image_stream);
	}

	action = e_web_view_get_action (web_view, "efhd-image-save");
	if (action)
		gtk_action_set_visible (action, visible);
}

static GtkActionEntry image_entries[] = {
	{ "efhd-image-save",
	  GTK_STOCK_SAVE,
	  N_("Save _Image..."),
	  NULL,
	  N_("Save the image to a file"),
	  G_CALLBACK (action_image_save_cb) }
};

static const gchar *image_ui =
	"<ui>"
	"  <popup name='context'>"
	"    <placeholder name='custom-actions-2'>"
	"      <menuitem action='efhd-image-save'/>"
	"    </placeholder>"
	"  </popup>"
	"</ui>";

static void
efhd_finalize (GObject *object)
{
	EMFormatHTMLDisplay *efhd;

	efhd = EM_FORMAT_HTML_DISPLAY (object);
	g_return_if_fail (efhd != NULL);

	if (efhd->priv->attachment_view) {
		gtk_widget_destroy (GTK_WIDGET (efhd->priv->attachment_view));
		efhd->priv->attachment_view = NULL;
	}

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
efhd_class_init (EMFormatHTMLDisplayClass *class)
{
	GObjectClass *object_class;
	EMFormatClass *format_class;
	EMFormatHTMLClass *format_html_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EMFormatHTMLDisplayPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->finalize = efhd_finalize;

	format_class = EM_FORMAT_CLASS (class);
	format_class->parse = efhd_parse;
/* FIXME WEBKIT format attachment?
	format_class->format_attachment = efhd_format_attachment;
*/
	format_class->format_optional = efhd_format_optional;
	format_class->format_secure = efhd_format_secure;

	format_html_class = EM_FORMAT_HTML_CLASS (class);
	format_html_class->html_widget_type = E_TYPE_MAIL_DISPLAY;

	efhd_builtin_init (class);
}

static void
efhd_init (EMFormatHTMLDisplay *efhd)
{
	efhd->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		efhd, EM_TYPE_FORMAT_HTML_DISPLAY, EMFormatHTMLDisplayPrivate);

	efhd->priv->attachment_view = E_ATTACHMENT_VIEW (e_mail_attachment_bar_new ());

	/* we want to convert url's etc */
	EM_FORMAT_HTML (efhd)->text_html_flags |=
		CAMEL_MIME_FILTER_TOHTML_CONVERT_URLS |
		CAMEL_MIME_FILTER_TOHTML_CONVERT_ADDRESSES;

	image_actions = e_web_view_get_action_group (web_view, "image");
	g_return_if_fail (image_actions != NULL);

	gtk_action_group_add_actions (
		image_actions, image_entries,
		G_N_ELEMENTS (image_entries), efhd);

	/* Because we are loading from a hard-coded string, there is
	 * no chance of I/O errors.  Failure here implies a malformed
	 * UI definition.  Full stop. */
	ui_manager = e_web_view_get_ui_manager (web_view);
	gtk_ui_manager_add_ui_from_string (ui_manager, image_ui, -1, &error);
	if (error != NULL)
		g_error ("%s", error->message);

	g_signal_connect (
		web_view, "update-actions",
		G_CALLBACK (efhd_web_view_update_actions_cb), efhd);
}

GType
em_format_html_display_get_type (void)
{
	static GType type = 0;

	if (G_UNLIKELY (type == 0)) {
		static const GTypeInfo type_info = {
			sizeof (EMFormatHTMLDisplayClass),
			(GBaseInitFunc) NULL,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) efhd_class_init,
			(GClassFinalizeFunc) NULL,
			NULL,  /* class_data */
			sizeof (EMFormatHTMLDisplay),
			0,     /* n_preallocs */
			(GInstanceInitFunc) efhd_init,
			NULL   /* value_table */
		};

		type = g_type_register_static (
			EM_TYPE_FORMAT_HTML, "EMFormatHTMLDisplay",
			&type_info, 0);
	}

	return type;
}

EMFormatHTMLDisplay *
em_format_html_display_new (void)
{
	return g_object_new (EM_TYPE_FORMAT_HTML_DISPLAY, NULL);
}

/* ********************************************************************** */

static EMFormatHandler type_builtin_table[] = {
	{ (gchar *) "x-evolution/message/prefix", efhd_message_prefix, },
	{ (gchar *) "x-evolution/message/attachment-bar", (EMFormatParseFunc) efhd_message_add_bar, },
	{ (gchar *) "x-evolution/message/attachment", efhd_parse_attachment, },
};

static void
efhd_builtin_init (EMFormatHTMLDisplayClass *efhc)
{
	gint i;

	EMFormatClass *emfc = (EMFormatClass *) efhc;

	for (i = 0; i < G_N_ELEMENTS (type_builtin_table); i++)
		em_format_class_add_handler (emfc, &type_builtin_table[i]);
}

static void
efhd_message_prefix (EMFormat *emf,
                     CamelMimePart *part,
                     GString *part_id,
                     EMFormatParserInfo *info,
                     GCancellable *cancellable)
{
	const gchar *flag, *comp, *due;
	time_t date;
	gchar *iconpath, *due_date_str;
	GString *buffer;
	EMFormatAttachmentPURI *puri;

	if (emf->folder == NULL || emf->message_uid == NULL
	    || (flag = camel_folder_get_message_user_tag(emf->folder, emf->message_uid, "follow-up")) == NULL
	    || flag[0] == 0)
		return;

	puri = (EMFormatAttachmentPURI *) em_format_puri_new (
			emf, sizeof (EMFormatAttachmentPURI), part, ".message_prefix");

	puri->attachment_view_part_id = g_strdup (part_id->str);

	comp = camel_folder_get_message_user_tag(emf->folder, emf->message_uid, "completed-on");
	iconpath = e_icon_factory_get_icon_filename (comp && comp[0] ? "stock_mail-flag-for-followup-done" : "stock_mail-flag-for-followup", GTK_ICON_SIZE_MENU);
	if (iconpath) {
		gchar *classid;

		classid = g_strdup_printf (
			"icon:///em-format-html-display/%s/%s",
			part_id->str,
			comp && comp[0] ? "comp" : "uncomp");

		puri->puri.uri = classid;

		g_free (classid);
	}

	buffer = g_string_new ("");

	if (comp && comp[0]) {
		date = camel_header_decode_date (comp, NULL);
		due_date_str = e_datetime_format_format (
			"mail", "header", DTFormatKindDateTime, date);
		g_string_append_printf (
			buffer, "%s, %s %s",
			flag, _("Completed on"),
			due_date_str ? due_date_str : "???");
		g_free (due_date_str);
	} else if ((due = camel_folder_get_message_user_tag(emf->folder, emf->message_uid, "due-by")) != NULL && due[0]) {
		time_t now;

		date = camel_header_decode_date (due, NULL);
		now = time (NULL);
		if (now > date)
			g_string_append_printf (
				buffer,
				"<b>%s</b> ",
				_("Overdue:"));

		due_date_str = e_datetime_format_format (
			"mail", "header", DTFormatKindDateTime, date);
		/* Translators: the "by" is part of the string,
		 * like "Follow-up by Tuesday, January 13, 2009" */
		g_string_append_printf (
			buffer, "%s %s %s",
			flag, _("by"),
			due_date_str ? due_date_str : "???");
		g_free (due_date_str);
	} else {
		g_string_append (buffer, flag);
	}

	puri->description = g_string_free (buffer, FALSE);
}

/* ********************************************************************** */

/* attachment button callback */
static GtkWidget*
efhd_attachment_button (EMFormat *emf,
						EMFormatPURI *puri,
						GCancellable *cancellable)
{
	EShell *shell;
	GtkWindow *window;
	EMFormatAttachmentPURI *info = (EMFormatAttachmentPURI *) puri;
	EMFormatHTML *efh = (EMFormatHTML *) emf;
	EMFormatHTMLDisplay *efhd = (EMFormatHTMLDisplay *) efh;
	EAttachmentStore *store;
	EAttachment *attachment;
	GtkWidget *widget;
	gpointer parent;
	guint32 size = 0;

	/* FIXME: handle default shown case */
	d(printf("adding attachment button/content\n"));

	if (g_cancellable_is_cancelled (cancellable))
		return NULL;

	if (emf->folder && emf->folder->summary && emf->message_uid) {
		CamelMessageInfo *mi;

		mi = camel_folder_summary_uid (emf->folder->summary, emf->message_uid);
		if (mi) {
			const CamelMessageContentInfo *ci;

			ci = camel_folder_summary_guess_content_info (mi,
					camel_mime_part_get_content_type (info->puri.part));
			if (ci) {
				size = ci->size;
				/* what if its not encoded in base64 ? is it a case to consider? */
				if (ci->encoding && !g_ascii_strcasecmp (ci->encoding, "base64"))
					size = size / 1.37;
			}
			camel_message_info_free (mi);
		}
	}

	if (!info || info->forward) {
		g_warning ("unable to expand the attachment\n");
		return NULL;
	}

	attachment = info->attachment;
	e_attachment_set_shown (attachment, info->shown);
	e_attachment_set_signed (attachment, info->sign);
	e_attachment_set_encrypted (attachment, info->encrypt);
	e_attachment_set_can_show (attachment, info->handle != NULL && info->handle->write_func);

	/* FIXME: Try to find a better way? */
	shell = e_shell_get_default ();
	window = e_shell_get_active_window (shell);
	if (E_IS_SHELL_WINDOW (window))
		parent = GTK_WIDGET (window);
	else
		parent = NULL;

	store = e_attachment_view_get_store (efhd->priv->attachment_view);
	e_attachment_store_add_attachment (store, info->attachment);

	e_attachment_load_async (
		info->attachment, (GAsyncReadyCallback)
		e_attachment_load_handle_error, parent);
	if (size != 0) {
		GFileInfo *fileinfo;

		fileinfo = e_attachment_get_file_info (info->attachment);
		g_file_info_set_size (fileinfo, size);
		e_attachment_set_file_info (info->attachment, fileinfo);
	}

	widget = e_attachment_button_new (efhd->priv->attachment_view);
	e_attachment_button_set_attachment (
		E_ATTACHMENT_BUTTON (widget), attachment);
	gtk_widget_set_can_focus (widget, TRUE);
	gtk_widget_show (widget);

	/* FIXME Not sure why the expanded callback can't just use
	 *       info->puri.format, but there seems to be lifecycle
	 *       issues with the PURI struct.  Maybe it should have
	 *       a reference count? */
	g_object_set_data (G_OBJECT (widget), "efh", efh);

	return widget;
}

static GtkWidget*
efhd_attachment_bar (EMFormat *emf,
		     EMFormatPURI *puri,
		     GCancellable *cancellable)
{
	EMFormatHTMLDisplay *efhd = (EMFormatHTMLDisplay *) emf;

	return GTK_WIDGET (efhd->priv->attachment_view);
}

static void
set_size_request_cb (gpointer message_part_id,
                     gpointer widget,
                     gpointer width)
{
	gtk_widget_set_size_request (widget, GPOINTER_TO_INT (width), -1);
}

static void
efhd_bar_resize (EMFormatHTML *efh,
                 GtkAllocation *event)
{
	EMFormatHTMLDisplayPrivate *priv;
	GtkAllocation allocation;
	EWebView *web_view;
	GtkWidget *widget;
	gint width;

	priv = EM_FORMAT_HTML_DISPLAY (efh)->priv;

	/* FIXME WEBKIT: Ehm :)
	web_view = em_format_html_get_web_view (efh);

	widget = GTK_WIDGET (web_view);
	gtk_widget_get_allocation (widget, &allocation);
	width = allocation.width - 12;

	if (width > 0) {
		g_hash_table_foreach (priv->attachment_views, set_size_request_cb, GINT_TO_POINTER (width));
	}
	*/
}

static void
efhd_message_add_bar (EMFormat *emf,
                      CamelMimePart *part,
                      GString *part_id,
                      EMFormatParserInfo *info,
                      GCancellable *cancellable)
{
	gchar *classid;
	EMFormatAttachmentPURI *puri;
	gint len;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	len = part_id->len;
	g_string_append (part_id, ".attachment-bar");
	puri = (EMFormatAttachmentPURI *) em_format_puri_new (
			emf, sizeof (EMFormatAttachmentPURI), part, part_id->str);
	puri->puri.widget_func = efhd_attachment_bar;
	em_format_add_puri (emf, (EMFormatPURI*) puri);

	g_string_truncate (part_id, len);
}

static void
efhd_optional_button_show (GtkWidget *widget,
                           GtkWidget *w)
{
	GtkWidget *label = g_object_get_data (G_OBJECT (widget), "text-label");

	if (gtk_widget_get_visible (w)) {
		gtk_widget_hide (w);
		gtk_label_set_text_with_mnemonic (GTK_LABEL (label), _("View _Unformatted"));
	} else {
		gtk_label_set_text_with_mnemonic (GTK_LABEL (label), _("Hide _Unformatted"));
		gtk_widget_show (w);
	}
}

static void
efhd_resize (GtkWidget *w,
             GtkAllocation *event,
             EMFormatHTML *efh)
{
	EWebView *web_view;
	GtkAllocation allocation;

	/* FIXME WEBKIT: Emh...
	web_view = em_format_html_get_web_view (efh);
	gtk_widget_get_allocation (GTK_WIDGET (web_view), &allocation);
	gtk_widget_set_size_request (w, allocation.width - 48, 250);
	*/
}

/* optional render attachment button callback */
static GtkWidget*
efhd_attachment_optional (EMFormat *efh,
						  EMFormatPURI *puri,
						  GCancellable *cancellable)
{
	GtkWidget *hbox, *vbox, *button, *mainbox, *scroll, *label, *img;
	AtkObject *a11y;
	GtkWidget *view;
	GtkTextBuffer *buffer;
	GByteArray *byte_array;
	EMFormatAttachmentPURI *info = (EMFormatAttachmentPURI *) puri;

	if (g_cancellable_is_cancelled (cancellable))
		return NULL;

	/* FIXME: handle default shown case */
	d(printf("adding attachment button/content for optional rendering\n"));

	if (!info || info->forward) {
		g_warning ("unable to expand the attachment\n");
		return NULL;
	}

	scroll = gtk_scrolled_window_new (NULL, NULL);
	mainbox = gtk_hbox_new (FALSE, 0);

	button = gtk_button_new ();
	hbox = gtk_hbox_new (FALSE, 0);
	img = gtk_image_new_from_icon_name (
		"stock_show-all", GTK_ICON_SIZE_BUTTON);
	label = gtk_label_new_with_mnemonic(_("View _Unformatted"));
	g_object_set_data (G_OBJECT (button), "text-label", (gpointer)label);
	gtk_box_pack_start (GTK_BOX (hbox), img, TRUE, TRUE, 2);
	gtk_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 2);
	gtk_widget_show_all (hbox);
	gtk_container_add (GTK_CONTAINER (button), GTK_WIDGET (hbox));
	if (info->handle)
		g_signal_connect (
			button, "clicked",
			G_CALLBACK (efhd_optional_button_show), scroll);
	else {
		gtk_widget_set_sensitive (button, FALSE);
		gtk_widget_set_can_focus (button, FALSE);
	}

	vbox = gtk_vbox_new (FALSE, 0);
	gtk_box_pack_start (GTK_BOX (mainbox), button, FALSE, FALSE, 6);

	button = gtk_button_new ();
	hbox = gtk_hbox_new (FALSE, 0);
	img = gtk_image_new_from_stock (
		GTK_STOCK_OPEN, GTK_ICON_SIZE_BUTTON);
	label = gtk_label_new_with_mnemonic(_("O_pen With"));
	gtk_box_pack_start (GTK_BOX (hbox), img, TRUE, TRUE, 2);
	gtk_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 2);
	gtk_box_pack_start (GTK_BOX (hbox), gtk_arrow_new (GTK_ARROW_DOWN, GTK_SHADOW_NONE), TRUE, TRUE, 2);
	gtk_widget_show_all (hbox);
	gtk_container_add (GTK_CONTAINER (button), GTK_WIDGET (hbox));

	a11y = gtk_widget_get_accessible (button);
	atk_object_set_name (a11y, _("Attachment"));

	gtk_box_pack_start (GTK_BOX (mainbox), button, FALSE, FALSE, 6);

	gtk_widget_show_all (mainbox);

	gtk_box_pack_start (GTK_BOX (vbox), mainbox, FALSE, FALSE, 6);

	view = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), FALSE);
	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
	byte_array = camel_stream_mem_get_byte_array (info->mstream);
	gtk_text_buffer_set_text (
		buffer, (gchar *) byte_array->data, byte_array->len);
	g_object_unref (info->mstream);
	info->mstream = NULL;
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
					GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll), GTK_SHADOW_IN);
	gtk_container_add (GTK_CONTAINER (scroll), GTK_WIDGET (view));
	gtk_box_pack_start (GTK_BOX (vbox), scroll, TRUE, TRUE, 6);
	gtk_widget_show (GTK_WIDGET (view));

	/* FIXME WEBKIT hmm, what to do?
	web_view = em_format_html_get_web_view (efh);
	gtk_widget_get_allocation (GTK_WIDGET (web_view), &allocation);
	gtk_widget_set_size_request (scroll, allocation.width - 48, 250);
	g_signal_connect (scroll, "size_allocate", G_CALLBACK(efhd_resize), efh);
	gtk_widget_show (scroll);
	*/

	if (!info->shown)
		gtk_widget_hide (scroll);

	gtk_widget_show (vbox);
	info->handle = NULL;

	return view;
}

static void
efhd_free_attach_puri_data (EMFormatPURI *puri)
{
	EMFormatAttachmentPURI *info = (EMFormatAttachmentPURI *) puri;

	g_return_if_fail (puri != NULL);

	if (info->attachment) {
		g_object_unref (info->attachment);
		info->attachment = NULL;
	}

	g_free (info->attachment_view_part_id);
	info->attachment_view_part_id = NULL;
}

/* returned object owned by html_display, thus do not unref it */
EAttachmentView *
em_format_html_display_get_attachment_view (EMFormatHTMLDisplay *efhd)
{
	g_return_val_if_fail (EM_IS_FORMAT_HTML_DISPLAY (efhd), NULL);

	return E_ATTACHMENT_VIEW (efhd->priv->attachment_view);
}

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
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

/*
  Concrete class for formatting mails to displayed html
*/

#ifndef EM_FORMAT_HTML_DISPLAY_H
#define EM_FORMAT_HTML_DISPLAY_H

#include <mail/em-format-html.h>
#include <misc/e-attachment-view.h>

/* Standard GObject macros */
#define EM_TYPE_FORMAT_HTML_DISPLAY \
	(em_format_html_display_get_type ())
#define EM_FORMAT_HTML_DISPLAY(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), EM_TYPE_FORMAT_HTML_DISPLAY, EMFormatHTMLDisplay))
#define EM_FORMAT_HTML_DISPLAY_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), EM_TYPE_FORMAT_HTML_DISPLAY, EMFormatHTMLDisplayClass))
#define EM_IS_FORMAT_HTML_DISPLAY(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), EM_TYPE_FORMAT_HTML_DISPLAY))
#define EM_IS_FORMAT_HTML_DISPLAY_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), EM_TYPE_FORMAT_HTML_DISPLAY))
#define EM_FORMAT_HTML_DISPLAY_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), EM_TYPE_FORMAT_HTML_DISPLAY, EMFormatHTMLDisplayClass))

G_BEGIN_DECLS

typedef struct _EMFormatHTMLDisplay EMFormatHTMLDisplay;
typedef struct _EMFormatHTMLDisplayClass EMFormatHTMLDisplayClass;
typedef struct _EMFormatHTMLDisplayPrivate EMFormatHTMLDisplayPrivate;
typedef struct _EMFormatAttachmentBarPURI EMFormatAttachmentBarPURI;
typedef struct _EMFormatAttachmentPURI EMFormatAttachmentPURI;
typedef struct _EMFormatSMIMEPURI EMFormatSMIMEPURI;

struct _EMFormatAttachmentBarPURI {
	EMFormatPURI puri;

	EAttachmentStore *store;
};

struct _EMFormatAttachmentPURI {
	EMFormatPURI puri;

	const EMFormatHandler *handle;

	const gchar *snoop_mime_type;

	/* for the > and V buttons */
	GtkWidget *forward, *down;
	guint shown:1;

	/* Attachment */
	EAttachment *attachment;
	gchar *attachment_view_part_id;
	gchar *description;

	/* image stuff */
	gint fit_width;
	gint fit_height;
	GtkImage *image;
	GtkWidget *event_box;

	/* Optional Text Mem Stream */
	CamelStreamMem *mstream;

	/* Signed / Encrypted */
	camel_cipher_validity_sign_t sign;
	camel_cipher_validity_encrypt_t encrypt;
};

struct _EMFormatSMIMEPURI {
	EMFormatPURI puri;

	gchar *description;

	gint signature;
	CamelCipherValidity *valid;
	GtkWidget *widget;
};


struct _EMFormatHTMLDisplay {
	EMFormatHTML parent;
	EMFormatHTMLDisplayPrivate *priv;

	struct _ESearchingTokenizer *search_tok;
};

struct _EMFormatHTMLDisplayClass {
	EMFormatHTMLClass parent_class;
};

GType		em_format_html_display_get_type	(void);
EMFormatHTMLDisplay *
		em_format_html_display_new	(void);
G_END_DECLS

#endif /* EM_FORMAT_HTML_DISPLAY_H */

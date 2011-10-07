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
 *		Michael Zucchi <notzed@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef EM_FORMAT_H
#define EM_FORMAT_H

#include <camel/camel.h>
#include <gtk/gtk.h>

/* Standard GObject macros */
#define EM_TYPE_FORMAT \
	(em_format_get_type ())
#define EM_FORMAT(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), EM_TYPE_FORMAT, EMFormat))
#define EM_FORMAT_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), EM_TYPE_FORMAT, EMFormatClass))
#define EM_IS_FORMAT(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), EM_TYPE_FORMAT))
#define EM_IS_FORMAT_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), EM_TYPE_FORMAT))
#define EM_FORMAT_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), EM_TYPE_FORMAT, EMFormatClass))

G_BEGIN_DECLS

#define EM_FORMAT_HEADER_BOLD (1<<0)
#define EM_FORMAT_HEADER_LAST (1<<4) /* reserve 4 slots */

#define EM_FORMAT_VALIDITY_FOUND_PGP		(1<<0)
#define EM_FORMAT_VALIDITY_FOUND_SMIME		(1<<1)
#define EM_FORMAT_VALIDITY_FOUND_SIGNED		(1<<2)
#define EM_FORMAT_VALIDITY_FOUND_ENCRYPTED	(1<<3)

typedef struct _EMFormat EMFormat;
typedef struct _EMFormatClass EMFormatClass;
typedef struct _EMFormatPrivate EMFormatPrivate;

typedef struct _EMFormatPURI EMFormatPURI;
typedef struct _EMFormatHeader EMFormatHeader;
typedef struct _EMFormatHandler EMFormatHandler;
typedef struct _EMFormatParserInfo EMFormatParserInfo;

typedef void		(*EMFormatParseFunc)	(EMFormat *emf,
					 	 CamelMimePart *part,
					 	 GString *part_id,
					 	 EMFormatParserInfo *info,
					 	 GCancellable *cancellable);
typedef void		(*EMFormatWriteFunc)	(EMFormat *emf,
					 	 EMFormatPURI *puri,
					 	 CamelStream *stream,
					 	 GCancellable *cancellable);
typedef GtkWidget*	(*EMFormatWidgetFunc)	(EMFormat *emf,
					 	 EMFormatPURI *puri,
					 	 GCancellable *cancellable);



typedef struct _EMFormatHandler EMFormatHandler;

typedef enum {
	EM_FORMAT_HANDLER_INLINE = 1 << 0,
	EM_FORMAT_HANDLER_INLINE_DISPOSITION = 1 << 1
} EMFormatHandlerFlags;

struct _EMFormatHandler {
	gchar *mime_type;
	EMFormatParseFunc parse_func;
	EMFormatWriteFunc write_func;
	EMFormatHandlerFlags flags;

	EMFormatHandler *old;
};

/**
 * Use this struct to pass additional information between
 * EMFormatParseFunc's.
 * Much cleaner then setting public property of EMFormat.
 */
struct _EMFormatParserInfo {
	const EMFormatHandler *handler;

	/* EM_FORMAT_VALIDITY_* flags */
	guint32 validity_type;
	CamelCipherValidity *validity;
};

struct _EMFormatHeader {
	guint32 flags;		/* E_FORMAT_HEADER_ * */
	gchar name[1];
};

#define EM_FORMAT_HEADER_BOLD (1<<0)
#define EM_FORMAT_HEADER_LAST (1<<4) /* reserve 4 slots */


struct _EMFormatPURI {
	CamelMimePart *part;

	EMFormat *emf;
	EMFormatWriteFunc write_func;
	EMFormatWidgetFunc widget_func;

	gchar *uri;
	gchar *cid;
	gchar *mime_type;

	/* EM_FORMAT_VALIDITY_* flags */
	guint32 validity_type;
	CamelCipherValidity *validity;
	CamelCipherValidity *validity_parent;

	gboolean is_attachment;

	void (*free)(EMFormatPURI *puri); /* optional callback for freeing user-fields */
};

struct _EMFormat {
	GObject parent;
	EMFormatPrivate *priv;

	CamelMimeMessage *message;
	CamelFolder *folder;
	gchar *message_uid;

	/* Defines order in which parts should be displayed */
	GList *mail_part_list;
	/* For quick search for parts by their URI/ID */
	GHashTable *mail_part_table;

	/* If empty, then all. */
	GQueue header_list;
};

struct _EMFormatClass {
	GObjectClass parent_class;

	GHashTable *type_handlers;

	void 		(*format_error) 		(EMFormat *emf,
							 const gchar *message);
	void		(*parse)			(EMFormat *emf,
							 CamelMimeMessage *message,
							 CamelFolder *folder,
							 GCancellable *cancellable);
	gboolean 	(*is_inline)			(EMFormat *emf,
							 const gchar *part_id,
							 CamelMimePart *part,
							 const EMFormatHandler *handler);
	void		(*format_attachment)		(EMFormat *emf,
							 EMFormatPURI *puri,
							 GCancellable *cancellable);
	void		(*format_optional)		(EMFormat *emf,
							 EMFormatPURI *puri,
							 CamelStream *mstream,
							 GCancellable *cancellable);
	void		(*format_secure)		(EMFormat *emf,
						 	 EMFormatPURI *puri,
							 GCancellable *cancellable);
	void		(*format_source)		(EMFormat *emf,
				 			 CamelStream *stream,
							 GCancellable *cancellable);
};

EMFormat*		em_format_new 			(void);

GType			em_format_get_type 		(void);

void			em_format_set_charset		(EMFormat *emf,
							 const gchar *charset);
const gchar*		em_format_get_charset		(EMFormat *emf);

void			em_format_set_default_charset	(EMFormat *emf,
						 	 const gchar *charset);
const gchar*		em_format_get_default_charset	(EMFormat *emf);

void			em_format_set_composer 		(EMFormat *emf,
			 				 gboolean composer);
gboolean		em_format_get_composer		(EMFormat *emf);

void			em_format_set_base_url		(EMFormat *emf,
			 				 CamelURL *url);
void			em_format_set_base_url_string	(EMFormat *emf,
							 const gchar *url_string);
CamelURL*		em_format_get_base_url		(EMFormat *emf);

void			em_format_clear_headers		(EMFormat *emf);

void			em_format_add_header 		(EMFormat *emf,
							 const gchar *name,
							 guint32 flags);
void			em_format_add_puri		(EMFormat *emf,
							 EMFormatPURI *puri);
EMFormatPURI*		em_format_find_puri		(EMFormat *emf,
							 const gchar *id);

void			em_format_class_add_handler	(EMFormatClass *emfc,
							 EMFormatHandler *handler);
void			em_format_class_remove_handler 	(EMFormatClass *emfc,
							 EMFormatHandler *handler);

const EMFormatHandler*	em_format_find_handler 		(EMFormat *emf,
							 const gchar *mime_type);
const EMFormatHandler* 	em_format_fallback_handler	(EMFormat *emf,
		                            		 const gchar *mime_type);

void			em_format_parse			(EMFormat *emf,
							 CamelMimeMessage *message,
							 CamelFolder *folder,
							 GCancellable *cancellable);
void 			em_format_parse_part		(EMFormat *emf,
							 CamelMimePart *part,
							 GString *part_id,
							 EMFormatParserInfo *info,
							 GCancellable *cancellable);
void 			em_format_parse_part_as		(EMFormat *emf,
							 CamelMimePart *part,
							 GString *part_id,
							 EMFormatParserInfo *info,
							 const gchar *mime_type,
							 GCancellable *cancellable);
gboolean		em_format_is_inline		(EMFormat *emf,
							 const gchar *part_id,
							 CamelMimePart *part,
							 const EMFormatHandler *handler);

gchar*			em_format_get_error_id		(EMFormat *emf);

void			em_format_format_error		(EMFormat *emf,
							 const gchar *format,
							 ...) G_GNUC_PRINTF (2, 3);
void			em_format_format_text		(EMFormat *emf,
							 CamelStream *stream,
							 CamelDataWrapper *dw,
							 GCancellable *cancellable);
void			em_format_format_source		(EMFormat *emf,
							 CamelStream *stream,
							 GCancellable *cancellable);
gchar*			em_format_describe_part		(CamelMimePart *part,
							 const gchar *mime_type);
gint			em_format_is_attachment 	(EMFormat *emf,
                	         	 	 	 CamelMimePart *part);
const gchar*		em_format_snoop_type	 	(CamelMimePart *part);

gchar *			em_format_build_mail_uri	(CamelFolder *folder,
							 const gchar *message_uid,
							 const gchar *part_uid);

/* EMFormatParseFunc that does nothing. Use it to disable
 * parsing of a specific mime type parts  */
void			em_format_empty_parser 		(EMFormat *emf,
							 CamelMimePart *part,
							 GString *part_id,
							 EMFormatParserInfo *info,
							 GCancellable *cancellable);

/* EMFormatWriteFunc that does nothing. Use it to disable
 * writing of a specific mime type parts */
void			em_format_empty_writer 		(EMFormat *emf,
							 EMFormatPURI *puri,
							 CamelStream *stream,
							 GCancellable *cancellable);


EMFormatPURI*		em_format_puri_new 		(EMFormat *emf,
							 gsize puri_size,
							 CamelMimePart *part,
							 const gchar *uri);
void			em_format_puri_free 		(EMFormatPURI *puri);

void			em_format_puri_write 		(EMFormatPURI *puri,
							 CamelStream *stream,
							 GCancellable *cancellable);

#endif /* EM_FORMAT_H */

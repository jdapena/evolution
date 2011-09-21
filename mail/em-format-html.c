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

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

#include <gtk/gtk.h>
#ifdef G_OS_WIN32
/* Work around 'DATADIR' and 'interface' lossage in <windows.h> */
#define DATADIR crap_DATADIR
#include <windows.h>
#undef DATADIR
#undef interface
#endif

#include <libebackend/e-extensible.h>
#include <libedataserver/e-time-utils.h>
#include <libedataserver/e-data-server-util.h>	/* for e_utf8_strftime, what about e_time_format_time? */

#include "e-util/e-datetime-format.h"
#include "e-util/e-icon-factory.h"
#include "e-util/e-util-private.h"
#include "e-util/e-util.h"
#include "misc/e-web-view.h"

#include <shell/e-shell.h>

#include <glib/gi18n.h>

#include <JavaScriptCore/JavaScript.h>

#include "e-mail-enumtypes.h"
#include "em-format-html.h"
#include "em-utils.h"
#include "mail-config.h"
#include <em-format/em-inline-filter.h>

#define d(x)

struct _EMFormatHTMLPrivate {
	GdkColor colors[EM_FORMAT_HTML_NUM_COLOR_TYPES];
	EMailImageLoadingPolicy image_loading_policy;

	EMFormatHTMLHeadersState headers_state;
	gboolean headers_collapsable;

	guint load_images_now	: 1;
	guint only_local_photos	: 1;
	guint show_sender_photo	: 1;
	guint show_real_date	: 1;
};

static gpointer parent_class;

enum {
	PROP_0,
	PROP_BODY_COLOR,
	PROP_CITATION_COLOR,
	PROP_CONTENT_COLOR,
	PROP_FRAME_COLOR,
	PROP_HEADER_COLOR,
	PROP_IMAGE_LOADING_POLICY,
	PROP_MARK_CITATIONS,
	PROP_ONLY_LOCAL_PHOTOS,
	PROP_SHOW_SENDER_PHOTO,
	PROP_SHOW_REAL_DATE,
	PROP_TEXT_COLOR,
	PROP_HEADERS_STATE,
	PROP_HEADERS_COLLAPSABLE
};

#define EFM_MESSAGE_START_ANAME "evolution_message_start"
#define EFH_MESSAGE_START "<A name=\"" EFM_MESSAGE_START_ANAME "\"></A>"
#define EFH_HTML_HEADER "<!DOCTYPE HTML>\n<html>\n"  \
		"<head>\n<meta name=\"generator\" content=\"Evolution Mail Component\" />\n" \
		"<title>Evolution Mail Display</title>\n" \
		"<link type=\"text/css\" rel=\"stylesheet\" href=\"evo-file://" EVOLUTION_PRIVDATADIR "/theme/webview.css\" />\n" \
		"<style type=\"text/css\">\n" \
		"  table th { color: #000; font-weight: bold; }\n" \
		"</style>\n" \
		"<script type=\"text/javascript\">\n" \
		"function body_loaded() { window.location.hash='" EFM_MESSAGE_START_ANAME "'; }\n" \
		"function collapse_addresses(field) {\n" \
		"  var e=window.document.getElementById(\"moreaddr-\"+field).style;\n" \
		"  var f=window.document.getElementById(\"moreaddr-ellipsis-\"+field).style;\n" \
		"  var g=window.document.getElementById(\"moreaddr-img-\"+field);\n" \
		"  if (e.display==\"inline\") { e.display=\"none\"; f.display=\"inline\"; g.src=g.src.substr(0,g.src.lastIndexOf(\"/\"))+\"/plus.png\"; }\n" \
		"  else { e.display=\"inline\"; f.display=\"none\"; g.src=g.src.substr(0,g.src.lastIndexOf(\"/\"))+\"/minus.png\"; }\n" \
		"}\n" \
		"function collapse_headers() {\n" \
		"  var f=window.document.getElementById(\"full-headers\").style;\n" \
		"  var s=window.document.getElementById(\"short-headers\").style;\n" \
		"  var i=window.document.getElementById(\"collapse-headers-img\");\n" \
		"  if (f.display==\"block\") { f.display=\"none\"; s.display=\"block\";\n" \
		"	i.src=i.src.substr(0,i.src.lastIndexOf(\"/\"))+\"/plus.png\"; window.headers_collapsed(true, window.em_format_html); }\n" \
		"  else { f.display=\"block\"; s.display=\"none\";\n" \
		"	 i.src=i.src.substr(0,i.src.lastIndexOf(\"/\"))+\"/minus.png\"; window.headers_collapsed(false, window.em_format_html); }\n" \
		"}\n" \
		"</script>\n" \
		"</head>\n" \
		"<body style=\"background: #%06x; color: #%06x;\" onLoad=\"body_loaded();\">"
#define EFH_HTML_FOOTER "</body></html>"

static void efh_parse_image			(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void efh_parse_text_enriched		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void efh_parse_text_plain		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void efh_parse_text_html			(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void efh_parse_message_external		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void efh_parse_message_deliverystatus	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void efh_parse_message_rfc822		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);

static void efh_write_image			(EMFormat *emf, EMFormatPURI *puri, CamelStream *stream, GCancellable *cancellable);
static void efh_write_text_enriched		(EMFormat *emf, EMFormatPURI *puri, CamelStream *stream, GCancellable *cancellable);
static void efh_write_text_plain		(EMFormat *emf, EMFormatPURI *puri, CamelStream *stream, GCancellable *cancellable);
static void efh_write_text_html			(EMFormat *emf, EMFormatPURI *puri, CamelStream *stream, GCancellable *cancellable);
static void efh_write_source			(EMFormat *emf, EMFormatPURI *puri, CamelStream *stream, GCancellable *cancellable);
static void efh_write_message_rfc822		(EMFormat *emf, EMFormatPURI *puri, CamelStream *stream, GCancellable *cancellable);
static void efh_write_headers			(EMFormat *emf, EMFormatPURI *puri, CamelStream *stream, GCancellable *cancellable);
/*****************************************************************************/
static void
efh_parse_image (EMFormat *emf,
		 CamelMimePart *part,
		 GString *part_id,
		 EMFormatParserInfo *info,
		 GCancellable *cancellable)
{
	EMFormatPURI *puri;
	const gchar *tmp;
	gchar *cid;
	gint len;

	if (g_cancellable_is_cancelled (cancellable))
		return;


	tmp = camel_mime_part_get_content_id (part);
	if (!tmp) {
		em_format_parse_part_as (emf, part, part_id, info, "x-evolution/message-attachment", cancellable);
		return;
	}

	cid = g_strdup_printf ("cid:%s", tmp);
	len = part_id->len;
	g_string_append (part_id, ".image");
	puri = em_format_puri_new (emf, sizeof (EMFormatPURI), part, part_id->str);
	puri->cid = cid;
	puri->write_func = efh_write_image;
	puri->mime_type = g_strdup (info->handler->mime_type);
	puri->is_attachment = TRUE;

	em_format_add_puri (emf, puri);
	g_string_truncate (part_id, len);
}

static void
efh_parse_text_enriched (EMFormat *emf,
			 CamelMimePart *part,
			 GString *part_id,
			 EMFormatParserInfo *info,
			 GCancellable *cancellable)
{
	EMFormatPURI *puri;
	const gchar *tmp;
	gchar *cid;
	gint len;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	tmp = camel_mime_part_get_content_id (part);
	if (!tmp) {
		cid = g_strdup_printf ("em-no-cid:%s", part_id->str);
	} else {
		cid = g_strdup_printf ("cid:%s", tmp);
	}

	len = part_id->len;
	g_string_append (part_id, ".text_enriched");
	puri = em_format_puri_new (emf, sizeof (EMFormatPURI), part, part_id->str);
	puri->cid = cid;
	puri->mime_type = g_strdup (info->handler->mime_type);
	puri->write_func = efh_write_text_enriched;

	em_format_add_puri (emf, puri);
	g_string_truncate (part_id, len);
}

static void
efh_parse_text_plain (EMFormat *emf,
		      CamelMimePart *part,
		      GString *part_id,
		      EMFormatParserInfo *info,
		      GCancellable *cancellable)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (emf);
	EMFormatPURI *puri;
	CamelStream *filtered_stream;
	CamelMultipart *mp;
	CamelDataWrapper *dw;
	CamelContentType *type;
	gint i, count, len;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	dw = camel_medium_get_content ((CamelMedium *) part);
	if (!dw)
		return;

	/* This scans the text part for inline-encoded data, creates
	   a multipart of all the parts inside it. */

	/* FIXME: We should discard this multipart if it only contains
	   the original text, but it makes this hash lookup more complex */

	/* TODO: We could probably put this in the superclass, since
	   no knowledge of html is required - but this messes with
	   filters a bit.  Perhaps the superclass should just deal with
	   html anyway and be done with it ... */

	/* FIXME WEBKIT no cache
	efhc = g_hash_table_lookup (
		efh->priv->text_inline_parts,
		emf->part_id->str);

	if (efhc == NULL || (mp = efhc->textmp) == NULL) {
	*/
	if (TRUE) {
		EMInlineFilter *inline_filter;
		CamelStream *null;
		CamelContentType *ct;
		gboolean charset_added = FALSE;
		const gchar *snoop_type;

		snoop_type = em_format_snoop_type (part);

		/* if we had to snoop the part type to get here, then
		 * use that as the base type, yuck */
		if (snoop_type == NULL
		    || (ct = camel_content_type_decode (snoop_type)) == NULL) {
			ct = dw->mime_type;
			camel_content_type_ref (ct);
		}

		if (dw->mime_type && ct != dw->mime_type && camel_content_type_param (dw->mime_type, "charset")) {
			camel_content_type_set_param (ct, "charset", camel_content_type_param (dw->mime_type, "charset"));
			charset_added = TRUE;
		}

		null = camel_stream_null_new ();
		filtered_stream = camel_stream_filter_new (null);
		g_object_unref (null);
		inline_filter = em_inline_filter_new (camel_mime_part_get_encoding (part), ct);
		camel_stream_filter_add (
			CAMEL_STREAM_FILTER (filtered_stream),
			CAMEL_MIME_FILTER (inline_filter));
		camel_data_wrapper_decode_to_stream_sync (
			dw, (CamelStream *) filtered_stream, cancellable, NULL);
		camel_stream_close ((CamelStream *) filtered_stream, cancellable, NULL);
		g_object_unref (filtered_stream);

		mp = em_inline_filter_get_multipart (inline_filter);
		/* FIXME WEBKIT
		if (efhc == NULL)
			efhc = efh_insert_cache (efh, emf->part_id->str);
		efhc->textmp = mp;
		*/

		if (charset_added) {
			camel_content_type_set_param (ct, "charset", NULL);
		}

		g_object_unref (inline_filter);
		camel_content_type_unref (ct);
	}

	/* We handle our made-up multipart here, so we don't recursively call ourselves */
	len = part_id->len;
	count = camel_multipart_get_number (mp);
	for (i=0;i<count;i++) {
		CamelMimePart *newpart = camel_multipart_get_part (mp, i);

		if (!newpart)
			continue;

		type = camel_mime_part_get_content_type (newpart);
		if (camel_content_type_is (type, "text", "*") && (!camel_content_type_is (type, "text", "calendar"))) {
			gint s_len = part_id->len;

			g_string_append (part_id, ".plain_text");
			puri = em_format_puri_new (emf, sizeof (EMFormatPURI), newpart, part_id->str);
			puri->write_func = efh_write_text_plain;
			puri->mime_type = g_strdup ("text/html");
			g_string_truncate (part_id, s_len);
			em_format_add_puri (emf, puri);
		} else {
			g_string_append_printf (part_id, ".inline.%d", i);
			em_format_parse_part (emf, CAMEL_MIME_PART (newpart), part_id, info, cancellable);
			g_string_truncate (part_id, len);
		}
	}

	g_object_unref (mp);
}

static void
efh_parse_text_html (EMFormat *emf,
		     CamelMimePart *part,
		     GString *part_id,
		     EMFormatParserInfo *info,
		     GCancellable *cancellable)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (emf);
	EMFormatPURI *puri;
	const gchar *location;
	gchar *cid = NULL;
	CamelURL *base;
	gint len;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	base = em_format_get_base_url (emf);
	location = camel_mime_part_get_content_location (part);
	if (location == NULL) {
		if (base)
			cid = camel_url_to_string (base, 0);
		else
			cid = g_strdup (part_id->str);
	} else {
		if (strchr (location, ':') == NULL && base != NULL) {
			CamelURL *uri;

			uri = camel_url_new_with_base (base, location);
			cid = camel_url_to_string (uri, 0);
			camel_url_free (uri);
		} else {
			cid = g_strdup (location);
		}
	}

	len = part_id->len;
	g_string_append (part_id, ".text_html");
	puri = em_format_puri_new (emf, sizeof (EMFormatPURI), part, part_id->str);
	puri->write_func = efh_write_text_html;

	em_format_add_puri (emf, puri);
	g_string_truncate (part_id, len);
}

static void
efh_parse_message_external (EMFormat *emf,
			    CamelMimePart *part,
			    GString *part_id,
			    EMFormatParserInfo *info,
			    GCancellable *cancellable)
{
	EMFormatPURI *puri;
	CamelMimePart *newpart;
	CamelContentType *type;
	const gchar *access_type;
	gchar *url = NULL, *desc = NULL;
	gchar *content;
	gint len;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	newpart = camel_mime_part_new ();

	/* needs to be cleaner */
	type = camel_mime_part_get_content_type (part);
	access_type = camel_content_type_param (type, "access-type");
	if (!access_type) {
		const gchar *msg = _("Malformed external-body part");
		camel_mime_part_set_content (newpart, msg, strlen (msg),
				"text/plain");
		goto addPart;
	}

	if (!g_ascii_strcasecmp(access_type, "ftp") ||
	    !g_ascii_strcasecmp(access_type, "anon-ftp")) {
		const gchar *name, *site, *dir, *mode;
		gchar *path;
		gchar ftype[16];

		name = camel_content_type_param (type, "name");
		site = camel_content_type_param (type, "site");
		dir = camel_content_type_param (type, "directory");
		mode = camel_content_type_param (type, "mode");
		if (name == NULL || site == NULL)
			goto fail;

		/* Generate the path. */
		if (dir)
			path = g_strdup_printf("/%s/%s", *dir=='/'?dir+1:dir, name);
		else
			path = g_strdup_printf("/%s", *name=='/'?name+1:name);

		if (mode && *mode)
			sprintf(ftype, ";type=%c",  *mode);
		else
			ftype[0] = 0;

		url = g_strdup_printf ("ftp://%s%s%s", site, path, ftype);
		g_free (path);
		desc = g_strdup_printf (_("Pointer to FTP site (%s)"), url);
	} else if (!g_ascii_strcasecmp (access_type, "local-file")) {
		const gchar *name, *site;

		name = camel_content_type_param (type, "name");
		site = camel_content_type_param (type, "site");
		if (name == NULL)
			goto fail;

		url = g_filename_to_uri (name, NULL, NULL);
		if (site)
			desc = g_strdup_printf(_("Pointer to local file (%s) valid at site \"%s\""), name, site);
		else
			desc = g_strdup_printf(_("Pointer to local file (%s)"), name);
	} else if (!g_ascii_strcasecmp (access_type, "URL")) {
		const gchar *urlparam;
		gchar *s, *d;

		/* RFC 2017 */
		urlparam = camel_content_type_param (type, "url");
		if (urlparam == NULL)
			goto fail;

		/* For obscure MIMEy reasons, the URL may be split into words */
		url = g_strdup (urlparam);
		s = d = url;
		while (*s) {
			if (!isspace ((guchar)*s))
				*d++ = *s;
			s++;
		}
		*d = 0;
		desc = g_strdup_printf (_("Pointer to remote data (%s)"), url);
	} else
		goto fail;

	content = g_strdup_printf ("<a href=\"%s\">%s</a>", url, desc);
	camel_mime_part_set_content (newpart, content, strlen (content), "text/html");
	g_free (content);

	g_free (url);
	g_free (desc);

fail:
	content = g_strdup_printf (
		_("Pointer to unknown external data (\"%s\" type)"),
		access_type);
	camel_mime_part_set_content (newpart, content, strlen (content), "text/plain");
	g_free (content);

addPart:
	len = part_id->len;
	g_string_append (part_id, ".msg_external");
	puri = em_format_puri_new (emf, sizeof (EMFormatPURI), part, part_id->str);
	puri->write_func = efh_write_text_html;
	puri->mime_type = g_strdup ("text/html");

	em_format_add_puri (emf, puri);
	g_string_truncate (part_id, len);
}

static void
efh_parse_message_deliverystatus (EMFormat *emf,
				  CamelMimePart *part,
				  GString *part_id,
				  EMFormatParserInfo *info,
				  GCancellable *cancellable)
{
	EMFormatPURI *puri;
	gint len;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	len = part_id->len;
	g_string_append (part_id, ".deliverystatus");
	puri = em_format_puri_new (emf, sizeof (EMFormatPURI), part, part_id->str);
	puri->write_func = efh_write_source;
	puri->mime_type = g_strdup ("text/html");

	em_format_add_puri (emf, puri);
	g_string_truncate (part_id, len);
}

static void
efh_parse_message_rfc822 (EMFormat *emf,
			  CamelMimePart *part,
			  GString *part_id,
			  EMFormatParserInfo *info,
			  GCancellable *cancellable)
{
	CamelDataWrapper *dw;
	CamelMimePart *opart;
	CamelStream *stream;
	CamelMimeParser *parser;
	CamelContentType *ct;
	gchar *cts;
	gint len;
	EMFormatParserInfo oinfo = *info;

	len = part_id->len;
	g_string_append (part_id, ".rfc822");

	stream = camel_stream_mem_new ();
	dw = camel_medium_get_content ((CamelMedium *) part);
	camel_data_wrapper_write_to_stream_sync (dw, stream, cancellable, NULL);
	g_seekable_seek (G_SEEKABLE (stream), 0, G_SEEK_SET, cancellable, NULL);

	parser = camel_mime_parser_new ();
	camel_mime_parser_init_with_stream (parser, stream, NULL);

	opart = camel_mime_part_new ();
	camel_mime_part_construct_from_parser_sync (opart, parser, cancellable, NULL);

	em_format_parse_part_as (emf, opart, part_id, &oinfo,
		"x-evolution/message", cancellable);

	g_string_truncate (part_id, len);

	g_object_unref (opart);
	g_object_unref (parser);
	g_object_unref (stream);
}


/*****************************************************************************/

static void
efh_write_image (EMFormat *emf,
		 EMFormatPURI *puri,
		 CamelStream *stream,
		 GCancellable *cancellable)
{
	GByteArray *ba;
	CamelDataWrapper *dw;
	gchar *content;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	/* FIXME: What about some error string when dw == NULL? */
	dw = camel_medium_get_content (CAMEL_MEDIUM (puri->part));
	if (dw) {
		CamelStream *stream_filter;
		CamelMimeFilter *filter;

		filter = camel_mime_filter_basic_new (CAMEL_MIME_FILTER_BASIC_BASE64_DEC);
		stream_filter = camel_stream_filter_new (stream);
		camel_stream_filter_add ((CamelStreamFilter *) stream_filter, filter);

		ba = camel_data_wrapper_get_byte_array (dw);
		content = g_strndup ((gchar *) ba->data, ba->len);
		camel_stream_write_string (stream_filter, content, cancellable, NULL);
		g_free (content);

		g_object_unref (filter);
		g_object_unref (stream_filter);
	}
}

static void
efh_write_text_enriched (EMFormat *emf,
			 EMFormatPURI *puri,
			 CamelStream *stream,
			 GCancellable *cancellable)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (emf);
	CamelStream *filtered_stream;
	CamelMimeFilter *enriched;
	guint32 flags = 0;
	gchar *content;
	CamelContentType *ct;
	gchar *mime_type = NULL;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	ct = camel_mime_part_get_content_type (puri->part);
	if (ct) {
		mime_type = camel_content_type_simple (ct);
	}

	if (!g_strcmp0(mime_type, "text/richtext")) {
		flags = CAMEL_MIME_FILTER_ENRICHED_IS_RICHTEXT;
		camel_stream_write_string (
			stream, "\n<!-- text/richtext -->\n",
			cancellable, NULL);
	} else {
		camel_stream_write_string (
			stream, "\n<!-- text/enriched -->\n",
			cancellable, NULL);
	}

	if (mime_type)
		g_free (mime_type);

	enriched = camel_mime_filter_enriched_new (flags);
	filtered_stream = camel_stream_filter_new (stream);
	camel_stream_filter_add (
		CAMEL_STREAM_FILTER (filtered_stream), enriched);
	g_object_unref (enriched);

	content = g_strdup_printf (
		"<div style=\"border: solid #%06x 1px; "
		"background-color: #%06x; padding: 10px; "
		"color: #%06x;\">\n" EFH_MESSAGE_START,
		e_color_to_value (
			&efh->priv->colors[
			EM_FORMAT_HTML_COLOR_FRAME]),
		e_color_to_value (
			&efh->priv->colors[
			EM_FORMAT_HTML_COLOR_CONTENT]),
		e_color_to_value (
			&efh->priv->colors[
			EM_FORMAT_HTML_COLOR_TEXT]));

	camel_stream_write_string (stream, content, cancellable, NULL);
	g_free (content);

	em_format_format_text (
		emf, (CamelStream *) filtered_stream,
		(CamelDataWrapper *) puri->part, cancellable);

	g_object_unref (filtered_stream);
	camel_stream_write_string (stream, "</div>", cancellable, NULL);
}

static void
efh_write_text_plain (EMFormat *emf,
		      EMFormatPURI *puri,
		      CamelStream *stream,
		      GCancellable *cancellable)
{
	CamelDataWrapper *dw;
	CamelStream *filtered_stream;
	CamelMimeFilter *html_filter;
	EMFormatHTML *efh = (EMFormatHTML*) emf;
	gchar *content;
	const gchar *format;
	guint32 flags;
	guint32 rgb;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	flags = efh->text_html_flags;

	dw = camel_medium_get_content (CAMEL_MEDIUM (puri->part));

	/* Check for RFC 2646 flowed text. */
	if (camel_content_type_is(dw->mime_type, "text", "plain")
	    && (format = camel_content_type_param(dw->mime_type, "format"))
	    && !g_ascii_strcasecmp(format, "flowed"))
		flags |= CAMEL_MIME_FILTER_TOHTML_FORMAT_FLOWED;

	rgb = e_color_to_value (
		&efh->priv->colors[EM_FORMAT_HTML_COLOR_CITATION]);
	filtered_stream = camel_stream_filter_new (stream);
	html_filter = camel_mime_filter_tohtml_new (flags, rgb);
	camel_stream_filter_add (
		CAMEL_STREAM_FILTER (filtered_stream), html_filter);
	g_object_unref (html_filter);

	content = g_strdup_printf (
			EFH_HTML_HEADER	"<div id=\"pre\" style=\"background: #%06x; padding: 10px;\">\n",
			e_color_to_value (&efh->priv->colors[EM_FORMAT_HTML_COLOR_BODY]),
			e_color_to_value (&efh->priv->colors[EM_FORMAT_HTML_COLOR_HEADER]),
			e_color_to_value (&efh->priv->colors[EM_FORMAT_HTML_COLOR_CONTENT]));

	camel_stream_write_string (stream, content, cancellable, NULL);
	em_format_format_text (emf, filtered_stream, (CamelDataWrapper *) puri->part, cancellable);

	g_object_unref (filtered_stream);
	g_free (content);

	camel_stream_write_string (stream, "</div>\n", cancellable, NULL);
	camel_stream_write_string (stream, EFH_HTML_FOOTER "\n", cancellable, NULL);
}

static void
efh_write_text_html (EMFormat *emf,
		     EMFormatPURI *puri,
		     CamelStream *stream,
		     GCancellable *cancellable)
{
	if (g_cancellable_is_cancelled (cancellable))
		return;

	em_format_format_text (
		emf, stream, (CamelDataWrapper *) puri->part, cancellable);
}

static void
efh_write_source (EMFormat *emf,
		  EMFormatPURI *puri,
		  CamelStream *stream,
		  GCancellable *cancellable)
{
	CamelStream *filtered_stream;
	CamelMimeFilter *filter;
	CamelDataWrapper *dw = (CamelDataWrapper *) puri->part;

	filtered_stream = camel_stream_filter_new (stream);

	filter = camel_mime_filter_tohtml_new (
		CAMEL_MIME_FILTER_TOHTML_CONVERT_NL |
		CAMEL_MIME_FILTER_TOHTML_CONVERT_SPACES |
		CAMEL_MIME_FILTER_TOHTML_PRESERVE_8BIT, 0);
	camel_stream_filter_add (
		CAMEL_STREAM_FILTER (filtered_stream), filter);
	g_object_unref (filter);

	camel_stream_write_string (
		stream, "<code class=\"pre\">", cancellable, NULL);
	em_format_format_text (emf, filtered_stream, dw, cancellable);
	camel_stream_write_string (
		stream, "</code>", cancellable, NULL);

	g_object_unref (filtered_stream);
}

static void
efh_write_headers (EMFormat *emf,
		   EMFormatPURI *puri,
		   CamelStream *stream,
		   GCancellable *cancellable)
{
	/* FIXME: We could handle this more nicely */
	em_format_html_format_headers ((EMFormatHTML *) emf, stream, (CamelMedium *)  puri->part, FALSE, cancellable);
}

/*****************************************************************************/

/* Notes:
 *
 * image/tiff is omitted because it's a multi-page image format, but
 * gdk-pixbuf unconditionally renders the first page only, and doesn't
 * even indicate through meta-data whether multiple pages are present
 * (see bug 335959).  Therefore, make no attempt to render TIFF images
 * inline and defer to an application that can handle multi-page TIFF
 * files properly like Evince or Gimp.  Once the referenced bug is
 * fixed we can reevaluate this policy.
 */
static EMFormatHandler type_builtin_table[] = {
	{ (gchar *) "image/gif", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/jpeg", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/png", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/x-png", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/x-bmp", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/bmp", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/svg", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/x-cmu-raster", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/x-ico", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/x-portable-anymap", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/x-portable-bitmap", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/x-portable-graymap", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/x-portable-pixmap", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/x-xpixmap", efh_parse_image, efh_write_image, },
	{ (gchar *) "text/enriched", efh_parse_text_enriched, efh_write_text_enriched, },
	{ (gchar *) "text/plain", efh_parse_text_plain, efh_write_text_plain, },
	{ (gchar *) "text/html", efh_parse_text_html, efh_write_text_html, },
	{ (gchar *) "text/richtext", efh_parse_text_enriched, efh_write_text_enriched, },
	{ (gchar *) "text/*", efh_parse_text_plain, efh_write_text_plain, },
        { (gchar *) "message/rfc822", efh_parse_message_rfc822, 0, EM_FORMAT_HANDLER_INLINE },
        { (gchar *) "message/news", efh_parse_message_rfc822, 0, EM_FORMAT_HANDLER_INLINE },
        { (gchar *) "message/delivery-status", efh_parse_message_deliverystatus, efh_write_text_plain, },
	{ (gchar *) "message/external-body", efh_parse_message_external, efh_write_text_plain, },
        { (gchar *) "message/*", efh_parse_message_rfc822, 0, EM_FORMAT_HANDLER_INLINE },

	/* This is where one adds those busted, non-registered types,
	   that some idiot mailer writers out there decide to pull out
	   of their proverbials at random. */
	{ (gchar *) "image/jpg", efh_parse_image, efh_write_image, },
	{ (gchar *) "image/pjpeg", efh_parse_image, efh_write_image, },

	/* special internal types */
	{ (gchar *) "x-evolution/message/rfc822", 0, efh_write_text_plain, },
	{ (gchar *) "x-evolution/message/headers", 0, efh_write_headers, },
};

static void
efh_builtin_init (EMFormatHTMLClass *efhc)
{
	EMFormatClass *emfc;
	gint ii;

	emfc = (EMFormatClass *) efhc;

	for (ii = 0; ii < G_N_ELEMENTS (type_builtin_table); ii++)
		em_format_class_add_handler (
			emfc, &type_builtin_table[ii]);
}

static void
efh_set_property (GObject *object,
                  guint property_id,
                  const GValue *value,
                  GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_BODY_COLOR:
			em_format_html_set_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_BODY,
				g_value_get_boxed (value));
			return;

		case PROP_CITATION_COLOR:
			em_format_html_set_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_CITATION,
				g_value_get_boxed (value));
			return;

		case PROP_CONTENT_COLOR:
			em_format_html_set_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_CONTENT,
				g_value_get_boxed (value));
			return;

		case PROP_FRAME_COLOR:
			em_format_html_set_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_FRAME,
				g_value_get_boxed (value));
			return;

		case PROP_HEADER_COLOR:
			em_format_html_set_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_HEADER,
				g_value_get_boxed (value));
			return;

		case PROP_IMAGE_LOADING_POLICY:
			em_format_html_set_image_loading_policy (
				EM_FORMAT_HTML (object),
				g_value_get_enum (value));
			return;

		case PROP_MARK_CITATIONS:
			em_format_html_set_mark_citations (
				EM_FORMAT_HTML (object),
				g_value_get_boolean (value));
			return;

		case PROP_ONLY_LOCAL_PHOTOS:
			em_format_html_set_only_local_photos (
				EM_FORMAT_HTML (object),
				g_value_get_boolean (value));
			return;

		case PROP_SHOW_SENDER_PHOTO:
			em_format_html_set_show_sender_photo (
				EM_FORMAT_HTML (object),
				g_value_get_boolean (value));
			return;

		case PROP_SHOW_REAL_DATE:
			em_format_html_set_show_real_date (
				EM_FORMAT_HTML (object),
				g_value_get_boolean (value));
			return;

		case PROP_TEXT_COLOR:
			em_format_html_set_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_TEXT,
				g_value_get_boxed (value));
			return;
		case PROP_HEADERS_STATE:
			em_format_html_set_headers_state (
				EM_FORMAT_HTML (object),
				g_value_get_int (value));
			return;
		case PROP_HEADERS_COLLAPSABLE:
			em_format_html_set_headers_collapsable (
				EM_FORMAT_HTML (object),
				g_value_get_boolean (value));
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
efh_get_property (GObject *object,
                  guint property_id,
                  GValue *value,
                  GParamSpec *pspec)
{
	GdkColor color;

	switch (property_id) {
		case PROP_BODY_COLOR:
			em_format_html_get_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_BODY,
				&color);
			g_value_set_boxed (value, &color);
			return;

		case PROP_CITATION_COLOR:
			em_format_html_get_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_CITATION,
				&color);
			g_value_set_boxed (value, &color);
			return;

		case PROP_CONTENT_COLOR:
			em_format_html_get_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_CONTENT,
				&color);
			g_value_set_boxed (value, &color);
			return;

		case PROP_FRAME_COLOR:
			em_format_html_get_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_FRAME,
				&color);
			g_value_set_boxed (value, &color);
			return;

		case PROP_HEADER_COLOR:
			em_format_html_get_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_HEADER,
				&color);
			g_value_set_boxed (value, &color);
			return;

		case PROP_IMAGE_LOADING_POLICY:
			g_value_set_enum (
				value,
				em_format_html_get_image_loading_policy (
				EM_FORMAT_HTML (object)));
			return;

		case PROP_MARK_CITATIONS:
			g_value_set_boolean (
				value, em_format_html_get_mark_citations (
				EM_FORMAT_HTML (object)));
			return;

		case PROP_ONLY_LOCAL_PHOTOS:
			g_value_set_boolean (
				value, em_format_html_get_only_local_photos (
				EM_FORMAT_HTML (object)));
			return;

		case PROP_SHOW_SENDER_PHOTO:
			g_value_set_boolean (
				value, em_format_html_get_show_sender_photo (
				EM_FORMAT_HTML (object)));
			return;

		case PROP_SHOW_REAL_DATE:
			g_value_set_boolean (
				value, em_format_html_get_show_real_date (
				EM_FORMAT_HTML (object)));
			return;

		case PROP_TEXT_COLOR:
			em_format_html_get_color (
				EM_FORMAT_HTML (object),
				EM_FORMAT_HTML_COLOR_TEXT,
				&color);
			g_value_set_boxed (value, &color);
			return;

		case PROP_HEADERS_STATE:
			g_value_set_int (
				value, em_format_html_get_headers_state (
				EM_FORMAT_HTML (object)));
			return;
		case PROP_HEADERS_COLLAPSABLE:
			g_value_set_boolean (
				value, em_format_html_get_headers_collapsable (
				EM_FORMAT_HTML (object)));
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
efh_finalize (GObject *object)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (object);
	EMFormatHTMLPrivate *priv = efh->priv;

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
efh_format_error (EMFormat *emf,
                  const gchar *message)
{
	EMFormatPURI *puri;
	CamelMimePart *part;
	GString *buffer;
	gchar *html;
	gchar *part_id;

	buffer = g_string_new ("<em><font color=\"red\">");

	html = camel_text_to_html (
		message, CAMEL_MIME_FILTER_TOHTML_CONVERT_NL |
		CAMEL_MIME_FILTER_TOHTML_CONVERT_URLS, 0);
	g_string_append (buffer, html);
	g_free (html);

	g_string_append (buffer, "</font></em><br>");

	part = camel_mime_part_new ();
	camel_mime_part_set_content (part, buffer->str, buffer->len, "text/html");

	puri = em_format_puri_new (emf, sizeof (EMFormatPURI), part, em_format_get_error_id (emf));
	puri->write_func = efh_write_text_html;
	puri->mime_type = g_strdup ("text/html");

	em_format_add_puri (emf, puri);

	g_string_free (buffer, TRUE);
}

static void
efh_format_source (EMFormat *emf,
		   CamelStream *stream,
		   CamelMimePart *part,
		   GCancellable *cancellable)
{
	CamelStream *filtered_stream;
	CamelMimeFilter *filter;
	CamelDataWrapper *dw = (CamelDataWrapper *) part;

	filtered_stream = camel_stream_filter_new (stream);

	filter = camel_mime_filter_tohtml_new (
		CAMEL_MIME_FILTER_TOHTML_CONVERT_NL |
		CAMEL_MIME_FILTER_TOHTML_CONVERT_SPACES |
		CAMEL_MIME_FILTER_TOHTML_PRESERVE_8BIT, 0);
	camel_stream_filter_add (
		CAMEL_STREAM_FILTER (filtered_stream), filter);
	g_object_unref (filter);

	camel_stream_write_string (
		stream, "<code class=\"pre\">", cancellable, NULL);
	em_format_format_text (emf, filtered_stream, dw, cancellable);
	camel_stream_write_string (
		stream, "</code>", cancellable, NULL);

	g_object_unref (filtered_stream);
}

/* FIXME WEBKIT: This should only create EMFormatPURI that will be then
 * processed to EAttachment (or whatever) widget!
 */
static void
efh_format_attachment (EMFormat *emf,
                       CamelStream *stream,
                       CamelMimePart *part,
                       const gchar *mime_type,
                       const EMFormatHandler *handle,
                       GCancellable *cancellable)
{
	gchar *text, *html;

	/* we display all inlined attachments only */

	/* this could probably be cleaned up ... */
	camel_stream_write_string (
		stream,
		"<table border=1 cellspacing=0 cellpadding=0><tr><td>"
		"<table width=10 cellspacing=0 cellpadding=0>"
		"<tr><td></td></tr></table></td>"
		"<td><table width=3 cellspacing=0 cellpadding=0>"
		"<tr><td></td></tr></table></td><td><font size=-1>\n",
		cancellable, NULL);

	/* output some info about it */
	text = em_format_describe_part (part, mime_type);
	html = camel_text_to_html (
		text, ((EMFormatHTML *) emf)->text_html_flags &
		CAMEL_MIME_FILTER_TOHTML_CONVERT_URLS, 0);
	camel_stream_write_string (stream, html, cancellable, NULL);
	g_free (html);
	g_free (text);

	camel_stream_write_string (
		stream, "</font></td></tr><tr></table>", cancellable, NULL);
/*
	if (handle && em_format_is_inline (emf, part_id->str, part, handle))
			handle->write_func (emf, part, stream, cancellable);
*/
}

static void
efh_base_init (EMFormatHTMLClass *class)
{
	efh_builtin_init (class);
}

static void
efh_class_init (EMFormatHTMLClass *class)
{
	GObjectClass *object_class;
	EMFormatClass *format_class;
	const gchar *user_cache_dir;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EMFormatHTMLPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = efh_set_property;
	object_class->get_property = efh_get_property;
	object_class->finalize = efh_finalize;

	format_class = EM_FORMAT_CLASS (class);
	format_class->format_error = efh_format_error;
	/*format_class->format_source = efh_format_source;
	format_class->format_attachment = efh_format_attachment;
	format_class->format_secure = efh_format_secure;
	*/
	g_object_class_install_property (
		object_class,
		PROP_BODY_COLOR,
		g_param_spec_boxed (
			"body-color",
			"Body Color",
			NULL,
			GDK_TYPE_COLOR,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_CITATION_COLOR,
		g_param_spec_boxed (
			"citation-color",
			"Citation Color",
			NULL,
			GDK_TYPE_COLOR,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_CONTENT_COLOR,
		g_param_spec_boxed (
			"content-color",
			"Content Color",
			NULL,
			GDK_TYPE_COLOR,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_FRAME_COLOR,
		g_param_spec_boxed (
			"frame-color",
			"Frame Color",
			NULL,
			GDK_TYPE_COLOR,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_HEADER_COLOR,
		g_param_spec_boxed (
			"header-color",
			"Header Color",
			NULL,
			GDK_TYPE_COLOR,
			G_PARAM_READWRITE));

	/* FIXME Make this a proper enum property. */
	g_object_class_install_property (
		object_class,
		PROP_IMAGE_LOADING_POLICY,
		g_param_spec_enum (
			"image-loading-policy",
			"Image Loading Policy",
			NULL,
			E_TYPE_MAIL_IMAGE_LOADING_POLICY,
			E_MAIL_IMAGE_LOADING_POLICY_ALWAYS,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_MARK_CITATIONS,
		g_param_spec_boolean (
			"mark-citations",
			"Mark Citations",
			NULL,
			TRUE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_ONLY_LOCAL_PHOTOS,
		g_param_spec_boolean (
			"only-local-photos",
			"Only Local Photos",
			NULL,
			TRUE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_SHOW_SENDER_PHOTO,
		g_param_spec_boolean (
			"show-sender-photo",
			"Show Sender Photo",
			NULL,
			FALSE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_SHOW_REAL_DATE,
		g_param_spec_boolean (
			"show-real-date",
			"Show real Date header value",
			NULL,
			TRUE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_TEXT_COLOR,
		g_param_spec_boxed (
			"text-color",
			"Text Color",
			NULL,
			GDK_TYPE_COLOR,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_HEADERS_STATE,
		g_param_spec_int (
			"headers-state",
			"Headers state",
			NULL,
			EM_FORMAT_HTML_HEADERS_STATE_EXPANDED,
			EM_FORMAT_HTML_HEADERS_STATE_COLLAPSED,
			EM_FORMAT_HTML_HEADERS_STATE_EXPANDED,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_HEADERS_COLLAPSABLE,
		g_param_spec_boolean (
			"headers-collapsable",
			NULL,
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	/* cache expiry - 2 hour access, 1 day max */

	/* FIXME WEBKIT - this emfh_http_cache is not used anywhere - remove?
	user_cache_dir = e_get_user_cache_dir ();
	emfh_http_cache = camel_data_cache_new (user_cache_dir, NULL);
	if (emfh_http_cache) {
		camel_data_cache_set_expire_age (emfh_http_cache, 24*60*60);
		camel_data_cache_set_expire_access (emfh_http_cache, 2*60*60);
	}
	*/
}

static void
efh_init (EMFormatHTML *efh,
	  EMFormatHTMLClass *class)
{
	GdkColor *color;

	efh->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		efh, EM_TYPE_FORMAT_HTML, EMFormatHTMLPrivate);

	/* FIXME WEBKIT
	efh->priv->text_inline_parts = g_hash_table_new_full (
		g_str_hash, g_str_equal,
		(GDestroyNotify) NULL,
		(GDestroyNotify) efh_free_cache);
	*/

	g_queue_init (&efh->pending_object_list);

	color = &efh->priv->colors[EM_FORMAT_HTML_COLOR_BODY];
	gdk_color_parse ("#eeeeee", color);

	color = &efh->priv->colors[EM_FORMAT_HTML_COLOR_CONTENT];
	gdk_color_parse ("#ffffff", color);

	color = &efh->priv->colors[EM_FORMAT_HTML_COLOR_FRAME];
	gdk_color_parse ("#3f3f3f", color);

	color = &efh->priv->colors[EM_FORMAT_HTML_COLOR_HEADER];
	gdk_color_parse ("#eeeeee", color);

	color = &efh->priv->colors[EM_FORMAT_HTML_COLOR_TEXT];
	gdk_color_parse ("#000000", color);

	efh->text_html_flags =
		CAMEL_MIME_FILTER_TOHTML_CONVERT_NL |
		CAMEL_MIME_FILTER_TOHTML_CONVERT_SPACES |
		CAMEL_MIME_FILTER_TOHTML_MARK_CITATION;
	efh->show_icon = TRUE;
	efh->state = EM_FORMAT_HTML_STATE_NONE;

	/* FIXME WEBKIT: emit signal? */
	/*
	g_signal_connect_swapped (
		efh, "notify::mark-citations",
		G_CALLBACK (e_mail_display_reload), efh->priv->mail_display);
	 */
	e_extensible_load_extensions (E_EXTENSIBLE (efh));
}

GType
em_format_html_get_type (void)
{
	static GType type = 0;

	if (G_UNLIKELY (type == 0)) {
		static const GTypeInfo type_info = {
			sizeof (EMFormatHTMLClass),
			(GBaseInitFunc) efh_base_init,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) efh_class_init,
			(GClassFinalizeFunc) NULL,
			NULL,  /* class_data */
			sizeof (EMFormatHTML),
			0,     /* n_preallocs */
			(GInstanceInitFunc) efh_init,
			NULL   /* value_table */
		};

		static const GInterfaceInfo extensible_info = {
			(GInterfaceInitFunc) NULL,
			(GInterfaceFinalizeFunc) NULL,
			NULL   /* interface_data */
		};

		type = g_type_register_static (
			em_format_get_type(), "EMFormatHTML",
			&type_info, G_TYPE_FLAG_ABSTRACT);

		g_type_add_interface_static (
			type, E_TYPE_EXTENSIBLE, &extensible_info);
	}

	return type;
}

/*****************************************************************************/

/* FIXME: This goes to EMailDisplay! */
void
em_format_html_load_images (EMFormatHTML *efh)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	if (efh->priv->image_loading_policy == E_MAIL_IMAGE_LOADING_POLICY_ALWAYS)
		return;

	/* This will remain set while we're still
	 * rendering the same message, then it wont be. */
	efh->priv->load_images_now = TRUE;
}

void
em_format_html_get_color (EMFormatHTML *efh,
                          EMFormatHTMLColorType type,
                          GdkColor *color)
{
	GdkColor *format_color;

	g_return_if_fail (EM_IS_FORMAT_HTML (efh));
	g_return_if_fail (type < EM_FORMAT_HTML_NUM_COLOR_TYPES);
	g_return_if_fail (color != NULL);

	format_color = &efh->priv->colors[type];

	color->red   = format_color->red;
	color->green = format_color->green;
	color->blue  = format_color->blue;
}

void
em_format_html_set_color (EMFormatHTML *efh,
                          EMFormatHTMLColorType type,
                          const GdkColor *color)
{
	GdkColor *format_color;
	const gchar *property_name;

	g_return_if_fail (EM_IS_FORMAT_HTML (efh));
	g_return_if_fail (type < EM_FORMAT_HTML_NUM_COLOR_TYPES);
	g_return_if_fail (color != NULL);

	format_color = &efh->priv->colors[type];

	if (gdk_color_equal (color, format_color))
		return;

	format_color->red   = color->red;
	format_color->green = color->green;
	format_color->blue  = color->blue;

	switch (type) {
		case EM_FORMAT_HTML_COLOR_BODY:
			property_name = "body-color";
			break;
		case EM_FORMAT_HTML_COLOR_CITATION:
			property_name = "citation-color";
			break;
		case EM_FORMAT_HTML_COLOR_CONTENT:
			property_name = "content-color";
			break;
		case EM_FORMAT_HTML_COLOR_FRAME:
			property_name = "frame-color";
			break;
		case EM_FORMAT_HTML_COLOR_HEADER:
			property_name = "header-color";
			break;
		case EM_FORMAT_HTML_COLOR_TEXT:
			property_name = "text-color";
			break;
		default:
			g_return_if_reached ();
	}

	g_object_notify (G_OBJECT (efh), property_name);
}

EMailImageLoadingPolicy
em_format_html_get_image_loading_policy (EMFormatHTML *efh)
{
	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), 0);

	return efh->priv->image_loading_policy;
}

void
em_format_html_set_image_loading_policy (EMFormatHTML *efh,
                                         EMailImageLoadingPolicy policy)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	if (policy == efh->priv->image_loading_policy)
		return;

	efh->priv->image_loading_policy = policy;

	g_object_notify (G_OBJECT (efh), "image-loading-policy");
}

gboolean
em_format_html_get_mark_citations (EMFormatHTML *efh)
{
	guint32 flags;

	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), FALSE);

	flags = efh->text_html_flags;

	return ((flags & CAMEL_MIME_FILTER_TOHTML_MARK_CITATION) != 0);
}

void
em_format_html_set_mark_citations (EMFormatHTML *efh,
                                   gboolean mark_citations)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	/* FIXME WEBKIT: Make this thread safe */
	if (mark_citations)
		efh->text_html_flags |=
			CAMEL_MIME_FILTER_TOHTML_MARK_CITATION;
	else
		efh->text_html_flags &=
			~CAMEL_MIME_FILTER_TOHTML_MARK_CITATION;

	g_object_notify (G_OBJECT (efh), "mark-citations");
}

gboolean
em_format_html_get_only_local_photos (EMFormatHTML *efh)
{
	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), FALSE);

	return efh->priv->only_local_photos;
}

void
em_format_html_set_only_local_photos (EMFormatHTML *efh,
                                      gboolean only_local_photos)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	efh->priv->only_local_photos = only_local_photos;

	g_object_notify (G_OBJECT (efh), "only-local-photos");
}

gboolean
em_format_html_get_show_sender_photo (EMFormatHTML *efh)
{
	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), FALSE);

	return efh->priv->show_sender_photo;
}

void
em_format_html_set_show_sender_photo (EMFormatHTML *efh,
                                      gboolean show_sender_photo)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	efh->priv->show_sender_photo = show_sender_photo;

	g_object_notify (G_OBJECT (efh), "show-sender-photo");
}

gboolean
em_format_html_get_show_real_date (EMFormatHTML *efh)
{
	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), FALSE);

	return efh->priv->show_real_date;
}

void
em_format_html_set_show_real_date (EMFormatHTML *efh,
                                   gboolean show_real_date)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	efh->priv->show_real_date =	show_real_date;

	g_object_notify (G_OBJECT (efh), "show-real-date");
}

EMFormatHTMLHeadersState
em_format_html_get_headers_state (EMFormatHTML *efh)
{
	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), EM_FORMAT_HTML_HEADERS_STATE_EXPANDED);

	return efh->priv->headers_state;
}

void
em_format_html_set_headers_state (EMFormatHTML *efh,
				  EMFormatHTMLHeadersState state)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	efh->priv->headers_state = state;

	g_object_notify (G_OBJECT (efh), "headers-state");
}

gboolean
em_format_html_get_headers_collapsable (EMFormatHTML *efh)
{
	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), FALSE);

	return efh->priv->headers_collapsable;
}

void
em_format_html_set_headers_collapsable (EMFormatHTML *efh,
					gboolean collapsable)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	efh->priv->headers_collapsable = collapsable;

	g_object_notify (G_OBJECT (efh), "headers-collapsable");
}

CamelMimePart *
em_format_html_file_part (EMFormatHTML *efh,
                          const gchar *mime_type,
                          const gchar *filename,
                          GCancellable *cancellable)
{
	CamelMimePart *part;
	CamelStream *stream;
	CamelDataWrapper *dw;
	gchar *basename;

	stream = camel_stream_fs_new_with_name (filename, O_RDONLY, 0, NULL);
	if (stream == NULL)
		return NULL;

	dw = camel_data_wrapper_new ();
	camel_data_wrapper_construct_from_stream_sync (
		dw, stream, cancellable, NULL);
	g_object_unref (stream);
	if (mime_type)
		camel_data_wrapper_set_mime_type (dw, mime_type);
	part = camel_mime_part_new ();
	camel_medium_set_content ((CamelMedium *) part, dw);
	g_object_unref (dw);
	basename = g_path_get_basename (filename);
	camel_mime_part_set_filename (part, basename);
	g_free (basename);

	return part;
}

gchar *
em_format_html_format_cert_infos (CamelCipherCertInfo *first_cinfo)
{
	GString *res = NULL;
	CamelCipherCertInfo *cinfo;

	if (!first_cinfo)
		return NULL;

	#define append(x) G_STMT_START {		\
		if (!res) {				\
			res = g_string_new (x);		\
		} else {				\
			g_string_append (res, x);	\
		}					\
	} G_STMT_END

	for (cinfo = first_cinfo; cinfo && cinfo->next; cinfo = cinfo->next) {
		if (!cinfo->name && !cinfo->email)
			continue;

		if (res)
			append (", ");

		if (cinfo->name && *cinfo->name) {
			append (cinfo->name);

			if (cinfo->email && *cinfo->email) {
				append (" &lt;");
				append (cinfo->email);
				append ("&gt;");
			}
		} else if (cinfo->email && *cinfo->email) {
			append (cinfo->email);
		}
	}

	#undef append

	if (!res)
		return NULL;

	return g_string_free (res, FALSE);
}




static void
efh_format_text_header (EMFormatHTML *emfh,
                        GString *buffer,
                        const gchar *label,
                        const gchar *value,
                        guint32 flags)
{
	const gchar *fmt, *html;
	gchar *mhtml = NULL;
	gboolean is_rtl;

	if (value == NULL)
		return;

	while (*value == ' ')
		value++;

	if (!(flags & EM_FORMAT_HTML_HEADER_HTML))
		html = mhtml = camel_text_to_html (value, emfh->text_html_flags, 0);
	else
		html = value;

	is_rtl = gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL;

	if (flags & EM_FORMAT_HTML_HEADER_NOCOLUMNS) {
		if (flags & EM_FORMAT_HEADER_BOLD) {
			fmt = "<tr><td><b>%s:</b> %s</td></tr>";
		} else {
			fmt = "<tr><td>%s: %s</td></tr>";
		}
	} else if (flags & EM_FORMAT_HTML_HEADER_NODEC) {
		if (is_rtl)
			fmt = "<tr><td align=\"right\" valign=\"top\" width=\"100%%\">%2$s</td><th valign=top align=\"left\" nowrap>%1$s<b>&nbsp;</b></th></tr>";
		else
			fmt = "<tr><th align=\"right\" valign=\"top\" nowrap>%s<b>&nbsp;</b></th><td valign=top>%s</td></tr>";
	} else {
		if (flags & EM_FORMAT_HEADER_BOLD) {
			if (is_rtl)
				fmt = "<tr><td align=\"right\" valign=\"top\" width=\"100%%\">%2$s</td><th align=\"left\" nowrap>%1$s:<b>&nbsp;</b></th></tr>";
			else
				fmt = "<tr><th align=\"right\" valign=\"top\" nowrap>%s:<b>&nbsp;</b></th><td>%s</td></tr>";
		} else {
			if (is_rtl)
				fmt = "<tr><td align=\"right\" valign=\"top\" width=\"100%\">%2$s</td><td align=\"left\" nowrap>%1$s:<b>&nbsp;</b></td></tr>";
			else
				fmt = "<tr><td align=\"right\" valign=\"top\" nowrap>%s:<b>&nbsp;</b></td><td>%s</td></tr>";
		}
	}

	g_string_append_printf (buffer, fmt, label, html);

	g_free (mhtml);
}

static const gchar *addrspec_hdrs[] = {
	"Sender", "From", "Reply-To", "To", "Cc", "Bcc",
	"Resent-Sender", "Resent-From", "Resent-Reply-To",
	"Resent-To", "Resent-Cc", "Resent-Bcc", NULL
};

static gchar *
efh_format_address (EMFormatHTML *efh,
                    GString *out,
                    struct _camel_header_address *a,
                    gchar *field)
{
	guint32 flags = CAMEL_MIME_FILTER_TOHTML_CONVERT_SPACES;
	gchar *name, *mailto, *addr;
	gint i = 0;
	gchar *str = NULL;
	gint limit = mail_config_get_address_count ();

	while (a) {
		if (a->name)
			name = camel_text_to_html (a->name, flags, 0);
		else
			name = NULL;

		switch (a->type) {
		case CAMEL_HEADER_ADDRESS_NAME:
			if (name && *name) {
				gchar *real, *mailaddr;

				if (strchr (a->name, ',') || strchr (a->name, ';'))
					g_string_append_printf (out, "&quot;%s&quot;", name);
				else
					g_string_append (out, name);

				g_string_append (out, " &lt;");

				/* rfc2368 for mailto syntax and url encoding extras */
				if ((real = camel_header_encode_phrase ((guchar *) a->name))) {
					mailaddr = g_strdup_printf("%s <%s>", real, a->v.addr);
					g_free (real);
					mailto = camel_url_encode (mailaddr, "?=&()");
					g_free (mailaddr);
				} else {
					mailto = camel_url_encode (a->v.addr, "?=&()");
				}
			} else {
				mailto = camel_url_encode (a->v.addr, "?=&()");
			}
			addr = camel_text_to_html (a->v.addr, flags, 0);
			g_string_append_printf (out, "<a href=\"mailto:%s\">%s</a>", mailto, addr);
			g_free (mailto);
			g_free (addr);

			if (name && *name)
				g_string_append (out, "&gt;");
			break;
		case CAMEL_HEADER_ADDRESS_GROUP:
			g_string_append_printf (out, "%s: ", name);
			efh_format_address (efh, out, a->v.members, field);
			g_string_append_printf (out, ";");
			break;
		default:
			g_warning ("Invalid address type");
			break;
		}

		g_free (name);

		i++;
		a = a->next;
		if (a)
			g_string_append (out, ", ");

		/* Let us add a '...' if we have more addresses */
		if (limit > 0 && (i == limit - 1)) {
			const gchar *id = NULL;

			if (strcmp (field, _("To")) == 0) {
				id = "to";
			} else if (strcmp (field, _("Cc")) == 0) {
				id = "cc";
			} else if (strcmp (field, _("Bcc")) == 0) {
				id = "bcc";
			}

			if (id) {
				g_string_append_printf (out, "<span id=\"moreaddr-%s\" style=\"display: none;\">", id);
				str = g_strdup_printf ("<img src=\"evo-file://%s/plus.png\" onClick=\"collapse_addresses('%s');\" id=\"moreaddr-img-%s\" class=\"navigable\">  ",
					EVOLUTION_IMAGESDIR, id, id);
			}
		}
	}

	if (str) {
		const gchar *id = NULL;

		if (strcmp (field, _("To")) == 0) {
			id = "to";
		} else if (strcmp (field, _("Cc")) == 0) {
			id = "cc";
		} else if (strcmp (field, _("Bcc")) == 0) {
			id = "bcc";
		}

		if (id) {
			g_string_append_printf (out, "</span><span class=\"navigable\" onClick=\"collapse_addresses('%s');\" " \
				"id=\"moreaddr-ellipsis-%s\" style=\"display: inline;\">...</span>",
				id, id);
		}
	}

	return str;
}

static void
canon_header_name (gchar *name)
{
	gchar *inptr = name;

	/* canonicalise the header name... first letter is
	 * capitalised and any letter following a '-' also gets
	 * capitalised */

	if (*inptr >= 'a' && *inptr <= 'z')
		*inptr -= 0x20;

	inptr++;

	while (*inptr) {
		if (inptr[-1] == '-' && *inptr >= 'a' && *inptr <= 'z')
			*inptr -= 0x20;
		else if (*inptr >= 'A' && *inptr <= 'Z')
			*inptr += 0x20;

		inptr++;
	}
}

static void
efh_format_header (EMFormat *emf,
                   GString *buffer,
                   CamelMedium *part,
                   struct _camel_header_raw *header,
                   guint32 flags,
                   const gchar *charset)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (emf);
	gchar *name, *buf, *value = NULL;
	const gchar *label, *txt;
	gboolean addrspec = FALSE;
	gchar *str_field = NULL;
	gint i;

	name = g_alloca (strlen (header->name) + 1);
	strcpy (name, header->name);
	canon_header_name (name);

	for (i = 0; addrspec_hdrs[i]; i++) {
		if (!strcmp (name, addrspec_hdrs[i])) {
			addrspec = TRUE;
			break;
		}
	}

	label = _(name);

	if (addrspec) {
		struct _camel_header_address *addrs;
		GString *html;
		gchar *img;
		const gchar *charset = em_format_get_charset (emf) ?
				em_format_get_charset (emf) : em_format_get_default_charset (emf);

		buf = camel_header_unfold (header->value);
 		if (!(addrs = camel_header_address_decode (buf, charset))) {
			g_free (buf);
			return;
		}

		g_free (buf);

		html = g_string_new("");
		img = efh_format_address (efh, html, addrs, (gchar *) label);

		if (img) {
			str_field = g_strdup_printf ("%s%s:", img, label);
			label = str_field;
			flags |= EM_FORMAT_HTML_HEADER_NODEC;
			g_free (img);
		}

		camel_header_address_list_clear (&addrs);
		txt = value = html->str;
		g_string_free (html, FALSE);

		flags |= EM_FORMAT_HEADER_BOLD | EM_FORMAT_HTML_HEADER_HTML;
	} else if (!strcmp (name, "Subject")) {
		buf = camel_header_unfold (header->value);
		txt = value = camel_header_decode_string (buf, charset);
		g_free (buf);

		flags |= EM_FORMAT_HEADER_BOLD;
	} else if (!strcmp(name, "X-evolution-mailer")) {
		/* pseudo-header */
		label = _("Mailer");
		txt = value = camel_header_format_ctext (header->value, charset);
		flags |= EM_FORMAT_HEADER_BOLD;
	} else if (!strcmp (name, "Date") || !strcmp (name, "Resent-Date")) {
		gint msg_offset, local_tz;
		time_t msg_date;
		struct tm local;
		gchar *html;
		gboolean hide_real_date;

		hide_real_date = !em_format_html_get_show_real_date (efh);

		txt = header->value;
		while (*txt == ' ' || *txt == '\t')
			txt++;

		html = camel_text_to_html (txt, efh->text_html_flags, 0);

		msg_date = camel_header_decode_date (txt, &msg_offset);
		e_localtime_with_offset (msg_date, &local, &local_tz);

		/* Convert message offset to minutes (e.g. -0400 --> -240) */
		msg_offset = ((msg_offset / 100) * 60) + (msg_offset % 100);
		/* Turn into offset from localtime, not UTC */
		msg_offset -= local_tz / 60;

		/* value will be freed at the end */
		if (!hide_real_date && !msg_offset) {
			/* No timezone difference; just show the real Date: header */
			txt = value = html;
		} else {
			gchar *date_str;

			date_str = e_datetime_format_format ("mail", "header",
							     DTFormatKindDateTime, msg_date);

			if (hide_real_date) {
				/* Show only the local-formatted date, losing all timezone
				 * information like Outlook does. Should we attempt to show
				 * it somehow? */
				txt = value = date_str;
			} else {
				txt = value = g_strdup_printf ("%s (<I>%s</I>)", html, date_str);
				g_free (date_str);
			}
			g_free (html);
		}
		flags |= EM_FORMAT_HTML_HEADER_HTML | EM_FORMAT_HEADER_BOLD;
	} else if (!strcmp(name, "Newsgroups")) {
		struct _camel_header_newsgroup *ng, *scan;
		GString *html;

		buf = camel_header_unfold (header->value);

		if (!(ng = camel_header_newsgroups_decode (buf))) {
			g_free (buf);
			return;
		}

		g_free (buf);

		html = g_string_new("");
		scan = ng;
		while (scan) {
			g_string_append_printf(html, "<a href=\"news:%s\">%s</a>", scan->newsgroup, scan->newsgroup);
			scan = scan->next;
			if (scan)
				g_string_append_printf(html, ", ");
		}

		camel_header_newsgroups_free (ng);

		txt = html->str;
		g_string_free (html, FALSE);
		flags |= EM_FORMAT_HEADER_BOLD | EM_FORMAT_HTML_HEADER_HTML;
	} else if (!strcmp (name, "Received") || !strncmp (name, "X-", 2)) {
		/* don't unfold Received nor extension headers */
		txt = value = camel_header_decode_string (header->value, charset);
	} else {
		/* don't unfold Received nor extension headers */
		buf = camel_header_unfold (header->value);
		txt = value = camel_header_decode_string (buf, charset);
		g_free (buf);
	}

	efh_format_text_header (efh, buffer, label, txt, flags);

	g_free (value);
	g_free (str_field);
}

static void
efh_format_short_headers (EMFormatHTML *efh,
			  GString *buffer,
			  CamelMedium *part,
			  gboolean visible,
			  GCancellable *cancellable)
{
	EMFormat *emf = EM_FORMAT (efh);
	const gchar *charset;
	CamelContentType *ct;
	const gchar *hdr_charset;
	gchar *evolution_imagesdir;
	gchar *subject = NULL;
	struct _camel_header_address *addrs = NULL;
	struct _camel_header_raw *header;
	GString *from;

	if (cancellable && g_cancellable_is_cancelled (cancellable))
		return;

	ct = camel_mime_part_get_content_type ((CamelMimePart *) part);
	charset = camel_content_type_param (ct, "charset");
	charset = camel_iconv_charset_name (charset);
	hdr_charset = em_format_get_charset (emf) ?
			em_format_get_charset (emf) : em_format_get_default_charset (emf);

	evolution_imagesdir = g_filename_to_uri (EVOLUTION_IMAGESDIR, NULL, NULL);
	from = g_string_new ("");

	g_string_append_printf (buffer, "<table cellspacing=\"0\" cellpadding=\"0\" border=\"0\" id=\"short-headers\" style=\"display: %s\">",
		visible ? "block" : "none");

	header = ((CamelMimePart *) part)->headers;
	while (header) {
		if (!g_ascii_strcasecmp (header->name, "From")) {
			GString *tmp;
			if (!(addrs = camel_header_address_decode (header->value, hdr_charset))) {
				header = header->next;
				continue;
			}
			tmp = g_string_new ("");
			efh_format_address (efh, tmp, addrs, header->name);

			if (tmp->len)
				g_string_printf (from, _("From: %s"), tmp->str);
			g_string_free (tmp, TRUE);

		} else if (!g_ascii_strcasecmp (header->name, "Subject")) {
			gchar *buf = NULL;
			buf = camel_header_unfold (header->value);
			g_free (subject);
			subject = camel_header_decode_string (buf, hdr_charset);
			g_free (buf);
		}
		header = header->next;
	}

	g_string_append_printf (
		buffer,
		"<tr><td><strong>%s</strong> %s%s%s</td></tr>",
		subject ? subject : _("(no subject)"),
		from->len ? "(" : "", from->str, from->len ? ")" : "");

	g_string_append (buffer, "</table>");

	g_free (subject);
	if (addrs)
		camel_header_address_list_clear (&addrs);

	g_string_free (from, TRUE);
	g_free (evolution_imagesdir);
}

static void
efh_format_full_headers (EMFormatHTML *efh,
			 GString *buffer,
			 CamelMedium *part,
			 gboolean all_headers,
			 gboolean visible,
			 GCancellable *cancellable)
{
	EMFormat *emf = EM_FORMAT (efh);
	const gchar *charset;
	CamelContentType *ct;
	struct _camel_header_raw *header;
	gboolean have_icon = FALSE;
	const gchar *photo_name = NULL;
	CamelInternetAddress *cia = NULL;
	gboolean face_decoded  = FALSE, contact_has_photo = FALSE;
	guchar *face_header_value = NULL;
	gsize face_header_len = 0;
	gchar *header_sender = NULL, *header_from = NULL, *name;
	gboolean mail_from_delegate = FALSE;
	const gchar *hdr_charset;
	gchar *evolution_imagesdir;

	if (cancellable && g_cancellable_is_cancelled (cancellable))
		return;

	ct = camel_mime_part_get_content_type ((CamelMimePart *) part);
	charset = camel_content_type_param (ct, "charset");
	charset = camel_iconv_charset_name (charset);
	hdr_charset = em_format_get_charset (emf) ?
			em_format_get_charset (emf) : em_format_get_default_charset (emf);

	evolution_imagesdir = g_filename_to_uri (EVOLUTION_IMAGESDIR, NULL, NULL);

	g_string_append_printf (buffer, "<table cellspacing=\"0\" cellpadding=\"0\" border=\"0\" id=\"full-headers\" style=\"display: %s\" width=\"100%%\">",
		visible ? "block" : "none");

	header = ((CamelMimePart *) part)->headers;
	while (header) {
		if (!g_ascii_strcasecmp (header->name, "Sender")) {
			struct _camel_header_address *addrs;
			GString *html;

			if (!(addrs = camel_header_address_decode (header->value, hdr_charset)))
				break;

			html = g_string_new("");
			name = efh_format_address (efh, html, addrs, header->name);

			header_sender = html->str;
			camel_header_address_list_clear (&addrs);

			g_string_free (html, FALSE);
			g_free (name);
		} else if (!g_ascii_strcasecmp (header->name, "From")) {
			struct _camel_header_address *addrs;
			GString *html;

			if (!(addrs = camel_header_address_decode (header->value, hdr_charset)))
				break;

			html = g_string_new("");
			name = efh_format_address (efh, html, addrs, header->name);

			header_from = html->str;
			camel_header_address_list_clear (&addrs);

			g_string_free (html, FALSE);
			g_free (name);
		} else if (!g_ascii_strcasecmp (header->name, "X-Evolution-Mail-From-Delegate")) {
			mail_from_delegate = TRUE;
		}

		header = header->next;
	}

	if (header_sender && header_from && mail_from_delegate) {
		gchar *bold_sender, *bold_from;

		g_string_append (
			buffer,
			"<tr><td><table border=1 width=\"100%%\" "
			"cellspacing=2 cellpadding=2><tr>");
		if (gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL)
			g_string_append (
				buffer, "<td align=\"right\" width=\"100%%\">");
		else
			g_string_append (
				buffer, "<td align=\"left\" width=\"100%%\">");
		bold_sender = g_strconcat ("<b>", header_sender, "</b>", NULL);
		bold_from = g_strconcat ("<b>", header_from, "</b>", NULL);
		/* Translators: This message suggests to the receipients
		 * that the sender of the mail is different from the one
		 * listed in From field. */
		g_string_append_printf (
			buffer,
			_("This message was sent by %s on behalf of %s"),
			bold_sender, bold_from);
		g_string_append (buffer, "</td></tr></table></td></tr>");
		g_free (bold_sender);
		g_free (bold_from);
	}

	g_free (header_sender);
	g_free (header_from);

	g_string_append (buffer, "<tr><td><table border=0 cellpadding=\"0\">\n");

	g_free (evolution_imagesdir);

	/* dump selected headers */
	if (all_headers) {
		header = ((CamelMimePart *) part)->headers;
		while (header) {
			efh_format_header (
				emf, buffer, part, header,
				EM_FORMAT_HTML_HEADER_NOCOLUMNS, charset);
			header = header->next;
		}
	} else {
		GList *link;
		gint mailer_shown = FALSE;

		link = g_queue_peek_head_link (&emf->header_list);

		while (link != NULL) {
			EMFormatHeader *h = link->data;
			gint mailer, face;

			header = ((CamelMimePart *) part)->headers;
			mailer = !g_ascii_strcasecmp (h->name, "X-Evolution-Mailer");
			face = !g_ascii_strcasecmp (h->name, "Face");

			while (header) {
				if (em_format_html_get_show_sender_photo (efh) &&
					!photo_name && !g_ascii_strcasecmp (header->name, "From"))
					photo_name = header->value;

				if (!mailer_shown && mailer && (!g_ascii_strcasecmp (header->name, "X-Mailer") ||
								!g_ascii_strcasecmp (header->name, "User-Agent") ||
								!g_ascii_strcasecmp (header->name, "X-Newsreader") ||
								!g_ascii_strcasecmp (header->name, "X-MimeOLE"))) {
					struct _camel_header_raw xmailer, *use_header = NULL;

					if (!g_ascii_strcasecmp (header->name, "X-MimeOLE")) {
						for (use_header = header->next; use_header; use_header = use_header->next) {
							if (!g_ascii_strcasecmp (use_header->name, "X-Mailer") ||
							    !g_ascii_strcasecmp (use_header->name, "User-Agent") ||
							    !g_ascii_strcasecmp (use_header->name, "X-Newsreader")) {
								/* even we have X-MimeOLE, then use rather the standard one, when available */
								break;
							}
						}
					}

					if (!use_header)
						use_header = header;

					xmailer.name = (gchar *) "X-Evolution-Mailer";
					xmailer.value = use_header->value;
					mailer_shown = TRUE;

					efh_format_header (
						emf, buffer, part,
						&xmailer, h->flags, charset);
					if (strstr(use_header->value, "Evolution"))
						have_icon = TRUE;
				} else if (!face_decoded && face && !g_ascii_strcasecmp (header->name, "Face")) {
					gchar *cp = header->value;

					/* Skip over spaces */
					while (*cp == ' ')
						cp++;

					face_header_value = g_base64_decode (cp, &face_header_len);
					face_header_value = g_realloc (face_header_value, face_header_len + 1);
					face_header_value[face_header_len] = 0;
					face_decoded = TRUE;
				/* Showing an encoded "Face" header makes little sense */
				} else if (!g_ascii_strcasecmp (header->name, h->name) && !face) {
					efh_format_header (
						emf, buffer, part,
						header, h->flags, charset);
				}

				header = header->next;
			}

			link = g_list_next (link);
		}
	}

	g_string_append (buffer, "</table></td>");

	if (photo_name) {
		const gchar *classid;
		CamelMimePart *photopart;
		gboolean only_local_photo;

		cia = camel_internet_address_new ();
		camel_address_decode ((CamelAddress *) cia, (const gchar *) photo_name);
		only_local_photo = em_format_html_get_only_local_photos (efh);
		photopart = em_utils_contact_photo (cia, only_local_photo);

		if (photopart) {
			EMFormatPURI *puri;
			contact_has_photo = TRUE;
			classid = "icon:///em-format-html/headers/photo";
			g_string_append_printf (
				buffer,
				"<td align=\"right\" valign=\"top\">"
				"<img width=64 src=\"%s\"></td>",
				classid);
			puri = em_format_puri_new (
					emf, sizeof (EMFormatPURI), photopart, classid);
			puri->write_func = efh_write_image;
			em_format_add_puri (emf, puri);
			g_object_unref (photopart);
		}
		g_object_unref (cia);
	}

	if (!contact_has_photo && face_decoded) {
		const gchar *classid;
		CamelMimePart *part;
		EMFormatPURI *puri;

		part = camel_mime_part_new ();
		camel_mime_part_set_content (
			(CamelMimePart *) part,
			(const gchar *) face_header_value,
			face_header_len, "image/png");
		classid = "icon:///em-format-html/headers/face/photo";
		g_string_append_printf (
			buffer,
			"<td align=\"right\" valign=\"top\">"
			"<img width=48 src=\"%s\"></td>",
			classid);

		puri = em_format_puri_new (
			emf, sizeof (EMFormatPURI), part, classid);
		puri->write_func = efh_write_image;
		em_format_add_puri (emf, puri);

		g_object_unref (part);
		g_free (face_header_value);
	}

	if (have_icon && efh->show_icon) {
		GtkIconInfo *icon_info;
		const gchar *classid;
		CamelMimePart *iconpart = NULL;
		EMFormatPURI *puri;

		classid = "icon:///em-format-html/header/icon";
		g_string_append_printf (
			buffer,
			"<td align=\"right\" valign=\"top\">"
			"<img width=16 height=16 src=\"%s\"></td>",
			classid);
			icon_info = gtk_icon_theme_lookup_icon (
			gtk_icon_theme_get_default (),
			"evolution", 16, GTK_ICON_LOOKUP_NO_SVG);
		if (icon_info != NULL) {
			iconpart = em_format_html_file_part (
				(EMFormatHTML *) emf, "image/png",
				gtk_icon_info_get_filename (icon_info),
				cancellable);
			gtk_icon_info_free (icon_info);
		}
		if (iconpart) {
			puri = em_format_puri_new (
					emf, sizeof (EMFormatPURI), iconpart, classid);
			puri->write_func = efh_write_image;
			em_format_add_puri (emf, puri);
			g_object_unref (iconpart);
		}
	}

	g_string_append (buffer, "</tr></table>");
}

void
em_format_html_format_headers (EMFormatHTML *efh,
			       CamelStream *stream,
			       CamelMedium *part,
			       gboolean all_headers,
			       GCancellable *cancellable)
{
	GString *buffer;

	if (!part)
		return;

	buffer = g_string_new ("");

	g_string_append_printf (
		buffer, EFH_HTML_HEADER,
		e_color_to_value (
			&efh->priv->colors[
			EM_FORMAT_HTML_COLOR_BODY]),
		e_color_to_value (
			&efh->priv->colors[
			EM_FORMAT_HTML_COLOR_HEADER]));

	if (efh->priv->headers_collapsable) {
		g_string_append_printf (buffer,
			"<img src=\"evo-file://%s/%s\" onClick=\"collapse_headers();\" class=\"navigable\" id=\"collapse-headers-img\" /></td><td>",
			EVOLUTION_IMAGESDIR,
			(efh->priv->headers_state == EM_FORMAT_HTML_HEADERS_STATE_COLLAPSED) ? "plus.png" : "minus.png");

		efh_format_short_headers (efh, buffer, part,
			(efh->priv->headers_state == EM_FORMAT_HTML_HEADERS_STATE_COLLAPSED),
			cancellable);
	}

	efh_format_full_headers (efh, buffer, part, all_headers,
		(efh->priv->headers_state == EM_FORMAT_HTML_HEADERS_STATE_EXPANDED),
		cancellable);

	g_string_append (buffer, "</td></tr></table>" EFH_HTML_FOOTER);

	camel_stream_write_string (stream, buffer->str, cancellable, NULL);

	g_string_free (buffer, true);
}

/* unref returned pointer with g_object_unref(), if not NULL */
CamelStream *
em_format_html_get_cached_image (EMFormatHTML *efh,
                                 const gchar *image_uri)
{
	g_return_val_if_fail (efh != NULL, NULL);
	g_return_val_if_fail (image_uri != NULL, NULL);

	/* FIXME WEBKIT This has not been ported yet
	if (!emfh_http_cache)
		return NULL;

	return camel_data_cache_get (
		emfh_http_cache, EMFH_HTTP_CACHE_PATH, image_uri, NULL);
	*/
	return NULL;
}

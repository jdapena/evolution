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
#include "mail-mt.h"

#define d(x)

#define EFM_MESSAGE_START_ANAME "evolution_message_start"
#define EFH_MESSAGE_START "<A name=\"" EFM_MESSAGE_START_ANAME "\"></A>"

struct _EMFormatHTMLCache {
	CamelMultipart *textmp;

	gchar partid[1];
};

struct _EMFormatHTMLPrivate {
	EWebView *web_view;

	CamelMimeMessage *last_part;	/* not reffed, DO NOT dereference */
	volatile gint format_id;		/* format thread id */
	guint format_timeout_id;
	struct _format_msg *format_timeout_msg;

	/* Table that re-maps text parts into a mutlipart/mixed, EMFormatHTMLCache * */
	GHashTable *text_inline_parts;

	GQueue pending_jobs;
	GMutex *lock;

	GdkColor colors[EM_FORMAT_HTML_NUM_COLOR_TYPES];
	EMailImageLoadingPolicy image_loading_policy;

	EMFormatHTMLHeadersState headers_state;
	gboolean headers_collapsable;

	guint load_images_now	: 1;
	guint only_local_photos	: 1;
	guint show_sender_photo	: 1;
	guint show_real_date	: 1;
};

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
	PROP_WEB_VIEW,
	PROP_HEADERS_STATE,
	PROP_HEADERS_COLLAPSABLE
};


static void	emfh_format_email		(struct _EMFormatHTMLJob *job,
	       	   				 GCancellable *cancellable);

static gboolean	efh_display_formatted_data	(gpointer data);

static GtkWidget* efh_create_plugin_widget	(WebKitWebView *web_view,
						 gchar *mime_type,
						 gchar *uri,
						 GHashTable *param,
						 gpointer user_data);
static void	efh_webview_frame_created	(WebKitWebView *web_view,
						 WebKitWebFrame *frame,
						 gpointer user_data);
static void	efh_resource_requested		(WebKitWebView *web_view,
						 WebKitWebFrame *frame,
						 WebKitWebResource *resource,
						 WebKitNetworkRequest *request,
						 WebKitNetworkResponse *reponse,
						 gpointer user_data);
static void	efh_install_js_callbacks	(WebKitWebView *web_view,
						 WebKitWebFrame *frame,
						 gpointer context,
						 gpointer window_object,
						 gpointer user_data);
static void	efh_format_message		(EMFormat *emf,
						 CamelStream *stream,
						 CamelMimePart *part,
						 const EMFormatHandler *info,
						 GCancellable *cancellable,
						 gboolean is_fallback);

static void	efh_format_secure		(EMFormat *emf,
						 CamelStream *stream,
						 CamelMimePart *part,
						 CamelCipherValidity *valid,
						 GCancellable *cancellable);

static void	efh_builtin_init		(EMFormatHTMLClass *efhc);

static void	efh_write_image			(EMFormat *emf,
						 CamelStream *stream,
						 EMFormatPURI *puri,
						 GCancellable *cancellable);

static gpointer parent_class;
static CamelDataCache *emfh_http_cache;

#define EMFH_HTTP_CACHE_PATH "http"

/* Sigh, this is so we have a cancellable, async rendering thread */
struct _format_msg {
	MailMsg base;

	EMFormatHTML *format;
	EMFormat *format_source;
	CamelFolder *folder;
	gchar *uid;
	CamelMimeMessage *message;
	gboolean cancelled;
};

static gchar *
efh_format_desc (struct _format_msg *m)
{
	return g_strdup(_("Formatting message"));
}

static void
efh_format_exec (struct _format_msg *m,
                 GCancellable *cancellable,
                 GError **error)
{
	EMFormat *format;
	struct _EMFormatHTMLJob *job;
	GNode *puri_level;
	CamelURL *base;

	if (m->format->priv->web_view == NULL) {
		m->cancelled = TRUE;
		return;
	}

	format = EM_FORMAT (m->format);

	puri_level = format->pending_uri_level;
	base = format->base;

	do {
		d(printf("processing job\n"));

		g_mutex_lock (m->format->priv->lock);
		while ((job = g_queue_pop_head (&m->format->priv->pending_jobs))) {

			g_mutex_unlock (m->format->priv->lock);

			/* This is an implicit check to see if the webview has been destroyed */
			if (m->format->priv->web_view == NULL)
				g_cancellable_cancel (cancellable);

			/* call jobs even if cancelled, so they can clean up resources */
			format->pending_uri_level = job->puri_level;
			if (job->base)
				format->base = job->base;
			/* Call the job's callback, usually a parser */
			job->callback (job, cancellable);
			format->base = base;

			/* Display stream created by the callback and free
			   the job struct */
			g_idle_add(efh_display_formatted_data, job);

			g_mutex_lock (m->format->priv->lock);
		}
		g_mutex_unlock (m->format->priv->lock);

	} while (!g_queue_is_empty (&m->format->priv->pending_jobs));

	d(printf("out of jobs, done\n"));

	format->pending_uri_level = puri_level;

	m->cancelled = m->cancelled || g_cancellable_is_cancelled (cancellable);

	m->format->priv->format_id = -1;
}

static void
efh_format_done (struct _format_msg *m)
{
	d(printf("formatting finished\n"));

	m->format->priv->format_id = -1;
	m->format->priv->load_images_now = FALSE;
	m->format->state = EM_FORMAT_HTML_STATE_NONE;
	g_signal_emit_by_name(m->format, "complete");
}

static void
efh_format_free (struct _format_msg *m)
{
	d(printf("formatter freed\n"));
	g_object_unref (m->format);
	if (m->folder)
		g_object_unref (m->folder);
	g_free (m->uid);
	if (m->message)
		g_object_unref (m->message);
	if (m->format_source)
		g_object_unref (m->format_source);
}

static MailMsgInfo efh_format_info = {
	sizeof (struct _format_msg),
	(MailMsgDescFunc) efh_format_desc,
	(MailMsgExecFunc) efh_format_exec,
	(MailMsgDoneFunc) efh_format_done,
	(MailMsgFreeFunc) efh_format_free
};

static gboolean
efh_display_formatted_data (gpointer data)
{
	/* This is an idle callback */

	struct _EMFormatHTMLJob *job = data;
	EWebView *web_view = job->format->priv->web_view;
	GByteArray *ba;
	gchar *content;

	if (web_view == NULL)
		goto cleanup;

	d(printf("displaying messsage\n"));

	ba = camel_stream_mem_get_byte_array (CAMEL_STREAM_MEM (job->stream));

	content = g_strndup ((gchar*) ba->data, ba->len);
	e_web_view_load_string (web_view, content);

	g_free (content);

cleanup:
	/* Clean up the job */
	g_object_unref (job->stream);
	if (job->base)
		camel_url_free (job->base);
	g_free (job);

	return FALSE;
}

static gboolean
efh_format_timeout (struct _format_msg *m)
{
	EMFormatHTML *efh = m->format;
	EMFormat *emf = EM_FORMAT (efh);
	struct _EMFormatHTMLPrivate *p = efh->priv;
	EWebView *web_view;

	web_view = em_format_html_get_web_view (efh);

	if (web_view == NULL) {
		mail_msg_unref (m);
		return FALSE;
	}

	d(printf("timeout called ...\n"));
	if (p->format_id != -1) {
		d(printf(" still waiting for cancellation to take effect, waiting ...\n"));
		return TRUE;
	}

	g_return_val_if_fail (g_queue_is_empty (&p->pending_jobs), FALSE);

	/* call super-class to kick it off */
	/* FIXME Not passing a GCancellable here. */
	EM_FORMAT_CLASS (parent_class)->format_clone (
		emf, m->folder, m->uid,
		m->message, m->format_source, NULL);
	em_format_html_clear_pobject (efh);

	/* FIXME: method off EMFormat? */
	if (emf->valid) {
		camel_cipher_validity_free (emf->valid);
		emf->valid = NULL;
		emf->valid_parent = NULL;
	}

	if (m->message == NULL) {
		mail_msg_unref (m);
		p->last_part = NULL;
	} else {
		struct _EMFormatHTMLJob *job;

		/* Queue a job for parsing the email main content */
		job = em_format_html_job_new (efh, emfh_format_email, m->message);
		job->stream = camel_stream_mem_new ();
		em_format_html_job_queue (efh, job);

		efh->state = EM_FORMAT_HTML_STATE_RENDERING;
#if HAVE_CLUTTER
		if (p->last_part != m->message && !e_shell_get_express_mode (e_shell_get_default ())) {
#else
		if (p->last_part != m->message) {
#endif
			gchar *str = g_strdup_printf ("<html><head></head><body>"
						      "<table width=\"100%%\" height=\"100%%\"><tr>"
						      "<td valign=\"middle\" align=\"center\"><h5>%s</h5></td>"
						      "</tr></table>"
						      "</body></html>", _("Formatting Message..."));
			e_web_view_load_string (web_view, str);
			g_free (str);

			g_hash_table_remove_all (p->text_inline_parts);

			p->last_part = m->message;
		}

		mail_msg_unordered_push (m);
	}

	p->format_timeout_id = 0;
	p->format_timeout_msg = NULL;

	return FALSE;
}

static void
efh_free_cache (struct _EMFormatHTMLCache *efhc)
{
	if (efhc->textmp)
		g_object_unref (efhc->textmp);
	g_free (efhc);
}

static struct _EMFormatHTMLCache *
efh_insert_cache (EMFormatHTML *efh,
                  const gchar *partid)
{
	struct _EMFormatHTMLCache *efhc;

	efhc = g_malloc0 (sizeof (*efh) + strlen (partid));
	strcpy (efhc->partid, partid);
	g_hash_table_insert (efh->priv->text_inline_parts, efhc->partid, efhc);

	return efhc;
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

		case PROP_WEB_VIEW:
			g_value_set_object (
				value, em_format_html_get_web_view (
				EM_FORMAT_HTML (object)));
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

	em_format_html_clear_pobject (efh);

	if (priv->format_timeout_id != 0) {
		g_source_remove (priv->format_timeout_id);
		priv->format_timeout_id = 0;
		mail_msg_unref (priv->format_timeout_msg);
		priv->format_timeout_msg = NULL;
	}

	/* This probably works ... */
	if (priv->format_id != -1)
		mail_msg_cancel (priv->format_id);

	if (priv->web_view != NULL) {
		g_object_unref (priv->web_view);
		efh->priv->web_view = NULL;
	}

	g_hash_table_destroy (priv->text_inline_parts);

	g_mutex_free (priv->lock);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
efh_format_clone (EMFormat *emf,
                  CamelFolder *folder,
                  const gchar *uid,
                  CamelMimeMessage *msg,
                  EMFormat *emfsource,
                  GCancellable *cancellable)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (emf);
	struct _format_msg *m;

	/* No webview, no need to format anything */
	if (efh->priv->web_view == NULL)
		return;

	d(printf("efh_format called\n"));
	if (efh->priv->format_timeout_id != 0) {
		d(printf(" timeout for last still active, removing ...\n"));
		g_source_remove (efh->priv->format_timeout_id);
		efh->priv->format_timeout_id = 0;
		mail_msg_unref (efh->priv->format_timeout_msg);
		efh->priv->format_timeout_msg = NULL;
	}

	if (emfsource != NULL)
		g_object_ref (emfsource);

	if (folder != NULL)
		g_object_ref (folder);

	if (msg != NULL)
		g_object_ref (msg);

	m = mail_msg_new (&efh_format_info);
	m->format = g_object_ref (emf);
	m->format_source = emfsource;
	m->folder = folder;
	m->uid = g_strdup (uid);
	m->message = msg;

	if (efh->priv->format_id == -1) {
		d(printf(" idle, forcing format\n"));
		efh_format_timeout (m);
	} else {
		d(printf(" still busy, cancelling and queuing wait\n"));
		/* cancel and poll for completion */
		mail_msg_cancel (efh->priv->format_id);
		efh->priv->format_timeout_msg = m;
		efh->priv->format_timeout_id = g_timeout_add (
			100, (GSourceFunc) efh_format_timeout, m);
	}
}

static void
efh_format_error (EMFormat *emf,
                  CamelStream *stream,
                  const gchar *txt)
{
	GString *buffer;
	gchar *html;

	buffer = g_string_new ("<em><font color=\"red\">");

	html = camel_text_to_html (
		txt, CAMEL_MIME_FILTER_TOHTML_CONVERT_NL |
		CAMEL_MIME_FILTER_TOHTML_CONVERT_URLS, 0);
	g_string_append (buffer, html);
	g_free (html);

	g_string_append (buffer, "</font></em><br>");

	camel_stream_write (stream, buffer->str, buffer->len, NULL, NULL);

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

	if (handle && em_format_is_inline (emf, emf->part_id->str, part, handle))
		handle->handler (emf, stream, part, handle, cancellable, FALSE);
}

static gboolean
efh_busy (EMFormat *emf)
{
	EMFormatHTMLPrivate *priv;

	priv = EM_FORMAT_HTML (emf)->priv;

	return (priv->format_id != -1);
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
	format_class->format_clone = efh_format_clone;
	format_class->format_error = efh_format_error;
	format_class->format_source = efh_format_source;
	format_class->format_attachment = efh_format_attachment;
	format_class->format_secure = efh_format_secure;
	format_class->busy = efh_busy;

	class->html_widget_type = E_TYPE_WEB_VIEW;

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
			TRUE,
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
		PROP_WEB_VIEW,
		g_param_spec_object (
			"web-view",
			"Web View",
			NULL,
			E_TYPE_WEB_VIEW,
			G_PARAM_READABLE));

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
		PROP_HEADERS_STATE,
		g_param_spec_boolean (
			"headers-collapsable",
			NULL,
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	/* cache expiry - 2 hour access, 1 day max */
	user_cache_dir = e_get_user_cache_dir ();
	emfh_http_cache = camel_data_cache_new (user_cache_dir, NULL);
	if (emfh_http_cache) {
		camel_data_cache_set_expire_age (emfh_http_cache, 24 *60 *60);
		camel_data_cache_set_expire_access (emfh_http_cache, 2 *60 *60);
	}
}

static void
efh_init (EMFormatHTML *efh,
          EMFormatHTMLClass *class)
{
	EWebView *web_view;
	GdkColor *color;

	efh->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		efh, EM_TYPE_FORMAT_HTML, EMFormatHTMLPrivate);

	g_queue_init (&efh->pending_object_list);
	g_queue_init (&efh->priv->pending_jobs);
	efh->priv->lock = g_mutex_new ();
	efh->priv->format_id = -1;
	efh->priv->text_inline_parts = g_hash_table_new_full (
		g_str_hash, g_str_equal,
		(GDestroyNotify) NULL,
		(GDestroyNotify) efh_free_cache);

	web_view = g_object_new (class->html_widget_type, NULL);
	efh->priv->web_view = g_object_ref_sink (web_view);

	g_signal_connect (
		web_view, "resource-request-starting",
		G_CALLBACK (efh_resource_requested), efh);
	g_signal_connect (
		web_view, "create-plugin-widget",
		G_CALLBACK (efh_create_plugin_widget), efh);
	g_signal_connect (
		web_view, "frame-created",
		G_CALLBACK (efh_webview_frame_created), efh);
	g_signal_connect (
		web_view, "window-object-cleared",
		G_CALLBACK (efh_install_js_callbacks), efh);

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

	g_signal_connect_swapped (
		efh, "notify::mark-citations",
		G_CALLBACK (em_format_queue_redraw), NULL);

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

EWebView *
em_format_html_get_web_view (EMFormatHTML *efh)
{
	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), NULL);

	return efh->priv->web_view;
}

void
em_format_html_load_images (EMFormatHTML *efh)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	if (efh->priv->image_loading_policy == E_MAIL_IMAGE_LOADING_POLICY_ALWAYS)
		return;

	/* This will remain set while we're still
	 * rendering the same message, then it wont be. */
	efh->priv->load_images_now = TRUE;
	em_format_queue_redraw (EM_FORMAT (efh));
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

	if (mark_citations)
		efh->text_html_flags |=
			CAMEL_MIME_FILTER_TOHTML_MARK_CITATION;
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

	efh->priv->show_real_date = show_real_date;

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

/* all this api is a pain in the bum ... */

EMFormatHTMLPObject *
em_format_html_add_pobject (EMFormatHTML *efh,
                            gsize size,
                            const gchar *classid,
                            CamelMimePart *part,
                            EMFormatHTMLPObjectFunc func)
{
	EMFormatHTMLPObject *pobj;

	if (size < sizeof (EMFormatHTMLPObject)) {
		g_warning ("size is less than the size of EMFormatHTMLPObject\n");
		size = sizeof (EMFormatHTMLPObject);
	}

	pobj = g_malloc0 (size);
	if (classid)
		pobj->classid = g_strdup (classid);
	else
		pobj->classid = g_strdup_printf("e-object:///%s", ((EMFormat *)efh)->part_id->str);

	pobj->format = efh;
	pobj->func = func;
	pobj->part = part;

	g_queue_push_tail (&efh->pending_object_list, pobj);

	return pobj;
}

EMFormatHTMLPObject *
em_format_html_find_pobject (EMFormatHTML *efh,
                             const gchar *classid)
{
	GList *link;

	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), NULL);
	g_return_val_if_fail (classid != NULL, NULL);

	link = g_queue_peek_head_link (&efh->pending_object_list);

	while (link != NULL) {
		EMFormatHTMLPObject *pw = link->data;

		if (!strcmp (pw->classid, classid))
			return pw;

		link = g_list_next (link);
	}

	return NULL;
}

EMFormatHTMLPObject *
em_format_html_find_pobject_func (EMFormatHTML *efh,
                                  CamelMimePart *part,
                                  EMFormatHTMLPObjectFunc func)
{
	GList *link;

	g_return_val_if_fail (EM_IS_FORMAT_HTML (efh), NULL);

	link = g_queue_peek_head_link (&efh->pending_object_list);

	while (link != NULL) {
		EMFormatHTMLPObject *pw = link->data;

		if (pw->func == func && pw->part == part)
			return pw;

		link = g_list_next (link);
	}

	return NULL;
}

void
em_format_html_remove_pobject (EMFormatHTML *efh,
                               EMFormatHTMLPObject *pobject)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));
	g_return_if_fail (pobject != NULL);

	g_queue_remove (&efh->pending_object_list, pobject);

	if (pobject->free != NULL)
		pobject->free (pobject);

	g_free (pobject->classid);
	g_free (pobject);
}

void
em_format_html_clear_pobject (EMFormatHTML *efh)
{
	g_return_if_fail (EM_IS_FORMAT_HTML (efh));

	while (!g_queue_is_empty (&efh->pending_object_list)) {
		EMFormatHTMLPObject *pobj;

		pobj = g_queue_pop_head (&efh->pending_object_list);
		em_format_html_remove_pobject (efh, pobj);
	}
}

struct _EMFormatHTMLJob *
em_format_html_job_new (EMFormatHTML *efh,
                        void (*callback) (struct _EMFormatHTMLJob *job,
                                          GCancellable *cancellable),
                        gpointer data)
{
	EMFormat *emf = EM_FORMAT (efh);
	struct _EMFormatHTMLJob *job = g_malloc0 (sizeof (*job));

	job->format = efh;
	job->puri_level = emf->pending_uri_level;
	job->callback = callback;
	job->u.data = data;

	if (emf->base)
		job->base = camel_url_copy (emf->base);

	return job;
}

void
em_format_html_job_queue (EMFormatHTML *efh,
                          struct _EMFormatHTMLJob *job)
{
	g_mutex_lock (efh->priv->lock);
	g_queue_push_tail (&efh->priv->pending_jobs, job);
	g_mutex_unlock (efh->priv->lock);

	/* If no formatting thread is running, then start one */
	if (efh->priv->format_id == -1) {
		struct _format_msg *m;

		d(printf("job queued, launching a new formatter thread\n"));

		m = mail_msg_new (&efh_format_info);
		m->format = g_object_ref (efh);

		mail_msg_unordered_push (m);
	} else {
		d(printf("job queued, a formatter thread already running\n"));
	}
}

/* ********************************************************************** */

static void
emfh_format_email (struct _EMFormatHTMLJob *job,
				   GCancellable *cancellable)
{
	EMFormat *format;

	d(printf(" running format_email task\n"));
	if (g_cancellable_is_cancelled (cancellable))
		return;

	format = EM_FORMAT (job->format);

	if (format->mode == EM_FORMAT_MODE_SOURCE) {
		em_format_format_source (
			format, job->stream,
			CAMEL_MIME_PART (job->u.msg), cancellable);
	} else {
		const EMFormatHandler *handle;
		const gchar *mime_type;

		mime_type = "x-evolution/message/prefix";
		handle = em_format_find_handler (format, mime_type);

		if (handle != NULL)
			handle->handler (
				format, job->stream,
				CAMEL_MIME_PART (job->u.msg), handle,
				cancellable, FALSE);

		mime_type = "x-evolution/message/rfc822";
		handle = em_format_find_handler (format, mime_type);

		if (handle != NULL)
			handle->handler (
				format, job->stream,
				CAMEL_MIME_PART (job->u.msg), handle,
				cancellable, FALSE);
	}
}

#if 0 /* WEBKIT */

static void
emfh_getpuri (struct _EMFormatHTMLJob *job,
              GCancellable *cancellable)
{
	d(printf(" running getpuri task\n"));
	if (!g_cancellable_is_cancelled (cancellable))
		job->u.puri->func (
			EM_FORMAT (job->format), job->stream,
			job->u.puri, cancellable);
}

static void
emfh_gethttp (struct _EMFormatHTMLJob *job,
              GCancellable *cancellable)
{
	CamelStream *cistream = NULL, *costream = NULL, *instream = NULL;
	CamelURL *url;
	CamelContentType *content_type;
	CamelHttpStream *tmp_stream;
	gssize n, total = 0, pc_complete = 0, nread = 0;
	gchar buffer[1500];
	const gchar *length;

	if (g_cancellable_is_cancelled (cancellable)
	    || (url = camel_url_new (job->u.uri, NULL)) == NULL)
		goto badurl;

	d(printf(" running load uri task: %s\n", job->u.uri));

	if (emfh_http_cache)
		instream = cistream = camel_data_cache_get (emfh_http_cache, EMFH_HTTP_CACHE_PATH, job->u.uri, NULL);

	if (instream == NULL) {
		EMailImageLoadingPolicy policy;
		gchar *proxy;

		policy = em_format_html_get_image_loading_policy (job->format);

		if (!(job->format->priv->load_images_now
		      || policy == E_MAIL_IMAGE_LOADING_POLICY_ALWAYS
		      || (policy == E_MAIL_IMAGE_LOADING_POLICY_SOMETIMES
			  && em_utils_in_addressbook ((CamelInternetAddress *) camel_mime_message_get_from (job->format->parent.message), FALSE)))) {
			/* TODO: Ideally we would put the http requests into another queue and only send them out
			   if the user selects 'load images', when they do.  The problem is how to maintain this
			   state with multiple renderings, and how to adjust the thread dispatch/setup routine to handle it */
			camel_url_free (url);
			goto done;
		}

		instream = camel_http_stream_new (CAMEL_HTTP_METHOD_GET, ((EMFormat *) job->format)->session, url);
		camel_http_stream_set_user_agent((CamelHttpStream *)instream, "CamelHttpStream/1.0 Evolution/" VERSION);
		proxy = em_utils_get_proxy_uri (job->u.uri);
		if (proxy) {
			camel_http_stream_set_proxy ((CamelHttpStream *) instream, proxy);
			g_free (proxy);
		}
		camel_operation_push_message (
			cancellable, _("Retrieving '%s'"), job->u.uri);
		tmp_stream = (CamelHttpStream *) instream;
		content_type = camel_http_stream_get_content_type (tmp_stream);
		length = camel_header_raw_find(&tmp_stream->headers, "Content-Length", NULL);
		d(printf("  Content-Length: %s\n", length));
		if (length != NULL)
			total = atoi (length);
		camel_content_type_unref (content_type);
	} else
		camel_operation_push_message (
			cancellable, _("Retrieving '%s'"), job->u.uri);

	camel_url_free (url);

	if (instream == NULL)
		goto done;

	if (emfh_http_cache != NULL && cistream == NULL)
		costream = camel_data_cache_add (emfh_http_cache, EMFH_HTTP_CACHE_PATH, job->u.uri, NULL);

	do {
		if (camel_operation_cancel_check (CAMEL_OPERATION (cancellable))) {
			n = -1;
			break;
		}
		/* FIXME: progress reporting in percentage, can we get the length always?  do we care? */
		n = camel_stream_read (instream, buffer, sizeof (buffer), cancellable, NULL);
		if (n > 0) {
			nread += n;
			/* If we didn't get a valid Content-Length header, do not try to calculate percentage */
			if (total != 0) {
				pc_complete = ((nread * 100) / total);
				camel_operation_progress (cancellable, pc_complete);
			}
			d(printf("  read %d bytes\n", (int)n));
			if (costream && camel_stream_write (costream, buffer, n, cancellable, NULL) == -1) {
				n = -1;
				break;
			}

			camel_stream_write (job->stream, buffer, n, cancellable, NULL);
		}
	} while (n>0);

	/* indicates success */
	if (n == 0)
		camel_stream_close (job->stream, cancellable, NULL);

	if (costream) {
		/* do not store broken files in a cache */
		if (n != 0)
			camel_data_cache_remove (emfh_http_cache, EMFH_HTTP_CACHE_PATH, job->u.uri, NULL);
		g_object_unref (costream);
	}

	g_object_unref (instream);
done:
	camel_operation_pop_message (cancellable);
badurl:
	g_free (job->u.uri);
}

#endif /* WEBKIT */

/* ********************************************************************** */
static void
efh_resource_requested (WebKitWebView *web_view, WebKitWebFrame *frame, WebKitWebResource *resource,
			WebKitNetworkRequest *request, WebKitNetworkResponse *response, gpointer user_data)
{
	EMFormatHTML *efh = user_data;
	EMFormatPURI *puri;
	const gchar *p_uri = webkit_network_request_get_uri (request);
	const gchar *uri;

	d(printf("URI requested '%s'\n", p_uri));

	if (g_str_has_prefix (p_uri, "puri:")) {
		uri = &p_uri[5];
	} else {
		uri = p_uri;
	}

	puri = em_format_find_puri (EM_FORMAT (efh), uri);
	if (puri) {
		CamelDataWrapper *dw;
		CamelContentType *ct;

		dw = camel_medium_get_content (CAMEL_MEDIUM (puri->part));
		if (!dw) {
			d(printf("PURI does not contain valid mimepart, skipping\n"));
			return;
		}

		ct = camel_data_wrapper_get_mime_type_field (dw);

		if (ct && (camel_content_type_is (ct, "text", "*")
			   || camel_content_type_is (ct, "image", "*")
			   || camel_content_type_is (ct, "application", "octet-stream"))) {

			gchar *data, *b64;
			gchar *cts = camel_data_wrapper_get_mime_type (dw);
			CamelStream *stream;
			GByteArray *ba;

			puri->use_count++;

			stream = camel_stream_mem_new ();

			camel_data_wrapper_decode_to_stream_sync (dw, stream, NULL, NULL);
			ba = camel_stream_mem_get_byte_array (CAMEL_STREAM_MEM (stream));
			b64 = g_base64_encode ((guchar*) ba->data, ba->len);
			if (camel_content_type_is (ct, "text", "*")) {
				const gchar *charset = camel_content_type_param (ct, "charset");
				data = g_strdup_printf ("data:%s;charset=%s;base64,%s", cts,
					charset ? charset : "utf-8", b64);
			} else {
				data = g_strdup_printf ("data:%s;base64,%s", cts, b64);
			}

			webkit_network_request_set_uri (request, data);
			g_free (b64);
			g_free (data);
			g_free (cts);
			g_object_unref (stream);

		} else {
			d(printf(" part is unknown type '%s', not using\n", ct ? camel_content_type_format(ct) : "<unset>"));
		}
	} else if (g_str_has_prefix(uri, "http:") || g_str_has_prefix (uri, "https:")) {
		d(printf(" Remote URI requetsed, webkit handling it\n"));
	} else if  (g_str_has_prefix (uri, "file:")) {
		gchar *data = NULL;
		gsize length = 0;
		gboolean status;
		gchar *path;

		path = g_filename_from_uri (uri, NULL, NULL);
		if (!path)
			return;

		d(printf(" Local URI requested, loading file '%s'\n", path));

		status = g_file_get_contents (path, &data, &length, NULL);
		if (status) {
			gchar *b64, *new_uri;
			gchar *ct;

			b64 = g_base64_encode ((guchar*) data, length);
			ct = g_content_type_guess (path, NULL, 0, NULL);

			new_uri =  g_strdup_printf ("data:%s;base64,%s", ct, b64);
			webkit_network_request_set_uri (request, new_uri);

			g_free (b64);
			g_free (new_uri);
			g_free (ct);
		}
		g_free (data);
		g_free (path);
	} else {
		d(printf("HTML Includes reference to unknown uri '%s'\n", uri));
	}

	g_signal_stop_emission_by_name (web_view, "resource-request-starting");
}

static GtkWidget*
efh_create_plugin_widget (WebKitWebView *web_view,
						  gchar *mime_type,
						  gchar *uri,
						  GHashTable *param,
						  gpointer user_data)
{
	EMFormatHTML *efh = user_data;
	EMFormatHTMLPObject *pobject;
	const gchar *classid;

	classid = g_hash_table_lookup (param, "data");
	if (!classid) {
		d(printf("Object does not have class-id, bailing.\n"));
		return NULL;
	}

	pobject = em_format_html_find_pobject (efh, classid);
	if (pobject) {
		GtkWidget *widget;

		d(printf("Creating widget for object '%s\n'", classid));

		/* This stops recursion of the part */
		g_queue_remove (&efh->pending_object_list, pobject);
		widget = pobject->func (efh, pobject);
		g_queue_push_head (&efh->pending_object_list, pobject);

		return widget;
	} else {
		d(printf("HTML includes reference to unknown object '%s'\n", uri));
		return NULL;
	}
}

static void
efh_webview_frame_loaded (GObject *object,
						  GParamSpec *pspec,
						  gpointer user_data)
{
	WebKitWebFrame *frame = WEBKIT_WEB_FRAME (object);
	WebKitWebView *web_view;
	const gchar *frame_name;
	gchar *script;
	GValue val = {0};

	/* Don't do anything until all content of the frame is loaded*/
	if (webkit_web_frame_get_load_status (frame) != WEBKIT_LOAD_FINISHED)
		return;

	web_view = webkit_web_frame_get_web_view (frame);
	frame_name = webkit_web_frame_get_name (frame);

	/* Get total height of the document inside the frame */
	e_web_view_frame_exec_script (E_WEB_VIEW (web_view), frame_name, "document.body.scrollHeight;", &val);

	/* Change height of the frame so that entire content is visible */
	script = g_strdup_printf ("window.document.getElementById(\"%s\").height=%d;", frame_name, (int)(g_value_get_double (&val) + 10));
	e_web_view_exec_script (E_WEB_VIEW (web_view), script, NULL);
	g_free (script);
}


static void
efh_webview_frame_created (WebKitWebView *web_view,
						   WebKitWebFrame *frame,
						   gpointer user_data)
{
	if (frame != webkit_web_view_get_main_frame (web_view)) {

		/* Get notified when all content of frame is loaded */
		g_signal_connect (frame,  "notify::load-status",
			G_CALLBACK (efh_webview_frame_loaded), NULL);
	}
}

static void
efh_headers_collapsed_state_changed (EWebView *web_view, size_t arg_count, const JSValueRef args[], gpointer user_data)
{
	EMFormatHTML *efh = user_data;
	JSGlobalContextRef ctx = e_web_view_get_global_context (web_view);

	gboolean collapsed = JSValueToBoolean (ctx, args[0]);

	if (collapsed) {
		em_format_html_set_headers_state (efh, EM_FORMAT_HTML_HEADERS_STATE_COLLAPSED);
	} else {
		em_format_html_set_headers_state (efh, EM_FORMAT_HTML_HEADERS_STATE_EXPANDED);
	}
}

static void
efh_install_js_callbacks (WebKitWebView *web_view, WebKitWebFrame *frame, gpointer context, gpointer window_object, gpointer user_data)
{
	if (frame != webkit_web_view_get_main_frame (web_view))
		return;

	e_web_view_install_js_callback (E_WEB_VIEW (web_view), "headers_collapsed",
		(EWebViewJSFunctionCallback) efh_headers_collapsed_state_changed, user_data);
}

/* ********************************************************************** */
#include "em-format/em-inline-filter.h"

/* FIXME: This is duplicated in em-format-html-display, should be exported or in security module */
static const struct {
	const gchar *icon, *shortdesc;
} smime_sign_table[5] = {
	{ "stock_signature-bad", N_("Unsigned") },
	{ "stock_signature-ok", N_("Valid signature") },
	{ "stock_signature-bad", N_("Invalid signature") },
	{ "stock_signature", N_("Valid signature, but cannot verify sender") },
	{ "stock_signature-bad", N_("Signature exists, but need public key") },
};

static const struct {
	const gchar *icon, *shortdesc;
} smime_encrypt_table[4] = {
	{ "stock_lock-broken", N_("Unencrypted") },
	{ "stock_lock", N_("Encrypted, weak"),},
	{ "stock_lock-ok", N_("Encrypted") },
	{ "stock_lock-ok", N_("Encrypted, strong") },
};

static const gchar *smime_sign_colour[4] = {
	"", " bgcolor=\"#88bb88\"", " bgcolor=\"#bb8888\"", " bgcolor=\"#e8d122\""
};

/* TODO: this could probably be virtual on em-format-html
 * then we only need one version of each type handler */
static void
efh_format_secure (EMFormat *emf,
                   CamelStream *stream,
                   CamelMimePart *part,
                   CamelCipherValidity *valid,
                   GCancellable *cancellable)
{
	EMFormatClass *format_class;

	format_class = EM_FORMAT_CLASS (parent_class);
	g_return_if_fail (format_class->format_secure != NULL);
	format_class->format_secure (emf, stream, part, valid, cancellable);

	/* To explain, if the validity is the same, then we are the
	 * base validity and now have a combined sign/encrypt validity
	 * we can display.  Primarily a new verification context is
	 * created when we have an embeded message. */
	if (emf->valid == valid
	    && (valid->encrypt.status != CAMEL_CIPHER_VALIDITY_ENCRYPT_NONE
		|| valid->sign.status != CAMEL_CIPHER_VALIDITY_SIGN_NONE)) {
		gchar *classid, *iconpath;
		const gchar *icon;
		CamelMimePart *iconpart;
		GString *buffer;

		buffer = g_string_sized_new (1024);

		g_string_append_printf (
			buffer,
			"<table border=0 width=\"100%%\" "
			"cellpadding=3 cellspacing=0%s><tr>",
			smime_sign_colour[valid->sign.status]);

		classid = g_strdup_printf (
			"smime:///em-format-html/%s/icon/signed",
			emf->part_id->str);
		g_string_append_printf (
			buffer,
			"<td valign=\"top\"><img src=\"%s\"></td>"
			"<td valign=\"top\" width=\"100%%\">", classid);

		if (valid->sign.status != 0)
			icon = smime_sign_table[valid->sign.status].icon;
		else
			icon = smime_encrypt_table[valid->encrypt.status].icon;
		iconpath = e_icon_factory_get_icon_filename (icon, GTK_ICON_SIZE_DIALOG);
		iconpart = em_format_html_file_part((EMFormatHTML *)emf, "image/png", iconpath, cancellable);
		if (iconpart) {
			(void) em_format_add_puri (emf, sizeof (EMFormatPURI), classid, iconpart, efh_write_image);
			g_object_unref (iconpart);
		}
		g_free (iconpath);
		g_free (classid);

		if (valid->sign.status != CAMEL_CIPHER_VALIDITY_SIGN_NONE) {
			gchar *signers;

			g_string_append (
				buffer, _(smime_sign_table[valid->sign.status].shortdesc));

			signers = em_format_html_format_cert_infos (
				(CamelCipherCertInfo *) valid->sign.signers.head);
			if (signers && *signers) {
				g_string_append_printf (
					buffer, " (%s)", signers);
			}
			g_free (signers);
		}

		if (valid->encrypt.status != CAMEL_CIPHER_VALIDITY_ENCRYPT_NONE) {
			if (valid->sign.status != CAMEL_CIPHER_VALIDITY_SIGN_NONE)
				g_string_append (buffer, "<br>");

			g_string_append (
				buffer, _(smime_encrypt_table[valid->encrypt.status].shortdesc));
		}

		g_string_append (buffer, "</td></tr></table>");

		camel_stream_write (
			stream, buffer->str,
			buffer->len, cancellable, NULL);

		g_string_free (buffer, TRUE);
	}
}

static void
efh_text_plain (EMFormat *emf,
                CamelStream *stream,
                CamelMimePart *part,
                const EMFormatHandler *info,
                GCancellable *cancellable,
                gboolean is_fallback)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (emf);
	CamelStream *filtered_stream;
	CamelMimeFilter *html_filter;
	CamelMultipart *mp;
	CamelDataWrapper *dw;
	CamelContentType *type;
	const gchar *format;
	guint32 flags;
	guint32 rgb;
	gint i, count, len;
	struct _EMFormatHTMLCache *efhc;

	flags = efh->text_html_flags;

	dw = camel_medium_get_content ((CamelMedium *) part);

	/* Check for RFC 2646 flowed text. */
	if (camel_content_type_is(dw->mime_type, "text", "plain")
	    && (format = camel_content_type_param(dw->mime_type, "format"))
	    && !g_ascii_strcasecmp(format, "flowed"))
		flags |= CAMEL_MIME_FILTER_TOHTML_FORMAT_FLOWED;

	/* This scans the text part for inline-encoded data, creates
	 * a multipart of all the parts inside it. */

	/* FIXME: We should discard this multipart if it only contains
	 * the original text, but it makes this hash lookup more complex */

	/* TODO: We could probably put this in the superclass, since
	 * no knowledge of html is required - but this messes with
	 * filters a bit.  Perhaps the superclass should just deal with
	 * html anyway and be done with it ... */

	efhc = g_hash_table_lookup (
		efh->priv->text_inline_parts,
		emf->part_id->str);

	if (efhc == NULL || (mp = efhc->textmp) == NULL) {
		EMInlineFilter *inline_filter;
		CamelStream *null;
		CamelContentType *ct;
		gboolean charset_added = FALSE;

		/* if we had to snoop the part type to get here, then
		 * use that as the base type, yuck */
		if (emf->snoop_mime_type == NULL
		    || (ct = camel_content_type_decode (emf->snoop_mime_type)) == NULL) {
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
		if (efhc == NULL)
			efhc = efh_insert_cache (efh, emf->part_id->str);
		efhc->textmp = mp;

		if (charset_added) {
			camel_content_type_set_param (ct, "charset", NULL);
		}

		g_object_unref (inline_filter);
		camel_content_type_unref (ct);
	}

	rgb = e_color_to_value (
		&efh->priv->colors[EM_FORMAT_HTML_COLOR_CITATION]);
	filtered_stream = camel_stream_filter_new (stream);
	html_filter = camel_mime_filter_tohtml_new (flags, rgb);
	camel_stream_filter_add (
		CAMEL_STREAM_FILTER (filtered_stream), html_filter);
	g_object_unref (html_filter);

	/* We handle our made-up multipart here, so we don't recursively call ourselves */

	len = emf->part_id->len;
	count = camel_multipart_get_number (mp);
	for (i = 0; i < count; i++) {
		CamelMimePart *newpart = camel_multipart_get_part (mp, i);

		if (!newpart)
			continue;

		type = camel_mime_part_get_content_type (newpart);
		if (camel_content_type_is (type, "text", "*") && (is_fallback || !camel_content_type_is (type, "text", "calendar"))) {
			gchar *content;

			content = g_strdup_printf (
				"<div style=\"border: solid #%06x 1px; "
				"background-color: #%06x; padding: 10px; "
				"color: #%06x;\">\n<div id=\"pre\">\n" EFH_MESSAGE_START,
				e_color_to_value (
					&efh->priv->colors[
					EM_FORMAT_HTML_COLOR_FRAME]),
				e_color_to_value (
					&efh->priv->colors[
					EM_FORMAT_HTML_COLOR_CONTENT]),
				e_color_to_value (
					&efh->priv->colors[
					EM_FORMAT_HTML_COLOR_TEXT]));
			camel_stream_write_string (
				stream, content, cancellable, NULL);
			g_free (content);

			em_format_format_text (
				emf, filtered_stream,
				(CamelDataWrapper *) newpart,
				cancellable);
			camel_stream_flush ((CamelStream *) filtered_stream, cancellable, NULL);
			camel_stream_write_string (stream, "</div>\n", cancellable, NULL);
			camel_stream_write_string (stream, "</div>\n", cancellable, NULL);
		} else {
			g_string_append_printf (emf->part_id, ".inline.%d", i);
			em_format_part (
				emf, stream, newpart, cancellable);
			g_string_truncate (emf->part_id, len);
		}
	}

	g_object_unref (filtered_stream);
}

static void
efh_text_enriched (EMFormat *emf,
                   CamelStream *stream,
                   CamelMimePart *part,
                   const EMFormatHandler *info,
                   GCancellable *cancellable,
                   gboolean is_fallback)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (emf);
	CamelStream *filtered_stream;
	CamelMimeFilter *enriched;
	guint32 flags = 0;
	gchar *content;

	if (!strcmp(info->mime_type, "text/richtext")) {
		flags = CAMEL_MIME_FILTER_ENRICHED_IS_RICHTEXT;
		camel_stream_write_string (
			stream, "\n<!-- text/richtext -->\n",
			cancellable, NULL);
	} else {
		camel_stream_write_string (
			stream, "\n<!-- text/enriched -->\n",
			cancellable, NULL);
	}

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
		(CamelDataWrapper *) part, cancellable);

	g_object_unref (filtered_stream);
	camel_stream_write_string (stream, "</div>", cancellable, NULL);
}

static void
efh_write_text_html (EMFormat *emf,
                     CamelStream *stream,
                     EMFormatPURI *puri,
                     GCancellable *cancellable)
{
#if d(!)0
	CamelStream *out;
	gint fd;
	CamelDataWrapper *dw;

	fd = dup (STDOUT_FILENO);
	out = camel_stream_fs_new_with_fd (fd);
	printf("writing text content to frame '%s'\n", puri->cid);
	dw = camel_medium_get_content (CAMEL_MEDIUM (puri->part));
	if (dw)
		camel_data_wrapper_write_to_stream_sync (dw, out, NULL, NULL);
	g_object_unref (out);
#endif
	em_format_format_text (
		emf, stream, (CamelDataWrapper *) puri->part, cancellable);
}

static void
efh_text_html (EMFormat *emf,
               CamelStream *stream,
               CamelMimePart *part,
               const EMFormatHandler *info,
               GCancellable *cancellable,
               gboolean is_fallback)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (emf);
	const gchar *location;
	gchar *cid = NULL;
	gchar *content;

	content = g_strdup_printf (
		"<div style=\"border: solid #%06x 1px; "
		"background-color: #%06x; color: #%06x;\">\n"
		"<!-- text/html -->\n" EFH_MESSAGE_START,
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

	/* TODO: perhaps we don't need to calculate this anymore now base is handled better */
	/* calculate our own location string so add_puri doesn't do it
	 * for us. our iframes are special cases, we need to use the
	 * proper base url to access them, but other children parts
	 * shouldn't blindly inherit the container's location. */
	location = camel_mime_part_get_content_location (part);
	if (location == NULL) {
		if (emf->base)
			cid = camel_url_to_string (emf->base, 0);
		else
			cid = g_strdup (emf->part_id->str);
	} else {
		if (strchr (location, ':') == NULL && emf->base != NULL) {
			CamelURL *uri;

			uri = camel_url_new_with_base (emf->base, location);
			cid = camel_url_to_string (uri, 0);
			camel_url_free (uri);
		} else {
			cid = g_strdup (location);
		}
	}

	em_format_add_puri (
		emf, sizeof (EMFormatPURI), cid,
		part, efh_write_text_html);
	d(printf("adding iframe, location %s\n", cid));
	content = g_strdup_printf (
		"<iframe name=\"html-frame-%s\" id=\"html-frame-%s\" src=\"puri:%s\" frameborder=0 scrolling=no width=\"100%%\" >" \
		"Could not get %s</iframe>\n</div>\n", cid, cid, cid, cid);
	camel_stream_write_string (stream, content, cancellable, NULL);
	g_free (content);
	g_free (cid);
}

/* This is a lot of code for something useless ... */
static void
efh_message_external (EMFormat *emf,
                      CamelStream *stream,
                      CamelMimePart *part,
                      const EMFormatHandler *info,
                      GCancellable *cancellable,
                      gboolean is_fallback)
{
	CamelContentType *type;
	const gchar *access_type;
	gchar *url = NULL, *desc = NULL;
	gchar *content;

	if (!part) {
		camel_stream_write_string (
			stream, _("Unknown external-body part."),
			cancellable, NULL);
		return;
	}

	/* needs to be cleaner */
	type = camel_mime_part_get_content_type (part);
	access_type = camel_content_type_param (type, "access-type");
	if (!access_type) {
		camel_stream_write_string (
			stream, _("Malformed external-body part."),
			cancellable, NULL);
		return;
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
			/* FIXME: use camel_isspace */
			if (!isspace ((guchar) * s))
				*d++ = *s;
			s++;
		}
		*d = 0;
		desc = g_strdup_printf (_("Pointer to remote data (%s)"), url);
	} else
		goto fail;

	content = g_strdup_printf ("<a href=\"%s\">%s</a>", url, desc);
	camel_stream_write_string (stream, content, cancellable, NULL);
	g_free (content);

	g_free (url);
	g_free (desc);

	return;

fail:
	content = g_strdup_printf (
		_("Pointer to unknown external data (\"%s\" type)"),
		access_type);
	camel_stream_write_string (stream, content, cancellable, NULL);
	g_free (content);
}

static void
efh_message_deliverystatus (EMFormat *emf,
                            CamelStream *stream,
                            CamelMimePart *part,
                            const EMFormatHandler *info,
                            GCancellable *cancellable,
                            gboolean is_fallback)
{
	EMFormatHTML *efh = EM_FORMAT_HTML (emf);
	CamelStream *filtered_stream;
	CamelMimeFilter *html_filter;
	guint32 rgb = 0x737373;
	gchar *content;

	/* Yuck, this is copied from efh_text_plain */
	content = g_strdup_printf (
		"<div style=\"border: solid #%06x 1px; "
		"background-color: #%06x; padding: 10px; "
		"color: #%06x;\">\n",
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

	filtered_stream = camel_stream_filter_new (stream);
	html_filter = camel_mime_filter_tohtml_new (efh->text_html_flags, rgb);
	camel_stream_filter_add (
		CAMEL_STREAM_FILTER (filtered_stream), html_filter);
	g_object_unref (html_filter);

	camel_stream_write_string (stream, "<div id=\"pre\">\n" EFH_MESSAGE_START, cancellable, NULL);
	em_format_format_text (
		emf, filtered_stream,
		(CamelDataWrapper *) part, cancellable);
	camel_stream_flush (filtered_stream, cancellable, NULL);
	camel_stream_write_string (stream, "</div>\n", cancellable, NULL);

	camel_stream_write_string (stream, "</div>", cancellable, NULL);
}

static void
emfh_write_related (EMFormat *emf,
                    CamelStream *stream,
                    EMFormatPURI *puri,
                    GCancellable *cancellable)
{
	em_format_format_content (emf, stream, puri->part, cancellable);

	camel_stream_close (stream, cancellable, NULL);
}

static void
emfh_multipart_related_check (struct _EMFormatHTMLJob *job,
                              GCancellable *cancellable)
{
	EMFormat *format;
	GList *link;
	gchar *oldpartid;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	format = EM_FORMAT (job->format);

	d(printf(" running multipart/related check task\n"));
	oldpartid = g_strdup (format->part_id->str);

	link = g_queue_peek_head_link (job->puri_level->data);

	if (!link) {
		g_string_printf (format->part_id, "%s", oldpartid);
		g_free (oldpartid);
		return;
	}

	while (link != NULL) {
		EMFormatPURI *puri = link->data;

		if (puri->use_count == 0) {
			d(printf("part '%s' '%s' used '%d'\n", puri->uri?puri->uri:"", puri->cid, puri->use_count));
			if (puri->func == emfh_write_related) {
				g_string_printf (format->part_id, "%s", puri->part_id);
				em_format_part (
					format, CAMEL_STREAM (job->stream),
					puri->part, cancellable);
			}
			/* else it was probably added by a previous format this loop */
		}

		link = g_list_next (link);
	}

	g_string_printf (format->part_id, "%s", oldpartid);
	g_free (oldpartid);
}

/* RFC 2387 */
static void
efh_multipart_related (EMFormat *emf,
                       CamelStream *stream,
                       CamelMimePart *part,
                       const EMFormatHandler *info,
                       GCancellable *cancellable,
                       gboolean is_fallback)
{
	CamelMultipart *mp = (CamelMultipart *) camel_medium_get_content ((CamelMedium *) part);
	CamelMimePart *body_part, *display_part = NULL;
	CamelContentType *content_type;
	const gchar *start;
	gint i, nparts, partidlen, displayid = 0;
	struct _EMFormatHTMLJob *job;

	if (!CAMEL_IS_MULTIPART (mp)) {
		em_format_format_source (emf, stream, part, cancellable);
		return;
	}

	nparts = camel_multipart_get_number (mp);
	content_type = camel_mime_part_get_content_type (part);
	start = camel_content_type_param (content_type, "start");
	if (start && strlen (start) > 2) {
		gint len;
		const gchar *cid;

		/* strip <>'s */
		len = strlen (start) - 2;
		start++;

		for (i = 0; i < nparts; i++) {
			body_part = camel_multipart_get_part (mp, i);
			cid = camel_mime_part_get_content_id (body_part);

			if (cid && !strncmp (cid, start, len) && strlen (cid) == len) {
				display_part = body_part;
				displayid = i;
				break;
			}
		}
	} else {
		display_part = camel_multipart_get_part (mp, 0);
	}

	if (display_part == NULL) {
		em_format_part_as (
			emf, stream, part,
			"multipart/mixed", cancellable);
		return;
	}

	em_format_push_level (emf);

	partidlen = emf->part_id->len;

	/* queue up the parts for possible inclusion */
	for (i = 0; i < nparts; i++) {
		body_part = camel_multipart_get_part (mp, i);
		if (body_part != display_part) {
			g_string_append_printf(emf->part_id, "related.%d", i);
			em_format_add_puri (emf, sizeof (EMFormatPURI), NULL, body_part, emfh_write_related);
			g_string_truncate (emf->part_id, partidlen);
			d(printf(" part '%s' added\n", camel_mime_part_get_content_id (body_part)));
		}
	}

	g_string_append_printf(emf->part_id, "related.%d", displayid);
	em_format_part (emf, stream, display_part, cancellable);
	g_string_truncate (emf->part_id, partidlen);
	camel_stream_flush (stream, cancellable, NULL);

	/* queue a job to check for un-referenced parts to add as attachments */
	job = em_format_html_job_new (
		EM_FORMAT_HTML (emf), emfh_multipart_related_check, NULL);
	job->stream = stream;
	g_object_ref (stream);
	em_format_html_job_queue ((EMFormatHTML *) emf, job);

	em_format_pull_level (emf);
}

static void
efh_write_image (EMFormat *emf,
                 CamelStream *stream,
                 EMFormatPURI *puri,
                 GCancellable *cancellable)
{
	CamelDataWrapper *dw = camel_medium_get_content ((CamelMedium *) puri->part);

	d(printf("writing image '%s'\n", puri->cid));
	camel_data_wrapper_decode_to_stream_sync (
		dw, stream, cancellable, NULL);
	camel_stream_close (stream, cancellable, NULL);
}

static void
efh_image (EMFormat *emf,
           CamelStream *stream,
           CamelMimePart *part,
           const EMFormatHandler *info,
           GCancellable *cancellable,
           gboolean is_fallback)
{
	EMFormatPURI *puri;
	gchar *content;

	puri = em_format_add_puri (
		emf, sizeof (EMFormatPURI), NULL, part, efh_write_image);

	content = g_strdup_printf (
		"<img hspace=10 vspace=10 src=\"%s\">", puri->cid);
	camel_stream_write_string (stream, content, cancellable, NULL);
	g_free (content);
}

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
	{ (gchar *) "image/gif", efh_image },
	{ (gchar *) "image/jpeg", efh_image },
	{ (gchar *) "image/png", efh_image },
	{ (gchar *) "image/x-png", efh_image },
	{ (gchar *) "image/x-bmp", efh_image },
	{ (gchar *) "image/bmp", efh_image },
	{ (gchar *) "image/svg", efh_image },
	{ (gchar *) "image/x-cmu-raster", efh_image },
	{ (gchar *) "image/x-ico", efh_image },
	{ (gchar *) "image/x-portable-anymap", efh_image },
	{ (gchar *) "image/x-portable-bitmap", efh_image },
	{ (gchar *) "image/x-portable-graymap", efh_image },
	{ (gchar *) "image/x-portable-pixmap", efh_image },
	{ (gchar *) "image/x-xpixmap", efh_image },
	{ (gchar *) "text/enriched", efh_text_enriched },
	{ (gchar *) "text/plain", efh_text_plain },
	{ (gchar *) "text/html", efh_text_html },
	{ (gchar *) "text/richtext", efh_text_enriched },
	{ (gchar *) "text/*", efh_text_plain },
	{ (gchar *) "message/external-body", efh_message_external },
	{ (gchar *) "message/delivery-status", efh_message_deliverystatus },
	{ (gchar *) "multipart/related", efh_multipart_related },

	/* This is where one adds those busted, non-registered types,
	 * that some idiot mailer writers out there decide to pull out
	 * of their proverbials at random. */

	{ (gchar *) "image/jpg", efh_image },
	{ (gchar *) "image/pjpeg", efh_image },

	/* special internal types */

	{ (gchar *) "x-evolution/message/rfc822", efh_format_message }
};

static void
efh_builtin_init (EMFormatHTMLClass *efhc)
{
	EMFormatClass *efc;
	gint ii;

	efc = (EMFormatClass *) efhc;

	for (ii = 0; ii < G_N_ELEMENTS (type_builtin_table); ii++)
		em_format_class_add_handler (
			efc, &type_builtin_table[ii]);
}

/* ********************************************************************** */

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
			gchar *evolution_imagesdir = g_filename_to_uri (EVOLUTION_IMAGESDIR, NULL, NULL);
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
				str = g_strdup_printf ("<img src=\"%s/plus.png\" onClick=\"collapse_addresses('%s');\" id=\"moreaddr-img-%s\" class=\"navigable\">  ",
					evolution_imagesdir, id, id);
			}

			g_free (evolution_imagesdir);
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

		buf = camel_header_unfold (header->value);
		if (!(addrs = camel_header_address_decode (buf, emf->charset ? emf->charset : emf->default_charset))) {
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
	hdr_charset = emf->charset ? emf->charset : emf->default_charset;

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
	hdr_charset = emf->charset ? emf->charset : emf->default_charset;

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
	if (emf->mode == EM_FORMAT_MODE_ALLHEADERS) {
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
		gchar *classid;
		CamelMimePart *photopart;
		gboolean only_local_photo;

		cia = camel_internet_address_new ();
		camel_address_decode ((CamelAddress *) cia, (const gchar *) photo_name);
		only_local_photo = em_format_html_get_only_local_photos (efh);
		photopart = em_utils_contact_photo (cia, only_local_photo);

		if (photopart) {
			contact_has_photo = TRUE;
			classid = g_strdup_printf (
				"icon:///em-format-html/%s/photo/header",
				emf->part_id->str);
			g_string_append_printf (
				buffer,
				"<td align=\"right\" valign=\"top\">"
				"<img width=64 src=\"%s\"></td>",
				classid);
			em_format_add_puri (emf, sizeof (EMFormatPURI), classid,
				photopart, efh_write_image);
			g_object_unref (photopart);

			g_free (classid);
		}
		g_object_unref (cia);
	}

	if (!contact_has_photo && face_decoded) {
		gchar *classid;
		CamelMimePart *part;

		part = camel_mime_part_new ();
		camel_mime_part_set_content (
			(CamelMimePart *) part,
			(const gchar *) face_header_value,
			face_header_len, "image/png");
		classid = g_strdup_printf (
			"icon:///em-format-html/face/photo/header");
		g_string_append_printf (
			buffer,
			"<td align=\"right\" valign=\"top\">"
			"<img width=48 src=\"%s\"></td>",
			classid);
		em_format_add_puri (
			emf, sizeof (EMFormatPURI),
			classid, part, efh_write_image);
		g_object_unref (part);
		g_free (classid);
		g_free (face_header_value);
	}

	if (have_icon && efh->show_icon) {
		GtkIconInfo *icon_info;
		gchar *classid;
		CamelMimePart *iconpart = NULL;

		classid = g_strdup_printf (
			"icon:///em-format-html/%s/icon/header",
			emf->part_id->str);
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
			em_format_add_puri (
				emf, sizeof (EMFormatPURI),
				classid, iconpart, efh_write_image);
			g_object_unref (iconpart);
		}
		g_free (classid);
	}

	g_string_append (buffer, "</tr></table>");
}

static void
efh_format_headers (EMFormatHTML *efh,
                    GString *buffer,
                    CamelMedium *part,
                    GCancellable *cancellable)
{
	gchar *evolution_imagesdir;

	if (!part)
		return;


	evolution_imagesdir = g_filename_to_uri (EVOLUTION_IMAGESDIR, NULL, NULL);

	g_string_append_printf (
		buffer, "<font color=\"#%06x\">\n"
		"<table border=\"0\" width=\"100%%\">"
		"<tr><td valign=\"top\" width=\"20\">",
		e_color_to_value (
			&efh->priv->colors[
			EM_FORMAT_HTML_COLOR_HEADER]));

	if (efh->priv->headers_collapsable) {
		g_string_append_printf (buffer,
			"<img src=\"%s/%s\" onClick=\"collapse_headers();\" class=\"navigable\" id=\"collapse-headers-img\" /></td><td>",
			evolution_imagesdir,
			(efh->priv->headers_state == EM_FORMAT_HTML_HEADERS_STATE_COLLAPSED) ? "plus.png" : "minus.png");

		efh_format_short_headers (efh, buffer, part,
			(efh->priv->headers_state == EM_FORMAT_HTML_HEADERS_STATE_COLLAPSED),
			cancellable);
	}

	efh_format_full_headers (efh, buffer, part,
		(efh->priv->headers_state == EM_FORMAT_HTML_HEADERS_STATE_EXPANDED),
		cancellable);

	g_string_append (buffer, "</td></tr></table></font>");

	g_free (evolution_imagesdir);
}

static void
efh_format_message (EMFormat *emf,
                    CamelStream *stream,
                    CamelMimePart *part,
                    const EMFormatHandler *info,
                    GCancellable *cancellable,
                    gboolean is_fallback)
{
	const EMFormatHandler *handle;
	GString *buffer;

	/* TODO: make this validity stuff a method */
	EMFormatHTML *efh = (EMFormatHTML *) emf;
	CamelCipherValidity *save = emf->valid, *save_parent = emf->valid_parent;

	emf->valid = NULL;
	emf->valid_parent = NULL;

	buffer = g_string_sized_new (1024);
	g_string_append_printf (buffer,
		"<!doctype html public \"-//W3C//DTD HTML 4.0 TRANSITIONAL//EN\">\n<html>\n"  \
		"<head>\n<meta name=\"generator\" content=\"Evolution Mail Component\">\n" \
		"<link type=\"text/css\" rel=\"stylesheet\" href=\"file://" EVOLUTION_PRIVDATADIR "/theme/webview.css\">\n" \
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
		"  if (f.display==\"block\") { f.display=\"none\"; s.display=\"block\";" \
		"	i.src=i.src.substr(0,i.src.lastIndexOf(\"/\"))+\"/plus.png\"; window.headers_collapsed(true, window.em_format_html); }\n" \
		"  else { f.display=\"block\"; s.display=\"none\";" \
		"	 i.src=i.src.substr(0,i.src.lastIndexOf(\"/\"))+\"/minus.png\"; window.headers_collapsed(false, window.em_format_html); }\n" \
		"}\n" \
		"</script>\n" \
		"</head>\n" \
		"<body bgcolor =\"#%06x\" text=\"#%06x\" marginwidth=6 marginheight=6 onLoad=\"body_loaded();\">",
		e_color_to_value (
			&efh->priv->colors[
			EM_FORMAT_HTML_COLOR_BODY]),
		e_color_to_value (
			&efh->priv->colors[
			EM_FORMAT_HTML_COLOR_HEADER]));

	if (emf->message != (CamelMimeMessage *) part)
		g_string_append (buffer, "<blockquote>\n");

	if (!efh->hide_headers)
		efh_format_headers (
			efh, buffer, CAMEL_MEDIUM (part), cancellable);

	camel_stream_write (
		stream, buffer->str, buffer->len, cancellable, NULL);

	g_string_free (buffer, TRUE);

	handle = em_format_find_handler(emf, "x-evolution/message/post-header");
	if (handle)
		handle->handler (
			emf, stream, part, handle, cancellable, FALSE);

	camel_stream_write_string (
		stream, EM_FORMAT_HTML_VPAD, cancellable, NULL);
	em_format_part (emf, stream, part, cancellable);

	if (emf->message != (CamelMimeMessage *) part)
		camel_stream_write_string (
			stream, "</blockquote>\n", cancellable, NULL);

	camel_stream_write_string (stream, "</body></html", cancellable, NULL);

	camel_cipher_validity_free (emf->valid);

	emf->valid = save;
	emf->valid_parent = save_parent;
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

/* unref returned pointer with g_object_unref(), if not NULL */
CamelStream *
em_format_html_get_cached_image (EMFormatHTML *efh,
                                 const gchar *image_uri)
{
	g_return_val_if_fail (efh != NULL, NULL);
	g_return_val_if_fail (image_uri != NULL, NULL);

	if (!emfh_http_cache)
		return NULL;

	return camel_data_cache_get (
		emfh_http_cache, EMFH_HTTP_CACHE_PATH, image_uri, NULL);
}

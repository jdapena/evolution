#define LIBSOUP_USE_UNSTABLE_REQUEST_API

#include "e-mail-request.h"
#include <libsoup/soup.h>
#include <glib/gi18n.h>
#include <camel/camel.h>

#include "em-format-html.h"

#define d(x)

G_DEFINE_TYPE (EMailRequest, e_mail_request, SOUP_TYPE_REQUEST)

struct _EMailRequestPrivate {
	EMFormatHTML *efh;

	CamelStream *output_stream;
	EMFormatPURI *puri;
	gchar *mime_type;

	gint content_length;

	GHashTable *uri_query;
};

static void
start_mail_formatting (GSimpleAsyncResult *res,
		       GObject *object,
		       GCancellable *cancellable)
{
	EMailRequest *request = E_MAIL_REQUEST (object);
	EMFormatHTML *efh = request->priv->efh;
	EMFormat *emf = EM_FORMAT (efh);
	GInputStream *stream;
	GByteArray *ba;
	gchar *part_id;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	if (request->priv->output_stream != NULL)
		g_object_unref (request->priv->output_stream);

	request->priv->output_stream = camel_stream_mem_new ();

	part_id = g_hash_table_lookup (request->priv->uri_query, "part_id");

	if (part_id) {
		request->priv->puri = em_format_find_puri (emf, part_id);
		if (request->priv->puri) {
			EMFormatWriterInfo info = {0};
			gchar *val;

			val = g_hash_table_lookup (request->priv->uri_query, "headers_collapsed");
			if (val)
				info.headers_collapsed = atoi (val);

			val = g_hash_table_lookup (request->priv->uri_query, "headers_collapsable");
			if (val)
				info.headers_collapsable = atoi (val);

			val = g_hash_table_lookup (request->priv->uri_query, "mode");
			if (val)
				info.mode = atoi (val);

			em_format_puri_write (request->priv->puri, request->priv->output_stream, &info, NULL);
		} else {
			g_warning ("Failed to lookup requested part '%s' - this should not happen!", part_id);
		}
	}

	/* Convert the GString to GInputStream and send it back to WebKit */
	ba = camel_stream_mem_get_byte_array (CAMEL_STREAM_MEM (request->priv->output_stream));

	if (!ba->data) {
		gchar *data = g_strdup_printf(_("Failed to load part '%s'"), part_id);
		g_byte_array_append (ba, (guchar*) data, strlen (data));
		g_free (data);
	}

	stream = g_memory_input_stream_new_from_data ((gchar*) ba->data, ba->len, NULL);
	g_simple_async_result_set_op_res_gpointer (res, stream, NULL);
}

static void
get_file_content (GSimpleAsyncResult *res,
	       	  GObject *object,
		  GCancellable *cancellable)
{
	EMailRequest *request = E_MAIL_REQUEST (object);
	SoupURI *uri;
	GInputStream *stream;
	gchar *contents;
	gsize length;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	uri = soup_request_get_uri (SOUP_REQUEST (request));

	if (g_file_get_contents (uri->path, &contents, &length, NULL)) {

		request->priv->mime_type = g_content_type_guess (uri->path, NULL, 0, NULL);
		request->priv->content_length = length;

		stream = g_memory_input_stream_new_from_data (contents, length, NULL);
		g_simple_async_result_set_op_res_gpointer (res, stream, NULL);

		/* FIXME - Freeing the data empties the result stream, but without it
		   the #contents leaks memory */
		//g_free (contents);
	}
}

static void
e_mail_request_init (EMailRequest *request)
{
	request->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		request, E_TYPE_MAIL_REQUEST, EMailRequestPrivate);

	request->priv->efh = NULL;
	request->priv->output_stream = NULL;
	request->priv->uri_query = NULL;
	request->priv->puri = NULL;
	request->priv->mime_type = NULL;
	request->priv->content_length = 0;
}

static void
mail_request_finalize (GObject *object)
{
	EMailRequest *request = E_MAIL_REQUEST (object);

	if (request->priv->output_stream) {
		g_object_unref (request->priv->output_stream);
		request->priv->output_stream = NULL;
	}

	if (request->priv->mime_type) {
		g_free (request->priv->mime_type);
		request->priv->mime_type = NULL;
	}

	if (request->priv->uri_query) {
		g_hash_table_destroy (request->priv->uri_query);
		request->priv->uri_query = NULL;
	}

	G_OBJECT_CLASS (e_mail_request_parent_class)->finalize (object);
}

static gboolean
mail_request_check_uri(SoupRequest *request,
		       SoupURI *uri,
		       GError **error)
{
	return ((strcmp (uri->scheme, "mail") == 0) ||
		(strcmp (uri->scheme, "evo-file") == 0));
}

static void
mail_request_send_async (SoupRequest *request,
			 GCancellable *cancellable,
			 GAsyncReadyCallback callback,
			 gpointer	user_data)
{
	SoupSession *session;
	EMailRequest *emr = E_MAIL_REQUEST (request);
	GSimpleAsyncResult *result;
	SoupURI *uri;

	session = soup_request_get_session (request);
	uri = soup_request_get_uri (request);

	d(printf("received request for %s\n", soup_uri_to_string (uri, FALSE)));

	if (g_strcmp0 (uri->scheme, "mail") == 0) {
		GHashTable *formatters;
		gchar *uri_str;

		if (!uri->query) {
			g_warning ("No query in URI %s", soup_uri_to_string (uri, FALSE));
			g_return_if_fail (uri->query);
		}

		formatters = g_object_get_data (G_OBJECT (session), "formatters");
		g_return_if_fail (formatters != NULL);

		uri_str = g_strdup_printf ("%s://%s%s", uri->scheme, uri->host, uri->path);
		emr->priv->efh = g_hash_table_lookup (formatters, uri_str);
		g_free (uri_str);
		g_return_if_fail (emr->priv->efh);

		emr->priv->uri_query = soup_form_decode (uri->query);
		result = g_simple_async_result_new (G_OBJECT (request), callback, user_data, mail_request_send_async);
		g_simple_async_result_run_in_thread (result, start_mail_formatting, G_PRIORITY_DEFAULT, cancellable);
	} else if (g_strcmp0 (uri->scheme, "evo-file") == 0) {
		/* WebKit won't allow us to load data through local file:// protocol, when using "remote" mail://
		   protocol, so we have evo-file:// which WebKit thinks it's remote, but in fact it behaves like file:// */
		result = g_simple_async_result_new (G_OBJECT (request), callback, user_data, mail_request_send_async);
		g_simple_async_result_run_in_thread (result, get_file_content, G_PRIORITY_DEFAULT, cancellable);
	}
}

static GInputStream*
mail_request_send_finish (SoupRequest *request,
			  GAsyncResult *result,
			  GError **error)
{
	GInputStream *stream;

	stream = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (result));
	g_object_unref (result);

	return stream;
}

static goffset
mail_request_get_content_length (SoupRequest *request)
{
	EMailRequest *emr = E_MAIL_REQUEST (request);
	GByteArray *ba;
	gint content_length = 0;

	if (emr->priv->content_length > 0)
		content_length = emr->priv->content_length;
	else if (emr->priv->output_stream) {
		ba = camel_stream_mem_get_byte_array (CAMEL_STREAM_MEM (emr->priv->output_stream));
		if (ba) {
			content_length = ba->len;
		}
	}

	d(printf("Content-Length: %d bytes\n", content_length));
	return content_length;
}

static const gchar*
mail_request_get_content_type (SoupRequest *request)
{
	EMailRequest *emr = E_MAIL_REQUEST (request);
	const gchar *mime_type;

	if (emr->priv->mime_type)
		mime_type = emr->priv->mime_type;
	else if (!emr->priv->puri)
		mime_type = "text/html";
	else if (!emr->priv->puri->mime_type) {
		CamelContentType *ct = camel_mime_part_get_content_type (emr->priv->puri->part);
		mime_type = camel_content_type_format (ct);
	} else
		mime_type = emr->priv->puri->mime_type;

	d(printf("Content-Type: %s\n", mime_type));
	return mime_type;
}

static const char *data_schemes[] = { "mail", "evo-file", NULL };

static void
e_mail_request_class_init (EMailRequestClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	SoupRequestClass *request_class = SOUP_REQUEST_CLASS (class);

	g_type_class_add_private (class, sizeof (EMailRequestPrivate));

	object_class->finalize = mail_request_finalize;

	request_class->schemes = data_schemes;
	request_class->send_async = mail_request_send_async;
	request_class->send_finish = mail_request_send_finish;
	request_class->get_content_type = mail_request_get_content_type;
	request_class->get_content_length = mail_request_get_content_length;
	request_class->check_uri = mail_request_check_uri;
}

#define LIBSOUP_USE_UNSTABLE_REQUEST_API

#include "e-mail-request.h"
#include <libsoup/soup.h>
#include <glib/gi18n.h>
#include <camel/camel.h>

#include "em-format-html.h"


G_DEFINE_TYPE (EMailRequest, e_mail_request, SOUP_TYPE_REQUEST)

struct _EMailRequestPrivate {
	EMFormatHTML *efh;
	CamelMimePart *part;

	CamelStream *output_stream;

	gchar *content_type;

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
		CamelContentType *ct;
		EMFormatPURI *puri;

		puri = em_format_find_puri (emf, part_id);
		if (puri) {
			request->priv->part = puri->part;
			ct = camel_mime_part_get_content_type (request->priv->part);
			if (ct) {
				request->priv->content_type = camel_content_type_format (ct);
			camel_content_type_unref (ct);
			} else {
				request->priv->content_type = g_strdup ("text/html");
			}
			em_format_html_format_message_part (efh, part_id, request->priv->output_stream, cancellable);
		}
	} else {
		request->priv->content_type = g_strdup ("text/html");
		request->priv->part = g_object_ref (CAMEL_MIME_PART (emf->message));
		em_format_html_format_message (efh, request->priv->output_stream, cancellable);
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
get_image_content (GSimpleAsyncResult *res,
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
		request->priv->content_type = g_content_type_guess (uri->path, NULL, 0, NULL);
		stream = g_memory_input_stream_new_from_data (contents, length, NULL);
		g_simple_async_result_set_op_res_gpointer (res, stream, NULL);
	}
}

static void
e_mail_request_init (EMailRequest *request)
{
	request->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		request, E_TYPE_MAIL_REQUEST, EMailRequestPrivate);

	request->priv->efh = NULL;
	request->priv->part = NULL;
	request->priv->output_stream = NULL;
	request->priv->uri_query = NULL;
}

static void
mail_request_finalize (GObject *object)
{
	EMailRequest *request = E_MAIL_REQUEST (object);

	if (request->priv->output_stream) {
		g_object_unref (request->priv->output_stream);
		request->priv->output_stream = NULL;
	}

	if (request->priv->content_type) {
		g_free (request->priv->content_type);
		request->priv->content_type = NULL;
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
	EMailRequest *emr = E_MAIL_REQUEST (request);
	GSimpleAsyncResult *result;
	SoupURI *uri;

	uri = soup_request_get_uri (request);

	if (g_strcmp0 (uri->scheme, "mail") == 0) {
		gchar *formatter;
		emr->priv->uri_query = soup_form_decode (uri->query);

		formatter = g_hash_table_lookup (emr->priv->uri_query, "formatter");

		emr->priv->efh = GINT_TO_POINTER (atoi (formatter));
		g_return_if_fail (EM_IS_FORMAT (emr->priv->efh));

		result = g_simple_async_result_new (G_OBJECT (request), callback, user_data, mail_request_send_async);
		g_simple_async_result_run_in_thread (result, start_mail_formatting, G_PRIORITY_DEFAULT, cancellable);
	} else if (g_strcmp0 (uri->scheme, "evo-file") == 0) {
		/* WebKit won't allow us to load data through local file:// protocol, when using "remote" mail://
		   protocol. evo-file:// behaves as file:// */
		result = g_simple_async_result_new (G_OBJECT (request), callback, user_data, mail_request_send_async);
		g_simple_async_result_run_in_thread (result, get_image_content, G_PRIORITY_DEFAULT, cancellable);
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

	if (emr->priv->output_stream) {
		ba = camel_stream_mem_get_byte_array (CAMEL_STREAM_MEM (emr->priv->output_stream));
		if (ba) {
			return ba->len;
		}
	}

	return 0;
}

static const gchar*
mail_request_get_content_type (SoupRequest *request)
{
	EMailRequest *emr = E_MAIL_REQUEST (request);

	return emr->priv->content_type;
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

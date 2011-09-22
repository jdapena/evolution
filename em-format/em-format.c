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
 *      Jeffrey Stedfast <fejj@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <gio/gio.h>
#include <glib/gi18n-lib.h>

#include "em-format.h"
#include "e-util/e-util.h"
#include "shell/e-shell.h"
#include "shell/e-shell-settings.h"

#define d(x)

struct _EMFormatPrivate {
	GNode *current_node;

	CamelSession *session;

	CamelURL *base_url;

	gchar *charset;
	gchar *default_charset;
	gboolean composer;

	gint last_error;
};

enum {
	PROP_0,
	PROP_CHARSET,
	PROP_DEFAULT_CHARSET,
	PROP_COMPOSER,
	PROP_BASE_URL
};

static gpointer parent_class;

/* PARSERS */
static void emf_parse_application_xpkcs7mime	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_application_mbox		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_multipart_alternative	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_multipart_appledouble	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_multipart_encrypted	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_multipart_mixed		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_multipart_signed		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_multipart_related		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_multipart_digest		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_message_deliverystatus	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_inlinepgp_signed		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_inlinepgp_encrypted	(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_message			(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_headers			(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_post_headers		(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);
static void emf_parse_source			(EMFormat *emf, CamelMimePart *part, GString *part_id, EMFormatParserInfo *info, GCancellable *cancellable);

/* WRITERS */
static void emf_write_text			(EMFormat *emf, EMFormatPURI *puri, CamelStream *stream, GCancellable *cancellable) {};
static void emf_write_source			(EMFormat *emf, EMFormatPURI *puri, CamelStream *stream, GCancellable *cancellable) {}

static void emf_error				(EMFormat *emf, const gchar *message) {};
static void emf_source				(EMFormat *emf, CamelStream *stream, GCancellable *cancellable);

/**************************************************************************/

static void
preserve_charset_in_content_type (CamelMimePart *ipart,
                                  CamelMimePart *opart)
{
	CamelDataWrapper *data_wrapper;
	CamelContentType *content_type;
	const gchar *charset;

	g_return_if_fail (ipart != NULL);
	g_return_if_fail (opart != NULL);

	data_wrapper = camel_medium_get_content (CAMEL_MEDIUM (ipart));
	content_type = camel_data_wrapper_get_mime_type_field (data_wrapper);

	if (content_type == NULL)
		return;

	charset = camel_content_type_param (content_type, "charset");

	if (charset == NULL || *charset == '\0')
		return;

	data_wrapper = camel_medium_get_content (CAMEL_MEDIUM (opart));
	content_type = camel_data_wrapper_get_mime_type_field (data_wrapper);

	camel_content_type_set_param (content_type, "charset", charset);
}

static CamelMimePart *
get_related_display_part (CamelMimePart *part,
  			  gint *out_displayid)
{
	CamelMultipart *mp;
	CamelMimePart *body_part, *display_part = NULL;
	CamelContentType *content_type;
	const gchar *start;
	gint i, nparts, displayid = 0;

	mp = (CamelMultipart *) camel_medium_get_content ((CamelMedium *) part);

	if (!CAMEL_IS_MULTIPART (mp))
		return NULL;

	nparts = camel_multipart_get_number (mp);
	content_type = camel_mime_part_get_content_type (part);
	start = camel_content_type_param (content_type, "start");
	if (start && strlen (start) > 2) {
		gint len;
		const gchar *cid;

		/* strip <>'s from CID */
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

	if (out_displayid)
		*out_displayid = displayid;

	return display_part;
}

static gboolean
related_display_part_is_attachment (EMFormat *emf,
				    CamelMimePart *part)
{
	CamelMimePart *display_part;

	display_part = get_related_display_part (part, NULL);
	return display_part && em_format_is_attachment (emf, display_part);
}

/**************************************************************************/
void
em_format_empty_parser (EMFormat *emf,
			CamelMimePart *part,
			GString *part_id,
			EMFormatParserInfo *info,
			GCancellable *cancellable)
{
	/* DO NOTHING */
}

#ifdef ENABLE_SMIME
static void
emf_parse_application_xpkcs7mime (EMFormat *emf,
				  CamelMimePart *part,
				  GString *part_id,
				  EMFormatParserInfo *info,
				  GCancellable *cancellable)
{
	CamelCipherContext *context;
	CamelMimePart *opart;
	CamelCipherValidity *valid;
	GError *local_error = NULL;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	context = camel_smime_context_new (emf->priv->session);

	opart = camel_mime_part_new ();
	valid = camel_cipher_context_decrypt_sync (
		context, part, opart, cancellable, &local_error);
	preserve_charset_in_content_type (part, opart);
	if (valid == NULL) {
		em_format_format_error (
			emf, "%s",
			local_error->message ? local_error->message :
			_("Could not parse S/MIME message: Unknown error"));
		g_clear_error (&local_error);

		em_format_parse_part_as (emf, part, part_id, info, NULL, cancellable);
	} else {
		EMFormatParserInfo encinfo = {
				info->handler,
				EM_FORMAT_VALIDITY_FOUND_ENCRYPTED | EM_FORMAT_VALIDITY_FOUND_SMIME,
				valid
		};
		gint len = part_id->len;

		g_string_append (part_id, ".encrypted");
		em_format_parse_part (emf, opart, part_id, &encinfo, cancellable);
		g_string_truncate (part_id, len);

		/* Add a widget with details about the encryption */
		g_string_append (part_id, ".encrypted.button");
		em_format_parse_part_as (emf, part, part_id, &encinfo,
			"x-evolution/message/x-secure-button", cancellable);
		g_string_truncate (part_id, len);

		camel_cipher_validity_free (valid);
	}

	g_object_unref (opart);
	g_object_unref (context);
}
#endif

/* RFC 4155 */
static void
emf_parse_application_mbox (EMFormat *emf,
                      	    CamelMimePart *mime_part,
                      	    GString *part_id,
                      	    EMFormatParserInfo *info,
                      	    GCancellable *cancellable)
{
	CamelMimeParser *parser;
	CamelStream *mem_stream;
	camel_mime_parser_state_t state;
	gint old_len;
	gint messages;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	/* Extract messages from the application/mbox part and
	 * render them as a flat list of messages. */

	/* XXX If the mbox has multiple messages, maybe render them
	 *     as a multipart/digest so each message can be expanded
	 *     or collapsed individually.
	 *
	 *     See attachment_handler_mail_x_uid_list() for example. */

	/* XXX This is based on em_utils_read_messages_from_stream().
	 *     Perhaps refactor that function to return an array of
	 *     messages instead of assuming we want to append them
	 *     to a folder? */

	parser = camel_mime_parser_new ();
	camel_mime_parser_scan_from (parser, TRUE);

	mem_stream = camel_stream_mem_new ();
	camel_data_wrapper_decode_to_stream_sync (
		camel_medium_get_content (CAMEL_MEDIUM (mime_part)),
		mem_stream, NULL, NULL);
	g_seekable_seek (G_SEEKABLE (mem_stream), 0, G_SEEK_SET, NULL, NULL);
	camel_mime_parser_init_with_stream (parser, mem_stream, NULL);
	g_object_unref (mem_stream);

	old_len = part_id->len;

	/* Extract messages from the mbox. */
	messages = 0;
	state = camel_mime_parser_step (parser, NULL, NULL);

	while (state == CAMEL_MIME_PARSER_STATE_FROM) {
		CamelMimeMessage *message;

		message = camel_mime_message_new ();
		mime_part = CAMEL_MIME_PART (message);

		if (!camel_mime_part_construct_from_parser_sync (
			mime_part, parser, NULL, NULL)) {
			g_object_unref (message);
			break;
		}

		g_string_append_printf (part_id, ".mbox.%d", messages);
		em_format_parse_part (emf, CAMEL_MIME_PART (message),
				part_id, info, cancellable);
		g_string_truncate (part_id, old_len);

		g_object_unref (message);

		/* Skip past CAMEL_MIME_PARSER_STATE_FROM_END. */
		camel_mime_parser_step (parser, NULL, NULL);

		state = camel_mime_parser_step (parser, NULL, NULL);

		messages++;
	}

	g_object_unref (parser);
}

/* RFC 1740 */
static void
emf_parse_multipart_alternative (EMFormat *emf,
                           	 CamelMimePart *part,
	                         GString *part_id,
    	                       	 EMFormatParserInfo *info,
        	                 GCancellable *cancellable)
{
	CamelMultipart *mp;
	gint i, nparts, bestid = 0;
	CamelMimePart *best = NULL;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	mp = (CamelMultipart *) camel_medium_get_content ((CamelMedium *) part);

	if (!CAMEL_IS_MULTIPART (mp)) {
		emf_parse_source (emf, part, part_id, info, cancellable);
		return;
	}

	/* as per rfc, find the last part we know how to display */
	nparts = camel_multipart_get_number (mp);
	for (i = 0; i < nparts; i++) {
		CamelMimePart *mpart;
		CamelDataWrapper *data_wrapper;
		CamelContentType *type;
		CamelStream *null_stream;
		gchar *mime_type;
		gsize content_size;
		/* GByteArray *ba; */

		if (g_cancellable_is_cancelled (cancellable))
			return;

		/* is it correct to use the passed in *part here? */
		mpart = camel_multipart_get_part (mp, i);

		if (mpart == NULL)
			continue;

		/* This may block even though the stream does not.
		 * XXX Pretty inefficient way to test if the MIME part
		 *     is empty.  Surely there's a quicker way? */
		null_stream = camel_stream_null_new ();
		data_wrapper = camel_medium_get_content (CAMEL_MEDIUM (mpart));
		camel_data_wrapper_decode_to_stream_sync (
			data_wrapper, null_stream, cancellable, NULL);
		content_size = CAMEL_STREAM_NULL (null_stream)->written;
		g_object_unref (null_stream);

		if (content_size == 0)
			continue;

		/* FIXME WEBKIT: This SHOULD do the same as the previous block of
		 * code, thought it seems to somehow corrupt/disable/remove the
		 * other mime part, so that has no content
		data_wrapper = camel_medium_get_content ((CamelMedium *) mpart);
		ba = camel_data_wrapper_get_byte_array (data_wrapper);
		if (ba->len == 0)
			continue;
		*/

		type = camel_mime_part_get_content_type (mpart);
		mime_type = camel_content_type_simple (type);

		camel_strdown (mime_type);

		if (!em_format_is_attachment (emf, mpart) &&
			 ((camel_content_type_is (type, "multipart", "related") == 0) ||
		       !related_display_part_is_attachment (emf, mpart)) &&
		     (em_format_find_handler (emf, mime_type)
		       || (best == NULL && em_format_fallback_handler (emf, mime_type)))) {
			best = mpart;
			bestid = i;
		}

		g_free (mime_type);
	}

	if (best) {
		gint len = part_id->len;

		g_string_append_printf(part_id, ".alternative.%d", bestid);
		em_format_parse_part (emf, best, part_id, info, cancellable);
		g_string_truncate (part_id, len);
	} else
		emf_parse_multipart_mixed (emf, part, part_id, info, cancellable);
}

/* RFC 1740 */
static void
emf_parse_multipart_appledouble (EMFormat *emf,
                           	 CamelMimePart *part,
	                         GString *part_id,
    	                      	 EMFormatParserInfo *info,
        	                 GCancellable *cancellable)
{
	CamelMultipart *mp;
	CamelMimePart *mime_part;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	mp = (CamelMultipart *) camel_medium_get_content ((CamelMedium *) part);

	if (!CAMEL_IS_MULTIPART (mp)) {
		emf_parse_source (emf, part, part_id, info, cancellable);
		return;
	}

	mime_part = camel_multipart_get_part (mp, 1);
	if (mime_part) {
		gint len;
		/* try the data fork for something useful, doubtful but who knows */
		len = part_id->len;
		g_string_append_printf(part_id, ".appledouble.1");
		em_format_parse_part (emf, mime_part, part_id, info, cancellable);
		g_string_truncate (part_id, len);
	} else {
		emf_parse_source (emf, part, part_id, info, cancellable);
	}
}

static void
emf_parse_multipart_encrypted (EMFormat *emf,
			       CamelMimePart *part,
                               GString *part_id,
			       EMFormatParserInfo *info,
			       GCancellable *cancellable)
{
	CamelCipherContext *context;
	const gchar *protocol;
	CamelMimePart *opart;
	CamelCipherValidity *valid;
	CamelMultipartEncrypted *mpe;
	GError *local_error = NULL;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	/* should this perhaps run off a key of ".secured" ? */
	/* FIXME WEBKIT what do we do about this?
	emfc = g_hash_table_lookup (emf->inline_table, emf->part_id->str);
	if (emfc && emfc->valid) {
		em_format_format_secure (
			emf, stream, emfc->secured,
			camel_cipher_validity_clone (emfc->valid),
			cancellable);
		return;
	}
	*/

	mpe = (CamelMultipartEncrypted*) camel_medium_get_content ((CamelMedium *) part);
	if (!CAMEL_IS_MULTIPART_ENCRYPTED (mpe)) {
		em_format_format_error (
			emf, _("Could not parse MIME message. "
			"Displaying as source."));
		emf_parse_source (emf, part, part_id, info, cancellable);
		return;
	}

	/* Currently we only handle RFC2015-style PGP encryption. */
	protocol = camel_content_type_param (
		((CamelDataWrapper *)mpe)->mime_type, "protocol");
	if (!protocol || g_ascii_strcasecmp (protocol, "application/pgp-encrypted") != 0) {
		em_format_format_error (emf, _("Unsupported encryption type for multipart/encrypted"));
		emf_parse_multipart_mixed (emf, part, part_id, info, cancellable);
		return;
	}

	context = camel_gpg_context_new (emf->priv->session);
	opart = camel_mime_part_new ();
	valid = camel_cipher_context_decrypt_sync (
		context, part, opart, cancellable, &local_error);
	preserve_charset_in_content_type (part, opart);
	if (valid == NULL) {
		em_format_format_error (
			emf, local_error->message ?
			_("Could not parse PGP/MIME message") :
			_("Could not parse PGP/MIME message: Unknown error"));
		if (local_error->message != NULL)
			em_format_format_error (
				emf, "%s", local_error->message);
		g_clear_error (&local_error);
		emf_parse_multipart_mixed (emf, part, part_id, info, cancellable);
	} else {
		EMFormatParserInfo encinfo = {
				info->handler,
				EM_FORMAT_VALIDITY_FOUND_ENCRYPTED | EM_FORMAT_VALIDITY_FOUND_PGP,
				camel_cipher_validity_clone (valid), };
		gint len = part_id->len;

		g_string_append (part_id, ".encrypted");
		em_format_parse_part (emf, opart, part_id, &encinfo, cancellable);
		g_string_truncate (part_id, len);

		/* Add a widget with details about the encryption */
		g_string_append (part_id, ".encrypted.button");
		em_format_parse_part_as (emf, part, part_id, &encinfo,
			"x-evolution/message/x-secure-button", cancellable);
		g_string_truncate (part_id, len);

		camel_cipher_validity_free (valid);
	}

	/* TODO: Make sure when we finalize this part, it is zero'd out */
	g_object_unref (opart);
	g_object_unref (context);
}

/* RFC 2046 */
static void
emf_parse_multipart_mixed (EMFormat *emf,
                     	   CamelMimePart *part,
                     	   GString *part_id,
                     	   EMFormatParserInfo *info,
                     	   GCancellable *cancellable)
{
	CamelMultipart *mp;
	gint i, nparts, len;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	mp = (CamelMultipart *) camel_medium_get_content ((CamelMedium *) part);

	if (!CAMEL_IS_MULTIPART (mp)) {
		emf_parse_source (emf, part, part_id, info, cancellable);
		return;
	}

	len = part_id->len;
	nparts = camel_multipart_get_number (mp);
	for (i = 0; i < nparts; i++) {
		CamelMimePart *subpart;

		subpart = camel_multipart_get_part (mp, i);

		g_string_append_printf(part_id, ".mixed.%d", i);
		em_format_parse_part (emf, subpart, part_id, info, cancellable);
		g_string_truncate (part_id, len);
	}
}

static void
emf_parse_multipart_signed (EMFormat *emf,
                      	    CamelMimePart *part,
                      	    GString *part_id,
                      	    EMFormatParserInfo *info,
                      	    GCancellable *cancellable)
{
	CamelMimePart *cpart;
	CamelMultipartSigned *mps;
	CamelCipherContext *cipher = NULL;
	guint32 validity_type;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	/* should this perhaps run off a key of ".secured" ? */
	/* FIXME WEBKIT: What do we do with this?
	emfc = g_hash_table_lookup (emf->inline_table, emf->part_id->str);
	if (emfc && emfc->valid) {
		em_format_format_secure (
			emf, stream, emfc->secured,
			camel_cipher_validity_clone (emfc->valid),
			cancellable);
		return;
	}
	*/

	mps = (CamelMultipartSigned *) camel_medium_get_content ((CamelMedium *) part);
	if (!CAMEL_IS_MULTIPART_SIGNED (mps)
	    || (cpart = camel_multipart_get_part ((CamelMultipart *) mps,
		CAMEL_MULTIPART_SIGNED_CONTENT)) == NULL) {
		em_format_format_error (
			emf, _("Could not parse MIME message. "
			"Displaying as source."));
		emf_parse_source (emf, part, part_id, info, cancellable);
		return;
	}

	/* FIXME: Should be done via a plugin interface */
	/* FIXME: duplicated in em-format-html-display.c */
	if (mps->protocol) {
#ifdef ENABLE_SMIME
		if (g_ascii_strcasecmp("application/x-pkcs7-signature", mps->protocol) == 0
		    || g_ascii_strcasecmp("application/pkcs7-signature", mps->protocol) == 0) {
			cipher = camel_smime_context_new (emf->priv->session);
			validity_type = EM_FORMAT_VALIDITY_FOUND_SMIME;
		} else
#endif
			if (g_ascii_strcasecmp("application/pgp-signature", mps->protocol) == 0) {
				cipher = camel_gpg_context_new (emf->priv->session);
				validity_type = EM_FORMAT_VALIDITY_FOUND_PGP;
			}
	}

	if (cipher == NULL) {
		em_format_format_error(emf, _("Unsupported signature format"));
		emf_parse_multipart_mixed (emf, part, part_id, info, cancellable);
	} else {
		CamelCipherValidity *valid;
		GError *local_error = NULL;

		valid = camel_cipher_context_verify_sync (
			cipher, part, cancellable, &local_error);
		if (valid == NULL) {
			em_format_format_error (
				emf, local_error->message ?
				_("Error verifying signature") :
				_("Unknown error verifying signature"));
			if (local_error->message != NULL)
				em_format_format_error (
					emf, "%s",
					local_error->message);
			g_clear_error (&local_error);
			emf_parse_multipart_mixed (emf, part, part_id,info,  cancellable);
		} else {
			EMFormatParserInfo signinfo = {
					info->handler,
					validity_type | EM_FORMAT_VALIDITY_FOUND_SIGNED,
					valid
			};

			gint i, nparts, len = part_id->len;
			nparts = camel_multipart_get_number (CAMEL_MULTIPART (mps));
			for (i = 0; i < nparts; i++) {
				CamelMimePart *subpart;
				subpart = camel_multipart_get_part (CAMEL_MULTIPART (mps), i);

				g_string_append_printf(part_id, ".signed.%d", i);
				em_format_parse_part (emf, subpart, part_id, &signinfo, cancellable);
				g_string_truncate (part_id, len);
			}

			/* Add a widget with details about the encryption */
			g_string_append (part_id, ".signed.button");
			em_format_parse_part_as (emf, part, part_id, &signinfo,
			"x-evolution/message/x-secure-button", cancellable);
			g_string_truncate (part_id, len);

			camel_cipher_validity_free (valid);
		}
	}

	g_object_unref (cipher);
}


/* RFC 2046 */
static void
emf_parse_multipart_digest (EMFormat *emf,
                     	    CamelMimePart *part,
                     	    GString *part_id,
                     	    EMFormatParserInfo *info,
                     	    GCancellable *cancellable)
{
	CamelMultipart *mp;
	gint i, nparts, len;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	mp = (CamelMultipart *) camel_medium_get_content ((CamelMedium *) part);

	if (!CAMEL_IS_MULTIPART (mp)) {
		emf_parse_source (emf, part, part_id, info, cancellable);
		return;
	}

	len = part_id->len;
	nparts = camel_multipart_get_number (mp);
	for (i = 0; i < nparts; i++) {
		CamelMimePart *subpart;
		CamelContentType *ct;
		gchar *cts;
		const EMFormatHandler *handler;

		subpart = camel_multipart_get_part (mp, i);

		if (!subpart)
			continue;

		g_string_append_printf(part_id, ".digest.%d", i);

		ct = camel_mime_part_get_content_type (subpart);
		/* According to RFC this shouldn't happen, but who knows... */
		if (ct && !camel_content_type_is (ct, "message", "rfc822")) {
			cts = camel_content_type_simple (ct);
			em_format_parse_part_as (emf, part, part_id, info, cts, cancellable);
			g_free (cts);
			g_string_truncate (part_id, len);
			continue;
		}

		handler = em_format_find_handler (emf, "message/rfc822");
		handler->parse_func (emf, subpart, part_id, info, cancellable);

		g_string_truncate (part_id, len);
	}
}

/* RFC 2387 */
static void
emf_parse_multipart_related (EMFormat *emf,
                       	     CamelMimePart *part,
                       	     GString *part_id,
                       	     EMFormatParserInfo *info,
                       	     GCancellable *cancellable)
{
	CamelMultipart *mp;
	CamelMimePart *body_part, *display_part = NULL;
	gint i, nparts, partidlen, displayid = 0;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	mp = (CamelMultipart *) camel_medium_get_content ((CamelMedium *) part);

	if (!CAMEL_IS_MULTIPART (mp)) {
		emf_parse_source (emf, part, part_id, info, cancellable);
		return;
	}

	display_part = get_related_display_part (part, &displayid);

	if (display_part == NULL) {
		emf_parse_multipart_mixed (
			emf, part, part_id, info, cancellable);
		return;
	}

	/* The to-be-displayed part goes first */
	partidlen = part_id->len;
	g_string_append_printf(part_id, ".related.%d", displayid);
	em_format_parse_part (emf, display_part, part_id, info, cancellable);
	g_string_truncate (part_id, partidlen);

	/* Process the related parts */
	nparts = camel_multipart_get_number (mp);
	for (i = 0; i < nparts; i++) {
		body_part = camel_multipart_get_part (mp, i);
		if (body_part != display_part) {
			g_string_append_printf(part_id, ".related.%d", i);
			em_format_parse_part (emf, body_part, part_id, info, cancellable);
			g_string_truncate (part_id, partidlen);
		}
	}
}



static void
emf_parse_message_deliverystatus (EMFormat *emf,
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
	puri->write_func = emf_write_text;
	puri->mime_type = g_strdup ("text/html");

	g_string_truncate (part_id, len);

	em_format_add_puri (emf, puri);
}

static void
emf_parse_inlinepgp_signed (EMFormat *emf,
                      	    CamelMimePart *ipart,
                      	    GString *part_id,
			    EMFormatParserInfo *info,
			    GCancellable *cancellable)
{
	CamelStream *filtered_stream;
	CamelMimeFilterPgp *pgp_filter;
	CamelContentType *content_type;
	CamelCipherContext *cipher;
	CamelCipherValidity *valid;
	CamelDataWrapper *dw;
	CamelMimePart *opart;
	CamelStream *ostream;
	gchar *type;
	gint len;
	GError *local_error = NULL;
	EMFormatParserInfo signinfo;
	GByteArray *ba;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	if (!ipart) {
		em_format_format_error(emf, _("Unknown error verifying signature"));
		return;
	}

	cipher = camel_gpg_context_new (emf->priv->session);
	/* Verify the signature of the message */
	valid = camel_cipher_context_verify_sync (
		cipher, ipart, cancellable, &local_error);
	if (!valid) {
		em_format_format_error (
			emf, local_error->message ?
			_("Error verifying signature") :
			_("Unknown error verifying signature"));
		if (local_error->message)
			em_format_format_error (
				emf, "%s", local_error->message);
		emf_parse_source (emf, ipart, part_id, info, cancellable);
		/* XXX I think this will loop:
		 * em_format_part_as(emf, stream, part, "text/plain"); */
		g_clear_error (&local_error);
		g_object_unref (cipher);
		return;
	}

	/* Setup output stream */
	ostream = camel_stream_mem_new ();
	filtered_stream = camel_stream_filter_new (ostream);

	/* Add PGP header / footer filter */
	pgp_filter = (CamelMimeFilterPgp *) camel_mime_filter_pgp_new ();
	camel_stream_filter_add (
		CAMEL_STREAM_FILTER (filtered_stream),
		CAMEL_MIME_FILTER (pgp_filter));
	g_object_unref (pgp_filter);

	/* Pass through the filters that have been setup */
	dw = camel_medium_get_content ((CamelMedium *) ipart);
	camel_data_wrapper_decode_to_stream_sync (
		dw, (CamelStream *) filtered_stream, cancellable, NULL);
	camel_stream_flush ((CamelStream *) filtered_stream, cancellable, NULL);
	g_object_unref (filtered_stream);

	/* Create a new text/plain MIME part containing the signed
	 * content preserving the original part's Content-Type params. */
	content_type = camel_mime_part_get_content_type (ipart);
	type = camel_content_type_format (content_type);
	content_type = camel_content_type_decode (type);
	g_free (type);

	g_free (content_type->type);
	content_type->type = g_strdup ("text");
	g_free (content_type->subtype);
	content_type->subtype = g_strdup ("plain");
	type = camel_content_type_format (content_type);
	camel_content_type_unref (content_type);

	ba = camel_stream_mem_get_byte_array ((CamelStreamMem *) ostream);
	opart = camel_mime_part_new ();
	camel_mime_part_set_content (opart, (gchar *) ba->data, ba->len, type);
	g_free (type);

	/* Pass it off to the real formatter */
	len = part_id->len;
	g_string_append (part_id, ".inlinepgp_signed");
	signinfo.handler = info->handler;
	signinfo.validity_type = EM_FORMAT_VALIDITY_FOUND_SIGNED | EM_FORMAT_VALIDITY_FOUND_PGP;
	signinfo.validity = valid;
	em_format_parse_part (emf, opart, part_id, &signinfo, cancellable);
	g_string_truncate (part_id, len);

	/* Add a widget with details about the encryption */
	g_string_append (part_id, ".inlinepgp_signed.button");
	em_format_parse_part_as (emf, opart, part_id, &signinfo,
		"x-evolution/message/x-secure-button", cancellable);
	g_string_truncate (part_id, len);

	/* Clean Up */
	camel_cipher_validity_free (valid);
	g_object_unref (dw);
	g_object_unref (opart);
	g_object_unref (ostream);
	g_object_unref (cipher);
}

static void
emf_parse_inlinepgp_encrypted (EMFormat *emf,
			       CamelMimePart *ipart,
			       GString *part_id,
			       EMFormatParserInfo *info,
			       GCancellable *cancellable)
{
	CamelCipherContext *cipher;
	CamelCipherValidity *valid;
	CamelMimePart *opart;
	CamelDataWrapper *dw;
	gchar *mime_type;
	gint len;
	GError *local_error = NULL;
	EMFormatParserInfo encinfo;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	cipher = camel_gpg_context_new (emf->priv->session);
	opart = camel_mime_part_new ();

	/* Decrypt the message */
	valid = camel_cipher_context_decrypt_sync (
		cipher, ipart, opart, cancellable, &local_error);

	if (!valid) {
		em_format_format_error (
			emf, _("Could not parse PGP message: "));
		if (local_error->message != NULL)
			em_format_format_error (
				emf, "%s", local_error->message);
		else
			em_format_format_error (
				emf, _("Unknown error"));
		emf_parse_source (emf, ipart, part_id, info, cancellable);
		/* XXX I think this will loop:
		 * em_format_part_as(emf, stream, part, "text/plain"); */

		g_clear_error (&local_error);
		g_object_unref (cipher);
		g_object_unref (opart);
		return;
	}

	dw = camel_medium_get_content ((CamelMedium *) opart);
	mime_type = camel_data_wrapper_get_mime_type (dw);

	/* this ensures to show the 'opart' as inlined, if possible */
	if (mime_type && g_ascii_strcasecmp (mime_type, "application/octet-stream") == 0) {
		const gchar *snoop = em_format_snoop_type (opart);

		if (snoop)
			camel_data_wrapper_set_mime_type (dw, snoop);
	}

	preserve_charset_in_content_type (ipart, opart);
	g_free (mime_type);

	/* Pass it off to the real formatter */
	len = part_id->len;
	g_string_append (part_id, ".inlinepgp_encrypted");
	encinfo.handler = info->handler;
	encinfo.validity_type = EM_FORMAT_VALIDITY_FOUND_ENCRYPTED | EM_FORMAT_VALIDITY_FOUND_PGP;
	encinfo.validity = valid;
	em_format_parse_part (emf, opart, part_id, &encinfo, cancellable);
	g_string_truncate (part_id, len);

	/* Add a widget with details about the encryption */
	g_string_append (part_id, ".inlinepgp_encrypted.button");
	em_format_parse_part_as (emf, opart, part_id, &encinfo,
		"x-evolution/message/x-secure-button", cancellable);
	g_string_truncate (part_id, len);

	/* Clean Up */
	camel_cipher_validity_free (valid);
	g_object_unref (opart);
	g_object_unref (cipher);
}

static void
emf_parse_message (EMFormat *emf,
		   CamelMimePart *part,
		   GString *part_id,
		   EMFormatParserInfo *info,
		   GCancellable *cancellable)
{
	/* Headers */
	em_format_parse_part_as (emf, part, part_id, info,
			"x-evolution/message/headers", cancellable);

	/* Anything that comes between headers and message body */
	em_format_parse_part_as (emf, part, part_id, info,
			"x-evolution/message/post-headers", cancellable);

	/* Begin parsing the message */
	em_format_parse_part (emf, part, part_id, info, cancellable);
}

static void
emf_parse_headers (EMFormat *emf,
		   CamelMimePart *part,
		   GString *part_id,
		   EMFormatParserInfo *info,
		   GCancellable *cancellable)
{
	EMFormatPURI *puri;
	gint len;

	len = part_id->len;
	g_string_append (part_id, ".headers");

	puri = em_format_puri_new (emf, sizeof (EMFormatPURI), part, part_id->str);
	puri->write_func = info->handler->write_func;
	puri->mime_type = g_strdup ("text/html");
	em_format_add_puri (emf, puri);

	g_string_truncate (part_id, len);
}

static void
emf_parse_post_headers (EMFormat *emf,
		        CamelMimePart *part,
		        GString *part_id,
		        EMFormatParserInfo *info,
		        GCancellable *cancellable)
{
	/* Add attachment bar */
	em_format_parse_part_as (emf, part, part_id, info,
		"x-evolution/message/attachment-bar", cancellable);
}

static void
emf_parse_source (EMFormat *emf,
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
	g_string_append (part_id, ".source");

	puri = em_format_puri_new (emf, sizeof (EMFormatPURI), part, part_id->str);
	puri->write_func = emf_write_source;
	puri->mime_type = g_strdup ("text/html");
	g_string_truncate (part_id, len);

	em_format_add_puri (emf, puri);
}

/**************************************************************************/

void
em_format_empty_writer (EMFormat *emf,
			EMFormatPURI *puri,
			CamelStream *stream,
			GCancellable *cancellable)
{
	/* DO NOTHING */
}

/**************************************************************************/

static void
emf_source (EMFormat *emf,
	    CamelStream *stream,
	    GCancellable *cancellable)
{
	GByteArray *ba;
	gchar *data;

	g_return_if_fail (EM_IS_FORMAT (emf));

	ba = camel_data_wrapper_get_byte_array ((CamelDataWrapper *) emf->message);

	data = g_strndup ((gchar *) ba->data, ba->len);
	camel_stream_write_string (stream, data, cancellable, NULL);
	g_free (data);
}

/**************************************************************************/

static void
emf_parse (EMFormat *emf,
	   CamelMimeMessage *message,
	   CamelFolder *folder,
	   GCancellable *cancellable)
{
	GString *part_id;
	EMFormatParserInfo info = { 0 };

	g_return_if_fail (EM_IS_FORMAT (emf));

	if (g_cancellable_is_cancelled (cancellable))
		return;

	if (message) {
		g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

		if (emf->message)
			g_object_unref (emf->message);
		emf->message = g_object_ref (message);
	}

	if (folder) {
		g_return_if_fail (CAMEL_IS_FOLDER  (folder));

		if (emf->folder)
			g_object_unref (emf->folder);
		emf->folder = g_object_ref (folder);
	}

	g_return_if_fail (emf->message);
	g_return_if_fail (emf->folder);

	part_id = g_string_new ("");

	em_format_parse_part_as (emf, CAMEL_MIME_PART (message), part_id, &info,
			"x-evolution/message", cancellable);

	g_string_free (part_id, TRUE);
}

static gboolean
emf_is_inline (EMFormat *emf,
               const gchar *part_id,
               CamelMimePart *mime_part,
               const EMFormatHandler *handle)
{
	//EMFormatCache *emfc;
	const gchar *disposition;

	if (handle == NULL)
		return FALSE;

	/* WEBKIT FIXME
	emfc = g_hash_table_lookup (emf->inline_table, part_id);
	if (emfc && emfc->state != INLINE_UNSET)
		return emfc->state & 1;
	*/

	/* Some types need to override the disposition.
	 * e.g. application/x-pkcs7-mime */
	if (handle->flags & EM_FORMAT_HANDLER_INLINE_DISPOSITION)
		return TRUE;

	disposition = camel_mime_part_get_disposition (mime_part);
	if (disposition != NULL)
		return g_ascii_strcasecmp (disposition, "inline") == 0;

	/* Otherwise, use the default for this handler type. */
	return (handle->flags & EM_FORMAT_HANDLER_INLINE) != 0;
}

/**************************************************************************/

static EMFormatHandler type_handlers[] = {
#ifdef ENABLE_SMIME
		{ (gchar *) "application/x-pkcs7-mime", emf_parse_application_xpkcs7mime, 0, EM_FORMAT_HANDLER_INLINE_DISPOSITION },
#endif
		{ (gchar *) "application/mbox", emf_parse_application_mbox, 0, EM_FORMAT_HANDLER_INLINE },
		{ (gchar *) "multipart/alternative", emf_parse_multipart_alternative, },
		{ (gchar *) "multipart/appledouble", emf_parse_multipart_appledouble, },
		{ (gchar *) "multipart/encrypted", emf_parse_multipart_encrypted, },
		{ (gchar *) "multipart/mixed", emf_parse_multipart_mixed, },
		{ (gchar *) "multipart/signed", emf_parse_multipart_signed, },
		{ (gchar *) "multipart/related", emf_parse_multipart_related, },
		{ (gchar *) "multipart/digest", emf_parse_multipart_digest, },
		{ (gchar *) "multipart/*", emf_parse_multipart_mixed, },

		/* Insert brokenly-named parts here */
#ifdef ENABLE_SMIME
		{ (gchar *) "application/pkcs7-mime", emf_parse_application_xpkcs7mime, 0, EM_FORMAT_HANDLER_INLINE_DISPOSITION },
#endif

		/* internal types */
		{ (gchar *) "application/x-inlinepgp-signed", emf_parse_inlinepgp_signed, },
		{ (gchar *) "application/x-inlinepgp-encrypted", emf_parse_inlinepgp_encrypted, },
		{ (gchar *) "x-evolution/message", emf_parse_message, },
		{ (gchar *) "x-evolution/message/headers", emf_parse_headers, },
		{ (gchar *) "x-evolution/message/post-headers", emf_parse_post_headers, },
		{ (gchar *) "x-evolution/message/source", emf_parse_source, },
};

/* note: also copied in em-mailer-prefs.c */
static const struct {
	const gchar *name;
	guint32 flags;
} default_headers[] = {
	{ N_("From"), EM_FORMAT_HEADER_BOLD },
	{ N_("Reply-To"), EM_FORMAT_HEADER_BOLD },
	{ N_("To"), EM_FORMAT_HEADER_BOLD },
	{ N_("Cc"), EM_FORMAT_HEADER_BOLD },
	{ N_("Bcc"), EM_FORMAT_HEADER_BOLD },
	{ N_("Subject"), EM_FORMAT_HEADER_BOLD },
	{ N_("Date"), EM_FORMAT_HEADER_BOLD },
	{ N_("Newsgroups"), EM_FORMAT_HEADER_BOLD },
	{ N_("Face"), 0 },
};

static void
em_format_get_property (GObject *object,
			guint property_id,
			GValue *value,
			GParamSpec *pspec)
{
	EMFormat *emf = EM_FORMAT (object);

	switch (property_id) {
		case PROP_CHARSET:
			g_value_set_string (
					value, em_format_get_charset (emf));
			return;
		case PROP_DEFAULT_CHARSET:
			g_value_set_string (
					value, em_format_get_default_charset (emf));
			return;
		case PROP_COMPOSER:
			g_value_set_boolean (
					value, em_format_get_composer (emf));
			return;
		case PROP_BASE_URL:
			g_value_set_object (
					value, em_format_get_base_url (emf));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
em_format_set_property (GObject *object,
			guint property_id,
			const GValue *value,
			GParamSpec *pspec)
{
	EMFormat *emf = EM_FORMAT (object);

	switch (property_id) {
		case PROP_CHARSET:
			em_format_set_charset (emf,
					g_value_get_string (value));
			return;
		case PROP_DEFAULT_CHARSET:
			em_format_set_default_charset (emf,
					g_value_get_string (value));
			return;
		case PROP_COMPOSER:
			em_format_set_composer (emf,
					g_value_get_boolean (value));
			return;
		case PROP_BASE_URL:
			em_format_set_base_url (emf,
					g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);

}

static void
em_format_finalize (GObject *object)
{
	EMFormat *emf = EM_FORMAT (object);

	if (emf->message) {
		g_object_unref (emf->message);
		emf->message = NULL;
	}

	if (emf->folder) {
		g_object_unref (emf->folder);
		emf->folder = NULL;
	}

	if (emf->mail_part_table) {
		/* This will destroy all the EMFormatPURI objects stored
		 * inside!!!! */
		g_hash_table_destroy (emf->mail_part_table);
		emf->mail_part_table = NULL;
	}

	if (emf->mail_part_list) {
		g_list_free (emf->mail_part_list);
		emf->mail_part_list = NULL;
	}


	if (emf->priv->base_url) {
		camel_url_free (emf->priv->base_url);
		emf->priv->base_url = NULL;
	}

	if (emf->priv->session) {
		g_object_unref (emf->priv->session);
		emf->priv->session = NULL;
	}

	if (emf->priv->charset) {
		g_free (emf->priv->charset);
		emf->priv->charset = NULL;
	}

	/* Chain up to parent's finalize() method */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
em_format_base_init (EMFormatClass *class)
{
	gint i;

	class->type_handlers = g_hash_table_new (g_str_hash, g_str_equal);

	for (i = 0; i < G_N_ELEMENTS (type_handlers); i++) {
		g_hash_table_insert (class->type_handlers,
				type_handlers[i].mime_type,
				&type_handlers[i]);
	}
}

static void
em_format_class_init (EMFormatClass *class)
{
	GObjectClass *object_class;

	parent_class = g_type_class_peek_parent (class);

	g_type_class_add_private (class, sizeof (EMFormatPrivate));

	class->format_error = emf_error;
	class->format_source = emf_source;
	class->parse = emf_parse;
	class->is_inline = emf_is_inline;

	object_class = G_OBJECT_CLASS (class);
	object_class->finalize = em_format_finalize;
	object_class->get_property = em_format_get_property;
	object_class->set_property = em_format_set_property;

	g_object_class_install_property (object_class,
			PROP_CHARSET,
			g_param_spec_string ("charset",
					NULL,
					NULL,
					NULL,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class,
			PROP_DEFAULT_CHARSET,
			g_param_spec_string ("default-charset",
					NULL,
					NULL,
					NULL,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class,
			PROP_COMPOSER,
			g_param_spec_boolean ("composer",
					NULL,
					NULL,
					FALSE,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class,
			PROP_BASE_URL,
			g_param_spec_pointer ("base-url",
					NULL,
					NULL,
					G_PARAM_READWRITE));
}

static void
em_format_init (EMFormat *emf)
{
	EShell *shell;
	EShellSettings *shell_settings;
	gint ii;

	emf->priv = G_TYPE_INSTANCE_GET_PRIVATE (emf,
			EM_TYPE_FORMAT, EMFormatPrivate);

	emf->message = NULL;
	emf->folder = NULL;
	emf->mail_part_list = NULL;
	emf->mail_part_table = g_hash_table_new_full (g_str_hash, g_str_equal,
			(GDestroyNotify) g_free, (GDestroyNotify) em_format_puri_free);

	shell = e_shell_get_default ();
	shell_settings = e_shell_get_shell_settings (shell);

	emf->priv->last_error = 0;

	emf->priv->session = e_shell_settings_get_pointer (shell_settings, "mail-session");
	g_return_if_fail (emf->priv->session);

	g_object_ref (emf->priv->session);

	/* Set the default headers */
	em_format_clear_headers (emf);
	for (ii = 0; ii < G_N_ELEMENTS (default_headers); ii++)
		em_format_add_header (
			emf, default_headers[ii].name,
			default_headers[ii].flags);
}

EMFormat*
em_format_new (void)
{
	EMFormat *emf = g_object_new (EM_TYPE_FORMAT, NULL);

	return emf;
}

GType
em_format_get_type (void)
{
	static GType type = 0;

	if (G_UNLIKELY (type == 0)) {
		static const GTypeInfo type_info = {
				sizeof (EMFormatClass),
				(GBaseInitFunc) em_format_base_init,
				(GBaseFinalizeFunc) NULL,
				(GClassInitFunc) em_format_class_init,
				(GClassFinalizeFunc) NULL,
				NULL,	/* class_data */
				sizeof (EMFormat),
				0,		/* n_preallocs */
				(GInstanceInitFunc) em_format_init,
				NULL	/* value_table */
		};

		type = g_type_register_static (
				G_TYPE_OBJECT, "EMFormat", &type_info, 0);
	}

	return type;
}

void
em_format_set_charset (EMFormat *emf,
		       const gchar *charset)
{
	g_return_if_fail (EM_IS_FORMAT (emf));

	if (emf->priv->charset)
		g_free (emf->priv->charset);

	emf->priv->charset = g_strdup (charset);

	g_object_notify (G_OBJECT (emf), "charset");
}

const gchar*
em_format_get_charset (EMFormat *emf)
{
	g_return_val_if_fail (EM_IS_FORMAT (emf), NULL);

	return emf->priv->charset;
}

void
em_format_set_default_charset (EMFormat *emf,
			       const gchar *charset)
{
	g_return_if_fail (EM_IS_FORMAT (emf));

	if (emf->priv->default_charset)
		g_free (emf->priv->default_charset);

	emf->priv->default_charset = g_strdup (charset);

	g_object_notify (G_OBJECT (emf), "default-charset");
}

const gchar*
em_format_get_default_charset (EMFormat *emf)
{
	g_return_val_if_fail (EM_IS_FORMAT (emf), NULL);

	return emf->priv->default_charset;
}

void
em_format_set_composer (EMFormat *emf,
			gboolean composer)
{
	g_return_if_fail (EM_IS_FORMAT (emf));

	if (emf->priv->composer && composer)
		return;

	emf->priv->composer = composer;

	g_object_notify (G_OBJECT (emf), "composer");
}

gboolean
em_format_get_composer (EMFormat *emf)
{
	g_return_val_if_fail (EM_IS_FORMAT (emf), FALSE);

	return emf->priv->composer;
}

void
em_format_set_base_url (EMFormat *emf,
			CamelURL *url)
{
	g_return_if_fail (EM_IS_FORMAT (emf));
	g_return_if_fail (url);

	if (emf->priv->base_url)
		camel_url_free (emf->priv->base_url);

	emf->priv->base_url = camel_url_copy (url);

	g_object_notify (G_OBJECT (emf), "base-url");
}

void
em_format_set_base_url_string (EMFormat *emf,
			       const gchar *url_string)
{
	g_return_if_fail (EM_IS_FORMAT (emf));
	g_return_if_fail (url_string && *url_string);

	if (emf->priv->base_url)
		camel_url_free (emf->priv->base_url);

	emf->priv->base_url = camel_url_new (url_string, NULL);

	g_object_notify (G_OBJECT (emf), "base-url");
}

CamelURL*
em_format_get_base_url (EMFormat *emf)
{
	g_return_val_if_fail (EM_IS_FORMAT (emf), NULL);

	return emf->priv->base_url;
}

/**
 * em_format_clear_headers:
 * @emf:
 *
 * Clear the list of headers to be displayed.  This will force all headers to
 * be shown.
 **/
void
em_format_clear_headers (EMFormat *emf)
{
	EMFormatHeader *eh;

	g_return_if_fail (EM_IS_FORMAT (emf));

	while ((eh = g_queue_pop_head (&emf->header_list)) != NULL)
		g_free (eh);
}

/**
 * em_format_add_header:
 * @emf:
 * @name: The name of the header, as it will appear during output.
 * @flags: EM_FORMAT_HEAD_* defines to control display attributes.
 *
 * Add a specific header to show.  If any headers are set, they will
 * be displayed in the order set by this function.  Certain known
 * headers included in this list will be shown using special
 * formatting routines.
 **/
void
em_format_add_header (EMFormat *emf,
                      const gchar *name,
                      guint32 flags)
{
	EMFormatHeader *h;

	g_return_if_fail (EM_IS_FORMAT (emf));
	g_return_if_fail (name && *name);

	h = g_malloc (sizeof (*h) + strlen (name));
	h->flags = flags;
	strcpy (h->name, name);
	g_queue_push_tail (&emf->header_list, h);
}

void
em_format_add_puri (EMFormat *emf,
		    EMFormatPURI *puri)
{
	g_return_if_fail (EM_IS_FORMAT (emf));
	g_return_if_fail (puri != NULL);

	emf->mail_part_list = g_list_append (emf->mail_part_list, puri);

	g_hash_table_insert (emf->mail_part_table,
			puri->uri, puri);
}

EMFormatPURI*
em_format_find_puri (EMFormat *emf,
		     const gchar *id)
{
	/* First handle CIDs... */
	if (g_str_has_prefix (id, "CID:") || g_str_has_prefix (id, "cid:")) {
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init (&iter, emf->mail_part_table);
		while (g_hash_table_iter_next (&iter, &key, &value)) {
			EMFormatPURI *puri = value;
			if (g_strcmp0 (puri->cid, id) == 0)
				return puri;
		}

		return NULL;
	}


	return g_hash_table_lookup (emf->mail_part_table, id);
}

void
em_format_class_add_handler (EMFormatClass *emfc,
		  	     EMFormatHandler *handler)
{
	EMFormatHandler *old_handler;

	g_return_if_fail (EM_IS_FORMAT_CLASS (emfc));
	g_return_if_fail (handler);

	old_handler = g_hash_table_lookup (
			emfc->type_handlers, handler->mime_type);

	handler->old = old_handler;

	/* If parse_func or write_func of the new handler is not set,
	 * use function from the old handler (if it exists).
	 * This way we can assign a new write_func for to an existing
	 * parse_func */
	if (old_handler && handler->parse_func == NULL) {
		handler->parse_func = old_handler->parse_func;
	}

	if (old_handler && handler->write_func == NULL) {
		handler->write_func = old_handler->write_func;
	}

	g_hash_table_insert (emfc->type_handlers,
			handler->mime_type, handler);
}

void
em_format_class_remove_handler (EMFormatClass *emfc,
				EMFormatHandler *handler)
{
	g_return_if_fail (EM_IS_FORMAT_CLASS (emfc));
	g_return_if_fail (handler);

	g_hash_table_remove (emfc->type_handlers, handler->mime_type);
}

const EMFormatHandler *
em_format_find_handler (EMFormat *emf,
						const gchar *mime_type)
{
	EMFormatClass *emfc;

	g_return_val_if_fail (EM_IS_FORMAT (emf), NULL);
	g_return_val_if_fail (mime_type && *mime_type, NULL);

	emfc = (EMFormatClass *) G_OBJECT_GET_CLASS (emf);

	return g_hash_table_lookup (
			emfc->type_handlers, mime_type);
}

/**
 * em_format_fallback_handler:
 * @emf:
 * @mime_type:
 *
 * Try to find a format handler based on the major type of the @mime_type.
 *
 * The subtype is replaced with "*" and a lookup performed.
 *
 * Return value:
 **/
const EMFormatHandler *
em_format_fallback_handler (EMFormat *emf,
			    const gchar *mime_type)
{
	gchar *mime, *s;

	s = strchr (mime_type, '/');
	if (s == NULL)
		mime = (gchar *) mime_type;
	else {
		gsize len = (s-mime_type)+1;

		mime = g_alloca (len+2);
		strncpy (mime, mime_type, len);
		strcpy(mime+len, "*");
	}

	return em_format_find_handler (emf, mime);
}

void
em_format_parse (EMFormat *emf,
		 CamelMimeMessage *message,
		 CamelFolder *folder,
		 GCancellable *cancellable)
{
	EMFormatClass *class;

	g_return_if_fail (EM_IS_FORMAT (emf));
	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));
	g_return_if_fail (CAMEL_IS_FOLDER (folder));

	class = EM_FORMAT_GET_CLASS (emf);
	g_return_if_fail (class->parse != NULL);

	class->parse (emf, message, folder, cancellable);
}

void
em_format_parse_part_as (EMFormat *emf,
			 CamelMimePart *part,
			 GString *part_id,
			 EMFormatParserInfo *info,
			 const gchar *mime_type,
			 GCancellable *cancellable)
{
	const EMFormatHandler *handler;
	EMFormatParserInfo ninfo = {
		0,
		info ? info->validity_type : 0,
		info ? info->validity : 0
	};

	handler = em_format_find_handler (emf, mime_type);
	if (handler) {
		ninfo.handler = handler;
		handler->parse_func (emf, part, part_id, &ninfo, cancellable);
	} else {
		handler = em_format_find_handler (emf, "x-evolution/message/attachment");
		ninfo.handler = handler;
		handler->parse_func (emf, part, part_id, &ninfo, cancellable);
	}
}

void
em_format_parse_part (EMFormat *emf,
		      CamelMimePart *part,
		      GString *part_id,
		      EMFormatParserInfo *info,
		      GCancellable *cancellable)
{
	CamelContentType *ct;
	gchar *mime_type;

	ct = camel_mime_part_get_content_type (part);
	if (ct) {
		mime_type = camel_content_type_simple (ct);
	} else {
		mime_type = (gchar *) "text/plain";
	}

	em_format_parse_part_as (emf, part, part_id, info, mime_type, cancellable);

	if (ct)
		g_free (mime_type);
}


gboolean
em_format_is_inline (EMFormat *emf,
		     const gchar *part_id,
		     CamelMimePart *part,
		     const EMFormatHandler *handler)
{
	EMFormatClass *class;

	g_return_val_if_fail (EM_IS_FORMAT (emf), FALSE);
	g_return_val_if_fail (part_id && *part_id, FALSE);
	g_return_val_if_fail (CAMEL_IS_MIME_PART (part), FALSE);
	g_return_val_if_fail (handler, FALSE);

	class = EM_FORMAT_GET_CLASS (emf);
	g_return_val_if_fail (class->is_inline != NULL, FALSE);

	return class->is_inline (emf, part_id, part, handler);

}

gchar*
em_format_get_error_id (EMFormat *emf)
{
	g_return_val_if_fail (EM_IS_FORMAT (emf), NULL);

	emf->priv->last_error++;
	return g_strdup_printf (".error.%d", emf->priv->last_error);
}

void
em_format_format_error (EMFormat *emf,
                        const gchar *format,
                        ...)
{
	EMFormatClass *class;
	gchar *errmsg;
	va_list ap;

	g_return_if_fail (EM_IS_FORMAT (emf));
	g_return_if_fail (format != NULL);

	class = EM_FORMAT_GET_CLASS (emf);
	g_return_if_fail (class->format_error != NULL);

	va_start (ap, format);
	errmsg = g_strdup_vprintf (format, ap);
	class->format_error (emf, errmsg);
	g_free (errmsg);
	va_end (ap);
}

void
em_format_format_source (EMFormat *emf,
			 CamelStream *stream,
			 GCancellable *cancellable)
{
	EMFormatClass *class;

	g_return_if_fail (EM_IS_FORMAT (emf));
	g_return_if_fail (CAMEL_IS_STREAM (stream));

	class = EM_FORMAT_GET_CLASS (emf);
	g_return_if_fail (class->format_source != NULL);

	class->format_source (emf, stream, cancellable);
}

/**
 * em_format_format_text:
 * @emf:
 * @stream: Where to write the converted text
 * @part: Part whose container is to be formatted
 * @cancellable: optional #GCancellable object, or %NULL
 *
 * Decode/output a part's content to @stream.
 **/
void
em_format_format_text (EMFormat *emf,
                       CamelStream *stream,
                       CamelDataWrapper *dw,
                       GCancellable *cancellable)
{
	CamelStream *filter_stream;
	CamelMimeFilter *filter;
	const gchar *charset = NULL;
	CamelMimeFilterWindows *windows = NULL;
	CamelStream *mem_stream = NULL;
	const gchar *key;
	gsize size;
	gsize max;
	GConfClient *gconf;

	if (g_cancellable_is_cancelled (cancellable))
		return;

	if (emf->priv->charset) {
		charset = emf->priv->charset;
	} else if (dw->mime_type
		   && (charset = camel_content_type_param (dw->mime_type, "charset"))
		   && g_ascii_strncasecmp(charset, "iso-8859-", 9) == 0) {
		CamelStream *null;

		/* Since a few Windows mailers like to claim they sent
		 * out iso-8859-# encoded text when they really sent
		 * out windows-cp125#, do some simple sanity checking
		 * before we move on... */

		null = camel_stream_null_new ();
		filter_stream = camel_stream_filter_new (null);
		g_object_unref (null);

		windows = (CamelMimeFilterWindows *) camel_mime_filter_windows_new (charset);
		camel_stream_filter_add (
			CAMEL_STREAM_FILTER (filter_stream),
			CAMEL_MIME_FILTER (windows));

		camel_data_wrapper_decode_to_stream_sync (
			dw, (CamelStream *) filter_stream, cancellable, NULL);
		camel_stream_flush ((CamelStream *) filter_stream, cancellable, NULL);
		g_object_unref (filter_stream);

		charset = camel_mime_filter_windows_real_charset (windows);
	} else if (charset == NULL) {
		charset = emf->priv->default_charset;
	}

	mem_stream = (CamelStream *) camel_stream_mem_new ();
	filter_stream = camel_stream_filter_new (mem_stream);

	if ((filter = camel_mime_filter_charset_new (charset, "UTF-8"))) {
		camel_stream_filter_add (
			CAMEL_STREAM_FILTER (filter_stream),
			CAMEL_MIME_FILTER (filter));
		g_object_unref (filter);
	}

	max = -1;

	gconf = gconf_client_get_default ();
	key = "/apps/evolution/mail/display/force_message_limit";
	if (gconf_client_get_bool (gconf, key, NULL)) {
		key = "/apps/evolution/mail/display/message_text_part_limit";
		max = gconf_client_get_int (gconf, key, NULL);
		if (max == 0)
			max = -1;
	}
	g_object_unref (gconf);

	size = camel_data_wrapper_decode_to_stream_sync (
			camel_medium_get_content ((CamelMedium *) dw),
		(CamelStream *) filter_stream, cancellable, NULL);
	camel_stream_flush ((CamelStream *) filter_stream, cancellable, NULL);
	g_object_unref (filter_stream);

	g_seekable_seek (G_SEEKABLE (mem_stream), 0, G_SEEK_SET, NULL, NULL);

	if (max == -1 || size == -1 || size < (max * 1024) || emf->priv->composer) {
		camel_stream_write_to_stream (
			mem_stream, (CamelStream *) stream, cancellable, NULL);
		camel_stream_flush ((CamelStream *) mem_stream, cancellable, NULL);
	} else {
		/* FIXME WEBKIT
		EM_FORMAT_GET_CLASS (emf)->format_optional (
			emf, stream, (CamelMimePart *) dw,
			mem_stream, cancellable);
		*/
	}

	if (windows)
		g_object_unref (windows);

	g_object_unref (mem_stream);
}

/**
 * em_format_describe_part:
 * @part:
 * @mimetype:
 *
 * Generate a simple textual description of a part, @mime_type represents
 * the content.
 *
 * Return value:
 **/
gchar *
em_format_describe_part (CamelMimePart *part,
                         const gchar *mime_type)
{
	GString *stext;
	const gchar *filename, *description;
	gchar *content_type, *desc;

	stext = g_string_new("");
	content_type = g_content_type_from_mime_type (mime_type);
	desc = g_content_type_get_description (content_type ? content_type : mime_type);
	g_free (content_type);
	g_string_append_printf (stext, _("%s attachment"), desc ? desc : mime_type);
	g_free (desc);

	filename = camel_mime_part_get_filename (part);
	description = camel_mime_part_get_description (part);

	if (!filename || !*filename) {
		CamelDataWrapper *content = camel_medium_get_content (CAMEL_MEDIUM (part));

		if (content && CAMEL_IS_MIME_MESSAGE (content))
			filename = camel_mime_message_get_subject (CAMEL_MIME_MESSAGE (content));
	}

	if (filename != NULL && *filename != '\0') {
		gchar *basename = g_path_get_basename (filename);
		g_string_append_printf (stext, " (%s)", basename);
		g_free (basename);
	}

	if (description != NULL && *description != '\0' &&
		g_strcmp0 (filename, description) != 0)
		g_string_append_printf (stext, ", \"%s\"", description);

	return g_string_free (stext, FALSE);
}

/**
 * em_format_is_attachment:
 * @emf:
 * @part: Part to check.
 *
 * Returns true if the part is an attachment.
 *
 * A part is not considered an attachment if it is a
 * multipart, or a text part with no filename.  It is used
 * to determine if an attachment header should be displayed for
 * the part.
 *
 * Content-Disposition is not checked.
 *
 * Return value: TRUE/FALSE
 **/
gint
em_format_is_attachment (EMFormat *emf,
                         CamelMimePart *part)
{
	/*CamelContentType *ct = camel_mime_part_get_content_type(part);*/
	CamelDataWrapper *dw = camel_medium_get_content ((CamelMedium *) part);

	if (!dw)
		return 0;

	d(printf("checking is attachment %s/%s\n", dw->mime_type->type, dw->mime_type->subtype));
	return !(camel_content_type_is (dw->mime_type, "multipart", "*")
		 || camel_content_type_is (
			dw->mime_type, "application", "x-pkcs7-mime")
		 || camel_content_type_is (
			dw->mime_type, "application", "pkcs7-mime")
		 || camel_content_type_is (
			dw->mime_type, "application", "x-inlinepgp-signed")
		 || camel_content_type_is (
			dw->mime_type, "application", "x-inlinepgp-encrypted")
		 || camel_content_type_is (
			dw->mime_type, "x-evolution", "evolution-rss-feed")
		 || camel_content_type_is (dw->mime_type, "text", "calendar")
		 || camel_content_type_is (dw->mime_type, "text", "x-calendar")
		 || (camel_content_type_is (dw->mime_type, "text", "*")
		     && camel_mime_part_get_filename (part) == NULL));
}

/**
 * em_format_snoop_type:
 * @part:
 *
 * Tries to snoop the mime type of a part.
 *
 * Return value: NULL if unknown (more likely application/octet-stream).
 **/
const gchar *
em_format_snoop_type (CamelMimePart *part)
{
	/* cache is here only to be able still return const gchar * */
	static GHashTable *types_cache = NULL;

	const gchar *filename;
	gchar *name_type = NULL, *magic_type = NULL, *res, *tmp;
	CamelDataWrapper *dw;

	filename = camel_mime_part_get_filename (part);
	if (filename != NULL)
		name_type = e_util_guess_mime_type (filename, FALSE);

	dw = camel_medium_get_content ((CamelMedium *) part);
	if (!camel_data_wrapper_is_offline (dw)) {
		GByteArray *byte_array;
		CamelStream *stream;

		byte_array = g_byte_array_new ();
		stream = camel_stream_mem_new_with_byte_array (byte_array);

		if (camel_data_wrapper_decode_to_stream_sync (dw, stream, NULL, NULL) > 0) {
			gchar *content_type;

			content_type = g_content_type_guess (
				filename, byte_array->data,
				byte_array->len, NULL);

			if (content_type != NULL)
				magic_type = g_content_type_get_mime_type (content_type);

			g_free (content_type);
		}

		g_object_unref (stream);
	}

	d(printf("snooped part, magic_type '%s' name_type '%s'\n", magic_type, name_type));

	/* If gvfs doesn't recognize the data by magic, but it
	 * contains English words, it will call it text/plain. If the
	 * filename-based check came up with something different, use
	 * that instead and if it returns "application/octet-stream"
	 * try to do better with the filename check.
	 */

	if (magic_type) {
		if (name_type
		    && (!strcmp(magic_type, "text/plain")
			|| !strcmp(magic_type, "application/octet-stream")))
			res = name_type;
		else
			res = magic_type;
	} else
		res = name_type;

	if (res != name_type)
		g_free (name_type);

	if (res != magic_type)
		g_free (magic_type);

	if (!types_cache)
		types_cache = g_hash_table_new_full (
			g_str_hash, g_str_equal,
			(GDestroyNotify) g_free,
			(GDestroyNotify) NULL);

	if (res) {
		tmp = g_hash_table_lookup (types_cache, res);
		if (tmp) {
			g_free (res);
			res = tmp;
		} else {
			g_hash_table_insert (types_cache, res, res);
		}
	}

	return res;

	/* We used to load parts to check their type, we dont anymore,
	   see bug #211778 for some discussion */
}

gchar*
em_format_build_mail_uri (CamelFolder *folder,
			  const gchar *message_uid,
			  const gchar *part_uid)
{
	CamelStore *store;
	gchar *uri, *tmp;

	g_return_val_if_fail (CAMEL_IS_FOLDER (folder), NULL);
	g_return_val_if_fail (message_uid && *message_uid, NULL);

	store = camel_folder_get_parent_store (folder);

	if (part_uid && *part_uid) {
		uri = g_strdup_printf ("mail://%s/%s/%s?part_id=%s",
				camel_service_get_uid (CAMEL_SERVICE (store)),
				camel_folder_get_full_name (folder),
				message_uid,
				part_uid);
	} else {
		uri = g_strdup_printf ("mail://%s/%s/%s",
				camel_service_get_uid (CAMEL_SERVICE (store)),
				camel_folder_get_full_name (folder),
				message_uid);
	}

	/* For some reason, webkit won't accept URL with username, but
	 * without password (mail://store@host/folder/mail), so we
	 * will replace the '@' symbol by '/' to get URL like
	 * mail://store/host/folder/mail which is OK
	 */
	tmp = strchr (uri, '@');
	if (tmp) {
		tmp[0] = '/';
	}

	return uri;
}


/**************************************************************************/
EMFormatPURI*
em_format_puri_new (EMFormat *emf,
		    gsize puri_size,
		    CamelMimePart *part,
		    const gchar *uri)
{
	EMFormatPURI *puri;

	g_return_val_if_fail (EM_IS_FORMAT (emf), NULL);
	g_return_val_if_fail (puri_size >= sizeof (EMFormatPURI), NULL);

	puri = (EMFormatPURI *) g_malloc0 (puri_size);
	puri->emf = emf;

	if (part)
		puri->part = g_object_ref (part);

	if (uri)
		puri->uri = g_strdup (uri);

	return puri;
}

void
em_format_puri_free (EMFormatPURI *puri)
{
	g_return_if_fail (puri);

	if (puri->part)
		g_object_unref (puri->part);

	if (puri->uri)
		g_free (puri->uri);

	if (puri->cid)
		g_free (puri->cid);

	if (puri->mime_type)
		g_free (puri->mime_type);

	if (puri->validity)
		camel_cipher_validity_free (puri->validity);

	if (puri->validity_parent)
		camel_cipher_validity_free (puri->validity_parent);

	if (puri->free)
		puri->free(puri);

	g_free (puri);
}


void
em_format_puri_write (EMFormatPURI *puri,
		      CamelStream *stream,
		      GCancellable *cancellable)
{
	g_return_if_fail (puri);
	g_return_if_fail (CAMEL_IS_STREAM (stream));

	if (puri->write_func) {
		puri->write_func (puri->emf, puri, stream, cancellable);
	} else {
		const EMFormatHandler *handler;
		const gchar *mime_type;

		if (puri->mime_type) {
			mime_type = puri->mime_type;
		} else {
			mime_type = (gchar *) "plain/text";
		}

		handler = em_format_find_handler (puri->emf, mime_type);
		if (handler && handler->write_func) {
			handler->write_func (puri->emf,
					puri, stream, cancellable);
		}
	}
}

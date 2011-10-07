/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
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
 *		Ettore Perazzoli (ettore@ximian.com)
 *		Jeffrey Stedfast (fejj@ximian.com)
 *		Miguel de Icaza  (miguel@ximian.com)
 *		Radek Doulik     (rodo@ximian.com)
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>

#include "e-util/e-account-utils.h"
#include "e-util/e-alert-dialog.h"
#include "e-util/e-dialog-utils.h"
#include "e-util/e-signature-utils.h"
#include "e-util/e-util-private.h"
#include "em-format/em-format.h"
#include "em-format/em-format-quote.h"

#include "e-composer-private.h"

typedef struct _AsyncContext AsyncContext;

struct _AsyncContext {
	EActivity *activity;

	CamelMimeMessage *message;
	CamelDataWrapper *top_level_part;
	CamelDataWrapper *text_plain_part;

	EAccount *account;
	CamelSession *session;
	CamelInternetAddress *from;

	CamelTransferEncoding plain_encoding;
	GtkPrintOperationAction print_action;

	GPtrArray *recipients;

	guint skip_content  : 1;
	guint need_thread   : 1;
	guint pgp_sign      : 1;
	guint pgp_encrypt   : 1;
	guint smime_sign    : 1;
	guint smime_encrypt : 1;
};

/* Flags for building a message. */
typedef enum {
	COMPOSER_FLAG_HTML_CONTENT		= 1 << 0,
	COMPOSER_FLAG_SAVE_OBJECT_DATA		= 1 << 1,
	COMPOSER_FLAG_PRIORITIZE_MESSAGE	= 1 << 2,
	COMPOSER_FLAG_REQUEST_READ_RECEIPT	= 1 << 3,
	COMPOSER_FLAG_PGP_SIGN			= 1 << 4,
	COMPOSER_FLAG_PGP_ENCRYPT		= 1 << 5,
	COMPOSER_FLAG_SMIME_SIGN		= 1 << 6,
	COMPOSER_FLAG_SMIME_ENCRYPT		= 1 << 7
} ComposerFlags;

enum {
	PROP_0,
	PROP_FOCUS_TRACKER,
	PROP_SHELL
};

enum {
	PRESEND,
	SEND,
	SAVE_TO_DRAFTS,
	SAVE_TO_OUTBOX,
	PRINT,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

/* used by e_msg_composer_add_message_attachments () */
static void	add_attachments_from_multipart	(EMsgComposer *composer,
						 CamelMultipart *multipart,
						 gboolean just_inlines,
						 gint depth);

/* used by e_msg_composer_new_with_message () */
static void	handle_multipart		(EMsgComposer *composer,
						 CamelMultipart *multipart,
						 GCancellable *cancellable,
						 gint depth);
static void	handle_multipart_alternative	(EMsgComposer *composer,
						 CamelMultipart *multipart,
						 GCancellable *cancellable,
						 gint depth);
static void	handle_multipart_encrypted	(EMsgComposer *composer,
						 CamelMimePart *multipart,
						 GCancellable *cancellable,
						 gint depth);
static void	handle_multipart_signed		(EMsgComposer *composer,
						 CamelMultipart *multipart,
						 GCancellable *cancellable,
						 gint depth);

static void	e_msg_composer_alert_sink_init	(EAlertSinkInterface *interface);

gboolean 	check_blacklisted_file		(gchar *filename);

G_DEFINE_TYPE_WITH_CODE (
	EMsgComposer,
	e_msg_composer,
	GTKHTML_TYPE_EDITOR,
	G_IMPLEMENT_INTERFACE (
		E_TYPE_ALERT_SINK, e_msg_composer_alert_sink_init)
	G_IMPLEMENT_INTERFACE (E_TYPE_EXTENSIBLE, NULL))

static void
async_context_free (AsyncContext *context)
{
	if (context->activity != NULL)
		g_object_unref (context->activity);

	if (context->message != NULL)
		g_object_unref (context->message);

	if (context->top_level_part != NULL)
		g_object_unref (context->top_level_part);

	if (context->text_plain_part != NULL)
		g_object_unref (context->text_plain_part);

	if (context->account != NULL)
		g_object_unref (context->account);

	if (context->session != NULL)
		g_object_unref (context->session);

	if (context->from != NULL)
		g_object_unref (context->from);

	if (context->recipients != NULL)
		g_ptr_array_free (context->recipients, TRUE);

	g_slice_free (AsyncContext, context);
}

/**
 * emcu_part_to_html:
 * @part:
 *
 * Converts a mime part's contents into html text.  If @credits is given,
 * then it will be used as an attribution string, and the
 * content will be cited.  Otherwise no citation or attribution
 * will be performed.
 *
 * Return Value: The part in displayable html format.
 **/
static gchar *
emcu_part_to_html (CamelMimePart *part,
                   gssize *len,
                   EMFormat *source,
                   GCancellable *cancellable)
{
	EMFormatQuote *emfq;
	CamelStreamMem *mem;
	GByteArray *buf;
	gchar *text;

	buf = g_byte_array_new ();
	mem = (CamelStreamMem *) camel_stream_mem_new ();
	camel_stream_mem_set_byte_array (mem, buf);

	emfq = em_format_quote_new (NULL, (CamelStream *) mem, EM_FORMAT_QUOTE_KEEP_SIG);
	em_format_set_composer ((EMFormat *) emfq, TRUE);
	if (source) {
		/* Copy over things we can, other things are internal.
		 * XXX Perhaps need different api than 'clone'. */
		if (em_format_get_default_charset (source))
			em_format_set_default_charset (
				(EMFormat *) emfq, em_format_get_default_charset (source));
		if (em_format_get_charset (source))
			em_format_set_charset (
				(EMFormat *) emfq, em_format_get_charset (source));
	}

	em_format_format_text (EM_FORMAT (emfq),
			CAMEL_STREAM (mem), CAMEL_DATA_WRAPPER (part), cancellable);
	g_object_unref (emfq);

	camel_stream_write((CamelStream *) mem, "", 1, cancellable, NULL);
	g_object_unref (mem);

	text = (gchar *) buf->data;
	if (len)
		*len = buf->len-1;
	g_byte_array_free (buf, FALSE);

	return text;
}

/* copy of mail_tool_remove_xevolution_headers */
static struct _camel_header_raw *
emcu_remove_xevolution_headers (CamelMimeMessage *message)
{
	struct _camel_header_raw *scan, *list = NULL;

	for (scan = ((CamelMimePart *) message)->headers; scan; scan = scan->next)
		if (!strncmp(scan->name, "X-Evolution", 11))
			camel_header_raw_append (&list, scan->name, scan->value, scan->offset);

	for (scan = list; scan; scan = scan->next)
		camel_medium_remove_header ((CamelMedium *) message, scan->name);

	return list;
}

static EDestination **
destination_list_to_vector_sized (GList *list,
                                  gint n)
{
	EDestination **destv;
	gint i = 0;

	if (n == -1)
		n = g_list_length (list);

	if (n == 0)
		return NULL;

	destv = g_new (EDestination *, n + 1);
	while (list != NULL && i < n) {
		destv[i] = E_DESTINATION (list->data);
		list->data = NULL;
		i++;
		list = g_list_next (list);
	}
	destv[i] = NULL;

	return destv;
}

static EDestination **
destination_list_to_vector (GList *list)
{
	return destination_list_to_vector_sized (list, -1);
}

#define LINE_LEN 72

static gboolean
text_requires_quoted_printable (const gchar *text,
                                gsize len)
{
	const gchar *p;
	gsize pos;

	if (!text)
		return FALSE;

	if (len == -1)
		len = strlen (text);

	if (len >= 5 && strncmp (text, "From ", 5) == 0)
		return TRUE;

	for (p = text, pos = 0; pos + 6 <= len; pos++, p++) {
		if (*p == '\n' && strncmp (p + 1, "From ", 5) == 0)
			return TRUE;
	}

	return FALSE;
}

static CamelTransferEncoding
best_encoding (GByteArray *buf,
               const gchar *charset)
{
	gchar *in, *out, outbuf[256], *ch;
	gsize inlen, outlen;
	gint status, count = 0;
	iconv_t cd;

	if (!charset)
		return -1;

	cd = camel_iconv_open (charset, "utf-8");
	if (cd == (iconv_t) -1)
		return -1;

	in = (gchar *) buf->data;
	inlen = buf->len;
	do {
		out = outbuf;
		outlen = sizeof (outbuf);
		status = camel_iconv (cd, (const gchar **) &in, &inlen, &out, &outlen);
		for (ch = out - 1; ch >= outbuf; ch--) {
			if ((guchar) *ch > 127)
				count++;
		}
	} while (status == (gsize) -1 && errno == E2BIG);
	camel_iconv_close (cd);

	if (status == (gsize) -1 || status > 0)
		return -1;

	if ((count == 0) && (buf->len < LINE_LEN) &&
		!text_requires_quoted_printable (
		(const gchar *) buf->data, buf->len))
		return CAMEL_TRANSFER_ENCODING_7BIT;
	else if (count <= buf->len * 0.17)
		return CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE;
	else
		return CAMEL_TRANSFER_ENCODING_BASE64;
}

static gchar *
best_charset (GByteArray *buf,
              const gchar *default_charset,
              CamelTransferEncoding *encoding)
{
	const gchar *charset;

	/* First try US-ASCII */
	*encoding = best_encoding (buf, "US-ASCII");
	if (*encoding == CAMEL_TRANSFER_ENCODING_7BIT)
		return NULL;

	/* Next try the user-specified charset for this message */
	*encoding = best_encoding (buf, default_charset);
	if (*encoding != -1)
		return g_strdup (default_charset);

	/* Now try the user's default charset from the mail config */
	charset = e_composer_get_default_charset ();
	*encoding = best_encoding (buf, charset);
	if (*encoding != -1)
		return g_strdup (charset);

	/* Try to find something that will work */
	charset = camel_charset_best (
		(const gchar *) buf->data, buf->len);
	if (charset == NULL) {
		*encoding = CAMEL_TRANSFER_ENCODING_7BIT;
		return NULL;
	}

	*encoding = best_encoding (buf, charset);

	return g_strdup (charset);
}

static void
clear_current_images (EMsgComposer *composer)
{
	EMsgComposerPrivate *p = composer->priv;
	g_list_free (p->current_images);
	p->current_images = NULL;
}

void
e_msg_composer_clear_inlined_table (EMsgComposer *composer)
{
	EMsgComposerPrivate *p = composer->priv;

	g_hash_table_remove_all (p->inline_images);
	g_hash_table_remove_all (p->inline_images_by_url);
}

static void
add_inlined_images (EMsgComposer *composer,
                    CamelMultipart *multipart)
{
	EMsgComposerPrivate *p = composer->priv;

	GList *d = p->current_images;
	GHashTable *added;

	added = g_hash_table_new (g_direct_hash, g_direct_equal);
	while (d) {
		CamelMimePart *part = d->data;

		if (!g_hash_table_lookup (added, part)) {
			camel_multipart_add_part (multipart, part);
			g_hash_table_insert (added, part, part);
		}
		d = d->next;
	}
	g_hash_table_destroy (added);
}

/* These functions builds a CamelMimeMessage for the message that the user has
 * composed in 'composer'.
 */

static void
set_recipients_from_destv (CamelMimeMessage *msg,
                           EDestination **to_destv,
                           EDestination **cc_destv,
                           EDestination **bcc_destv,
                           gboolean redirect)
{
	CamelInternetAddress *to_addr;
	CamelInternetAddress *cc_addr;
	CamelInternetAddress *bcc_addr;
	CamelInternetAddress *target;
	const gchar *text_addr, *header;
	gboolean seen_hidden_list = FALSE;
	gint i;

	to_addr  = camel_internet_address_new ();
	cc_addr  = camel_internet_address_new ();
	bcc_addr = camel_internet_address_new ();

	for (i = 0; to_destv != NULL && to_destv[i] != NULL; ++i) {
		text_addr = e_destination_get_address (to_destv[i]);

		if (text_addr && *text_addr) {
			target = to_addr;
			if (e_destination_is_evolution_list (to_destv[i])
			    && !e_destination_list_show_addresses (to_destv[i])) {
				target = bcc_addr;
				seen_hidden_list = TRUE;
			}

			if (camel_address_decode (CAMEL_ADDRESS (target), text_addr) <= 0)
				camel_internet_address_add (target, "", text_addr);
		}
	}

	for (i = 0; cc_destv != NULL && cc_destv[i] != NULL; ++i) {
		text_addr = e_destination_get_address (cc_destv[i]);
		if (text_addr && *text_addr) {
			target = cc_addr;
			if (e_destination_is_evolution_list (cc_destv[i])
			    && !e_destination_list_show_addresses (cc_destv[i])) {
				target = bcc_addr;
				seen_hidden_list = TRUE;
			}

			if (camel_address_decode (CAMEL_ADDRESS (target), text_addr) <= 0)
				camel_internet_address_add (target, "", text_addr);
		}
	}

	for (i = 0; bcc_destv != NULL && bcc_destv[i] != NULL; ++i) {
		text_addr = e_destination_get_address (bcc_destv[i]);
		if (text_addr && *text_addr) {
			if (camel_address_decode (CAMEL_ADDRESS (bcc_addr), text_addr) <= 0)
				camel_internet_address_add (bcc_addr, "", text_addr);
		}
	}

	if (redirect)
		header = CAMEL_RECIPIENT_TYPE_RESENT_TO;
	else
		header = CAMEL_RECIPIENT_TYPE_TO;

	if (camel_address_length (CAMEL_ADDRESS (to_addr)) > 0) {
		camel_mime_message_set_recipients (msg, header, to_addr);
	} else if (seen_hidden_list) {
		camel_medium_set_header (
			CAMEL_MEDIUM (msg), header, "Undisclosed-Recipient:;");
	}

	header = redirect ? CAMEL_RECIPIENT_TYPE_RESENT_CC : CAMEL_RECIPIENT_TYPE_CC;
	if (camel_address_length (CAMEL_ADDRESS (cc_addr)) > 0) {
		camel_mime_message_set_recipients (msg, header, cc_addr);
	}

	header = redirect ? CAMEL_RECIPIENT_TYPE_RESENT_BCC : CAMEL_RECIPIENT_TYPE_BCC;
	if (camel_address_length (CAMEL_ADDRESS (bcc_addr)) > 0) {
		camel_mime_message_set_recipients (msg, header, bcc_addr);
	}

	g_object_unref (to_addr);
	g_object_unref (cc_addr);
	g_object_unref (bcc_addr);
}

static void
build_message_headers (EMsgComposer *composer,
                       CamelMimeMessage *message,
                       gboolean redirect)
{
	EComposerHeaderTable *table;
	EComposerHeader *header;
	EAccount *account;
	const gchar *subject;
	const gchar *reply_to;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));
	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	table = e_msg_composer_get_header_table (composer);

	/* Subject: */
	subject = e_composer_header_table_get_subject (table);
	camel_mime_message_set_subject (message, subject);

	account = e_composer_header_table_get_account (table);
	if (account != NULL) {
		CamelMedium *medium;
		CamelInternetAddress *addr;
		const gchar *header_name;
		const gchar *name = account->id->name;
		const gchar *address = account->id->address;
		gchar *transport_uid;

		medium = CAMEL_MEDIUM (message);

		/* From: / Resent-From: */
		addr = camel_internet_address_new ();
		camel_internet_address_add (addr, name, address);
		if (redirect) {
			gchar *value;

			value = camel_address_encode (CAMEL_ADDRESS (addr));
			camel_medium_set_header (medium, "Resent-From", value);
			g_free (value);
		} else
			camel_mime_message_set_from (message, addr);
		g_object_unref (addr);

		/* X-Evolution-Account */
		header_name = "X-Evolution-Account";
		camel_medium_set_header (medium, header_name, account->uid);

		/* X-Evolution-Fcc */
		header_name = "X-Evolution-Fcc";
		camel_medium_set_header (medium, header_name, account->sent_folder_uri);

		/* X-Evolution-Transport */
		header_name = "X-Evolution-Transport";
		transport_uid = g_strconcat (
			account->uid, "-transport", NULL);
		camel_medium_set_header (medium, header_name, transport_uid);
		g_free (transport_uid);
	}

	/* Reply-To: */
	reply_to = e_composer_header_table_get_reply_to (table);
	if (reply_to != NULL && *reply_to != '\0') {
		CamelInternetAddress *addr;

		addr = camel_internet_address_new ();

		if (camel_address_unformat (CAMEL_ADDRESS (addr), reply_to) > 0)
			camel_mime_message_set_reply_to (message, addr);

		g_object_unref (addr);
	}

	/* To:, Cc:, Bcc: */
	header = e_composer_header_table_get_header (
		table, E_COMPOSER_HEADER_TO);
	if (e_composer_header_get_visible (header)) {
		EDestination **to, **cc, **bcc;

		to = e_composer_header_table_get_destinations_to (table);
		cc = e_composer_header_table_get_destinations_cc (table);
		bcc = e_composer_header_table_get_destinations_bcc (table);

		set_recipients_from_destv (message, to, cc, bcc, redirect);

		e_destination_freev (to);
		e_destination_freev (cc);
		e_destination_freev (bcc);
	}

	/* Date: */
	camel_mime_message_set_date (message, CAMEL_MESSAGE_DATE_CURRENT, 0);

	/* X-Evolution-PostTo: */
	header = e_composer_header_table_get_header (
		table, E_COMPOSER_HEADER_POST_TO);
	if (e_composer_header_get_visible (header)) {
		CamelMedium *medium;
		const gchar *name = "X-Evolution-PostTo";
		GList *list, *iter;

		medium = CAMEL_MEDIUM (message);
		camel_medium_remove_header (medium, name);

		list = e_composer_header_table_get_post_to (table);
		for (iter = list; iter != NULL; iter = iter->next) {
			gchar *folder = iter->data;
			camel_medium_add_header (medium, name, folder);
			g_free (folder);
		}
		g_list_free (list);
	}
}

static CamelCipherHash
account_hash_algo_to_camel_hash (const gchar *hash_algo)
{
	CamelCipherHash res = CAMEL_CIPHER_HASH_DEFAULT;

	if (hash_algo && *hash_algo) {
		if (g_ascii_strcasecmp (hash_algo, "sha1") == 0)
			res = CAMEL_CIPHER_HASH_SHA1;
		else if (g_ascii_strcasecmp (hash_algo, "sha256") == 0)
			res = CAMEL_CIPHER_HASH_SHA256;
		else if (g_ascii_strcasecmp (hash_algo, "sha384") == 0)
			res = CAMEL_CIPHER_HASH_SHA384;
		else if (g_ascii_strcasecmp (hash_algo, "sha512") == 0)
			res = CAMEL_CIPHER_HASH_SHA512;
	}

	return res;
}

static void
composer_add_charset_filter (CamelStream *stream,
                             const gchar *charset)
{
	CamelMimeFilter *filter;

	filter = camel_mime_filter_charset_new ("UTF-8", charset);
	camel_stream_filter_add (CAMEL_STREAM_FILTER (stream), filter);
	g_object_unref (filter);
}

static void
composer_add_quoted_printable_filter (CamelStream *stream)
{
	CamelMimeFilter *filter;

	filter = camel_mime_filter_basic_new (CAMEL_MIME_FILTER_BASIC_QP_ENC);
	camel_stream_filter_add (CAMEL_STREAM_FILTER (stream), filter);
	g_object_unref (filter);

	filter = camel_mime_filter_canon_new (CAMEL_MIME_FILTER_CANON_FROM);
	camel_stream_filter_add (CAMEL_STREAM_FILTER (stream), filter);
	g_object_unref (filter);
}

/* Helper for composer_build_message_thread() */
static gboolean
composer_build_message_pgp (AsyncContext *context,
                            GCancellable *cancellable,
                            GError **error)
{
	CamelCipherContext *cipher;
	CamelDataWrapper *content;
	CamelMimePart *mime_part;
	const gchar *pgp_userid;
	gboolean have_pgp_key;

	/* Return silently if we're not signing or encrypting with PGP. */
	if (!context->pgp_sign && !context->pgp_encrypt)
		return TRUE;

	have_pgp_key =
		(context->account != NULL) &&
		(context->account->pgp_key != NULL) &&
		(context->account->pgp_key[0] != '\0');

	mime_part = camel_mime_part_new ();

	camel_medium_set_content (
		CAMEL_MEDIUM (mime_part),
		context->top_level_part);

	if (context->top_level_part == context->text_plain_part)
		camel_mime_part_set_encoding (
			mime_part, context->plain_encoding);

	g_object_unref (context->top_level_part);
	context->top_level_part = NULL;

	if (have_pgp_key)
		pgp_userid = context->account->pgp_key;
	else
		camel_internet_address_get (
			context->from, 0, NULL, &pgp_userid);

	if (context->pgp_sign) {
		CamelMimePart *npart;
		gboolean success;

		npart = camel_mime_part_new ();

		cipher = camel_gpg_context_new (context->session);
		if (context->account != NULL)
			camel_gpg_context_set_always_trust (
				CAMEL_GPG_CONTEXT (cipher),
				context->account->pgp_always_trust);

		success = camel_cipher_context_sign_sync (
			cipher, pgp_userid,
			account_hash_algo_to_camel_hash (
				(context->account != NULL) ?
				e_account_get_string (context->account,
				E_ACCOUNT_PGP_HASH_ALGORITHM) : NULL),
			mime_part, npart, cancellable, error);

		g_object_unref (cipher);

		g_object_unref (mime_part);

		if (!success) {
			g_object_unref (npart);
			return FALSE;
		}

		mime_part = npart;
	}

	if (context->pgp_encrypt) {
		CamelMimePart *npart;
		gboolean encrypt_to_self;
		gboolean success;

		encrypt_to_self =
			(context->account != NULL) &&
			(context->account->pgp_encrypt_to_self) &&
			(pgp_userid != NULL);

		npart = camel_mime_part_new ();

		/* Check to see if we should encrypt to self.
		 * NB gets removed immediately after use */
		if (encrypt_to_self)
			g_ptr_array_add (
				context->recipients,
				g_strdup (pgp_userid));

		cipher = camel_gpg_context_new (context->session);
		if (context->account != NULL)
			camel_gpg_context_set_always_trust (
				CAMEL_GPG_CONTEXT (cipher),
				context->account->pgp_always_trust);

		success = camel_cipher_context_encrypt_sync (
			cipher, pgp_userid, context->recipients,
			mime_part, npart, cancellable, error);

		g_object_unref (cipher);

		if (encrypt_to_self)
			g_ptr_array_set_size (
				context->recipients,
				context->recipients->len - 1);

		g_object_unref (mime_part);

		if (!success) {
			g_object_unref (npart);
			return FALSE;
		}

		mime_part = npart;
	}

	content = camel_medium_get_content (CAMEL_MEDIUM (mime_part));
	context->top_level_part = g_object_ref (content);

	g_object_unref (mime_part);

	return TRUE;
}

#ifdef HAVE_SSL
static gboolean
composer_build_message_smime (AsyncContext *context,
                              GCancellable *cancellable,
                              GError **error)
{
	CamelCipherContext *cipher;
	CamelMimePart *mime_part;
	gboolean have_signing_certificate;
	gboolean have_encryption_certificate;

	/* Return silently if we're not signing or encrypting with S/MIME. */
	if (!context->smime_sign && !context->smime_encrypt)
		return TRUE;

	have_signing_certificate =
		(context->account != NULL) &&
		(context->account->smime_sign_key != NULL) &&
		(context->account->smime_sign_key[0] != '\0');

	have_encryption_certificate =
		(context->account != NULL) &&
		(context->account->smime_encrypt_key != NULL) &&
		(context->account->smime_encrypt_key[0] != '\0');

	if (context->smime_sign && !have_signing_certificate) {
		g_set_error (
			error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
			_("Cannot sign outgoing message: "
			  "No signing certificate set for "
			  "this account"));
		return FALSE;
	}

	if (context->smime_encrypt && !have_encryption_certificate) {
		g_set_error (
			error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
			_("Cannot encrypt outgoing message: "
			  "No encryption certificate set for "
			  "this account"));
		return FALSE;
	}

	mime_part = camel_mime_part_new ();

	camel_medium_set_content (
		CAMEL_MEDIUM (mime_part),
		context->top_level_part);

	if (context->top_level_part == context->text_plain_part)
		camel_mime_part_set_encoding (
			mime_part, context->plain_encoding);

	g_object_unref (context->top_level_part);
	context->top_level_part = NULL;

	if (context->smime_sign) {
		CamelMimePart *npart;
		gboolean success;

		npart = camel_mime_part_new ();

		cipher = camel_smime_context_new (context->session);

		/* if we're also encrypting, envelope-sign rather than clear-sign */
		if (context->smime_encrypt) {
			camel_smime_context_set_sign_mode (
				(CamelSMIMEContext *) cipher,
				CAMEL_SMIME_SIGN_ENVELOPED);
			camel_smime_context_set_encrypt_key (
				(CamelSMIMEContext *) cipher, TRUE,
				context->account->smime_encrypt_key);
		} else if (have_encryption_certificate) {
			camel_smime_context_set_encrypt_key (
				(CamelSMIMEContext *) cipher, TRUE,
				context->account->smime_encrypt_key);
		}

		success = camel_cipher_context_sign_sync (
			cipher, context->account->smime_sign_key,
			account_hash_algo_to_camel_hash (
				(context->account != NULL) ?
				e_account_get_string (context->account,
				E_ACCOUNT_SMIME_HASH_ALGORITHM) : NULL),
			mime_part, npart, cancellable, error);

		g_object_unref (cipher);

		g_object_unref (mime_part);

		if (!success) {
			g_object_unref (npart);
			return FALSE;
		}

		mime_part = npart;
	}

	if (context->smime_encrypt) {
		gboolean success;

		/* check to see if we should encrypt to self, NB removed after use */
		if (context->account->smime_encrypt_to_self)
			g_ptr_array_add (
				context->recipients, g_strdup (
				context->account->smime_encrypt_key));

		cipher = camel_smime_context_new (context->session);
		camel_smime_context_set_encrypt_key (
			(CamelSMIMEContext *) cipher, TRUE,
			context->account->smime_encrypt_key);

		success = camel_cipher_context_encrypt_sync (
			cipher, NULL,
			context->recipients, mime_part,
			CAMEL_MIME_PART (context->message),
			cancellable, error);

		g_object_unref (cipher);

		if (!success)
			return FALSE;

		if (context->account->smime_encrypt_to_self)
			g_ptr_array_set_size (
				context->recipients,
				context->recipients->len - 1);
	}

	/* we replaced the message directly, we don't want to do reparenting foo */
	if (context->smime_encrypt) {
		context->skip_content = TRUE;
	} else {
		CamelDataWrapper *content;

		content = camel_medium_get_content (
			CAMEL_MEDIUM (mime_part));
		context->top_level_part = g_object_ref (content);
	}

	g_object_unref (mime_part);

	return TRUE;
}
#endif

static void
composer_build_message_thread (GSimpleAsyncResult *simple,
                               EMsgComposer *composer,
                               GCancellable *cancellable)
{
	AsyncContext *context;
	GError *error = NULL;

	context = g_simple_async_result_get_op_res_gpointer (simple);

	/* Setup working recipient list if we're encrypting. */
	if (context->pgp_encrypt || context->smime_encrypt) {
		gint ii, jj;

		const gchar *types[] = {
			CAMEL_RECIPIENT_TYPE_TO,
			CAMEL_RECIPIENT_TYPE_CC,
			CAMEL_RECIPIENT_TYPE_BCC
		};

		context->recipients = g_ptr_array_new_with_free_func (
			(GDestroyNotify) g_free);
		for (ii = 0; ii < G_N_ELEMENTS (types); ii++) {
			CamelInternetAddress *addr;
			const gchar *address;

			addr = camel_mime_message_get_recipients (
				context->message, types[ii]);
			for (jj = 0; camel_internet_address_get (addr, jj, NULL, &address); jj++)
				g_ptr_array_add (
					context->recipients,
					g_strdup (address));
		}
	}

	if (!composer_build_message_pgp (context, cancellable, &error)) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

#if defined (HAVE_NSS)
	if (!composer_build_message_smime (context, cancellable, &error)) {
		g_simple_async_result_take_error (simple, error);
		return;
	}
#endif /* HAVE_NSS */
}

static void
composer_add_evolution_format_header (CamelMedium *medium,
                                      ComposerFlags flags)
{
	GString *string;

	string = g_string_sized_new (128);

	if (flags & COMPOSER_FLAG_HTML_CONTENT)
		g_string_append (string, "text/html");
	else
		g_string_append (string, "text/plain");

	if (flags & COMPOSER_FLAG_PGP_SIGN)
		g_string_append (string, ", pgp-sign");

	if (flags & COMPOSER_FLAG_PGP_ENCRYPT)
		g_string_append (string, ", pgp-encrypt");

	if (flags & COMPOSER_FLAG_SMIME_SIGN)
		g_string_append (string, ", smime-sign");

	if (flags & COMPOSER_FLAG_SMIME_ENCRYPT)
		g_string_append (string, ", smime-encrypt");

	camel_medium_add_header (
		medium, "X-Evolution-Format", string->str);

	g_string_free (string, TRUE);
}

static void
composer_build_message (EMsgComposer *composer,
                        ComposerFlags flags,
                        gint io_priority,
                        GCancellable *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
	EMsgComposerPrivate *priv;
	GSimpleAsyncResult *simple;
	AsyncContext *context;
	GtkhtmlEditor *editor;
	EAttachmentView *view;
	EAttachmentStore *store;
	EComposerHeaderTable *table;
	CamelDataWrapper *html;
	const gchar *iconv_charset = NULL;
	CamelMultipart *body = NULL;
	CamelContentType *type;
	CamelSession *session;
	CamelStream *stream;
	CamelStream *mem_stream;
	CamelMimePart *part;
	GByteArray *data;
	EAccount *account;
	gchar *charset;
	gint i;

	priv = composer->priv;
	editor = GTKHTML_EDITOR (composer);
	table = e_msg_composer_get_header_table (composer);
	account = e_composer_header_table_get_account (table);
	view = e_msg_composer_get_attachment_view (composer);
	store = e_attachment_view_get_store (view);
	session = e_msg_composer_get_session (composer);

	/* Do all the non-blocking work here, and defer
	 * any blocking operations to a separate thread. */

	context = g_slice_new0 (AsyncContext);
	context->account = g_object_ref (account);
	context->session = g_object_ref (session);
	context->from = e_msg_composer_get_from (composer);

	if (flags & COMPOSER_FLAG_PGP_SIGN)
		context->pgp_sign = TRUE;

	if (flags & COMPOSER_FLAG_PGP_ENCRYPT)
		context->pgp_encrypt = TRUE;

	if (flags & COMPOSER_FLAG_SMIME_SIGN)
		context->smime_sign = TRUE;

	if (flags & COMPOSER_FLAG_SMIME_ENCRYPT)
		context->smime_encrypt = TRUE;

	context->need_thread =
		context->pgp_sign || context->pgp_encrypt ||
		context->smime_sign || context->smime_encrypt;

	simple = g_simple_async_result_new (
		G_OBJECT (composer), callback,
		user_data, composer_build_message);

	g_simple_async_result_set_op_res_gpointer (
		simple, context, (GDestroyNotify) async_context_free);

	/* If this is a redirected message, just tweak the headers. */
	if (priv->redirect) {
		context->skip_content = TRUE;
		context->message = g_object_ref (priv->redirect);
		build_message_headers (composer, context->message, TRUE);
		g_simple_async_result_complete (simple);
		g_object_unref (simple);
		return;
	}

	context->message = camel_mime_message_new ();

	/* Explicitly generate a Message-ID header here so it's
	 * consistent for all outbound streams (SMTP, Fcc, etc). */
	camel_mime_message_set_message_id (context->message, NULL);

	build_message_headers (composer, context->message, FALSE);
	for (i = 0; i < priv->extra_hdr_names->len; i++) {
		camel_medium_add_header (
			CAMEL_MEDIUM (context->message),
			priv->extra_hdr_names->pdata[i],
			priv->extra_hdr_values->pdata[i]);
	}

	/* Disposition-Notification-To */
	if (flags & COMPOSER_FLAG_REQUEST_READ_RECEIPT) {
		gchar *mdn_address = account->id->reply_to;

		if (mdn_address == NULL || *mdn_address == '\0')
			mdn_address = account->id->address;

		camel_medium_add_header (
			CAMEL_MEDIUM (context->message),
			"Disposition-Notification-To", mdn_address);
	}

	/* X-Priority */
	if (flags & COMPOSER_FLAG_PRIORITIZE_MESSAGE)
		camel_medium_add_header (
			CAMEL_MEDIUM (context->message),
			"X-Priority", "1");

	/* Organization */
	if (account != NULL && account->id->organization != NULL) {
		gchar *organization;

		organization = camel_header_encode_string (
			(const guchar *) account->id->organization);
		camel_medium_set_header (
			CAMEL_MEDIUM (context->message),
			"Organization", organization);
		g_free (organization);
	}

	/* X-Evolution-Format */
	composer_add_evolution_format_header (
		CAMEL_MEDIUM (context->message), flags);

	/* Build the text/plain part. */

	if (priv->mime_body) {
		if (text_requires_quoted_printable (priv->mime_body, -1)) {
			context->plain_encoding =
				CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE;
		} else {
			context->plain_encoding = CAMEL_TRANSFER_ENCODING_7BIT;
			for (i = 0; priv->mime_body[i]; i++) {
				if ((guchar) priv->mime_body[i] > 127) {
					context->plain_encoding =
					CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE;
					break;
				}
			}
		}

		data = g_byte_array_new ();
		g_byte_array_append (
			data, (const guint8 *) priv->mime_body,
			strlen (priv->mime_body));
		type = camel_content_type_decode (priv->mime_type);

	} else {
		gchar *text;
		gsize length;

		data = g_byte_array_new ();
		text = gtkhtml_editor_get_text_plain (editor, &length);
		g_byte_array_append (data, (guint8 *) text, (guint) length);
		g_free (text);

		type = camel_content_type_new ("text", "plain");
		charset = best_charset (
			data, priv->charset, &context->plain_encoding);
		if (charset != NULL) {
			camel_content_type_set_param (type, "charset", charset);
			iconv_charset = camel_iconv_charset_name (charset);
			g_free (charset);
		}
	}

	mem_stream = camel_stream_mem_new_with_byte_array (data);
	stream = camel_stream_filter_new (mem_stream);
	g_object_unref (mem_stream);

	/* Convert the stream to the appropriate charset. */
	if (iconv_charset && g_ascii_strcasecmp (iconv_charset, "UTF-8") != 0)
		composer_add_charset_filter (stream, iconv_charset);

	/* Encode the stream to quoted-printable if necessary. */
	if (context->plain_encoding == CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE)
		composer_add_quoted_printable_filter (stream);

	/* Construct the content object.  This does not block since
	 * we're constructing the data wrapper from a memory stream. */
	context->top_level_part = camel_data_wrapper_new ();
	camel_data_wrapper_construct_from_stream_sync (
		context->top_level_part, stream, NULL, NULL);
	g_object_unref (stream);

	context->text_plain_part = g_object_ref (context->top_level_part);

	/* Avoid re-encoding the data when adding it to a MIME part. */
	if (context->plain_encoding == CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE)
		context->top_level_part->encoding = context->plain_encoding;

	camel_data_wrapper_set_mime_type_field (
		context->top_level_part, type);

	camel_content_type_unref (type);

	/* Build the text/html part, and wrap it and the text/plain part
	 * in a multipart/alternative part.  Additionally, if there are
	 * inline images then wrap the multipart/alternative part along
	 * with the images in a multipart/related part.
	 *
	 * So the structure of all this will be:
	 *
	 *    multipart/related
	 *        multipart/alternative
	 *            text/plain
	 *            text/html
	 *        image/<<whatever>>
	 *        image/<<whatever>>
	 *        ...
	 */

	if (flags & COMPOSER_FLAG_HTML_CONTENT) {
		gchar *text;
		gsize length;
		gboolean pre_encode;

		clear_current_images (composer);

		if (flags & COMPOSER_FLAG_SAVE_OBJECT_DATA)
			gtkhtml_editor_run_command (editor, "save-data-on");

		data = g_byte_array_new ();
		text = gtkhtml_editor_get_text_html (editor, &length);
		g_byte_array_append (data, (guint8 *) text, (guint) length);
		pre_encode = text_requires_quoted_printable (text, length);
		g_free (text);

		if (flags & COMPOSER_FLAG_SAVE_OBJECT_DATA)
			gtkhtml_editor_run_command (editor, "save-data-off");

		mem_stream = camel_stream_mem_new_with_byte_array (data);
		stream = camel_stream_filter_new (mem_stream);
		g_object_unref (mem_stream);

		if (pre_encode)
			composer_add_quoted_printable_filter (stream);

		/* Construct the content object.  This does not block since
		 * we're constructing the data wrapper from a memory stream. */
		html = camel_data_wrapper_new ();
		camel_data_wrapper_construct_from_stream_sync (
			html, stream, NULL, NULL);
		g_object_unref (stream);

		camel_data_wrapper_set_mime_type (
			html, "text/html; charset=utf-8");

		/* Avoid re-encoding the data when adding it to a MIME part. */
		if (pre_encode)
			html->encoding =
				CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE;

		/* Build the multipart/alternative */
		body = camel_multipart_new ();
		camel_data_wrapper_set_mime_type (
			CAMEL_DATA_WRAPPER (body), "multipart/alternative");
		camel_multipart_set_boundary (body, NULL);

		/* Add the text/plain part. */
		part = camel_mime_part_new ();
		camel_medium_set_content (
			CAMEL_MEDIUM (part), context->top_level_part);
		camel_mime_part_set_encoding (part, context->plain_encoding);
		camel_multipart_add_part (body, part);
		g_object_unref (part);

		/* Add the text/html part. */
		part = camel_mime_part_new ();
		camel_medium_set_content (CAMEL_MEDIUM (part), html);
		camel_mime_part_set_encoding (
			part, CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE);
		camel_multipart_add_part (body, part);
		g_object_unref (part);

		g_object_unref (context->top_level_part);
		g_object_unref (html);

		/* If there are inlined images, construct a multipart/related
		 * containing the multipart/alternative and the images. */
		if (priv->current_images) {
			CamelMultipart *html_with_images;

			html_with_images = camel_multipart_new ();
			camel_data_wrapper_set_mime_type (
				CAMEL_DATA_WRAPPER (html_with_images),
				"multipart/related; "
				"type=\"multipart/alternative\"");
			camel_multipart_set_boundary (html_with_images, NULL);

			part = camel_mime_part_new ();
			camel_medium_set_content (
				CAMEL_MEDIUM (part),
				CAMEL_DATA_WRAPPER (body));
			camel_multipart_add_part (html_with_images, part);
			g_object_unref (part);

			g_object_unref (body);

			add_inlined_images (composer, html_with_images);
			clear_current_images (composer);

			context->top_level_part =
				CAMEL_DATA_WRAPPER (html_with_images);
		} else
			context->top_level_part =
				CAMEL_DATA_WRAPPER (body);
	}

	/* If there are attachments, wrap what we've built so far
	 * along with the attachments in a multipart/mixed part. */
	if (e_attachment_store_get_num_attachments (store) > 0) {
		CamelMultipart *multipart = camel_multipart_new ();

		/* Generate a random boundary. */
		camel_multipart_set_boundary (multipart, NULL);

		part = camel_mime_part_new ();
		camel_medium_set_content (
			CAMEL_MEDIUM (part),
			context->top_level_part);
		if (context->top_level_part == context->text_plain_part)
			camel_mime_part_set_encoding (
				part, context->plain_encoding);
		camel_multipart_add_part (multipart, part);
		g_object_unref (part);

		e_attachment_store_add_to_multipart (
			store, multipart, priv->charset);

		g_object_unref (context->top_level_part);
		context->top_level_part = CAMEL_DATA_WRAPPER (multipart);
	}

	/* Run any blocking operations in a separate thread. */
	if (context->need_thread)
		g_simple_async_result_run_in_thread (
			simple, (GSimpleAsyncThreadFunc)
			composer_build_message_thread,
			io_priority, cancellable);
	else
		g_simple_async_result_complete (simple);

	g_object_unref (simple);
}

static CamelMimeMessage *
composer_build_message_finish (EMsgComposer *composer,
                               GAsyncResult *result,
                               GError **error)
{
	GSimpleAsyncResult *simple;
	AsyncContext *context;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (composer), composer_build_message), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	context = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	/* Finalize some details before returning. */

	if (!context->skip_content)
		camel_medium_set_content (
			CAMEL_MEDIUM (context->message),
			context->top_level_part);

	if (context->top_level_part == context->text_plain_part)
		camel_mime_part_set_encoding (
			CAMEL_MIME_PART (context->message),
			context->plain_encoding);

	return g_object_ref (context->message);
}

/* Signatures */

static gchar *
encode_signature_uid (ESignature *signature)
{
	const gchar *uid;
	const gchar *s;
	gchar *ename, *e;
	gint len = 0;

	uid = e_signature_get_uid (signature);

	s = uid;
	while (*s) {
		len++;
		if (*s == '"' || *s == '.' || *s == '=')
			len++;
		s++;
	}

	ename = g_new (gchar, len + 1);

	s = uid;
	e = ename;
	while (*s) {
		if (*s == '"') {
			*e = '.';
			e++;
			*e = '1';
			e++;
		} else if (*s == '=') {
			*e = '.';
			e++;
			*e = '2';
			e++;
		} else {
			*e = *s;
			e++;
		}
		if (*s == '.') {
			*e = '.';
			e++;
		}
		s++;
	}
	*e = 0;

	return ename;
}

static gchar *
decode_signature_name (const gchar *name)
{
	const gchar *s;
	gchar *dname, *d;
	gint len = 0;

	s = name;
	while (*s) {
		len++;
		if (*s == '.') {
			s++;
			if (!*s || !(*s == '.' || *s == '1' || *s == '2'))
				return NULL;
		}
		s++;
	}

	dname = g_new (char, len + 1);

	s = name;
	d = dname;
	while (*s) {
		if (*s == '.') {
			s++;
			if (!*s || !(*s == '.' || *s == '1' || *s == '2')) {
				g_free (dname);
				return NULL;
			}
			if (*s == '1')
				*d = '"';
			else if (*s == '2')
				*d = '=';
			else
				*d = '.';
		} else
			*d = *s;
		d++;
		s++;
	}
	*d = 0;

	return dname;
}

static gboolean
is_top_signature (EMsgComposer *composer)
{
	EShell *shell;
	EShellSettings *shell_settings;
	EMsgComposerPrivate *priv = composer->priv;

	g_return_val_if_fail (priv != NULL, FALSE);

	/* The composer had been created from a stored message, thus the
	 * signature placement is either there already, or pt it at the
	 * bottom regardless of a preferences (which is for reply anyway,
	 * not for Edit as new) */
	if (priv->is_from_message)
		return FALSE;

	shell = e_msg_composer_get_shell (composer);
	shell_settings = e_shell_get_shell_settings (shell);

	return e_shell_settings_get_boolean (
		shell_settings, "composer-top-signature");
}

static gboolean
add_signature_delim (EMsgComposer *composer)
{
	EShell *shell;
	EShellSettings *shell_settings;

	shell = e_msg_composer_get_shell (composer);
	shell_settings = e_shell_get_shell_settings (shell);

	return !e_shell_settings_get_boolean (
		shell_settings, "composer-no-signature-delim");
}

#define CONVERT_SPACES CAMEL_MIME_FILTER_TOHTML_CONVERT_SPACES
#define NO_SIGNATURE_TEXT	\
	"<!--+GtkHTML:<DATA class=\"ClueFlow\" " \
	"                     key=\"signature\" " \
	"                   value=\"1\">-->" \
	"<!--+GtkHTML:<DATA class=\"ClueFlow\" " \
	"                     key=\"signature_name\" " \
	"                   value=\"uid:Noname\">--><BR>"

static gchar *
get_signature_html (EMsgComposer *composer)
{
	EComposerHeaderTable *table;
	gchar *text = NULL, *html = NULL;
	ESignature *signature;
	gboolean format_html, add_delim;

	table = e_msg_composer_get_header_table (composer);
	signature = e_composer_header_table_get_signature (table);

	if (!signature)
		return NULL;

	add_delim = add_signature_delim (composer);

	if (!e_signature_get_autogenerated (signature)) {
		const gchar *filename;

		filename = e_signature_get_filename (signature);
		if (filename == NULL)
			return NULL;

		format_html = e_signature_get_is_html (signature);

		if (e_signature_get_is_script (signature))
			text = e_run_signature_script (filename);
		else
			text = e_read_signature_file (signature, TRUE, NULL);
	} else {
		EAccount *account;
		EAccountIdentity *id;
		gchar *organization = NULL;
		gchar *address = NULL;
		gchar *name = NULL;

		account = e_composer_header_table_get_account (table);
		if (!account)
			return NULL;

		id = account->id;
		if (id->address != NULL)
			address = camel_text_to_html (
				id->address, CONVERT_SPACES, 0);
		if (id->name != NULL)
			name = camel_text_to_html (
				id->name, CONVERT_SPACES, 0);
		if (id->organization != NULL)
			organization = camel_text_to_html (
				id->organization, CONVERT_SPACES, 0);

		text = g_strdup_printf ("%s%s%s%s%s%s%s%s%s",
					add_delim ? "-- \n<BR>" : "",
					name ? name : "",
					(address && *address) ? " &lt;<A HREF=\"mailto:" : "",
					address ? address : "",
					(address && *address) ? "\">" : "",
					address ? address : "",
					(address && *address) ? "</A>&gt;" : "",
					(organization && *organization) ? "<BR>" : "",
					organization ? organization : "");
		g_free (address);
		g_free (name);
		g_free (organization);
		format_html = TRUE;
	}

	/* printf ("text: %s\n", text); */
	if (text) {
		gchar *encoded_uid = NULL;
		const gchar *sig_delim = format_html ? "-- \n<BR>" : "-- \n";
		const gchar *sig_delim_ent = format_html ? "\n-- \n<BR>" : "\n-- \n";

		if (signature)
			encoded_uid = encode_signature_uid (signature);

		/* The signature dash convention ("-- \n") is specified
		 * in the "Son of RFC 1036", section 4.3.2.
		 * http://www.chemie.fu-berlin.de/outerspace/netnews/son-of-1036.html
		 */
		html = g_strdup_printf (
			"<!--+GtkHTML:<DATA class=\"ClueFlow\" "
			"    key=\"signature\" value=\"1\">-->"
			"<!--+GtkHTML:<DATA class=\"ClueFlow\" "
			"    key=\"signature_name\" value=\"uid:%s\">-->"
			"<TABLE WIDTH=\"100%%\" CELLSPACING=\"0\""
			" CELLPADDING=\"0\"><TR><TD>"
			"%s%s%s%s"
			"%s</TD></TR></TABLE>",
			encoded_uid ? encoded_uid : "",
			format_html ? "" : "<PRE>\n",
			!add_delim ? "" :
				(!strncmp (
				sig_delim, text, strlen (sig_delim)) ||
				strstr (text, sig_delim_ent))
				? "" : sig_delim,
			text,
			format_html ? "" : "</PRE>\n",
			is_top_signature (composer) ? "<BR>" : "");
		g_free (text);
		g_free (encoded_uid);
		text = html;
	}

	return text;
}

static void
set_editor_text (EMsgComposer *composer,
                 const gchar *text,
                 gboolean set_signature)
{
	gchar *body = NULL;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));
	g_return_if_fail (text != NULL);

	/*
	 *
	 * Keeping Signatures in the beginning of composer
	 * ------------------------------------------------
	 *
	 * Purists are gonna blast me for this.
	 * But there are so many people (read Outlook users) who want this.
	 * And Evo is an exchange-client, Outlook-replacement etc.
	 * So Here it goes :(
	 *
	 * -- Sankar
	 *
	 */

	if (is_top_signature (composer)) {
		/* put marker to the top */
		body = g_strdup_printf ("<BR>" NO_SIGNATURE_TEXT "%s", text);
	} else {
		/* no marker => to the bottom */
		body = g_strdup_printf ("%s<BR>", text);
	}

	gtkhtml_editor_set_text_html (GTKHTML_EDITOR (composer), body, -1);

	if (set_signature)
		e_msg_composer_show_sig_file (composer);

	g_free (body);
}

/* Miscellaneous callbacks.  */

static void
attachment_store_changed_cb (EMsgComposer *composer)
{
	GtkhtmlEditor *editor;

	/* Mark the editor as changed so it prompts about unsaved
	 * changes on close. */
	editor = GTKHTML_EDITOR (composer);
	gtkhtml_editor_set_changed (editor, TRUE);
}

static void
msg_composer_subject_changed_cb (EMsgComposer *composer)
{
	EComposerHeaderTable *table;
	const gchar *subject;

	table = e_msg_composer_get_header_table (composer);
	subject = e_composer_header_table_get_subject (table);

	if (subject == NULL || *subject == '\0')
		subject = _("Compose Message");

	gtk_window_set_title (GTK_WINDOW (composer), subject);
}

static void
msg_composer_account_changed_cb (EMsgComposer *composer)
{
	EMsgComposerPrivate *p = composer->priv;
	EComposerHeaderTable *table;
	GtkToggleAction *action;
	ESignature *signature;
	EAccount *account;
	gboolean active, can_sign;
	const gchar *uid;

	table = e_msg_composer_get_header_table (composer);
	account = e_composer_header_table_get_account (table);

	if (account == NULL) {
		e_msg_composer_show_sig_file (composer);
		return;
	}

	can_sign = (!account->pgp_no_imip_sign || p->mime_type == NULL ||
		g_ascii_strncasecmp (p->mime_type, "text/calendar", 13) != 0);

	action = GTK_TOGGLE_ACTION (ACTION (PGP_SIGN));
	active = account->pgp_always_sign && can_sign;
	gtk_toggle_action_set_active (action, active);

	action = GTK_TOGGLE_ACTION (ACTION (SMIME_SIGN));
	active = account->smime_sign_default && can_sign;
	gtk_toggle_action_set_active (action, active);

	action = GTK_TOGGLE_ACTION (ACTION (SMIME_ENCRYPT));
	active = account->smime_encrypt_default;
	gtk_toggle_action_set_active (action, active);

	uid = account->id->sig_uid;
	signature = uid ? e_get_signature_by_uid (uid) : NULL;
	e_composer_header_table_set_signature (table, signature);
	e_msg_composer_show_sig_file (composer);
}

static void
msg_composer_paste_clipboard_targets_cb (GtkClipboard *clipboard,
                                         GdkAtom *targets,
                                         gint n_targets,
                                         EMsgComposer *composer)
{
	GtkhtmlEditor *editor;
	gboolean html_mode;

	editor = GTKHTML_EDITOR (composer);
	html_mode = gtkhtml_editor_get_html_mode (editor);

	/* Order is important here to ensure common use cases are
	 * handled correctly.  See GNOME bug #603715 for details. */

	if (gtk_targets_include_uri (targets, n_targets)) {
		e_composer_paste_uris (composer, clipboard);
		return;
	}

	/* Only paste HTML content in HTML mode. */
	if (html_mode) {
		if (e_targets_include_html (targets, n_targets)) {
			e_composer_paste_html (composer, clipboard);
			return;
		}
	}

	if (gtk_targets_include_text (targets, n_targets)) {
		e_composer_paste_text (composer, clipboard);
		return;
	}

	if (gtk_targets_include_image (targets, n_targets, TRUE)) {
		e_composer_paste_image (composer, clipboard);
		return;
	}
}

static void
msg_composer_paste_clipboard_cb (EWebViewGtkHTML *web_view,
                                 EMsgComposer *composer)
{
	GtkClipboard *clipboard;

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);

	gtk_clipboard_request_targets (
		clipboard, (GtkClipboardTargetsReceivedFunc)
		msg_composer_paste_clipboard_targets_cb, composer);

	g_signal_stop_emission_by_name (web_view, "paste-clipboard");
}

static void
msg_composer_realize_gtkhtml_cb (GtkWidget *widget,
                                 EMsgComposer *composer)
{
	EAttachmentView *view;
	GtkTargetList *target_list;
	GtkTargetEntry *targets;
	gint n_targets;

	/* XXX GtkHTML doesn't set itself up as a drag destination until
	 *     it's realized, and we need to amend to its target list so
	 *     it will accept the same drag targets as the attachment bar.
	 *     Do this any earlier and GtkHTML will just overwrite us. */

	/* When redirecting a message, the message body is not
	 * editable and therefore cannot be a drag destination. */
	if (!e_web_view_gtkhtml_get_editable (E_WEB_VIEW_GTKHTML (widget)))
		return;

	view = e_msg_composer_get_attachment_view (composer);

	target_list = e_attachment_view_get_target_list (view);
	targets = gtk_target_table_new_from_list (target_list, &n_targets);

	target_list = gtk_drag_dest_get_target_list (widget);
	gtk_target_list_add_table (target_list, targets, n_targets);

	gtk_target_table_free (targets, n_targets);
}

static gboolean
msg_composer_drag_motion_cb (GtkWidget *widget,
                             GdkDragContext *context,
                             gint x,
                             gint y,
                             guint time,
                             EMsgComposer *composer)
{
	EAttachmentView *view;

	view = e_msg_composer_get_attachment_view (composer);

	/* Stop the signal from propagating to GtkHtml. */
	g_signal_stop_emission_by_name (widget, "drag-motion");

	return e_attachment_view_drag_motion (view, context, x, y, time);
}

static void
msg_composer_drag_data_received_cb (GtkWidget *widget,
                                    GdkDragContext *context,
                                    gint x,
                                    gint y,
                                    GtkSelectionData *selection,
                                    guint info,
                                    guint time,
                                    EMsgComposer *composer)
{
	EAttachmentView *view;

	/* HTML mode has a few special cases for drops... */
	if (gtkhtml_editor_get_html_mode (GTKHTML_EDITOR (composer))) {

		/* If we're receiving an image, we want the image to be
		 * inserted in the message body.  Let GtkHtml handle it. */
		if (gtk_selection_data_targets_include_image (selection, TRUE))
			return;

		/* If we're receiving URIs and -all- the URIs point to
		 * image files, we want the image(s) to be inserted in
		 * the message body.  Let GtkHtml handle it. */
		if (e_composer_selection_is_image_uris (composer, selection))
			return;
	}

	view = e_msg_composer_get_attachment_view (composer);

	/* Forward the data to the attachment view.  Note that calling
	 * e_attachment_view_drag_data_received() will not work because
	 * that function only handles the case where all the other drag
	 * handlers have failed. */
	e_attachment_paned_drag_data_received (
		E_ATTACHMENT_PANED (view),
		context, x, y, selection, info, time);

	/* Stop the signal from propagating to GtkHtml. */
	g_signal_stop_emission_by_name (widget, "drag-data-received");
}

static void
msg_composer_notify_header_cb (EMsgComposer *composer)
{
	GtkhtmlEditor *editor;

	editor = GTKHTML_EDITOR (composer);
	gtkhtml_editor_set_changed (editor, TRUE);
}

static gboolean
msg_composer_delete_event_cb (EMsgComposer *composer)
{
	EShell *shell;
	GtkApplication *application;
	GList *windows;

	shell = e_msg_composer_get_shell (composer);

	/* If the "async" action group is insensitive, it means an
	 * asynchronous operation is in progress.  Block the event. */
	if (!gtk_action_group_get_sensitive (composer->priv->async_actions))
		return TRUE;

	application = GTK_APPLICATION (shell);
	windows = gtk_application_get_windows (application);

	if (g_list_length (windows) == 1) {
		/* This is the last watched window, use the quit
		 * mechanism to have a draft saved properly */
		e_shell_quit (shell, E_SHELL_QUIT_ACTION);
	} else {
		/* There are more watched windows opened,
		 * invoke only a close action */
		gtk_action_activate (ACTION (CLOSE));
	}

	return TRUE;
}

static void
msg_composer_prepare_for_quit_cb (EShell *shell,
                                  EActivity *activity,
                                  EMsgComposer *composer)
{
	if (e_msg_composer_is_exiting (composer)) {
		/* needs save draft first */
		g_object_ref (activity);
		g_object_weak_ref (
			G_OBJECT (composer), (GWeakNotify)
			g_object_unref, activity);
		gtk_action_activate (ACTION (SAVE_DRAFT));
	}
}

static void
msg_composer_quit_requested_cb (EShell *shell,
                                EShellQuitReason reason,
                                EMsgComposer *composer)
{
	if (e_msg_composer_is_exiting (composer)) {
		g_signal_handlers_disconnect_by_func (
			shell, msg_composer_quit_requested_cb, composer);
		g_signal_handlers_disconnect_by_func (
			shell, msg_composer_prepare_for_quit_cb, composer);
	} else if (!e_msg_composer_can_close (composer, FALSE) &&
			!e_msg_composer_is_exiting (composer)) {
		e_shell_cancel_quit (shell);
	}
}

static void
msg_composer_set_shell (EMsgComposer *composer,
                        EShell *shell)
{
	g_return_if_fail (E_IS_SHELL (shell));
	g_return_if_fail (composer->priv->shell == NULL);

	composer->priv->shell = shell;

	g_object_add_weak_pointer (
		G_OBJECT (shell), &composer->priv->shell);
}

static void
msg_composer_set_property (GObject *object,
                           guint property_id,
                           const GValue *value,
                           GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_SHELL:
			msg_composer_set_shell (
				E_MSG_COMPOSER (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
msg_composer_get_property (GObject *object,
                           guint property_id,
                           GValue *value,
                           GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_FOCUS_TRACKER:
			g_value_set_object (
				value, e_msg_composer_get_focus_tracker (
				E_MSG_COMPOSER (object)));
			return;

		case PROP_SHELL:
			g_value_set_object (
				value, e_msg_composer_get_shell (
				E_MSG_COMPOSER (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
msg_composer_finalize (GObject *object)
{
	EMsgComposer *composer = E_MSG_COMPOSER (object);

	e_composer_private_finalize (composer);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_msg_composer_parent_class)->finalize (object);
}

static void
msg_composer_gallery_drag_data_get (GtkIconView *icon_view,
                                    GdkDragContext *context,
                                    GtkSelectionData *selection_data,
                                    guint target_type,
                                    guint time)
{
	GtkTreePath *path;
	GtkCellRenderer *cell;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GdkAtom target;
	gchar *str_data;

	if (!gtk_icon_view_get_cursor (icon_view, &path, &cell))
		return;

	target = gtk_selection_data_get_target (selection_data);

	model = gtk_icon_view_get_model (icon_view);
	gtk_tree_model_get_iter (model, &iter, path);
	gtk_tree_model_get (model, &iter, 1, &str_data, -1);
	gtk_tree_path_free (path);

	/* only supports "text/uri-list" */
	gtk_selection_data_set (
		selection_data, target, 8,
		(guchar *) str_data, strlen (str_data));
	g_free (str_data);
}

static void
msg_composer_constructed (GObject *object)
{
	EShell *shell;
	EShellSettings *shell_settings;
	GtkhtmlEditor *editor;
	EMsgComposer *composer;
	EAttachmentView *view;
	EAttachmentStore *store;
	EComposerHeaderTable *table;
	EWebViewGtkHTML *web_view;
	GtkUIManager *ui_manager;
	GtkToggleAction *action;
	GArray *array;
	const gchar *id;
	gboolean active;
	guint binding_id;

	editor = GTKHTML_EDITOR (object);
	composer = E_MSG_COMPOSER (object);

	shell = e_msg_composer_get_shell (composer);
	shell_settings = e_shell_get_shell_settings (shell);

	if (e_shell_get_express_mode (shell)) {
		GtkWindow *parent = e_shell_get_active_window (shell);
		gtk_window_set_transient_for (GTK_WINDOW (composer), parent);
	}

	e_composer_private_constructed (composer);

	web_view = e_msg_composer_get_web_view (composer);
	ui_manager = gtkhtml_editor_get_ui_manager (editor);
	view = e_msg_composer_get_attachment_view (composer);
	table = E_COMPOSER_HEADER_TABLE (composer->priv->header_table);

	gtk_window_set_title (GTK_WINDOW (composer), _("Compose Message"));
	gtk_window_set_icon_name (GTK_WINDOW (composer), "mail-message-new");

	g_signal_connect (
		object, "delete-event",
		G_CALLBACK (msg_composer_delete_event_cb), NULL);

	e_shell_adapt_window_size (shell, GTK_WINDOW (object));

	gtk_application_add_window (
		GTK_APPLICATION (shell), GTK_WINDOW (object));

	g_signal_connect (
		shell, "quit-requested",
		G_CALLBACK (msg_composer_quit_requested_cb), composer);

	g_signal_connect (
		shell, "prepare-for-quit",
		G_CALLBACK (msg_composer_prepare_for_quit_cb), composer);

	/* Restore Persistent State */

	array = composer->priv->gconf_bridge_binding_ids;

	binding_id = gconf_bridge_bind_window (
		gconf_bridge_get (),
		COMPOSER_GCONF_WINDOW_PREFIX,
		GTK_WINDOW (composer), TRUE, FALSE);
	g_array_append_val (array, binding_id);

	/* Honor User Preferences */

	action = GTK_TOGGLE_ACTION (ACTION (REQUEST_READ_RECEIPT));
	active = e_shell_settings_get_boolean (
		shell_settings, "composer-request-receipt");
	gtk_toggle_action_set_active (action, active);

	/* Clipboard Support */

	g_signal_connect (
		web_view, "paste-clipboard",
		G_CALLBACK (msg_composer_paste_clipboard_cb), composer);

	/* Drag-and-Drop Support */

	g_signal_connect (
		web_view, "realize",
		G_CALLBACK (msg_composer_realize_gtkhtml_cb), composer);

	g_signal_connect (
		web_view, "drag-motion",
		G_CALLBACK (msg_composer_drag_motion_cb), composer);

	g_signal_connect (
		web_view, "drag-data-received",
		G_CALLBACK (msg_composer_drag_data_received_cb), composer);

	g_signal_connect (
		composer->priv->gallery_icon_view, "drag-data-get",
		G_CALLBACK (msg_composer_gallery_drag_data_get), NULL);

	/* Configure Headers */

	e_composer_header_table_set_account_list (
		table, e_get_account_list ());
	e_composer_header_table_set_signature_list (
		table, e_get_signature_list ());

	g_signal_connect_swapped (
		table, "notify::account",
		G_CALLBACK (msg_composer_account_changed_cb), composer);
	g_signal_connect_swapped (
		table, "notify::destinations-bcc",
		G_CALLBACK (msg_composer_notify_header_cb), composer);
	g_signal_connect_swapped (
		table, "notify::destinations-cc",
		G_CALLBACK (msg_composer_notify_header_cb), composer);
	g_signal_connect_swapped (
		table, "notify::destinations-to",
		G_CALLBACK (msg_composer_notify_header_cb), composer);
	g_signal_connect_swapped (
		table, "notify::reply-to",
		G_CALLBACK (msg_composer_notify_header_cb), composer);
	g_signal_connect_swapped (
		table, "notify::signature",
		G_CALLBACK (e_msg_composer_show_sig_file), composer);
	g_signal_connect_swapped (
		table, "notify::subject",
		G_CALLBACK (msg_composer_subject_changed_cb), composer);
	g_signal_connect_swapped (
		table, "notify::subject",
		G_CALLBACK (msg_composer_notify_header_cb), composer);

	msg_composer_account_changed_cb (composer);

	/* Attachments */

	store = e_attachment_view_get_store (view);

	g_signal_connect_swapped (
		store, "row-deleted",
		G_CALLBACK (attachment_store_changed_cb), composer);

	g_signal_connect_swapped (
		store, "row-inserted",
		G_CALLBACK (attachment_store_changed_cb), composer);

	/* Initialization may have tripped the "changed" state. */
	gtkhtml_editor_set_changed (editor, FALSE);

	id = "org.gnome.evolution.composer";
	e_plugin_ui_register_manager (ui_manager, id, composer);
	e_plugin_ui_enable_manager (ui_manager, id);

	e_extensible_load_extensions (E_EXTENSIBLE (composer));

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_msg_composer_parent_class)->constructed (object);
}

static void
msg_composer_dispose (GObject *object)
{
	EMsgComposer *composer = E_MSG_COMPOSER (object);
	EShell *shell;

	if (composer->priv->address_dialog != NULL) {
		gtk_widget_destroy (composer->priv->address_dialog);
		composer->priv->address_dialog = NULL;
	}

	/* FIXME Our EShell is already unreferenced. */
	shell = e_shell_get_default ();

	g_signal_handlers_disconnect_by_func (
		shell, msg_composer_quit_requested_cb, composer);
	g_signal_handlers_disconnect_by_func (
		shell, msg_composer_prepare_for_quit_cb, composer);

	e_composer_private_dispose (composer);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_msg_composer_parent_class)->dispose (object);
}

static void
msg_composer_map (GtkWidget *widget)
{
	EComposerHeaderTable *table;
	GtkWidget *input_widget;
	const gchar *text;

	/* Chain up to parent's map() method. */
	GTK_WIDGET_CLASS (e_msg_composer_parent_class)->map (widget);

	table = e_msg_composer_get_header_table (E_MSG_COMPOSER (widget));

	/* If the 'To' field is empty, focus it. */
	input_widget =
		e_composer_header_table_get_header (
		table, E_COMPOSER_HEADER_TO)->input_widget;
	text = gtk_entry_get_text (GTK_ENTRY (input_widget));
	if (gtk_widget_get_visible (input_widget) && (text == NULL || *text == '\0')) {
		gtk_widget_grab_focus (input_widget);
		return;
	}

	/* If not, check the 'Subject' field. */
	input_widget =
		e_composer_header_table_get_header (
		table, E_COMPOSER_HEADER_SUBJECT)->input_widget;
	text = gtk_entry_get_text (GTK_ENTRY (input_widget));
	if (gtk_widget_get_visible (input_widget) && (text == NULL || *text == '\0')) {
		gtk_widget_grab_focus (input_widget);
		return;
	}

	/* Jump to the editor as a last resort. */
	gtkhtml_editor_run_command (GTKHTML_EDITOR (widget), "grab-focus");
}

static gboolean
msg_composer_key_press_event (GtkWidget *widget,
                              GdkEventKey *event)
{
	EMsgComposer *composer = E_MSG_COMPOSER (widget);
	GtkWidget *input_widget;
	GtkhtmlEditor *editor;
	EWebViewGtkHTML *web_view;

	editor = GTKHTML_EDITOR (widget);
	composer = E_MSG_COMPOSER (widget);
	web_view = e_msg_composer_get_web_view (composer);

	input_widget =
		e_composer_header_table_get_header (
		e_msg_composer_get_header_table (composer),
		E_COMPOSER_HEADER_SUBJECT)->input_widget;

#ifdef HAVE_XFREE
	if (event->keyval == XF86XK_Send) {
		e_msg_composer_send (composer);
		return TRUE;
	}
#endif /* HAVE_XFREE */

	if (event->keyval == GDK_KEY_Escape) {
		gtk_action_activate (ACTION (CLOSE));
		return TRUE;
	}

	if (event->keyval == GDK_KEY_Tab && gtk_widget_is_focus (input_widget)) {
		gtkhtml_editor_run_command (editor, "grab-focus");
		return TRUE;
	}

	if (event->keyval == GDK_KEY_ISO_Left_Tab &&
		gtk_widget_is_focus (GTK_WIDGET (web_view))) {
		gtk_widget_grab_focus (input_widget);
		return TRUE;
	}

	/* Chain up to parent's key_press_event() method. */
	return GTK_WIDGET_CLASS (e_msg_composer_parent_class)->
		key_press_event (widget, event);
}

static void
msg_composer_cut_clipboard (GtkhtmlEditor *editor)
{
	/* Do nothing.  EFocusTracker handles this. */
}

static void
msg_composer_copy_clipboard (GtkhtmlEditor *editor)
{
	/* Do nothing.  EFocusTracker handles this. */
}

static void
msg_composer_paste_clipboard (GtkhtmlEditor *editor)
{
	/* Do nothing.  EFocusTracker handles this. */
}

static void
msg_composer_select_all (GtkhtmlEditor *editor)
{
	/* Do nothing.  EFocusTracker handles this. */
}

static void
msg_composer_command_before (GtkhtmlEditor *editor,
                             const gchar *command)
{
	EMsgComposer *composer;
	const gchar *data;

	composer = E_MSG_COMPOSER (editor);

	if (strcmp (command, "insert-paragraph") != 0)
		return;

	if (composer->priv->in_signature_insert)
		return;

	data = gtkhtml_editor_get_paragraph_data (editor, "orig");
	if (data != NULL && *data == '1') {
		gtkhtml_editor_run_command (editor, "text-default-color");
		gtkhtml_editor_run_command (editor, "italic-off");
		return;
	};

	data = gtkhtml_editor_get_paragraph_data (editor, "signature");
	if (data != NULL && *data == '1') {
		gtkhtml_editor_run_command (editor, "text-default-color");
		gtkhtml_editor_run_command (editor, "italic-off");
	}
}

static void
msg_composer_command_after (GtkhtmlEditor *editor,
                            const gchar *command)
{
	EMsgComposer *composer;
	const gchar *data;

	composer = E_MSG_COMPOSER (editor);

	if (strcmp (command, "insert-paragraph") != 0)
		return;

	if (composer->priv->in_signature_insert)
		return;

	gtkhtml_editor_run_command (editor, "italic-off");

	data = gtkhtml_editor_get_paragraph_data (editor, "orig");
	if (data != NULL && *data == '1')
		e_msg_composer_reply_indent (composer);
	gtkhtml_editor_set_paragraph_data (editor, "orig", "0");

	data = gtkhtml_editor_get_paragraph_data (editor, "signature");
	if (data == NULL || *data != '1')
		return;

	/* Clear the signature. */
	if (gtkhtml_editor_is_paragraph_empty (editor))
		gtkhtml_editor_set_paragraph_data (editor, "signature" ,"0");

	else if (gtkhtml_editor_is_previous_paragraph_empty (editor) &&
		gtkhtml_editor_run_command (editor, "cursor-backward")) {

		gtkhtml_editor_set_paragraph_data (editor, "signature", "0");
		gtkhtml_editor_run_command (editor, "cursor-forward");
	}

	gtkhtml_editor_run_command (editor, "text-default-color");
	gtkhtml_editor_run_command (editor, "italic-off");
}

static gchar *
msg_composer_image_uri (GtkhtmlEditor *editor,
                        const gchar *uri)
{
	EMsgComposer *composer;
	GHashTable *hash_table;
	CamelMimePart *part;
	const gchar *cid;

	composer = E_MSG_COMPOSER (editor);

	hash_table = composer->priv->inline_images_by_url;
	part = g_hash_table_lookup (hash_table, uri);

	if (part == NULL && g_str_has_prefix (uri, "file:"))
		part = e_msg_composer_add_inline_image_from_file (
			composer, uri + 5);

	if (part == NULL && g_str_has_prefix (uri, "cid:")) {
		hash_table = composer->priv->inline_images;
		part = g_hash_table_lookup (hash_table, uri);
	}

	if (part == NULL)
		return NULL;

	composer->priv->current_images =
		g_list_prepend (composer->priv->current_images, part);

	cid = camel_mime_part_get_content_id (part);
	if (cid == NULL)
		return NULL;

	return g_strconcat ("cid:", cid, NULL);
}

static void
msg_composer_object_deleted (GtkhtmlEditor *editor)
{
	const gchar *data;

	if (!gtkhtml_editor_is_paragraph_empty (editor))
		return;

	data = gtkhtml_editor_get_paragraph_data (editor, "orig");
	if (data != NULL && *data == '1') {
		gtkhtml_editor_set_paragraph_data (editor, "orig", "0");
		gtkhtml_editor_run_command (editor, "indent-zero");
		gtkhtml_editor_run_command (editor, "style-normal");
		gtkhtml_editor_run_command (editor, "text-default-color");
		gtkhtml_editor_run_command (editor, "italic-off");
		gtkhtml_editor_run_command (editor, "insert-paragraph");
		gtkhtml_editor_run_command (editor, "delete-back");
	}

	data = gtkhtml_editor_get_paragraph_data (editor, "signature");
	if (data != NULL && *data == '1')
		gtkhtml_editor_set_paragraph_data (editor, "signature", "0");
}

static gboolean
msg_composer_presend (EMsgComposer *composer)
{
	/* This keeps the signal accumulator at TRUE. */
	return TRUE;
}

static void
msg_composer_submit_alert (EAlertSink *alert_sink,
                           EAlert *alert)
{
	EMsgComposerPrivate *priv;
	EAlertBar *alert_bar;
	GtkWidget *dialog;
	GtkWindow *parent;

	priv = E_MSG_COMPOSER (alert_sink)->priv;

	switch (e_alert_get_message_type (alert)) {
		case GTK_MESSAGE_INFO:
		case GTK_MESSAGE_WARNING:
		case GTK_MESSAGE_ERROR:
			alert_bar = E_ALERT_BAR (priv->alert_bar);
			e_alert_bar_add_alert (alert_bar, alert);
			break;

		default:
			parent = GTK_WINDOW (alert_sink);
			dialog = e_alert_dialog_new (parent, alert);
			gtk_dialog_run (GTK_DIALOG (dialog));
			gtk_widget_destroy (dialog);
			break;
	}
}

static gboolean
msg_composer_accumulator_false_abort (GSignalInvocationHint *ihint,
                                      GValue *return_accu,
                                      const GValue *handler_return,
                                      gpointer dummy)
{
	gboolean v_boolean;

	v_boolean = g_value_get_boolean (handler_return);
	g_value_set_boolean (return_accu, v_boolean);

	/* FALSE means abort the signal emission. */
	return v_boolean;
}

static void
e_msg_composer_class_init (EMsgComposerClass *class)
{
	GObjectClass *object_class;
	GtkWidgetClass *widget_class;
	GtkhtmlEditorClass *editor_class;

	g_type_class_add_private (class, sizeof (EMsgComposerPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = msg_composer_set_property;
	object_class->get_property = msg_composer_get_property;
	object_class->dispose = msg_composer_dispose;
	object_class->finalize = msg_composer_finalize;
	object_class->constructed = msg_composer_constructed;

	widget_class = GTK_WIDGET_CLASS (class);
	widget_class->map = msg_composer_map;
	widget_class->key_press_event = msg_composer_key_press_event;

	editor_class = GTKHTML_EDITOR_CLASS (class);
	editor_class->cut_clipboard = msg_composer_cut_clipboard;
	editor_class->copy_clipboard = msg_composer_copy_clipboard;
	editor_class->paste_clipboard = msg_composer_paste_clipboard;
	editor_class->select_all = msg_composer_select_all;
	editor_class->command_before = msg_composer_command_before;
	editor_class->command_after = msg_composer_command_after;
	editor_class->image_uri = msg_composer_image_uri;
	editor_class->link_clicked = NULL; /* EWebView handles this */
	editor_class->object_deleted = msg_composer_object_deleted;

	class->presend = msg_composer_presend;

	g_object_class_install_property (
		object_class,
		PROP_FOCUS_TRACKER,
		g_param_spec_object (
			"focus-tracker",
			NULL,
			NULL,
			E_TYPE_FOCUS_TRACKER,
			G_PARAM_READABLE));

	g_object_class_install_property (
		object_class,
		PROP_SHELL,
		g_param_spec_object (
			"shell",
			"Shell",
			"The EShell singleton",
			E_TYPE_SHELL,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY));

	signals[PRESEND] = g_signal_new (
		"presend",
		G_OBJECT_CLASS_TYPE (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EMsgComposerClass, presend),
		msg_composer_accumulator_false_abort,
		NULL,
		e_marshal_BOOLEAN__VOID,
		G_TYPE_BOOLEAN, 0);

	signals[SEND] = g_signal_new (
		"send",
		G_OBJECT_CLASS_TYPE (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EMsgComposerClass, send),
		NULL, NULL,
		e_marshal_VOID__OBJECT_OBJECT,
		G_TYPE_NONE, 2,
		CAMEL_TYPE_MIME_MESSAGE,
		E_TYPE_ACTIVITY);

	signals[SAVE_TO_DRAFTS] = g_signal_new (
		"save-to-drafts",
		G_OBJECT_CLASS_TYPE (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EMsgComposerClass, save_to_drafts),
		NULL, NULL,
		e_marshal_VOID__OBJECT_OBJECT,
		G_TYPE_NONE, 2,
		CAMEL_TYPE_MIME_MESSAGE,
		E_TYPE_ACTIVITY);

	signals[SAVE_TO_OUTBOX] = g_signal_new (
		"save-to-outbox",
		G_OBJECT_CLASS_TYPE (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EMsgComposerClass, save_to_outbox),
		NULL, NULL,
		e_marshal_VOID__OBJECT_OBJECT,
		G_TYPE_NONE, 2,
		CAMEL_TYPE_MIME_MESSAGE,
		E_TYPE_ACTIVITY);

	signals[PRINT] = g_signal_new (
		"print",
		G_OBJECT_CLASS_TYPE (class),
		G_SIGNAL_RUN_LAST,
		0, NULL, NULL,
		e_marshal_VOID__ENUM_OBJECT_OBJECT,
		G_TYPE_NONE, 3,
		GTK_TYPE_PRINT_OPERATION_ACTION,
		CAMEL_TYPE_MIME_MESSAGE,
		E_TYPE_ACTIVITY);
}

static void
e_msg_composer_alert_sink_init (EAlertSinkInterface *interface)
{
	interface->submit_alert = msg_composer_submit_alert;
}

static void
e_msg_composer_init (EMsgComposer *composer)
{
	composer->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		composer, E_TYPE_MSG_COMPOSER, EMsgComposerPrivate);
}

/* Callbacks.  */

/**
 * e_msg_composer_new:
 * @shell: an #EShell
 *
 * Create a new message composer widget.
 *
 * Returns: A pointer to the newly created widget
 **/
EMsgComposer *
e_msg_composer_new (EShell *shell)
{
	g_return_val_if_fail (E_IS_SHELL (shell), NULL);

	return g_object_new (
		E_TYPE_MSG_COMPOSER,
		"html", e_web_view_gtkhtml_new (), "shell", shell, NULL);
}

EFocusTracker *
e_msg_composer_get_focus_tracker (EMsgComposer *composer)
{
	g_return_val_if_fail (E_IS_MSG_COMPOSER (composer), NULL);

	return composer->priv->focus_tracker;
}

static void
e_msg_composer_set_pending_body (EMsgComposer *composer,
                                 gchar *text,
                                 gssize length)
{
	g_object_set_data_full (
		G_OBJECT (composer), "body:text",
		text, (GDestroyNotify) g_free);
}

static void
e_msg_composer_flush_pending_body (EMsgComposer *composer)
{
	const gchar *body;

	body = g_object_get_data (G_OBJECT (composer), "body:text");

	if (body != NULL)
		set_editor_text (composer, body, FALSE);

	g_object_set_data (G_OBJECT (composer), "body:text", NULL);
}

static void
add_attachments_handle_mime_part (EMsgComposer *composer,
                                  CamelMimePart *mime_part,
                                  gboolean just_inlines,
                                  gboolean related,
                                  gint depth)
{
	CamelContentType *content_type;
	CamelDataWrapper *wrapper;

	if (!mime_part)
		return;

	content_type = camel_mime_part_get_content_type (mime_part);
	wrapper = camel_medium_get_content (CAMEL_MEDIUM (mime_part));

	if (CAMEL_IS_MULTIPART (wrapper)) {
		/* another layer of multipartness... */
		add_attachments_from_multipart (
			composer, (CamelMultipart *) wrapper,
			just_inlines, depth + 1);
	} else if (just_inlines) {
		if (camel_mime_part_get_content_id (mime_part) ||
		    camel_mime_part_get_content_location (mime_part))
			e_msg_composer_add_inline_image_from_mime_part (
				composer, mime_part);
	} else if (related && camel_content_type_is (content_type, "image", "*")) {
		e_msg_composer_add_inline_image_from_mime_part (composer, mime_part);
	} else if (camel_content_type_is (content_type, "text", "*") &&
		camel_mime_part_get_filename (mime_part) == NULL) {
		/* Do nothing if this is a text/anything without a
		 * filename, otherwise attach it too. */
	} else {
		e_msg_composer_attach (composer, mime_part);
	}
}

static void
add_attachments_from_multipart (EMsgComposer *composer,
                                CamelMultipart *multipart,
                                gboolean just_inlines,
                                gint depth)
{
	/* find appropriate message attachments to add to the composer */
	CamelMimePart *mime_part;
	gboolean related;
	gint i, nparts;

	related = camel_content_type_is (
		CAMEL_DATA_WRAPPER (multipart)->mime_type,
		"multipart", "related");

	if (CAMEL_IS_MULTIPART_SIGNED (multipart)) {
		mime_part = camel_multipart_get_part (
			multipart, CAMEL_MULTIPART_SIGNED_CONTENT);
		add_attachments_handle_mime_part (
			composer, mime_part, just_inlines, related, depth);
	} else if (CAMEL_IS_MULTIPART_ENCRYPTED (multipart)) {
		/* XXX What should we do in this case? */
	} else {
		nparts = camel_multipart_get_number (multipart);

		for (i = 0; i < nparts; i++) {
			mime_part = camel_multipart_get_part (multipart, i);
			add_attachments_handle_mime_part (
				composer, mime_part, just_inlines,
				related, depth);
		}
	}
}

/**
 * e_msg_composer_add_message_attachments:
 * @composer: the composer to add the attachments to.
 * @message: the source message to copy the attachments from.
 * @just_inlines: whether to attach all attachments or just add
 * inline images.
 *
 * Walk through all the mime parts in @message and add them to the composer
 * specified in @composer.
 */
void
e_msg_composer_add_message_attachments (EMsgComposer *composer,
                                        CamelMimeMessage *message,
                                        gboolean just_inlines)
{
	CamelDataWrapper *wrapper;

	wrapper = camel_medium_get_content (CAMEL_MEDIUM (message));
	if (!CAMEL_IS_MULTIPART (wrapper))
		return;

	add_attachments_from_multipart (
		composer, (CamelMultipart *) wrapper, just_inlines, 0);
}

static void
handle_multipart_signed (EMsgComposer *composer,
                         CamelMultipart *multipart,
                         GCancellable *cancellable,
                         gint depth)
{
	CamelContentType *content_type;
	CamelDataWrapper *content;
	CamelMimePart *mime_part;
	GtkToggleAction *action = NULL;
	const gchar *protocol;

	content = CAMEL_DATA_WRAPPER (multipart);
	content_type = camel_data_wrapper_get_mime_type_field (content);
	protocol = camel_content_type_param (content_type, "protocol");

	if (protocol == NULL)
		action = NULL;
	else if (g_ascii_strcasecmp (protocol, "application/pgp-signature") == 0)
		action = GTK_TOGGLE_ACTION (ACTION (PGP_SIGN));
	else if (g_ascii_strcasecmp (protocol, "application/x-pkcs7-signature") == 0)
		action = GTK_TOGGLE_ACTION (ACTION (SMIME_SIGN));

	if (action)
		gtk_toggle_action_set_active (action, TRUE);

	mime_part = camel_multipart_get_part (
		multipart, CAMEL_MULTIPART_SIGNED_CONTENT);

	if (mime_part == NULL)
		return;

	content_type = camel_mime_part_get_content_type (mime_part);
	content = camel_medium_get_content (CAMEL_MEDIUM (mime_part));

	if (CAMEL_IS_MULTIPART (content)) {
		multipart = CAMEL_MULTIPART (content);

		/* Note: depth is preserved here because we're not
		 * counting multipart/signed as a multipart, instead
		 * we want to treat the content part as our mime part
		 * here. */

		if (CAMEL_IS_MULTIPART_SIGNED (content)) {
			/* Handle the signed content and configure
			 * the composer to sign outgoing messages. */
			handle_multipart_signed (
				composer, multipart, cancellable, depth);

		} else if (CAMEL_IS_MULTIPART_ENCRYPTED (content)) {
			/* Decrypt the encrypted content and configure
			 * the composer to encrypt outgoing messages. */
			handle_multipart_encrypted (
				composer, mime_part, cancellable, depth);

		} else if (camel_content_type_is (content_type, "multipart", "alternative")) {
			/* This contains the text/plain and text/html
			 * versions of the message body. */
			handle_multipart_alternative (
				composer, multipart, cancellable, depth);

		} else {
			/* There must be attachments... */
			handle_multipart (
				composer, multipart, cancellable, depth);
		}

	} else if (camel_content_type_is (content_type, "text", "*")) {
		gchar *html;
		gssize length;

		html = emcu_part_to_html (mime_part, &length, NULL, cancellable);
		e_msg_composer_set_pending_body (composer, html, length);
	} else {
		e_msg_composer_attach (composer, mime_part);
	}
}

static void
handle_multipart_encrypted (EMsgComposer *composer,
                            CamelMimePart *multipart,
                            GCancellable *cancellable,
                            gint depth)
{
	CamelContentType *content_type;
	CamelCipherContext *cipher;
	CamelDataWrapper *content;
	CamelMimePart *mime_part;
	CamelSession *session;
	CamelCipherValidity *valid;
	GtkToggleAction *action = NULL;
	const gchar *protocol;

	content_type = camel_mime_part_get_content_type (multipart);
	protocol = camel_content_type_param (content_type, "protocol");

	if (protocol && g_ascii_strcasecmp (protocol, "application/pgp-encrypted") == 0)
		action = GTK_TOGGLE_ACTION (ACTION (PGP_ENCRYPT));
	else if (content_type && (
		    camel_content_type_is (content_type, "application", "x-pkcs7-mime")
		 || camel_content_type_is (content_type, "application", "pkcs7-mime")))
		action = GTK_TOGGLE_ACTION (ACTION (SMIME_ENCRYPT));

	if (action)
		gtk_toggle_action_set_active (action, TRUE);

	session = e_msg_composer_get_session (composer);
	cipher = camel_gpg_context_new (session);
	mime_part = camel_mime_part_new ();
	valid = camel_cipher_context_decrypt_sync (
		cipher, multipart, mime_part, cancellable, NULL);
	g_object_unref (cipher);

	if (valid == NULL)
		return;

	camel_cipher_validity_free (valid);

	content_type = camel_mime_part_get_content_type (mime_part);

	content = camel_medium_get_content (CAMEL_MEDIUM (mime_part));

	if (CAMEL_IS_MULTIPART (content)) {
		CamelMultipart *content_multipart = CAMEL_MULTIPART (content);

		/* Note: depth is preserved here because we're not
		 * counting multipart/encrypted as a multipart, instead
		 * we want to treat the content part as our mime part
		 * here. */

		if (CAMEL_IS_MULTIPART_SIGNED (content)) {
			/* Handle the signed content and configure the
			 * composer to sign outgoing messages. */
			handle_multipart_signed (
				composer, content_multipart, cancellable, depth);

		} else if (CAMEL_IS_MULTIPART_ENCRYPTED (content)) {
			/* Decrypt the encrypted content and configure the
			 * composer to encrypt outgoing messages. */
			handle_multipart_encrypted (
				composer, mime_part, cancellable, depth);

		} else if (camel_content_type_is (content_type, "multipart", "alternative")) {
			/* This contains the text/plain and text/html
			 * versions of the message body. */
			handle_multipart_alternative (
				composer, content_multipart, cancellable, depth);

		} else {
			/* There must be attachments... */
			handle_multipart (
				composer, content_multipart, cancellable, depth);
		}

	} else if (camel_content_type_is (content_type, "text", "*")) {
		gchar *html;
		gssize length;

		html = emcu_part_to_html (mime_part, &length, NULL, cancellable);
		e_msg_composer_set_pending_body (composer, html, length);
	} else {
		e_msg_composer_attach (composer, mime_part);
	}

	g_object_unref (mime_part);
}

static void
handle_multipart_alternative (EMsgComposer *composer,
                              CamelMultipart *multipart,
                              GCancellable *cancellable,
                              gint depth)
{
	/* Find the text/html part and set the composer body to it's contents */
	CamelMimePart *text_part = NULL;
	gint i, nparts;

	nparts = camel_multipart_get_number (multipart);

	for (i = 0; i < nparts; i++) {
		CamelContentType *content_type;
		CamelDataWrapper *content;
		CamelMimePart *mime_part;

		mime_part = camel_multipart_get_part (multipart, i);

		if (!mime_part)
			continue;

		content_type = camel_mime_part_get_content_type (mime_part);
		content = camel_medium_get_content (CAMEL_MEDIUM (mime_part));

		if (CAMEL_IS_MULTIPART (content)) {
			CamelMultipart *mp;

			mp = CAMEL_MULTIPART (content);

			if (CAMEL_IS_MULTIPART_SIGNED (content)) {
				/* Handle the signed content and configure
				 * the composer to sign outgoing messages. */
				handle_multipart_signed (
					composer, mp, cancellable, depth + 1);

			} else if (CAMEL_IS_MULTIPART_ENCRYPTED (content)) {
				/* Decrypt the encrypted content and configure
				 * the composer to encrypt outgoing messages. */
				handle_multipart_encrypted (
					composer, mime_part,
					cancellable, depth + 1);

			} else {
				/* Depth doesn't matter so long as we
				 * don't pass 0. */
				handle_multipart (
					composer, mp, cancellable, depth + 1);
			}

		} else if (camel_content_type_is (content_type, "text", "html")) {
			/* text/html is preferable, so once we find it we're done... */
			text_part = mime_part;
			break;
		} else if (camel_content_type_is (content_type, "text", "*")) {
			/* anyt text part not text/html is second rate so the first
			 * text part we find isn't necessarily the one we'll use. */
			if (!text_part)
				text_part = mime_part;
		} else {
			e_msg_composer_attach (composer, mime_part);
		}
	}

	if (text_part) {
		gchar *html;
		gssize length;

		html = emcu_part_to_html (text_part, &length, NULL, cancellable);
		e_msg_composer_set_pending_body (composer, html, length);
	}
}

static void
handle_multipart (EMsgComposer *composer,
                  CamelMultipart *multipart,
                  GCancellable *cancellable,
                  gint depth)
{
	gint i, nparts;

	nparts = camel_multipart_get_number (multipart);

	for (i = 0; i < nparts; i++) {
		CamelContentType *content_type;
		CamelDataWrapper *content;
		CamelMimePart *mime_part;

		mime_part = camel_multipart_get_part (multipart, i);

		if (!mime_part)
			continue;

		content_type = camel_mime_part_get_content_type (mime_part);
		content = camel_medium_get_content (CAMEL_MEDIUM (mime_part));

		if (CAMEL_IS_MULTIPART (content)) {
			CamelMultipart *mp;

			mp = CAMEL_MULTIPART (content);

			if (CAMEL_IS_MULTIPART_SIGNED (content)) {
				/* Handle the signed content and configure
				 * the composer to sign outgoing messages. */
				handle_multipart_signed (
					composer, mp, cancellable, depth + 1);

			} else if (CAMEL_IS_MULTIPART_ENCRYPTED (content)) {
				/* Decrypt the encrypted content and configure
				 * the composer to encrypt outgoing messages. */
				handle_multipart_encrypted (
					composer, mime_part,
					cancellable, depth + 1);

			} else if (camel_content_type_is (
				content_type, "multipart", "alternative")) {
				handle_multipart_alternative (
					composer, mp, cancellable, depth + 1);

			} else {
				/* Depth doesn't matter so long as we
				 * don't pass 0. */
				handle_multipart (
					composer, mp, cancellable, depth + 1);
			}

		} else if (depth == 0 && i == 0) {
			gchar *html;
			gssize length;

			/* Since the first part is not multipart/alternative,
			 * this must be the body. */
			html = emcu_part_to_html (
				mime_part, &length, NULL, cancellable);
			e_msg_composer_set_pending_body (composer, html, length);
		} else if (camel_mime_part_get_content_id (mime_part) ||
			   camel_mime_part_get_content_location (mime_part)) {
			/* special in-line attachment */
			e_msg_composer_add_inline_image_from_mime_part (
				composer, mime_part);
		} else {
			/* normal attachment */
			e_msg_composer_attach (composer, mime_part);
		}
	}
}

static void
set_signature_gui (EMsgComposer *composer)
{
	GtkhtmlEditor *editor;
	EComposerHeaderTable *table;
	ESignature *signature = NULL;
	const gchar *data;
	gchar *decoded;

	editor = GTKHTML_EDITOR (composer);
	table = e_msg_composer_get_header_table (composer);

	if (!gtkhtml_editor_search_by_data (editor, 1, "ClueFlow", "signature", "1"))
		return;

	data = gtkhtml_editor_get_paragraph_data (editor, "signature_name");
	if (g_str_has_prefix (data, "uid:")) {
		decoded = decode_signature_name (data + 4);
		signature = e_get_signature_by_uid (decoded);
		g_free (decoded);
	} else if (g_str_has_prefix (data, "name:")) {
		decoded = decode_signature_name (data + 5);
		signature = e_get_signature_by_name (decoded);
		g_free (decoded);
	}

	e_composer_header_table_set_signature (table, signature);
}

/**
 * e_msg_composer_new_with_message:
 * @shell: an #EShell
 * @message: The message to use as the source
 *
 * Create a new message composer widget.
 *
 * Note: Designed to work only for messages constructed using Evolution.
 *
 * Returns: A pointer to the newly created widget
 **/
EMsgComposer *
e_msg_composer_new_with_message (EShell *shell,
                                 CamelMimeMessage *message,
                                 GCancellable *cancellable)
{
	CamelInternetAddress *to, *cc, *bcc;
	GList *To = NULL, *Cc = NULL, *Bcc = NULL, *postto = NULL;
	const gchar *format, *subject;
	EDestination **Tov, **Ccv, **Bccv;
	GHashTable *auto_cc, *auto_bcc;
	CamelContentType *content_type;
	struct _camel_header_raw *headers;
	CamelDataWrapper *content;
	EAccount *account = NULL;
	gchar *account_name;
	EMsgComposer *composer;
	EMsgComposerPrivate *priv;
	EComposerHeaderTable *table;
	GtkToggleAction *action;
	struct _camel_header_raw *xev;
	gint len, i;

	g_return_val_if_fail (E_IS_SHELL (shell), NULL);

	headers = CAMEL_MIME_PART (message)->headers;
	while (headers != NULL) {
		gchar *value;

		if (strcmp (headers->name, "X-Evolution-PostTo") == 0) {
			value = g_strstrip (g_strdup (headers->value));
			postto = g_list_append (postto, value);
		}

		headers = headers->next;
	}

	composer = e_msg_composer_new (shell);
	priv = composer->priv;
	table = e_msg_composer_get_header_table (composer);

	if (postto) {
		e_composer_header_table_set_post_to_list (table, postto);
		g_list_foreach (postto, (GFunc) g_free, NULL);
		g_list_free (postto);
		postto = NULL;
	}

	/* Restore the Account preference */
	account_name = (gchar *) camel_medium_get_header (
		CAMEL_MEDIUM (message), "X-Evolution-Account");
	if (account_name) {
		account_name = g_strdup (account_name);
		g_strstrip (account_name);

		account = e_get_account_by_uid (account_name);
		if (account == NULL)
			/* XXX Backwards compatibility */
			account = e_get_account_by_name (account_name);

		if (account != NULL) {
			g_free (account_name);
			account_name = g_strdup (account->name);
		}
	}

	if (postto == NULL) {
		auto_cc = g_hash_table_new_full (
			camel_strcase_hash, camel_strcase_equal,
			(GDestroyNotify) g_free,
			(GDestroyNotify) NULL);

		auto_bcc = g_hash_table_new_full (
			camel_strcase_hash, camel_strcase_equal,
			(GDestroyNotify) g_free,
			(GDestroyNotify) NULL);

		if (account) {
			CamelInternetAddress *iaddr;

			/* hash our auto-recipients for this account */
			if (account->always_cc) {
				iaddr = camel_internet_address_new ();
				if (camel_address_decode (CAMEL_ADDRESS (iaddr), account->cc_addrs) != -1) {
					for (i = 0; i < camel_address_length (CAMEL_ADDRESS (iaddr)); i++) {
						const gchar *name, *addr;

						if (!camel_internet_address_get (iaddr, i, &name, &addr))
							continue;

						g_hash_table_insert (
							auto_cc,
							g_strdup (addr),
							GINT_TO_POINTER (TRUE));
					}
				}
				g_object_unref (iaddr);
			}

			if (account->always_bcc) {
				iaddr = camel_internet_address_new ();
				if (camel_address_decode (CAMEL_ADDRESS (iaddr), account->bcc_addrs) != -1) {
					for (i = 0; i < camel_address_length (CAMEL_ADDRESS (iaddr)); i++) {
						const gchar *name, *addr;

						if (!camel_internet_address_get (iaddr, i, &name, &addr))
							continue;

						g_hash_table_insert (
							auto_bcc,
							g_strdup (addr),
							GINT_TO_POINTER (TRUE));
					}
				}
				g_object_unref (iaddr);
			}
		}

		to = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO);
		cc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC);
		bcc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_BCC);

		len = CAMEL_ADDRESS (to)->addresses->len;
		for (i = 0; i < len; i++) {
			const gchar *name, *addr;

			if (camel_internet_address_get (to, i, &name, &addr)) {
				EDestination *dest = e_destination_new ();
				e_destination_set_name (dest, name);
				e_destination_set_email (dest, addr);
				To = g_list_append (To, dest);
			}
		}
		Tov = destination_list_to_vector (To);
		g_list_free (To);

		len = CAMEL_ADDRESS (cc)->addresses->len;
		for (i = 0; i < len; i++) {
			const gchar *name, *addr;

			if (camel_internet_address_get (cc, i, &name, &addr)) {
				EDestination *dest = e_destination_new ();
				e_destination_set_name (dest, name);
				e_destination_set_email (dest, addr);

				if (g_hash_table_lookup (auto_cc, addr))
					e_destination_set_auto_recipient (dest, TRUE);

				Cc = g_list_append (Cc, dest);
			}
		}

		Ccv = destination_list_to_vector (Cc);
		g_hash_table_destroy (auto_cc);
		g_list_free (Cc);

		len = CAMEL_ADDRESS (bcc)->addresses->len;
		for (i = 0; i < len; i++) {
			const gchar *name, *addr;

			if (camel_internet_address_get (bcc, i, &name, &addr)) {
				EDestination *dest = e_destination_new ();
				e_destination_set_name (dest, name);
				e_destination_set_email (dest, addr);

				if (g_hash_table_lookup (auto_bcc, addr))
					e_destination_set_auto_recipient (dest, TRUE);

				Bcc = g_list_append (Bcc, dest);
			}
		}

		Bccv = destination_list_to_vector (Bcc);
		g_hash_table_destroy (auto_bcc);
		g_list_free (Bcc);
	} else {
		Tov = NULL;
		Ccv = NULL;
		Bccv = NULL;
	}

	subject = camel_mime_message_get_subject (message);

	e_composer_header_table_set_account_name (table, account_name);
	e_composer_header_table_set_destinations_to (table, Tov);
	e_composer_header_table_set_destinations_cc (table, Ccv);
	e_composer_header_table_set_destinations_bcc (table, Bccv);
	e_composer_header_table_set_subject (table, subject);

	g_free (account_name);

	e_destination_freev (Tov);
	e_destination_freev (Ccv);
	e_destination_freev (Bccv);

	/* Restore the format editing preference */
	format = camel_medium_get_header (
		CAMEL_MEDIUM (message), "X-Evolution-Format");
	if (format != NULL) {
		gchar **flags;

		while (*format && camel_mime_is_lwsp (*format))
			format++;

		flags = g_strsplit (format, ", ", 0);
		for (i = 0; flags[i]; i++) {
			if (g_ascii_strcasecmp (flags[i], "text/html") == 0)
				gtkhtml_editor_set_html_mode (
					GTKHTML_EDITOR (composer), TRUE);
			else if (g_ascii_strcasecmp (flags[i], "text/plain") == 0)
				gtkhtml_editor_set_html_mode (
					GTKHTML_EDITOR (composer), FALSE);
			else if (g_ascii_strcasecmp (flags[i], "pgp-sign") == 0) {
				action = GTK_TOGGLE_ACTION (ACTION (PGP_SIGN));
				gtk_toggle_action_set_active (action, TRUE);
			} else if (g_ascii_strcasecmp (flags[i], "pgp-encrypt") == 0) {
				action = GTK_TOGGLE_ACTION (ACTION (PGP_ENCRYPT));
				gtk_toggle_action_set_active (action, TRUE);
			} else if (g_ascii_strcasecmp (flags[i], "smime-sign") == 0) {
				action = GTK_TOGGLE_ACTION (ACTION (SMIME_SIGN));
				gtk_toggle_action_set_active (action, TRUE);
			} else if (g_ascii_strcasecmp (flags[i], "smime-encrypt") == 0) {
				action = GTK_TOGGLE_ACTION (ACTION (SMIME_ENCRYPT));
				gtk_toggle_action_set_active (action, TRUE);
			}
		}
		g_strfreev (flags);
	}

	/* Remove any other X-Evolution-* headers that may have been set */
	xev = emcu_remove_xevolution_headers (message);
	camel_header_raw_clear (&xev);

	/* Check for receipt request */
	if (camel_medium_get_header (
		CAMEL_MEDIUM (message), "Disposition-Notification-To")) {
		action = GTK_TOGGLE_ACTION (ACTION (REQUEST_READ_RECEIPT));
		gtk_toggle_action_set_active (action, TRUE);
	}

	/* Check for mail priority */
	if (camel_medium_get_header (CAMEL_MEDIUM (message), "X-Priority")) {
		action = GTK_TOGGLE_ACTION (ACTION (PRIORITIZE_MESSAGE));
		gtk_toggle_action_set_active (action, TRUE);
	}

	/* set extra headers */
	headers = CAMEL_MIME_PART (message)->headers;
	while (headers) {
		if (g_ascii_strcasecmp (headers->name, "References") == 0 ||
		    g_ascii_strcasecmp (headers->name, "In-Reply-To") == 0) {
			g_ptr_array_add (
				composer->priv->extra_hdr_names,
				g_strdup (headers->name));
			g_ptr_array_add (
				composer->priv->extra_hdr_values,
				g_strdup (headers->value));
		}

		headers = headers->next;
	}

	/* Restore the attachments and body text */
	content = camel_medium_get_content (CAMEL_MEDIUM (message));
	if (CAMEL_IS_MULTIPART (content)) {
		CamelMimePart *mime_part;
		CamelMultipart *multipart;

		multipart = CAMEL_MULTIPART (content);
		mime_part = CAMEL_MIME_PART (message);
		content_type = camel_mime_part_get_content_type (mime_part);

		if (CAMEL_IS_MULTIPART_SIGNED (content)) {
			/* Handle the signed content and configure the
			 * composer to sign outgoing messages. */
			handle_multipart_signed (
				composer, multipart, cancellable, 0);

		} else if (CAMEL_IS_MULTIPART_ENCRYPTED (content)) {
			/* Decrypt the encrypted content and configure the
			 * composer to encrypt outgoing messages. */
			handle_multipart_encrypted (
				composer, mime_part, cancellable, 0);

		} else if (camel_content_type_is (
			content_type, "multipart", "alternative")) {
			/* This contains the text/plain and text/html
			 * versions of the message body. */
			handle_multipart_alternative (
				composer, multipart, cancellable, 0);

		} else {
			/* There must be attachments... */
			handle_multipart (
				composer, multipart, cancellable, 0);
		}
	} else {
		CamelMimePart *mime_part;
		gchar *html;
		gssize length;

		mime_part = CAMEL_MIME_PART (message);
		content_type = camel_mime_part_get_content_type (mime_part);

		if (content_type != NULL && (
			camel_content_type_is (
				content_type, "application", "x-pkcs7-mime") ||
			camel_content_type_is (
				content_type, "application", "pkcs7-mime")))
			gtk_toggle_action_set_active (
				GTK_TOGGLE_ACTION (
				ACTION (SMIME_ENCRYPT)), TRUE);

		html = emcu_part_to_html (
			CAMEL_MIME_PART (message),
			&length, NULL, cancellable);
		e_msg_composer_set_pending_body (composer, html, length);
	}

	priv->is_from_message = TRUE;

	/* We wait until now to set the body text because we need to
	 * ensure that the attachment bar has all the attachments before
	 * we request them. */
	e_msg_composer_flush_pending_body (composer);

	set_signature_gui (composer);

	return composer;
}

/**
 * e_msg_composer_new_redirect:
 * @shell: an #EShell
 * @message: The message to use as the source
 *
 * Create a new message composer widget.
 *
 * Returns: A pointer to the newly created widget
 **/
EMsgComposer *
e_msg_composer_new_redirect (EShell *shell,
                             CamelMimeMessage *message,
                             const gchar *resent_from,
                             GCancellable *cancellable)
{
	EMsgComposer *composer;
	EComposerHeaderTable *table;
	EWebViewGtkHTML *web_view;
	const gchar *subject;

	g_return_val_if_fail (E_IS_SHELL (shell), NULL);
	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	composer = e_msg_composer_new_with_message (
		shell, message, cancellable);
	table = e_msg_composer_get_header_table (composer);

	subject = camel_mime_message_get_subject (message);

	composer->priv->redirect = message;
	g_object_ref (message);

	e_composer_header_table_set_account_name (table, resent_from);
	e_composer_header_table_set_subject (table, subject);

	web_view = e_msg_composer_get_web_view (composer);
	e_web_view_gtkhtml_set_editable (web_view, FALSE);

	return composer;
}

/**
 * e_msg_composer_get_session:
 * @composer: an #EMsgComposer
 *
 * Returns the mail module's global #CamelSession instance.  Calling
 * this function will load the mail module if it isn't already loaded.
 *
 * Returns: the mail module's #CamelSession
 **/
CamelSession *
e_msg_composer_get_session (EMsgComposer *composer)
{
	EShell *shell;
	EShellSettings *shell_settings;
	CamelSession *session;

	g_return_val_if_fail (E_IS_MSG_COMPOSER (composer), NULL);

	shell = e_msg_composer_get_shell (composer);
	shell_settings = e_shell_get_shell_settings (shell);

	session = e_shell_settings_get_pointer (shell_settings, "mail-session");
	g_return_val_if_fail (CAMEL_IS_SESSION (session), NULL);

	return session;
}

/**
 * e_msg_composer_get_shell:
 * @composer: an #EMsgComposer
 *
 * Returns the #EShell that was passed to e_msg_composer_new().
 *
 * Returns: the #EShell
 **/
EShell *
e_msg_composer_get_shell (EMsgComposer *composer)
{
	g_return_val_if_fail (E_IS_MSG_COMPOSER (composer), NULL);

	return E_SHELL (composer->priv->shell);
}

/**
 * e_msg_composer_get_web_view:
 * @composer: an #EMsgComposer
 *
 * Returns the #EWebView widget in @composer.
 *
 * Returns: the #EWebView
 **/
EWebViewGtkHTML *
e_msg_composer_get_web_view (EMsgComposer *composer)
{
	GtkHTML *html;
	GtkhtmlEditor *editor;

	g_return_val_if_fail (E_IS_MSG_COMPOSER (composer), NULL);

	/* This is a convenience function to avoid
	 * repeating this awkwardness everywhere */
	editor = GTKHTML_EDITOR (composer);
	html = gtkhtml_editor_get_html (editor);

	return E_WEB_VIEW_GTKHTML (html);
}

static void
msg_composer_send_cb (EMsgComposer *composer,
                      GAsyncResult *result,
                      AsyncContext *context)
{
	CamelMimeMessage *message;
	EAlertSink *alert_sink;
	GtkhtmlEditor *editor;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	message = e_msg_composer_get_message_finish (composer, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (message == NULL);
		async_context_free (context);
		g_error_free (error);

		gtk_window_present (GTK_WINDOW (composer));
		return;
	}

	if (error != NULL) {
		g_warn_if_fail (message == NULL);
		e_alert_submit (
			alert_sink,
			"mail-composer:no-build-message",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);

		gtk_window_present (GTK_WINDOW (composer));
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	g_signal_emit (
		composer, signals[SEND], 0,
		message, context->activity);

	g_object_unref (message);

	async_context_free (context);

	/* XXX This should be elsewhere. */
	editor = GTKHTML_EDITOR (composer);
	gtkhtml_editor_set_changed (editor, FALSE);
}

/**
 * e_msg_composer_send:
 * @composer: an #EMsgComposer
 *
 * Send the message in @composer.
 **/
void
e_msg_composer_send (EMsgComposer *composer)
{
	AsyncContext *context;
	EAlertSink *alert_sink;
	EActivityBar *activity_bar;
	GCancellable *cancellable;
	gboolean proceed_with_send = TRUE;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	/* This gives the user a chance to abort the send. */
	g_signal_emit (composer, signals[PRESEND], 0, &proceed_with_send);

	if (!proceed_with_send) {
		gtk_window_present (GTK_WINDOW (composer));
		return;
	}

	context = g_slice_new0 (AsyncContext);
	context->activity = e_composer_activity_new (composer);

	alert_sink = E_ALERT_SINK (composer);
	e_activity_set_alert_sink (context->activity, alert_sink);

	cancellable = camel_operation_new ();
	e_activity_set_cancellable (context->activity, cancellable);
	g_object_unref (cancellable);

	activity_bar = E_ACTIVITY_BAR (composer->priv->activity_bar);
	e_activity_bar_set_activity (activity_bar, context->activity);

	e_msg_composer_get_message (
		composer, G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) msg_composer_send_cb,
		context);
}

static void
msg_composer_save_to_drafts_cb (EMsgComposer *composer,
                                GAsyncResult *result,
                                AsyncContext *context)
{
	CamelMimeMessage *message;
	EAlertSink *alert_sink;
	GtkhtmlEditor *editor;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	message = e_msg_composer_get_message_draft_finish (
		composer, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (message == NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	if (error != NULL) {
		g_warn_if_fail (message == NULL);
		e_alert_submit (
			alert_sink,
			"mail-composer:no-build-message",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	g_signal_emit (
		composer, signals[SAVE_TO_DRAFTS],
		0, message, context->activity);

	g_object_unref (message);

	async_context_free (context);

	/* XXX This should be elsewhere. */
	editor = GTKHTML_EDITOR (composer);
	gtkhtml_editor_set_changed (editor, FALSE);
}

/**
 * e_msg_composer_save_to_drafts:
 * @composer: an #EMsgComposer
 *
 * Save the message in @composer to the selected account's Drafts folder.
 **/
void
e_msg_composer_save_to_drafts (EMsgComposer *composer)
{
	AsyncContext *context;
	EAlertSink *alert_sink;
	EActivityBar *activity_bar;
	GCancellable *cancellable;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	context = g_slice_new0 (AsyncContext);
	context->activity = e_composer_activity_new (composer);

	alert_sink = E_ALERT_SINK (composer);
	e_activity_set_alert_sink (context->activity, alert_sink);

	cancellable = camel_operation_new ();
	e_activity_set_cancellable (context->activity, cancellable);
	g_object_unref (cancellable);

	activity_bar = E_ACTIVITY_BAR (composer->priv->activity_bar);
	e_activity_bar_set_activity (activity_bar, context->activity);

	e_msg_composer_get_message_draft (
		composer, G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) msg_composer_save_to_drafts_cb,
		context);
}

static void
msg_composer_save_to_outbox_cb (EMsgComposer *composer,
                                GAsyncResult *result,
                                AsyncContext *context)
{
	CamelMimeMessage *message;
	EAlertSink *alert_sink;
	GtkhtmlEditor *editor;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	message = e_msg_composer_get_message_finish (composer, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (message == NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	if (error != NULL) {
		g_warn_if_fail (message == NULL);
		e_alert_submit (
			alert_sink,
			"mail-composer:no-build-message",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	g_signal_emit (
		composer, signals[SAVE_TO_OUTBOX],
		0, message, context->activity);

	g_object_unref (message);

	async_context_free (context);

	/* XXX This should be elsewhere. */
	editor = GTKHTML_EDITOR (composer);
	gtkhtml_editor_set_changed (editor, FALSE);
}

/**
 * e_msg_composer_save_to_outbox:
 * @composer: an #EMsgComposer
 *
 * Save the message in @composer to the local Outbox folder.
 **/
void
e_msg_composer_save_to_outbox (EMsgComposer *composer)
{
	AsyncContext *context;
	EAlertSink *alert_sink;
	EActivityBar *activity_bar;
	GCancellable *cancellable;
	gboolean proceed_with_save = TRUE;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	/* This gives the user a chance to abort the save. */
	g_signal_emit (composer, signals[PRESEND], 0, &proceed_with_save);

	if (!proceed_with_save)
		return;

	context = g_slice_new0 (AsyncContext);
	context->activity = e_composer_activity_new (composer);

	alert_sink = E_ALERT_SINK (composer);
	e_activity_set_alert_sink (context->activity, alert_sink);

	cancellable = camel_operation_new ();
	e_activity_set_cancellable (context->activity, cancellable);
	g_object_unref (cancellable);

	activity_bar = E_ACTIVITY_BAR (composer->priv->activity_bar);
	e_activity_bar_set_activity (activity_bar, context->activity);

	e_msg_composer_get_message (
		composer, G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) msg_composer_save_to_outbox_cb,
		context);
}

static void
msg_composer_print_cb (EMsgComposer *composer,
                       GAsyncResult *result,
                       AsyncContext *context)
{
	CamelMimeMessage *message;
	EAlertSink *alert_sink;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	message = e_msg_composer_get_message_print_finish (
		composer, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (message == NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	if (error != NULL) {
		g_warn_if_fail (message == NULL);
		async_context_free (context);
		e_alert_submit (
			alert_sink,
			"mail-composer:no-build-message",
			error->message, NULL);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	g_signal_emit (
		composer, signals[PRINT], 0,
		context->print_action, message, context->activity);

	g_object_unref (message);

	async_context_free (context);
}

/**
 * e_msg_composer_print:
 * @composer: an #EMsgComposer
 * @print_action: the print action to start
 *
 * Print the message in @composer.
 **/
void
e_msg_composer_print (EMsgComposer *composer,
                      GtkPrintOperationAction print_action)
{
	AsyncContext *context;
	EAlertSink *alert_sink;
	EActivityBar *activity_bar;
	GCancellable *cancellable;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	context = g_slice_new0 (AsyncContext);
	context->activity = e_composer_activity_new (composer);
	context->print_action = print_action;

	alert_sink = E_ALERT_SINK (composer);
	e_activity_set_alert_sink (context->activity, alert_sink);

	cancellable = camel_operation_new ();
	e_activity_set_cancellable (context->activity, cancellable);
	g_object_unref (cancellable);

	activity_bar = E_ACTIVITY_BAR (composer->priv->activity_bar);
	e_activity_bar_set_activity (activity_bar, context->activity);

	e_msg_composer_get_message_print (
		composer, G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) msg_composer_print_cb,
		context);
}

static GList *
add_recipients (GList *list,
                const gchar *recips)
{
	CamelInternetAddress *cia;
	const gchar *name, *addr;
	gint num, i;

	cia = camel_internet_address_new ();
	num = camel_address_decode (CAMEL_ADDRESS (cia), recips);

	for (i = 0; i < num; i++) {
		if (camel_internet_address_get (cia, i, &name, &addr)) {
			EDestination *dest = e_destination_new ();
			e_destination_set_name (dest, name);
			e_destination_set_email (dest, addr);

			list = g_list_append (list, dest);
		}
	}

	return list;
}

static gboolean
list_contains_addr (const GList *list,
                    EDestination *dest)
{
	g_return_val_if_fail (dest != NULL, FALSE);

	while (list != NULL) {
		if (e_destination_equal (dest, list->data))
			return TRUE;

		list = list->next;
	}

	return FALSE;
}

static void
merge_cc_bcc (EDestination **addrv,
              GList **merge_into,
              const GList *to,
              const GList *cc,
              const GList *bcc)
{
	gint ii;

	for (ii = 0; addrv && addrv[ii]; ii++) {
		if (!list_contains_addr (to, addrv[ii]) &&
		    !list_contains_addr (cc, addrv[ii]) &&
		    !list_contains_addr (bcc, addrv[ii]))
			*merge_into = g_list_append (
				*merge_into, g_object_ref (addrv[ii]));
	}
}

static void
merge_always_cc_and_bcc (EComposerHeaderTable *table,
                         const GList *to,
                         GList **cc,
                         GList **bcc)
{
	EDestination **addrv;

	g_return_if_fail (table != NULL);
	g_return_if_fail (cc != NULL);
	g_return_if_fail (bcc != NULL);

	addrv = e_composer_header_table_get_destinations_cc (table);
	merge_cc_bcc (addrv, cc, to, *cc, *bcc);
	e_destination_freev (addrv);

	addrv = e_composer_header_table_get_destinations_bcc (table);
	merge_cc_bcc (addrv, bcc, to, *cc, *bcc);
	e_destination_freev (addrv);
}

static const gchar *blacklisted_files [] = {".", "etc", ".."};

gboolean check_blacklisted_file (gchar *filename)
{
	gboolean blacklisted = FALSE;
	gint i,j,len;
	gchar **filename_part;

	filename_part = g_strsplit (filename, G_DIR_SEPARATOR_S, -1);
	len = g_strv_length(filename_part);
	for(i = 0; !blacklisted && i < G_N_ELEMENTS(blacklisted_files); i++)
	{
		for (j = 0; !blacklisted && j < len;j++)
			if (g_str_has_prefix (filename_part[j], blacklisted_files[i]))
				blacklisted = TRUE;
	}

	g_strfreev(filename_part);
	
	return blacklisted;
}

static void
handle_mailto (EMsgComposer *composer,
               const gchar *mailto)
{
	EAttachmentView *view;
	EAttachmentStore *store;
	EComposerHeaderTable *table;
	GList *to = NULL, *cc = NULL, *bcc = NULL;
	EDestination **tov, **ccv, **bccv;
	gchar *subject = NULL, *body = NULL;
	gchar *header, *content, *buf;
	gsize nread, nwritten;
	const gchar *p;
	gint len, clen;

	table = e_msg_composer_get_header_table (composer);
	view = e_msg_composer_get_attachment_view (composer);
	store = e_attachment_view_get_store (view);

	buf = g_strdup (mailto);

	/* Parse recipients (everything after ':' until '?' or eos). */
	p = buf + 7;
	len = strcspn (p, "?");
	if (len) {
		content = g_strndup (p, len);
		camel_url_decode (content);
		to = add_recipients (to, content);
		g_free (content);
	}

	p += len;
	if (*p == '?') {
		p++;

		while (*p) {
			len = strcspn (p, "=&");

			/* If it's malformed, give up. */
			if (p[len] != '=')
				break;

			header = (gchar *) p;
			header[len] = '\0';
			p += len + 1;

			clen = strcspn (p, "&");

			content = g_strndup (p, clen);

			if (!g_ascii_strcasecmp (header, "to")) {
				camel_url_decode (content);
				to = add_recipients (to, content);
			} else if (!g_ascii_strcasecmp (header, "cc")) {
				camel_url_decode (content);
				cc = add_recipients (cc, content);
			} else if (!g_ascii_strcasecmp (header, "bcc")) {
				camel_url_decode (content);
				bcc = add_recipients (bcc, content);
			} else if (!g_ascii_strcasecmp (header, "subject")) {
				g_free (subject);
				camel_url_decode (content);
				if (g_utf8_validate (content, -1, NULL)) {
					subject = content;
					content = NULL;
				} else {
					subject = g_locale_to_utf8 (content, clen, &nread,
								    &nwritten, NULL);
					if (subject) {
						subject = g_realloc (subject, nwritten + 1);
						subject[nwritten] = '\0';
					}
				}
			} else if (!g_ascii_strcasecmp (header, "body")) {
				g_free (body);
				camel_url_decode (content);
				if (g_utf8_validate (content, -1, NULL)) {
					body = content;
					content = NULL;
				} else {
					body = g_locale_to_utf8 (
						content, clen, &nread,
						&nwritten, NULL);
					if (body) {
						body = g_realloc (body, nwritten + 1);
						body[nwritten] = '\0';
					}
				}
			} else if (!g_ascii_strcasecmp (header, "attach") ||
				   !g_ascii_strcasecmp (header, "attachment")) {
				EAttachment *attachment;
				gboolean check = FALSE;

				camel_url_decode (content);
				check = check_blacklisted_file(content);
				if(check)
					e_alert_submit (
		                        	E_ALERT_SINK (composer),
                			        "mail:blacklisted-file", content, NULL);
				if (g_ascii_strncasecmp (content, "file:", 5) == 0)
					attachment = e_attachment_new_for_uri (content);
				else
					attachment = e_attachment_new_for_path (content);
				e_attachment_store_add_attachment (store, attachment);
				e_attachment_load_async (
					attachment, (GAsyncReadyCallback)
					e_attachment_load_handle_error, composer);
				g_object_unref (attachment);
			} else if (!g_ascii_strcasecmp (header, "from")) {
				/* Ignore */
			} else if (!g_ascii_strcasecmp (header, "reply-to")) {
				/* ignore */
			} else {
				/* add an arbitrary header? */
				camel_url_decode (content);
				e_msg_composer_add_header (composer, header, content);
			}

			g_free (content);

			p += clen;
			if (*p == '&') {
				p++;
				if (!g_ascii_strncasecmp (p, "amp;", 4))
					p += 4;
			}
		}
	}

	g_free (buf);

	merge_always_cc_and_bcc (table, to, &cc, &bcc);

	tov  = destination_list_to_vector (to);
	ccv  = destination_list_to_vector (cc);
	bccv = destination_list_to_vector (bcc);

	g_list_free (to);
	g_list_free (cc);
	g_list_free (bcc);

	e_composer_header_table_set_destinations_to (table, tov);
	e_composer_header_table_set_destinations_cc (table, ccv);
	e_composer_header_table_set_destinations_bcc (table, bccv);

	e_destination_freev (tov);
	e_destination_freev (ccv);
	e_destination_freev (bccv);

	e_composer_header_table_set_subject (table, subject);
	g_free (subject);

	if (body) {
		gchar *htmlbody;

		htmlbody = camel_text_to_html (body, CAMEL_MIME_FILTER_TOHTML_PRE, 0);
		set_editor_text (composer, htmlbody, TRUE);
		g_free (htmlbody);
	}
}

/**
 * e_msg_composer_new_from_url:
 * @shell: an #EShell
 * @url: a mailto URL
 *
 * Create a new message composer widget, and fill in fields as
 * defined by the provided URL.
 **/
EMsgComposer *
e_msg_composer_new_from_url (EShell *shell,
                             const gchar *url)
{
	EMsgComposer *composer;

	g_return_val_if_fail (E_IS_SHELL (shell), NULL);
	g_return_val_if_fail (g_ascii_strncasecmp (url, "mailto:", 7) == 0, NULL);

	composer = e_msg_composer_new (shell);

	handle_mailto (composer, url);

	return composer;
}

/**
 * e_msg_composer_set_body_text:
 * @composer: a composer object
 * @text: the HTML text to initialize the editor with
 * @update_signature: whether update signature in the text after setting it;
 *    Might be usually called with TRUE.
 *
 * Loads the given HTML text into the editor.
 **/
void
e_msg_composer_set_body_text (EMsgComposer *composer,
                              const gchar *text,
                              gboolean update_signature)
{
	g_return_if_fail (E_IS_MSG_COMPOSER (composer));
	g_return_if_fail (text != NULL);

	set_editor_text (composer, text, update_signature);
}

/**
 * e_msg_composer_set_body:
 * @composer: a composer object
 * @body: the data to initialize the composer with
 * @mime_type: the MIME type of data
 *
 * Loads the given data into the composer as the message body.
 **/
void
e_msg_composer_set_body (EMsgComposer *composer,
                         const gchar *body,
                         const gchar *mime_type)
{
	EMsgComposerPrivate *p = composer->priv;
	EComposerHeaderTable *table;
	EWebViewGtkHTML *web_view;
	gchar *buff;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	table = e_msg_composer_get_header_table (composer);

	buff = g_markup_printf_escaped ("<b>%s</b>",
		_("The composer contains a non-text "
		  "message body, which cannot be edited."));
	set_editor_text (composer, buff, FALSE);
	g_free (buff);

	gtkhtml_editor_set_html_mode (GTKHTML_EDITOR (composer), FALSE);

	web_view = e_msg_composer_get_web_view (composer);
	e_web_view_gtkhtml_set_editable (web_view, FALSE);

	g_free (p->mime_body);
	p->mime_body = g_strdup (body);
	g_free (p->mime_type);
	p->mime_type = g_strdup (mime_type);

	if (g_ascii_strncasecmp (p->mime_type, "text/calendar", 13) == 0) {
		EAccount *account;

		account = e_composer_header_table_get_account (table);
		if (account && account->pgp_no_imip_sign) {
			GtkToggleAction *action;

			action = GTK_TOGGLE_ACTION (ACTION (PGP_SIGN));
			gtk_toggle_action_set_active (action, FALSE);

			action = GTK_TOGGLE_ACTION (ACTION (SMIME_SIGN));
			gtk_toggle_action_set_active (action, FALSE);
		}
	}
}

/**
 * e_msg_composer_add_header:
 * @composer: an #EMsgComposer
 * @name: the header's name
 * @value: the header's value
 *
 * Adds a new custom header created from @name and @value.  The header
 * is not shown in the user interface but will be added to the resulting
 * MIME message when sending or saving.
 **/
void
e_msg_composer_add_header (EMsgComposer *composer,
                           const gchar *name,
                           const gchar *value)
{
	EMsgComposerPrivate *priv;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));
	g_return_if_fail (name != NULL);
	g_return_if_fail (value != NULL);

	priv = composer->priv;

	g_ptr_array_add (priv->extra_hdr_names, g_strdup (name));
	g_ptr_array_add (priv->extra_hdr_values, g_strdup (value));
}

/**
 * e_msg_composer_set_header:
 * @composer: an #EMsgComposer
 * @name: the header's name
 * @value: the header's value
 *
 * Replaces all custom headers matching @name that were added with
 * e_msg_composer_add_header() or e_msg_composer_set_header(), with
 * a new custom header created from @name and @value.  The header is
 * not shown in the user interface but will be added to the resulting
 * MIME message when sending or saving.
 **/
void
e_msg_composer_set_header (EMsgComposer *composer,
                           const gchar *name,
                           const gchar *value)
{
	g_return_if_fail (E_IS_MSG_COMPOSER (composer));
	g_return_if_fail (name != NULL);
	g_return_if_fail (value != NULL);

	e_msg_composer_remove_header (composer, name);
	e_msg_composer_add_header (composer, name, value);
}

/**
 * e_msg_composer_remove_header:
 * @composer: an #EMsgComposer
 * @name: the header's name
 *
 * Removes all custom headers matching @name that were added with
 * e_msg_composer_add_header() or e_msg_composer_set_header().
 **/
void
e_msg_composer_remove_header (EMsgComposer *composer,
                              const gchar *name)
{
	EMsgComposerPrivate *priv;
	guint ii;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));
	g_return_if_fail (name != NULL);

	priv = composer->priv;

	for (ii = 0; ii < priv->extra_hdr_names->len; ii++) {
		if (g_strcmp0 (priv->extra_hdr_names->pdata[ii], name) == 0) {
			g_free (priv->extra_hdr_names->pdata[ii]);
			g_free (priv->extra_hdr_values->pdata[ii]);
			g_ptr_array_remove_index (priv->extra_hdr_names, ii);
			g_ptr_array_remove_index (priv->extra_hdr_values, ii);
		}
	}
}

/**
 * e_msg_composer_set_draft_headers:
 * @composer: an #EMsgComposer
 * @folder_uri: folder URI of the last saved draft
 * @message_uid: message UID of the last saved draft
 *
 * Add special X-Evolution-Draft headers to remember the most recently
 * saved draft message, even across Evolution sessions.  These headers
 * can be used to mark the draft message for deletion after saving a
 * newer draft or sending the composed message.
 **/
void
e_msg_composer_set_draft_headers (EMsgComposer *composer,
                                  const gchar *folder_uri,
                                  const gchar *message_uid)
{
	const gchar *header_name;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));
	g_return_if_fail (folder_uri != NULL);
	g_return_if_fail (message_uid != NULL);

	header_name = "X-Evolution-Draft-Folder";
	e_msg_composer_set_header (composer, header_name, folder_uri);

	header_name = "X-Evolution-Draft-Message";
	e_msg_composer_set_header (composer, header_name, message_uid);
}

/**
 * e_msg_composer_set_source_headers:
 * @composer: an #EMsgComposer
 * @folder_uri: folder URI of the source message
 * @message_uid: message UID of the source message
 * @flags: flags to set on the source message after sending
 *
 * Add special X-Evolution-Source headers to remember the message being
 * forwarded or replied to, even across Evolution sessions.  These headers
 * can be used to set appropriate flags on the source message after sending
 * the composed message.
 **/
void
e_msg_composer_set_source_headers (EMsgComposer *composer,
                                   const gchar *folder_uri,
                                   const gchar *message_uid,
                                   CamelMessageFlags flags)
{
	GString *buffer;
	const gchar *header_name;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));
	g_return_if_fail (folder_uri != NULL);
	g_return_if_fail (message_uid != NULL);

	buffer = g_string_sized_new (32);

	if (flags & CAMEL_MESSAGE_ANSWERED)
		g_string_append (buffer, "ANSWERED ");
	if (flags & CAMEL_MESSAGE_ANSWERED_ALL)
		g_string_append (buffer, "ANSWERED_ALL ");
	if (flags & CAMEL_MESSAGE_FORWARDED)
		g_string_append (buffer, "FORWARDED ");
	if (flags & CAMEL_MESSAGE_SEEN)
		g_string_append (buffer, "SEEN ");

	header_name = "X-Evolution-Source-Folder";
	e_msg_composer_set_header (composer, header_name, folder_uri);

	header_name = "X-Evolution-Source-Message";
	e_msg_composer_set_header (composer, header_name, message_uid);

	header_name = "X-Evolution-Source-Flags";
	e_msg_composer_set_header (composer, header_name, buffer->str);

	g_string_free (buffer, TRUE);
}

/**
 * e_msg_composer_attach:
 * @composer: a composer object
 * @mime_part: the #CamelMimePart to attach
 *
 * Attaches @attachment to the message being composed in the composer.
 **/
void
e_msg_composer_attach (EMsgComposer *composer,
                       CamelMimePart *mime_part)
{
	EAttachmentView *view;
	EAttachmentStore *store;
	EAttachment *attachment;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));
	g_return_if_fail (CAMEL_IS_MIME_PART (mime_part));

	view = e_msg_composer_get_attachment_view (composer);
	store = e_attachment_view_get_store (view);

	attachment = e_attachment_new ();
	e_attachment_set_mime_part (attachment, mime_part);
	e_attachment_store_add_attachment (store, attachment);
	e_attachment_load_async (
		attachment, (GAsyncReadyCallback)
		e_attachment_load_handle_error, composer);
	g_object_unref (attachment);
}

/**
 * e_msg_composer_add_inline_image_from_file:
 * @composer: a composer object
 * @filename: the name of the file containing the image
 *
 * This reads in the image in @filename and adds it to @composer
 * as an inline image, to be wrapped in a multipart/related.
 *
 * Returns: the newly-created CamelMimePart (which must be reffed
 * if the caller wants to keep its own reference), or %NULL on error.
 **/
CamelMimePart *
e_msg_composer_add_inline_image_from_file (EMsgComposer *composer,
                                           const gchar *filename)
{
	gchar *mime_type, *cid, *url, *name, *dec_file_name;
	CamelStream *stream;
	CamelDataWrapper *wrapper;
	CamelMimePart *part;
	EMsgComposerPrivate *p = composer->priv;

	dec_file_name = g_strdup (filename);
	camel_url_decode (dec_file_name);

	if (!g_file_test (dec_file_name, G_FILE_TEST_IS_REGULAR))
		return NULL;

	stream = camel_stream_fs_new_with_name (
		dec_file_name, O_RDONLY, 0, NULL);
	if (!stream)
		return NULL;

	wrapper = camel_data_wrapper_new ();
	camel_data_wrapper_construct_from_stream_sync (
		wrapper, stream, NULL, NULL);
	g_object_unref (CAMEL_OBJECT (stream));

	mime_type = e_util_guess_mime_type (dec_file_name, TRUE);
	if (mime_type == NULL)
		mime_type = g_strdup ("application/octet-stream");
	camel_data_wrapper_set_mime_type (wrapper, mime_type);
	g_free (mime_type);

	part = camel_mime_part_new ();
	camel_medium_set_content (CAMEL_MEDIUM (part), wrapper);
	g_object_unref (wrapper);

	cid = camel_header_msgid_generate ();
	camel_mime_part_set_content_id (part, cid);
	name = g_path_get_basename (dec_file_name);
	camel_mime_part_set_filename (part, name);
	g_free (name);
	camel_mime_part_set_encoding (part, CAMEL_TRANSFER_ENCODING_BASE64);

	url = g_strdup_printf ("file:%s", dec_file_name);
	g_hash_table_insert (p->inline_images_by_url, url, part);

	url = g_strdup_printf ("cid:%s", cid);
	g_hash_table_insert (p->inline_images, url, part);
	g_free (cid);

	g_free (dec_file_name);

	return part;
}

/**
 * e_msg_composer_add_inline_image_from_mime_part:
 * @composer: a composer object
 * @part: a CamelMimePart containing image data
 *
 * This adds the mime part @part to @composer as an inline image, to
 * be wrapped in a multipart/related.
 **/
void
e_msg_composer_add_inline_image_from_mime_part (EMsgComposer *composer,
                                                CamelMimePart *part)
{
	gchar *url;
	const gchar *location, *cid;
	EMsgComposerPrivate *p = composer->priv;

	cid = camel_mime_part_get_content_id (part);
	if (!cid) {
		camel_mime_part_set_content_id (part, NULL);
		cid = camel_mime_part_get_content_id (part);
	}

	url = g_strdup_printf ("cid:%s", cid);
	g_hash_table_insert (p->inline_images, url, part);
	g_object_ref (part);

	location = camel_mime_part_get_content_location (part);
	if (location != NULL)
		g_hash_table_insert (
			p->inline_images_by_url,
			g_strdup (location), part);
}

static void
composer_get_message_ready (EMsgComposer *composer,
                            GAsyncResult *result,
                            GSimpleAsyncResult *simple)
{
	CamelMimeMessage *message;
	GError *error = NULL;

	message = composer_build_message_finish (composer, result, &error);

	if (message != NULL)
		g_simple_async_result_set_op_res_gpointer (
			simple, message, (GDestroyNotify) g_object_unref);

	if (error != NULL) {
		g_warn_if_fail (message == NULL);
		g_simple_async_result_take_error (simple, error);
	}

	g_simple_async_result_complete (simple);

	g_object_unref (simple);
}

/**
 * e_msg_composer_get_message:
 * @composer: an #EMsgComposer
 *
 * Retrieve the message edited by the user as a #CamelMimeMessage.  The
 * #CamelMimeMessage object is created on the fly; subsequent calls to this
 * function will always create new objects from scratch.
 **/
void
e_msg_composer_get_message (EMsgComposer *composer,
                            gint io_priority,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
	GSimpleAsyncResult *simple;
	GtkAction *action;
	ComposerFlags flags = 0;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	simple = g_simple_async_result_new (
		G_OBJECT (composer), callback,
		user_data, e_msg_composer_get_message);

	if (gtkhtml_editor_get_html_mode (GTKHTML_EDITOR (composer)))
		flags |= COMPOSER_FLAG_HTML_CONTENT;

	action = ACTION (PRIORITIZE_MESSAGE);
	if (gtk_toggle_action_get_active (GTK_TOGGLE_ACTION (action)))
		flags |= COMPOSER_FLAG_PRIORITIZE_MESSAGE;

	action = ACTION (REQUEST_READ_RECEIPT);
	if (gtk_toggle_action_get_active (GTK_TOGGLE_ACTION (action)))
		flags |= COMPOSER_FLAG_REQUEST_READ_RECEIPT;

	action = ACTION (PGP_SIGN);
	if (gtk_toggle_action_get_active (GTK_TOGGLE_ACTION (action)))
		flags |= COMPOSER_FLAG_PGP_SIGN;

	action = ACTION (PGP_ENCRYPT);
	if (gtk_toggle_action_get_active (GTK_TOGGLE_ACTION (action)))
		flags |= COMPOSER_FLAG_PGP_ENCRYPT;

#ifdef HAVE_NSS
	action = ACTION (SMIME_SIGN);
	if (gtk_toggle_action_get_active (GTK_TOGGLE_ACTION (action)))
		flags |= COMPOSER_FLAG_SMIME_SIGN;

	action = ACTION (SMIME_ENCRYPT);
	if (gtk_toggle_action_get_active (GTK_TOGGLE_ACTION (action)))
		flags |= COMPOSER_FLAG_SMIME_ENCRYPT;
#endif

	composer_build_message (
		composer, flags, io_priority,
		cancellable, (GAsyncReadyCallback)
		composer_get_message_ready, simple);
}

CamelMimeMessage *
e_msg_composer_get_message_finish (EMsgComposer *composer,
                                   GAsyncResult *result,
                                   GError **error)
{
	GSimpleAsyncResult *simple;
	CamelMimeMessage *message;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (composer),
		e_msg_composer_get_message), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	message = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	return g_object_ref (message);
}

void
e_msg_composer_get_message_print (EMsgComposer *composer,
                                  gint io_priority,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
	GSimpleAsyncResult *simple;
	ComposerFlags flags = 0;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	simple = g_simple_async_result_new (
		G_OBJECT (composer), callback,
		user_data, e_msg_composer_get_message_print);

	flags |= COMPOSER_FLAG_HTML_CONTENT;
	flags |= COMPOSER_FLAG_SAVE_OBJECT_DATA;

	composer_build_message (
		composer, flags, io_priority,
		cancellable, (GAsyncReadyCallback)
		composer_get_message_ready, simple);
}

CamelMimeMessage *
e_msg_composer_get_message_print_finish (EMsgComposer *composer,
                                         GAsyncResult *result,
                                         GError **error)
{
	GSimpleAsyncResult *simple;
	CamelMimeMessage *message;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (composer),
		e_msg_composer_get_message_print), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	message = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	return g_object_ref (message);
}

void
e_msg_composer_get_message_draft (EMsgComposer *composer,
                                  gint io_priority,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
	GSimpleAsyncResult *simple;
	ComposerFlags flags = 0;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	simple = g_simple_async_result_new (
		G_OBJECT (composer), callback,
		user_data, e_msg_composer_get_message_draft);

	if (gtkhtml_editor_get_html_mode (GTKHTML_EDITOR (composer)))
		flags |= COMPOSER_FLAG_HTML_CONTENT;

	composer_build_message (
		composer, flags, io_priority,
		cancellable, (GAsyncReadyCallback)
		composer_get_message_ready, simple);
}

CamelMimeMessage *
e_msg_composer_get_message_draft_finish (EMsgComposer *composer,
                                         GAsyncResult *result,
                                         GError **error)
{
	GSimpleAsyncResult *simple;
	CamelMimeMessage *message;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (composer),
		e_msg_composer_get_message_draft), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	message = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	return g_object_ref (message);
}

/**
 * e_msg_composer_show_sig:
 * @composer: A message composer widget
 *
 * Set a signature
 **/
void
e_msg_composer_show_sig_file (EMsgComposer *composer)
{
	GtkhtmlEditor *editor;
	gchar *html_text;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	editor = GTKHTML_EDITOR (composer);

	if (composer->priv->redirect)
		return;

	composer->priv->in_signature_insert = TRUE;

	gtkhtml_editor_freeze (editor);
	gtkhtml_editor_run_command (editor, "cursor-position-save");
	gtkhtml_editor_undo_begin (editor, "Set signature", "Reset signature");

	/* Delete the old signature. */
	gtkhtml_editor_run_command (editor, "block-selection");
	gtkhtml_editor_run_command (editor, "cursor-bod");
	if (gtkhtml_editor_search_by_data (editor, 1, "ClueFlow", "signature", "1")) {
		gtkhtml_editor_run_command (editor, "select-paragraph");
		gtkhtml_editor_run_command (editor, "delete");
		gtkhtml_editor_set_paragraph_data (editor, "signature", "0");
		gtkhtml_editor_run_command (editor, "delete-back");
	}
	gtkhtml_editor_run_command (editor, "unblock-selection");

	html_text = get_signature_html (composer);
	if (html_text) {
		gtkhtml_editor_run_command (editor, "insert-paragraph");
		if (!gtkhtml_editor_run_command (editor, "cursor-backward"))
			gtkhtml_editor_run_command (editor, "insert-paragraph");
		else
			gtkhtml_editor_run_command (editor, "cursor-forward");

		gtkhtml_editor_set_paragraph_data (editor, "orig", "0");
		gtkhtml_editor_run_command (editor, "indent-zero");
		gtkhtml_editor_run_command (editor, "style-normal");
		gtkhtml_editor_insert_html (editor, html_text);
		g_free (html_text);
	} else if (is_top_signature (composer)) {
		/* insert paragraph after the signature ClueFlow things */
		if (gtkhtml_editor_run_command (editor, "cursor-forward"))
			gtkhtml_editor_run_command (editor, "insert-paragraph");
	}

	gtkhtml_editor_undo_end (editor);
	gtkhtml_editor_run_command (editor, "cursor-position-restore");
	gtkhtml_editor_thaw (editor);

	composer->priv->in_signature_insert = FALSE;
}

CamelInternetAddress *
e_msg_composer_get_from (EMsgComposer *composer)
{
	CamelInternetAddress *address;
	EComposerHeaderTable *table;
	EAccount *account;

	g_return_val_if_fail (E_IS_MSG_COMPOSER (composer), NULL);

	table = e_msg_composer_get_header_table (composer);

	account = e_composer_header_table_get_account (table);
	if (account == NULL)
		return NULL;

	address = camel_internet_address_new ();
	camel_internet_address_add (
		address, account->id->name, account->id->address);

	return address;
}

CamelInternetAddress *
e_msg_composer_get_reply_to (EMsgComposer *composer)
{
	CamelInternetAddress *address;
	EComposerHeaderTable *table;
	const gchar *reply_to;

	g_return_val_if_fail (E_IS_MSG_COMPOSER (composer), NULL);

	table = e_msg_composer_get_header_table (composer);

	reply_to = e_composer_header_table_get_reply_to (table);
	if (reply_to == NULL || *reply_to == '\0')
		return NULL;

	address = camel_internet_address_new ();
	if (camel_address_unformat (CAMEL_ADDRESS (address), reply_to) == -1) {
		g_object_unref (CAMEL_OBJECT (address));
		return NULL;
	}

	return address;
}

/**
 * e_msg_composer_get_raw_message_text:
 *
 * Returns the text/plain of the message from composer
 **/
GByteArray *
e_msg_composer_get_raw_message_text (EMsgComposer *composer)
{
	GtkhtmlEditor *editor;
	GByteArray *array;
	gchar *text;
	gsize length;

	g_return_val_if_fail (E_IS_MSG_COMPOSER (composer), NULL);

	array = g_byte_array_new ();
	editor = GTKHTML_EDITOR (composer);
	text = gtkhtml_editor_get_text_plain (editor, &length);
	g_byte_array_append (array, (guint8 *) text, (guint) length);
	g_free (text);

	return array;
}

gboolean
e_msg_composer_is_exiting (EMsgComposer *composer)
{
	g_return_val_if_fail (composer != NULL, FALSE);

	return composer->priv->application_exiting;
}

void
e_msg_composer_request_close (EMsgComposer *composer)
{
	g_return_if_fail (composer != NULL);

	composer->priv->application_exiting = TRUE;
}

/* Returns whether can close the composer immediately. It will return FALSE
 * also when saving to drafts, but the e_msg_composer_is_exiting will return
 * TRUE for this case.  can_save_draft means whether can save draft
 * immediately, or rather keep it on the caller (when FALSE). If kept on the
 * folder, then returns FALSE and sets interval variable to return TRUE in
 * e_msg_composer_is_exiting. */
gboolean
e_msg_composer_can_close (EMsgComposer *composer,
                          gboolean can_save_draft)
{
	gboolean res = FALSE;
	GtkhtmlEditor *editor;
	EComposerHeaderTable *table;
	GdkWindow *window;
	GtkWidget *widget;
	const gchar *subject;
	gint response;

	editor = GTKHTML_EDITOR (composer);
	widget = GTK_WIDGET (composer);

	if (!gtkhtml_editor_get_changed (editor))
		return TRUE;

	window = gtk_widget_get_window (widget);
	gdk_window_raise (window);

	table = e_msg_composer_get_header_table (composer);
	subject = e_composer_header_table_get_subject (table);

	if (subject == NULL || *subject == '\0')
		subject = _("Untitled Message");

	response = e_alert_run_dialog_for_args (
		GTK_WINDOW (composer),
		"mail-composer:exit-unsaved",
		subject, NULL);

	switch (response) {
		case GTK_RESPONSE_YES:
			gtk_widget_hide (widget);
			e_msg_composer_request_close (composer);
			if (can_save_draft)
				gtk_action_activate (ACTION (SAVE_DRAFT));
			break;

		case GTK_RESPONSE_NO:
			res = TRUE;
			break;

		case GTK_RESPONSE_CANCEL:
			break;
	}

	return res;
}

void
e_msg_composer_reply_indent (EMsgComposer *composer)
{
	GtkhtmlEditor *editor;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	editor = GTKHTML_EDITOR (composer);

	if (!gtkhtml_editor_is_paragraph_empty (editor)) {
		if (gtkhtml_editor_is_previous_paragraph_empty (editor))
			gtkhtml_editor_run_command (editor, "cursor-backward");
		else {
			gtkhtml_editor_run_command (editor, "text-default-color");
			gtkhtml_editor_run_command (editor, "italic-off");
			gtkhtml_editor_run_command (editor, "insert-paragraph");
			return;
		}
	}

	gtkhtml_editor_run_command (editor, "style-normal");
	gtkhtml_editor_run_command (editor, "indent-zero");
	gtkhtml_editor_run_command (editor, "text-default-color");
	gtkhtml_editor_run_command (editor, "italic-off");
}

EComposerHeaderTable *
e_msg_composer_get_header_table (EMsgComposer *composer)
{
	g_return_val_if_fail (E_IS_MSG_COMPOSER (composer), NULL);

	return E_COMPOSER_HEADER_TABLE (composer->priv->header_table);
}

EAttachmentView *
e_msg_composer_get_attachment_view (EMsgComposer *composer)
{
	g_return_val_if_fail (E_IS_MSG_COMPOSER (composer), NULL);

	return E_ATTACHMENT_VIEW (composer->priv->attachment_paned);
}

GList *
e_load_spell_languages (void)
{
	GConfClient *client;
	GList *spell_languages = NULL;
	GSList *list;
	const gchar *key;
	GError *error = NULL;

	/* Ask GConf for a list of spell check language codes. */
	client = gconf_client_get_default ();
	key = COMPOSER_GCONF_SPELL_LANGUAGES_KEY;
	list = gconf_client_get_list (client, key, GCONF_VALUE_STRING, &error);
	g_object_unref (client);

	/* Convert the codes to spell language structs. */
	while (list != NULL) {
		gchar *language_code = list->data;
		const GtkhtmlSpellLanguage *language;

		language = gtkhtml_spell_language_lookup (language_code);
		if (language != NULL)
			spell_languages = g_list_prepend (
				spell_languages, (gpointer) language);

		list = g_slist_delete_link (list, list);
		g_free (language_code);
	}

	spell_languages = g_list_reverse (spell_languages);

	/* Pick a default spell language if GConf came back empty. */
	if (spell_languages == NULL) {
		const GtkhtmlSpellLanguage *language;

		language = gtkhtml_spell_language_lookup (NULL);

		if (language) {
			spell_languages = g_list_prepend (
				spell_languages, (gpointer) language);

		/* Don't overwrite the stored spell check language
		 * codes if there was a problem retrieving them. */
			if (error == NULL)
				e_save_spell_languages (spell_languages);
		}
	}

	if (error != NULL) {
		g_warning ("%s", error->message);
		g_error_free (error);
	}

	return spell_languages;
}

void
e_save_spell_languages (GList *spell_languages)
{
	GConfClient *client;
	GSList *list = NULL;
	const gchar *key;
	GError *error = NULL;

	/* Build a list of spell check language codes. */
	while (spell_languages != NULL) {
		const GtkhtmlSpellLanguage *language;
		const gchar *language_code;

		language = spell_languages->data;
		language_code = gtkhtml_spell_language_get_code (language);
		list = g_slist_prepend (list, (gpointer) language_code);

		spell_languages = g_list_next (spell_languages);
	}

	list = g_slist_reverse (list);

	/* Save the language codes to GConf. */
	client = gconf_client_get_default ();
	key = COMPOSER_GCONF_SPELL_LANGUAGES_KEY;
	gconf_client_set_list (client, key, GCONF_VALUE_STRING, list, &error);
	g_object_unref (client);

	g_slist_free (list);

	if (error != NULL) {
		g_warning ("%s", error->message);
		g_error_free (error);
	}
}

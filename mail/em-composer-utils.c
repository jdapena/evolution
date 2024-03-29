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
 *		Jeffrey Stedfast <fejj@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <gtk/gtk.h>

#include <libedataserver/e-data-server-util.h>
#include <glib/gi18n.h>

#include "mail-mt.h"
#include "mail-ops.h"
#include "mail-tools.h"
#include "mail-send-recv.h"

#include "e-util/e-account-utils.h"
#include "e-util/e-alert-dialog.h"
#include "e-util/e-alert-sink.h"
#include "e-util/e-util.h"

#include "shell/e-shell.h"

#include "e-mail-folder-utils.h"
#include "e-mail-local.h"
#include "e-mail-session.h"
#include "e-mail-session-utils.h"
#include "em-utils.h"
#include "em-composer-utils.h"
#include "composer/e-msg-composer.h"
#include "composer/e-composer-actions.h"
#include "composer/e-composer-post-header.h"
#include "em-folder-selector.h"
#include "em-folder-tree.h"
#include "em-format-html.h"
#include "em-format-html-print.h"
#include "em-format-quote.h"
#include "em-event.h"

#ifdef G_OS_WIN32
#ifdef gmtime_r
#undef gmtime_r
#endif

/* The gmtime() in Microsoft's C library is MT-safe */
#define gmtime_r(tp,tmp) (gmtime(tp)?(*(tmp)=*gmtime(tp),(tmp)):0)
#endif

#define GCONF_KEY_TEMPLATE_PLACEHOLDERS \
	"/apps/evolution/mail/template_placeholders"

typedef struct _AsyncContext AsyncContext;
typedef struct _ForwardData ForwardData;

struct _AsyncContext {
	CamelMimeMessage *message;
	EMsgComposer *composer;
	EActivity *activity;
	EMailReader *reader;
	GPtrArray *ptr_array;
	EMailForwardStyle style;
	gchar *folder_uri;
	gchar *message_uid;
	gboolean replace;
	GtkWidget *destroy_when_done;
};

struct _ForwardData {
	EShell *shell;
	CamelFolder *folder;
	GPtrArray *uids;
	EMailForwardStyle style;
};

static void
async_context_free (AsyncContext *context)
{
	if (context->message != NULL)
		g_object_unref (context->message);

	if (context->composer != NULL)
		g_object_unref (context->composer);

	if (context->activity != NULL)
		g_object_unref (context->activity);

	if (context->reader != NULL)
		g_object_unref (context->reader);

	if (context->ptr_array != NULL)
		g_ptr_array_unref (context->ptr_array);

	if (context->destroy_when_done != NULL)
		gtk_widget_destroy (context->destroy_when_done);

	g_free (context->folder_uri);
	g_free (context->message_uid);

	g_slice_free (AsyncContext, context);
}

static void
forward_data_free (ForwardData *data)
{
	if (data->shell != NULL)
		g_object_unref (data->shell);

	if (data->folder != NULL)
		g_object_unref (data->folder);

	if (data->uids != NULL)
		em_utils_uids_free (data->uids);

	g_slice_free (ForwardData, data);
}

static gboolean
ask_confirm_for_unwanted_html_mail (EMsgComposer *composer,
                                    EDestination **recipients)
{
	gboolean res;
	GString *str;
	gint i;

	str = g_string_new ("");
	for (i = 0; recipients[i] != NULL; ++i) {
		if (!e_destination_get_html_mail_pref (recipients[i])) {
			const gchar *name;

			name = e_destination_get_textrep (recipients[i], FALSE);

			g_string_append_printf (str, "     %s\n", name);
		}
	}

	if (str->len)
		res = em_utils_prompt_user (
			GTK_WINDOW (composer),
			"/apps/evolution/mail/prompts/unwanted_html",
			"mail:ask-send-html", str->str, NULL);
	else
		res = TRUE;

	g_string_free (str, TRUE);

	return res;
}

static gboolean
ask_confirm_for_empty_subject (EMsgComposer *composer)
{
	return em_utils_prompt_user (
		GTK_WINDOW (composer),
		"/apps/evolution/mail/prompts/empty_subject",
		"mail:ask-send-no-subject", NULL);
}

static gboolean
ask_confirm_for_only_bcc (EMsgComposer *composer,
                          gboolean hidden_list_case)
{
	/* If the user is mailing a hidden contact list, it is possible for
	 * them to create a message with only Bcc recipients without really
	 * realizing it.  To try to avoid being totally confusing, I've changed
	 * this dialog to provide slightly different text in that case, to
	 * better explain what the hell is going on. */

	return em_utils_prompt_user (
		GTK_WINDOW (composer),
		"/apps/evolution/mail/prompts/only_bcc",
		hidden_list_case ?
		"mail:ask-send-only-bcc-contact" :
		"mail:ask-send-only-bcc", NULL);
}

static gboolean
is_group_definition (const gchar *str)
{
	const gchar *colon;

	if (!str || !*str)
		return FALSE;

	colon = strchr (str, ':');
	return colon > str && strchr (str, ';') > colon;
}

static gboolean
composer_presend_check_recipients (EMsgComposer *composer)
{
	EDestination **recipients;
	EDestination **recipients_bcc;
	CamelInternetAddress *cia;
	EComposerHeaderTable *table;
	EComposerHeader *post_to_header;
	GString *invalid_addrs = NULL;
	gboolean check_passed = FALSE;
	gint hidden = 0;
	gint shown = 0;
	gint num = 0;
	gint num_bcc = 0;
	gint num_post = 0;
	gint ii;

	/* We should do all of the validity checks based on the composer,
	 * and not on the created message, as extra interaction may occur
	 * when we get the message (e.g. passphrase to sign a message). */

	table = e_msg_composer_get_header_table (composer);
	recipients = e_composer_header_table_get_destinations (table);

	cia = camel_internet_address_new ();

	/* See which ones are visible, present, etc. */
	for (ii = 0; recipients != NULL && recipients[ii] != NULL; ii++) {
		const gchar *addr;
		gint len, j;

		addr = e_destination_get_address (recipients[ii]);
		if (addr == NULL || *addr == '\0')
			continue;

		camel_address_decode (CAMEL_ADDRESS (cia), addr);
		len = camel_address_length (CAMEL_ADDRESS (cia));

		if (len > 0) {
			if (!e_destination_is_evolution_list (recipients[ii])) {
				for (j = 0; j < len; j++) {
					const gchar *name = NULL, *eml = NULL;

					if (!camel_internet_address_get (cia, j, &name, &eml) ||
					    !eml ||
					    strchr (eml, '@') <= eml) {
						if (!invalid_addrs)
							invalid_addrs = g_string_new ("");
						else
							g_string_append (invalid_addrs, ", ");

						if (name)
							g_string_append (invalid_addrs, name);
						if (eml) {
							g_string_append (invalid_addrs, name ? " <" : "");
							g_string_append (invalid_addrs, eml);
							g_string_append (invalid_addrs, name ? ">" : "");
						}
					}
				}
			}

			camel_address_remove (CAMEL_ADDRESS (cia), -1);
			num++;
			if (e_destination_is_evolution_list (recipients[ii])
			    && !e_destination_list_show_addresses (recipients[ii])) {
				hidden++;
			} else {
				shown++;
			}
		} else if (is_group_definition (addr)) {
			/* like an address, it will not claim on only-bcc */
			shown++;
			num++;
		} else if (!invalid_addrs) {
			invalid_addrs = g_string_new (addr);
		} else {
			g_string_append (invalid_addrs, ", ");
			g_string_append (invalid_addrs, addr);
		}
	}

	recipients_bcc = e_composer_header_table_get_destinations_bcc (table);
	if (recipients_bcc) {
		for (ii = 0; recipients_bcc[ii] != NULL; ii++) {
			const gchar *addr;

			addr = e_destination_get_address (recipients_bcc[ii]);
			if (addr == NULL || *addr == '\0')
				continue;

			camel_address_decode (CAMEL_ADDRESS (cia), addr);
			if (camel_address_length (CAMEL_ADDRESS (cia)) > 0) {
				camel_address_remove (CAMEL_ADDRESS (cia), -1);
				num_bcc++;
			}
		}

		e_destination_freev (recipients_bcc);
	}

	g_object_unref (cia);

	post_to_header = e_composer_header_table_get_header (
		table, E_COMPOSER_HEADER_POST_TO);
	if (e_composer_header_get_visible (post_to_header)) {
		GList *postlist;

		postlist = e_composer_header_table_get_post_to (table);
		num_post = g_list_length (postlist);
		g_list_foreach (postlist, (GFunc) g_free, NULL);
		g_list_free (postlist);
	}

	/* I'm sensing a lack of love, er, I mean recipients. */
	if (num == 0 && num_post == 0) {
		e_alert_submit (
			E_ALERT_SINK (composer),
			"mail:send-no-recipients", NULL);
		goto finished;
	}

	if (invalid_addrs) {
		if (!em_utils_prompt_user (
			GTK_WINDOW (composer),
			"/apps/evolution/mail/prompts/send_invalid_recip",
			strstr (invalid_addrs->str, ", ") ?
				"mail:ask-send-invalid-recip-multi" :
				"mail:ask-send-invalid-recip-one",
			invalid_addrs->str, NULL)) {
			g_string_free (invalid_addrs, TRUE);
			goto finished;
		}

		g_string_free (invalid_addrs, TRUE);
	}

	if (num > 0 && (num == num_bcc || shown == 0)) {
		/* this means that the only recipients are Bcc's */
		if (!ask_confirm_for_only_bcc (composer, shown == 0))
			goto finished;
	}

	check_passed = TRUE;

finished:
	if (recipients != NULL)
		e_destination_freev (recipients);

	return check_passed;
}

static gboolean
composer_presend_check_account (EMsgComposer *composer)
{
	EComposerHeaderTable *table;
	EAccount *account;
	gboolean check_passed;

	table = e_msg_composer_get_header_table (composer);
	account = e_composer_header_table_get_account (table);
	check_passed = (account != NULL && account->enabled);

	if (!check_passed)
		e_alert_submit (
			E_ALERT_SINK (composer),
			"mail:send-no-account-enabled", NULL);

	return check_passed;
}

static gboolean
composer_presend_check_downloads (EMsgComposer *composer)
{
	EAttachmentView *view;
	EAttachmentStore *store;
	gboolean check_passed = TRUE;

	view = e_msg_composer_get_attachment_view (composer);
	store = e_attachment_view_get_store (view);

	if (e_attachment_store_get_num_loading (store) > 0) {
		if (!em_utils_prompt_user (GTK_WINDOW (composer), NULL,
		    "mail-composer:ask-send-message-pending-download", NULL))
			check_passed = FALSE;
	}

	return check_passed;
}

static gboolean
composer_presend_check_plugins (EMsgComposer *composer)
{
	EMEvent *eme;
	EMEventTargetComposer *target;
	gpointer data;

	/** @Event: composer.presendchecks
	 * @Title: Composer PreSend Checks
	 * @Target: EMEventTargetMessage
	 *
	 * composer.presendchecks is emitted during pre-checks for the
	 * message just before sending.  Since the e-plugin framework
	 * doesn't provide a way to return a value from the plugin,
	 * use 'presend_check_status' to set whether the check passed.
	 */
	eme = em_event_peek ();
	target = em_event_target_new_composer (eme, composer, 0);

	e_event_emit (
		(EEvent *) eme, "composer.presendchecks",
		(EEventTarget *) target);

	/* A non-NULL value for this key means the check failed. */
	data = g_object_get_data (G_OBJECT (composer), "presend_check_status");

	/* Clear the value in case we have to run these checks again. */
	g_object_set_data (G_OBJECT (composer), "presend_check_status", NULL);

	return (data == NULL);
}

static gboolean
composer_presend_check_subject (EMsgComposer *composer)
{
	EComposerHeaderTable *table;
	const gchar *subject;
	gboolean check_passed = TRUE;

	table = e_msg_composer_get_header_table (composer);
	subject = e_composer_header_table_get_subject (table);

	if (subject == NULL || subject[0] == '\0') {
		if (!ask_confirm_for_empty_subject (composer))
			check_passed = FALSE;
	}

	return check_passed;
}

static gboolean
composer_presend_check_unwanted_html (EMsgComposer *composer)
{
	EDestination **recipients;
	EComposerHeaderTable *table;
	GConfClient *client;
	gboolean check_passed = TRUE;
	gboolean html_mode;
	gboolean send_html;
	gboolean confirm_html;
	gint ii;

	client = gconf_client_get_default ();

	table = e_msg_composer_get_header_table (composer);
	recipients = e_composer_header_table_get_destinations (table);
	html_mode = gtkhtml_editor_get_html_mode (GTKHTML_EDITOR (composer));

	send_html = gconf_client_get_bool (
		client, "/apps/evolution/mail/composer/send_html", NULL);
	confirm_html = gconf_client_get_bool (
		client, "/apps/evolution/mail/prompts/unwanted_html", NULL);

	/* Only show this warning if our default is to send html.  If it
	 * isn't, we've manually switched into html mode in the composer
	 * and (presumably) had a good reason for doing this. */
	if (html_mode && send_html && confirm_html && recipients != NULL) {
		gboolean html_problem = FALSE;

		for (ii = 0; recipients[ii] != NULL; ii++) {
			if (!e_destination_get_html_mail_pref (recipients[ii]))
				html_problem = TRUE;
				break;
		}

		if (html_problem) {
			if (!ask_confirm_for_unwanted_html_mail (
				composer, recipients))
				check_passed = FALSE;
		}
	}

	if (recipients != NULL)
		e_destination_freev (recipients);

	g_object_unref (client);

	return check_passed;
}

static void
composer_send_completed (EMailSession *session,
                         GAsyncResult *result,
                         AsyncContext *context)
{
	GError *error = NULL;

	e_mail_session_send_to_finish (session, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_error_free (error);
		goto exit;
	}

	/* Post-processing errors are shown in the shell window. */
	if (g_error_matches (error, E_MAIL_ERROR, E_MAIL_ERROR_POST_PROCESSING)) {
		EAlert *alert;
		EShell *shell;

		shell = e_msg_composer_get_shell (context->composer);

		alert = e_alert_new (
			"mail-composer:send-post-processing-error",
			error->message, NULL);
		e_shell_submit_alert (shell, alert);
		g_object_unref (alert);

	/* All other errors are shown in the composer window. */
	} else if (error != NULL) {
		gint response;

		/* Clear the activity bar before
		 * presenting the error dialog. */
		g_object_unref (context->activity);
		context->activity = NULL;

		response = e_alert_run_dialog_for_args (
			GTK_WINDOW (context->composer),
			"mail-composer:send-error",
			error->message, NULL);
		if (response == GTK_RESPONSE_OK)  /* Try Again */
			e_msg_composer_send (context->composer);
		if (response == GTK_RESPONSE_ACCEPT)  /* Save to Outbox */
			e_msg_composer_save_to_outbox (context->composer);
		g_error_free (error);
		goto exit;
	}

	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);

	/* Wait for the EActivity's completion message to
	 * time out and then destroy the composer window. */
	g_object_weak_ref (
		G_OBJECT (context->activity), (GWeakNotify)
		gtk_widget_destroy, context->composer);

exit:
	async_context_free (context);
}

static void
em_utils_composer_send_cb (EMsgComposer *composer,
                           CamelMimeMessage *message,
                           EActivity *activity)
{
	AsyncContext *context;
	CamelSession *session;
	GCancellable *cancellable;

	context = g_slice_new0 (AsyncContext);
	context->message = g_object_ref (message);
	context->composer = g_object_ref (composer);
	context->activity = g_object_ref (activity);

	cancellable = e_activity_get_cancellable (activity);
	session = e_msg_composer_get_session (context->composer);

	e_mail_session_send_to (
		E_MAIL_SESSION (session), message,
		G_PRIORITY_DEFAULT, cancellable, NULL, NULL,
		(GAsyncReadyCallback) composer_send_completed,
		context);
}

static void
composer_set_no_change (EMsgComposer *composer)
{
	GtkhtmlEditor *editor;

	g_return_if_fail (composer != NULL);

	editor = GTKHTML_EDITOR (composer);

	gtkhtml_editor_drop_undo (editor);
	gtkhtml_editor_set_changed (editor, FALSE);
}

/* delete original messages from Outbox folder */
static void
manage_x_evolution_replace_outbox (EMsgComposer *composer,
                                   CamelMimeMessage *message,
                                   GCancellable *cancellable)
{
	const gchar *message_uid;
	const gchar *header;
	CamelFolder *outbox;

	g_return_if_fail (composer != NULL);
	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	header = "X-Evolution-Replace-Outbox-UID";
	message_uid = camel_medium_get_header (CAMEL_MEDIUM (message), header);
	e_msg_composer_remove_header (composer, header);

	if (!message_uid)
		return;

	outbox = e_mail_local_get_folder (E_MAIL_LOCAL_FOLDER_OUTBOX);
	g_return_if_fail (outbox != NULL);

	camel_folder_set_message_flags (
		outbox, message_uid,
		CAMEL_MESSAGE_DELETED | CAMEL_MESSAGE_SEEN,
		CAMEL_MESSAGE_DELETED | CAMEL_MESSAGE_SEEN);

	/* ignore errors here */
	camel_folder_synchronize_message_sync (
		outbox, message_uid, cancellable, NULL);
}

static void
composer_save_to_drafts_complete (EMailSession *session,
                                  GAsyncResult *result,
                                  AsyncContext *context)
{
	GError *error = NULL;

	/* We don't really care if this failed.  If something other than
	 * cancellation happened, emit a runtime warning so the error is
	 * not completely lost. */

	e_mail_session_handle_draft_headers_finish (session, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_error_free (error);

	} else if (error != NULL) {
		g_warning ("%s", error->message);
		g_error_free (error);

	} else
		e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);

	/* Encode the draft message we just saved into the EMsgComposer
	 * as X-Evolution-Draft headers.  The message will be marked for
	 * deletion if the user saves a newer draft message or sends the
	 * composed message. */
	e_msg_composer_set_draft_headers (
		context->composer, context->folder_uri,
		context->message_uid);

	async_context_free (context);
}

static void
composer_save_to_drafts_cleanup (CamelFolder *drafts_folder,
                                 GAsyncResult *result,
                                 AsyncContext *context)
{
	CamelSession *session;
	EAlertSink *alert_sink;
	GCancellable *cancellable;
	GError *error = NULL;

	session = e_msg_composer_get_session (context->composer);
	alert_sink = e_activity_get_alert_sink (context->activity);
	cancellable = e_activity_get_cancellable (context->activity);

	e_mail_folder_append_message_finish (
		drafts_folder, result, &context->message_uid, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (context->message_uid == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (context->message_uid == NULL);
		e_alert_submit (
			alert_sink,
			"mail-composer:save-to-drafts-error",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	/* Mark the previously saved draft message for deletion.
	 * Note: This is just a nice-to-have; ignore failures. */
	e_mail_session_handle_draft_headers (
		E_MAIL_SESSION (session), context->message,
		G_PRIORITY_DEFAULT, cancellable, (GAsyncReadyCallback)
		composer_save_to_drafts_complete, context);
}

static void
composer_save_to_drafts_append_mail (AsyncContext *context,
                                     CamelFolder *drafts_folder)
{
	CamelFolder *local_drafts_folder;
	GCancellable *cancellable;
	CamelMessageInfo *info;

	local_drafts_folder =
		e_mail_local_get_folder (E_MAIL_LOCAL_FOLDER_DRAFTS);

	if (drafts_folder == NULL)
		drafts_folder = g_object_ref (local_drafts_folder);

	cancellable = e_activity_get_cancellable (context->activity);

	info = camel_message_info_new (NULL);

	camel_message_info_set_flags (
		info, CAMEL_MESSAGE_DRAFT | CAMEL_MESSAGE_SEEN, ~0);

	camel_medium_remove_header (
		CAMEL_MEDIUM (context->message),
		"X-Evolution-Replace-Outbox-UID");

	e_mail_folder_append_message (
		drafts_folder, context->message,
		info, G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) composer_save_to_drafts_cleanup,
		context);

	camel_message_info_free (info);

	g_object_unref (drafts_folder);
}

static void
composer_save_to_drafts_got_folder (EMailSession *session,
                                    GAsyncResult *result,
                                    AsyncContext *context)
{
	CamelFolder *drafts_folder;
	GError *error = NULL;

	drafts_folder = e_mail_session_uri_to_folder_finish (
		session, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (drafts_folder == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		gint response;

		g_warn_if_fail (drafts_folder == NULL);

		/* XXX Not showing the error message in the dialog? */
		g_error_free (error);

		/* If we can't retrieve the Drafts folder for the
		 * selected account, ask the user if he wants to
		 * save to the local Drafts folder instead. */
		response = e_alert_run_dialog_for_args (
			GTK_WINDOW (context->composer),
			"mail:ask-default-drafts", NULL);
		if (response != GTK_RESPONSE_YES) {
			async_context_free (context);
			return;
		}
	}

	composer_save_to_drafts_append_mail (context, drafts_folder);
}

static void
em_utils_composer_save_to_drafts_cb (EMsgComposer *composer,
                                     CamelMimeMessage *message,
                                     EActivity *activity)
{
	AsyncContext *context;
	EComposerHeaderTable *table;
	const gchar *drafts_folder_uri = NULL;
	const gchar *local_drafts_folder_uri;
	CamelSession *session;
	EAccount *account;

	context = g_slice_new0 (AsyncContext);
	context->message = g_object_ref (message);
	context->composer = g_object_ref (composer);
	context->activity = g_object_ref (activity);

	session = e_msg_composer_get_session (composer);

	local_drafts_folder_uri =
		e_mail_local_get_folder_uri (E_MAIL_LOCAL_FOLDER_DRAFTS);

	table = e_msg_composer_get_header_table (composer);
	account = e_composer_header_table_get_account (table);

	if (account != NULL && account->enabled)
		drafts_folder_uri = account->drafts_folder_uri;

	if (g_strcmp0 (drafts_folder_uri, local_drafts_folder_uri) == 0)
		drafts_folder_uri = NULL;

	if (drafts_folder_uri == NULL) {
		composer_save_to_drafts_append_mail (context, NULL);
		context->folder_uri = g_strdup (local_drafts_folder_uri);
	} else {
		GCancellable *cancellable;

		cancellable = e_activity_get_cancellable (activity);
		context->folder_uri = g_strdup (drafts_folder_uri);

		e_mail_session_uri_to_folder (
			E_MAIL_SESSION (session),
			drafts_folder_uri, 0, G_PRIORITY_DEFAULT,
			cancellable, (GAsyncReadyCallback)
			composer_save_to_drafts_got_folder, context);
	}
}

static void
composer_save_to_outbox_completed (CamelFolder *outbox_folder,
                                   GAsyncResult *result,
                                   AsyncContext *context)
{
	EAlertSink *alert_sink;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	e_mail_folder_append_message_finish (
		outbox_folder, result, NULL, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_error_free (error);
		goto exit;

	} else if (error != NULL) {
		e_alert_submit (
			alert_sink,
			"mail-composer:append-to-outbox-error",
			error->message, NULL);
		g_error_free (error);
		goto exit;
	}

	/* special processing for Outbox folder */
	manage_x_evolution_replace_outbox (
		context->composer, context->message,
		e_activity_get_cancellable (context->activity));

	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);

	/* Wait for the EActivity's completion message to
	 * time out and then destroy the composer window. */
	g_object_weak_ref (
		G_OBJECT (context->activity), (GWeakNotify)
		gtk_widget_destroy, context->composer);

exit:
	async_context_free (context);
}

static void
em_utils_composer_save_to_outbox_cb (EMsgComposer *composer,
                                     CamelMimeMessage *message,
                                     EActivity *activity)
{
	AsyncContext *context;
	CamelFolder *outbox_folder;
	CamelMessageInfo *info;
	GCancellable *cancellable;

	context = g_slice_new0 (AsyncContext);
	context->message = g_object_ref (message);
	context->composer = g_object_ref (composer);
	context->activity = g_object_ref (activity);

	cancellable = e_activity_get_cancellable (activity);
	outbox_folder = e_mail_local_get_folder (E_MAIL_LOCAL_FOLDER_OUTBOX);

	info = camel_message_info_new (NULL);
	camel_message_info_set_flags (info, CAMEL_MESSAGE_SEEN, ~0);

	e_mail_folder_append_message (
		outbox_folder, message, info,
		G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) composer_save_to_outbox_completed,
		context);

	camel_message_info_free (info);
}

static void
em_utils_composer_print_cb (EMsgComposer *composer,
                            GtkPrintOperationAction action,
                            CamelMimeMessage *message,
                            EActivity *activity)
{
	EMFormatHTMLPrint *efhp;

	efhp = em_format_html_print_new (NULL, action);
	em_format_html_print_message (efhp, message, NULL, NULL);
	g_object_unref (efhp);
}

/* Composing messages... */

static EMsgComposer *
create_new_composer (EShell *shell,
                     const gchar *subject,
                     CamelFolder *folder)
{
	EMsgComposer *composer;
	EComposerHeaderTable *table;
	EAccount *account = NULL;

	composer = e_msg_composer_new (shell);

	table = e_msg_composer_get_header_table (composer);

	if (folder != NULL) {
		CamelStore *store;
		const gchar *uid;
		gchar *folder_uri;
		GList *list;

		store = camel_folder_get_parent_store (folder);
		uid = camel_service_get_uid (CAMEL_SERVICE (store));
		account = e_get_account_by_uid (uid);

		folder_uri = e_mail_folder_uri_from_folder (folder);

		list = g_list_prepend (NULL, folder_uri);
		e_composer_header_table_set_post_to_list (table, list);
		g_list_free (list);

		g_free (folder_uri);
	}

	e_composer_header_table_set_account (table, account);
	e_composer_header_table_set_subject (table, subject);

	return composer;
}

/**
 * em_utils_compose_new_message:
 * @shell: an #EShell
 * @folder: a #CamelFolder, or %NULL
 *
 * Opens a new composer window as a child window of @parent's toplevel
 * window.
 **/
void
em_utils_compose_new_message (EShell *shell,
                              CamelFolder *folder)
{
	EMsgComposer *composer;

	g_return_if_fail (E_IS_SHELL (shell));

	if (folder != NULL)
		g_return_if_fail (CAMEL_IS_FOLDER (folder));

	composer = create_new_composer (shell, "", folder);
	composer_set_no_change (composer);

	gtk_widget_show (GTK_WIDGET (composer));
}

/**
 * em_utils_compose_new_message_with_mailto:
 * @shell: an #EShell
 * @mailto: a mailto URL
 * @folder: a #CamelFolder, or %NULL
 *
 * Opens a new composer window as a child window of @parent's toplevel
 * window. If @mailto is non-NULL, the composer fields will be filled in
 * according to the values in the mailto URL.
 **/
EMsgComposer *
em_utils_compose_new_message_with_mailto (EShell *shell,
                                          const gchar *mailto,
                                          CamelFolder *folder)
{
	EMsgComposer *composer;
	EComposerHeaderTable *table;
	CamelService *service = NULL;

	g_return_val_if_fail (E_IS_SHELL (shell), NULL);

	if (folder != NULL)
		g_return_val_if_fail (CAMEL_IS_FOLDER (folder), NULL);

	if (mailto != NULL)
		composer = e_msg_composer_new_from_url (shell, mailto);
	else
		composer = e_msg_composer_new (shell);

	table = e_msg_composer_get_header_table (composer);

	if (folder != NULL) {
		CamelStore *store;

		store = camel_folder_get_parent_store (folder);
		service = CAMEL_SERVICE (store);
	}

	if (service != NULL) {
		const gchar *display_name;

		display_name = camel_service_get_display_name (service);
		e_composer_header_table_set_account_name (table, display_name);
	}

	composer_set_no_change (composer);

	gtk_window_present (GTK_WINDOW (composer));

	return composer;
}

static gboolean
replace_variables (GSList *clues,
                   CamelMimeMessage *message,
                   gchar **pstr)
{
	gint i;
	gboolean string_changed = FALSE, count1 = FALSE;
	gchar *str;

	g_return_val_if_fail (pstr != NULL, FALSE);
	g_return_val_if_fail (*pstr != NULL, FALSE);
	g_return_val_if_fail (message != NULL, FALSE);

	str = *pstr;

	for (i = 0; i < strlen (str); i++) {
		const gchar *cur = str + i;
		if (!g_ascii_strncasecmp (cur, "$", 1)) {
			const gchar *end = cur + 1;
			gchar *out;
			gchar **temp_str;
			GSList *list;

			while (*end && (g_unichar_isalnum (*end) || *end == '_'))
				end++;

			out = g_strndup ((const gchar *) cur, end - cur);

			temp_str = g_strsplit (str, out, 2);

			for (list = clues; list; list = g_slist_next (list)) {
				gchar **temp = g_strsplit (list->data, "=", 2);
				if (!g_ascii_strcasecmp (temp[0], out + 1)) {
					g_free (str);
					str = g_strconcat (temp_str[0], temp[1], temp_str[1], NULL);
					count1 = TRUE;
					string_changed = TRUE;
				} else
					count1 = FALSE;
				g_strfreev (temp);
			}

			if (!count1) {
				if (getenv (out + 1)) {
					g_free (str);
					str = g_strconcat (
						temp_str[0],
						getenv (out + 1),
						temp_str[1], NULL);
					count1 = TRUE;
					string_changed = TRUE;
				} else
					count1 = FALSE;
			}

			if (!count1) {
				CamelInternetAddress *to;
				const gchar *name, *addr;

				to = camel_mime_message_get_recipients (
					message, CAMEL_RECIPIENT_TYPE_TO);
				if (!camel_internet_address_get (to, 0, &name, &addr))
					continue;

				if (name && g_ascii_strcasecmp ("sender_name", out + 1) == 0) {
					g_free (str);
					str = g_strconcat (temp_str[0], name, temp_str[1], NULL);
					count1 = TRUE;
					string_changed = TRUE;
				} else if (addr && g_ascii_strcasecmp ("sender_email", out + 1) == 0) {
					g_free (str);
					str = g_strconcat (temp_str[0], addr, temp_str[1], NULL);
					count1 = TRUE;
					string_changed = TRUE;
				}
			}

			g_strfreev (temp_str);
			g_free (out);
		}
	}

	*pstr = str;

	return string_changed;
}

static void
traverse_parts (GSList *clues,
                CamelMimeMessage *message,
                CamelDataWrapper *content)
{
	g_return_if_fail (message != NULL);

	if (!content)
		return;

	if (CAMEL_IS_MULTIPART (content)) {
		guint i, n;
		CamelMultipart *multipart = CAMEL_MULTIPART (content);
		CamelMimePart *part;

		n = camel_multipart_get_number (multipart);
		for (i = 0; i < n; i++) {
			part = camel_multipart_get_part (multipart, i);
			if (!part)
				continue;

			traverse_parts (clues, message, CAMEL_DATA_WRAPPER (part));
		}
	} else if (CAMEL_IS_MIME_PART (content)) {
		CamelMimePart *part = CAMEL_MIME_PART (content);
		CamelContentType *type;
		CamelStream *stream;
		GByteArray *byte_array;
		gchar *str;

		content = camel_medium_get_content (CAMEL_MEDIUM (part));
		if (!content)
			return;

		if (CAMEL_IS_MULTIPART (content)) {
			traverse_parts (clues, message, CAMEL_DATA_WRAPPER (content));
			return;
		}

		type = camel_mime_part_get_content_type (part);
		if (!camel_content_type_is (type, "text", "*"))
			return;

		byte_array = g_byte_array_new ();
		stream = camel_stream_mem_new_with_byte_array (byte_array);
		camel_data_wrapper_decode_to_stream_sync (
			content, stream, NULL, NULL);

		str = g_strndup ((gchar *) byte_array->data, byte_array->len);
		g_object_unref (stream);

		if (replace_variables (clues, message, &str)) {
			stream = camel_stream_mem_new_with_buffer (str, strlen (str));
			camel_data_wrapper_construct_from_stream_sync (
				content, stream, NULL, NULL);
			g_object_unref (stream);
		}

		g_free (str);
	}
}

/* Editing messages... */

typedef enum {
	QUOTING_ATTRIBUTION,
	QUOTING_FORWARD,
	QUOTING_ORIGINAL
} QuotingTextEnum;

static struct {
	const gchar * gconf_key;
	const gchar * message;
} conf_messages[] = {
	[QUOTING_ATTRIBUTION] =
		{ "/apps/evolution/mail/composer/message_attribution",
		/* Note to translators: this is the attribution string used
		 * when quoting messages.  Each ${Variable} gets replaced
		 * with a value.  To see a full list of available variables,
		 * see mail/em-composer-utils.c:attribvars array. */
		  N_("On ${AbbrevWeekdayName}, ${Year}-${Month}-${Day} at "
		     "${24Hour}:${Minute} ${TimeZone}, ${Sender} wrote:") 
		},

	[QUOTING_FORWARD] =
		{ "/apps/evolution/mail/composer/message_forward",
		  N_("-------- Forwarded Message --------")
		},

	[QUOTING_ORIGINAL] =
		{ "/apps/evolution/mail/composer/message_original",
		  N_("-----Original Message-----")
		}
};

static gchar *
quoting_text (QuotingTextEnum type)
{
	GConfClient *client;
	gchar *text;

	client = gconf_client_get_default ();
	text = gconf_client_get_string (client, conf_messages[type].gconf_key, NULL);
	g_object_unref (client);

	if (text && *text)
		return text;

	g_free (text);

	return g_strdup (_(conf_messages[type].message));
}

/**
 * em_utils_edit_message:
 * @shell: an #EShell
 * @folder: a #CamelFolder
 * @message: a #CamelMimeMessage
 * @message_uid: UID of @message, or %NULL
 *
 * Opens a composer filled in with the headers/mime-parts/etc of
 * @message.
 **/
GtkWidget *
em_utils_edit_message (EShell *shell,
                       CamelFolder *folder,
                       CamelMimeMessage *message,
                       const gchar *message_uid)
{
	EMsgComposer *composer;
	gboolean folder_is_drafts;
	gboolean folder_is_outbox;
	gboolean folder_is_templates;

	g_return_val_if_fail (E_IS_SHELL (shell), NULL);
	g_return_val_if_fail (CAMEL_IS_FOLDER (folder), NULL);
	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	folder_is_drafts = em_utils_folder_is_drafts (folder);
	folder_is_outbox = em_utils_folder_is_outbox (folder);
	folder_is_templates = em_utils_folder_is_templates (folder);

	/* Template specific code follows. */
	if (folder_is_templates) {
		CamelDataWrapper *content;
		GConfClient *gconf;
		GSList *clue_list = NULL;

		gconf = gconf_client_get_default ();
		/* Get the list from gconf */
		clue_list = gconf_client_get_list (
			gconf, GCONF_KEY_TEMPLATE_PLACEHOLDERS,
			GCONF_VALUE_STRING, NULL );
		g_object_unref (gconf);

		content = camel_medium_get_content (CAMEL_MEDIUM (message));
		traverse_parts (clue_list, message, content);

		g_slist_foreach (clue_list, (GFunc) g_free, NULL);
		g_slist_free (clue_list);
	}

	composer = e_msg_composer_new_with_message (shell, message, NULL);

	e_msg_composer_remove_header (
		composer, "X-Evolution-Replace-Outbox-UID");

	if (message_uid != NULL && folder_is_drafts) {
		gchar *folder_uri;

		folder_uri = e_mail_folder_uri_from_folder (folder);

		e_msg_composer_set_draft_headers (
			composer, folder_uri, message_uid);

		g_free (folder_uri);

	} else if (message_uid != NULL && folder_is_outbox) {
		e_msg_composer_set_header (
			composer, "X-Evolution-Replace-Outbox-UID",
			message_uid);
	}

	composer_set_no_change (composer);

	gtk_widget_show (GTK_WIDGET (composer));

	return GTK_WIDGET (composer);
}

static void
edit_messages_cb (CamelFolder *folder,
                  GAsyncResult *result,
                  AsyncContext *context)
{
	EShell *shell;
	EMailBackend *backend;
	EAlertSink *alert_sink;
	GHashTable *hash_table;
	GHashTableIter iter;
	gpointer key, value;
	GError *error = NULL;

	alert_sink = e_mail_reader_get_alert_sink (context->reader);

	hash_table = e_mail_folder_get_multiple_messages_finish (
		folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (hash_table == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (hash_table == NULL);
		e_alert_submit (
			alert_sink,
			"mail:get-multiple-messages",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (hash_table != NULL);

	backend = e_mail_reader_get_backend (context->reader);
	shell = e_shell_backend_get_shell (E_SHELL_BACKEND (backend));

	/* Open each message in its own composer window. */

	g_hash_table_iter_init (&iter, hash_table);

	while (g_hash_table_iter_next (&iter, &key, &value)) {
		CamelMimeMessage *message;

		if (!context->replace)
			key = NULL;

		message = CAMEL_MIME_MESSAGE (value);
		camel_medium_remove_header (CAMEL_MEDIUM (value), "X-Mailer");
		em_utils_edit_message (shell, folder, message, key);
	}

	g_hash_table_unref (hash_table);

	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);

	async_context_free (context);
}

/**
 * em_utils_edit_messages:
 * @reader: an #EMailReader
 * @folder: folder containing messages to edit
 * @uids: uids of messages to edit
 * @replace: replace the existing message(s) when sent or saved.
 *
 * Opens a composer for each message to be edited.
 **/
void
em_utils_edit_messages (EMailReader *reader,
                        CamelFolder *folder,
                        GPtrArray *uids,
                        gboolean replace)
{
	EActivity *activity;
	AsyncContext *context;
	GCancellable *cancellable;

	g_return_if_fail (E_IS_MAIL_READER (reader));
	g_return_if_fail (CAMEL_IS_FOLDER (folder));
	g_return_if_fail (uids != NULL);

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->reader = g_object_ref (reader);
	context->ptr_array = g_ptr_array_ref (uids);
	context->replace = replace;

	e_mail_folder_get_multiple_messages (
		folder, uids, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		edit_messages_cb, context);
}

static void
emu_update_composers_security (EMsgComposer *composer,
                               guint32 validity_found)
{
	GtkToggleAction *action;

	g_return_if_fail (composer != NULL);

	/* Pre-set only for encrypted messages, not for signed */
	/*if (validity_found & EM_FORMAT_VALIDITY_FOUND_SIGNED) {
		if (validity_found & EM_FORMAT_VALIDITY_FOUND_SMIME)
			action = GTK_TOGGLE_ACTION (E_COMPOSER_ACTION_SMIME_SIGN (composer));
		else
			action = GTK_TOGGLE_ACTION (E_COMPOSER_ACTION_PGP_SIGN (composer));
		gtk_toggle_action_set_active (action, TRUE);
	}*/

	if (validity_found & EM_FORMAT_VALIDITY_FOUND_ENCRYPTED) {
		if (validity_found & EM_FORMAT_VALIDITY_FOUND_SMIME)
			action = GTK_TOGGLE_ACTION (E_COMPOSER_ACTION_SMIME_ENCRYPT (composer));
		else
			action = GTK_TOGGLE_ACTION (E_COMPOSER_ACTION_PGP_ENCRYPT (composer));
		gtk_toggle_action_set_active (action, TRUE);
	}
}

static void
real_update_forwarded_flag (gpointer uid,
                            gpointer folder)
{
	if (uid && folder)
		camel_folder_set_message_flags (
			folder, uid, CAMEL_MESSAGE_FORWARDED,
			CAMEL_MESSAGE_FORWARDED);
}

static void
update_forwarded_flags_cb (EMsgComposer *composer,
                           ForwardData *data)
{
	if (data && data->uids && data->folder)
		g_ptr_array_foreach (
			data->uids, real_update_forwarded_flag, data->folder);
}

static void
setup_forward_attached_callbacks (EMsgComposer *composer,
                                  CamelFolder *folder,
                                  GPtrArray *uids)
{
	ForwardData *data;

	if (!composer || !folder || !uids || !uids->len)
		return;

	g_object_ref (folder);

	data = g_slice_new0 (ForwardData);
	data->folder = g_object_ref (folder);
	data->uids = em_utils_uids_copy (uids);

	g_signal_connect (
		composer, "send",
		G_CALLBACK (update_forwarded_flags_cb), data);
	g_signal_connect (
		composer, "save-to-drafts",
		G_CALLBACK (update_forwarded_flags_cb), data);

	g_object_set_data_full (
		G_OBJECT (composer), "forward-data", data,
		(GDestroyNotify) forward_data_free);
}

static EMsgComposer *
forward_attached (EShell *shell,
                  CamelFolder *folder,
                  GPtrArray *uids,
                  CamelMimePart *part,
                  gchar *subject)
{
	EMsgComposer *composer;

	composer = create_new_composer (shell, subject, folder);

	e_msg_composer_attach (composer, part);

	if (uids)
		setup_forward_attached_callbacks (composer, folder, uids);

	composer_set_no_change (composer);

	gtk_widget_show (GTK_WIDGET (composer));

	return composer;
}

static void
forward_attached_cb (CamelFolder *folder,
                     GAsyncResult *result,
                     AsyncContext *context)
{
	EShell *shell;
	EMailBackend *backend;
	EAlertSink *alert_sink;
	CamelMimePart *part;
	gchar *subject = NULL;
	GError *error = NULL;

	alert_sink = e_mail_reader_get_alert_sink (context->reader);

	part = e_mail_folder_build_attachment_finish (
		folder, result, &subject, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (part == NULL);
		g_warn_if_fail (subject == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (part == NULL);
		g_warn_if_fail (subject == NULL);
		e_alert_submit (
			alert_sink,
			"mail:get-multiple-messages",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_PART (part));

	backend = e_mail_reader_get_backend (context->reader);
	shell = e_shell_backend_get_shell (E_SHELL_BACKEND (backend));

	forward_attached (shell, folder, context->ptr_array, part, subject);

	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);

	g_object_unref (part);
	g_free (subject);

	async_context_free (context);
}

static EMsgComposer *
forward_non_attached (EShell *shell,
                      CamelFolder *folder,
                      const gchar *uid,
                      CamelMimeMessage *message,
                      EMailForwardStyle style)
{
	EMsgComposer *composer = NULL;
	gchar *text, *forward;
	guint32 validity_found = 0;
	guint32 flags;

	flags = EM_FORMAT_QUOTE_HEADERS | EM_FORMAT_QUOTE_KEEP_SIG;
	if (style == E_MAIL_FORWARD_STYLE_QUOTED)
		flags |= EM_FORMAT_QUOTE_CITE;

	forward = quoting_text (QUOTING_FORWARD);
	text = em_utils_message_to_html (
		message, forward, flags, NULL, NULL, &validity_found);

	if (text != NULL) {
		CamelDataWrapper *content;
		gchar *subject;

		subject = mail_tool_generate_forward_subject (message);
		composer = create_new_composer (shell, subject, folder);
		g_free (subject);

		content = camel_medium_get_content (CAMEL_MEDIUM (message));

		if (CAMEL_IS_MULTIPART (content))
			e_msg_composer_add_message_attachments (
				composer, message, FALSE);

		e_msg_composer_set_body_text (composer, text, TRUE);

		if (uid != NULL) {
			gchar *folder_uri;

			folder_uri = e_mail_folder_uri_from_folder (folder);

			e_msg_composer_set_source_headers (
				composer, folder_uri, uid,
				CAMEL_MESSAGE_FORWARDED);

			g_free (folder_uri);
		}

		emu_update_composers_security (
			composer, validity_found);
		composer_set_no_change (composer);
		gtk_widget_show (GTK_WIDGET (composer));

		g_free (text);
	}

	g_free (forward);

	return composer;
}

/**
 * em_utils_forward_message:
 * @shell: an #EShell
 * @message: a #CamelMimeMessage to forward
 * @style: the forward style to use
 * @folder: a #CamelFolder, or %NULL
 * @uid: the UID of %message, or %NULL
 *
 * Forwards a message in the given style.  See em_utils_forward_messages()
 * for more details about forwarding styles.
 **/
EMsgComposer *
em_utils_forward_message (EShell *shell,
                          CamelMimeMessage *message,
                          EMailForwardStyle style,
                          CamelFolder *folder,
                          const gchar *uid)
{
	CamelMimePart *part;
	gchar *subject;
	EMsgComposer *composer = NULL;

	g_return_val_if_fail (E_IS_SHELL (shell), NULL);
	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	switch (style) {
		case E_MAIL_FORWARD_STYLE_ATTACHED:
		default:
			part = mail_tool_make_message_attachment (message);
			subject = mail_tool_generate_forward_subject (message);

			composer = forward_attached (
				shell, NULL, NULL, part, subject);

			g_object_unref (part);
			g_free (subject);
			break;

		case E_MAIL_FORWARD_STYLE_INLINE:
		case E_MAIL_FORWARD_STYLE_QUOTED:
			composer = forward_non_attached (
				shell, folder, uid, message, style);
			break;
	}

	return composer;
}

static void
forward_got_messages_cb (CamelFolder *folder,
                         GAsyncResult *result,
                         AsyncContext *context)
{
	EShell *shell;
	EMailBackend *backend;
	EAlertSink *alert_sink;
	GHashTable *hash_table;
	GHashTableIter iter;
	gpointer key, value;
	GError *error = NULL;

	alert_sink = e_mail_reader_get_alert_sink (context->reader);

	hash_table = e_mail_folder_get_multiple_messages_finish (
		folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (hash_table == NULL);
		context->destroy_when_done = NULL;
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (hash_table == NULL);
		e_alert_submit (
			alert_sink,
			"mail:get-multiple-messages",
			error->message, NULL);
		context->destroy_when_done = NULL;
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (hash_table != NULL);

	backend = e_mail_reader_get_backend (context->reader);
	shell = e_shell_backend_get_shell (E_SHELL_BACKEND (backend));

	/* Create a new composer window for each message. */

	g_hash_table_iter_init (&iter, hash_table);

	while (g_hash_table_iter_next (&iter, &key, &value))
		em_utils_forward_message (
			shell, value, context->style, folder, key);

	g_hash_table_unref (hash_table);

	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);

	async_context_free (context);
}

/**
 * em_utils_forward_messages:
 * @shell: an #EShell
 * @folder: folder containing messages to forward
 * @uids: uids of messages to forward
 * @style: the forward style to use
 * @destroy_when_done: a #GtkWidget to destroy with gtk_widget_destroy() when done; can be NULL
 *
 * Forwards a group of messages in the given style.
 *
 * If @style is #E_MAIL_FORWARD_STYLE_ATTACHED, the new message is
 * created as follows.  If there is more than a single message in @uids,
 * a multipart/digest will be constructed and attached to a new composer
 * window preset with the appropriate header defaults for forwarding the
 * first message in the list.  If only one message is to be forwarded,
 * it is forwarded as a simple message/rfc822 attachment.
 *
 * If @style is #E_MAIL_FORWARD_STYLE_INLINE, each message is forwarded
 * in its own composer window in 'inline' form.
 *
 * If @style is #E_MAIL_FORWARD_STYLE_QUOTED, each message is forwarded
 * in its own composer window in 'quoted' form (each line starting with
 * a "> ").
 **/
void
em_utils_forward_messages (EMailReader *reader,
                           CamelFolder *folder,
                           GPtrArray *uids,
                           EMailForwardStyle style,
                           GtkWidget *destroy_when_done)
{
	EActivity *activity;
	AsyncContext *context;
	GCancellable *cancellable;

	g_return_if_fail (E_IS_MAIL_READER (reader));
	g_return_if_fail (CAMEL_IS_FOLDER (folder));
	g_return_if_fail (uids != NULL);

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->reader = g_object_ref (reader);
	context->ptr_array = g_ptr_array_ref (uids);
	context->style = style;
	context->destroy_when_done = destroy_when_done;

	switch (style) {
		case E_MAIL_FORWARD_STYLE_ATTACHED:
			e_mail_folder_build_attachment (
				folder, uids, G_PRIORITY_DEFAULT,
				cancellable, (GAsyncReadyCallback)
				forward_attached_cb, context);
			break;

		case E_MAIL_FORWARD_STYLE_INLINE:
		case E_MAIL_FORWARD_STYLE_QUOTED:
			e_mail_folder_get_multiple_messages (
				folder, uids, G_PRIORITY_DEFAULT,
				cancellable, (GAsyncReadyCallback)
				forward_got_messages_cb, context);
			break;

		default:
			g_warn_if_reached ();
	}
}

/* Redirecting messages... */

static EMsgComposer *
redirect_get_composer (EShell *shell,
                       CamelMimeMessage *message)
{
	CamelMedium *medium;
	EAccount *account;

	medium = CAMEL_MEDIUM (message);

	/* QMail will refuse to send a message if it finds one of
	 * it's Delivered-To headers in the message, so remove all
	 * Delivered-To headers. Fixes bug #23635. */
	while (camel_medium_get_header (medium, "Delivered-To"))
		camel_medium_remove_header (medium, "Delivered-To");

	while (camel_medium_get_header (medium, "Bcc"))
		camel_medium_remove_header (medium, "Bcc");

	while (camel_medium_get_header (medium, "Resent-Bcc"))
		camel_medium_remove_header (medium, "Resent-Bcc");

	account = em_utils_guess_account_with_recipients (message, NULL);

	return e_msg_composer_new_redirect (
		shell, message, account ? account->name : NULL, NULL);
}

/**
 * em_utils_redirect_message:
 * @shell: an #EShell
 * @message: message to redirect
 *
 * Opens a composer to redirect @message (Note: only headers will be
 * editable). Adds Resent-From/Resent-To/etc headers.
 **/
void
em_utils_redirect_message (EShell *shell,
                           CamelMimeMessage *message)
{
	EMsgComposer *composer;

	g_return_if_fail (E_IS_SHELL (shell));
	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	composer = redirect_get_composer (shell, message);

	gtk_widget_show (GTK_WIDGET (composer));

	composer_set_no_change (composer);
}

/* Message disposition notifications, rfc 2298 */
void
em_utils_handle_receipt (EMailBackend *backend,
                         CamelFolder *folder,
                         const gchar *message_uid,
                         CamelMimeMessage *message)
{
	EAccount *account;
	const gchar *addr;
	CamelMessageInfo *info;

	g_return_if_fail (E_IS_MAIL_BACKEND (backend));
	g_return_if_fail (CAMEL_IS_FOLDER (folder));
	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	info = camel_folder_get_message_info (folder, message_uid);
	if (info == NULL)
		return;

	if (camel_message_info_user_flag (info, "receipt-handled")) {
		camel_folder_free_message_info (folder, info);
		return;
	}

	addr = camel_medium_get_header (
		CAMEL_MEDIUM (message), "Disposition-Notification-To");
	if (addr == NULL) {
		camel_folder_free_message_info (folder, info);
		return;
	}

	camel_message_info_set_user_flag (info, "receipt-handled", TRUE);
	camel_folder_free_message_info (folder, info);

	account = em_utils_guess_account_with_recipients (message, folder);

	/* TODO Should probably decode/format the address,
	 *      since it could be in rfc2047 format. */
	if (addr == NULL) {
		addr = "";
	} else {
		while (camel_mime_is_lwsp (*addr))
			addr++;
	}

	if (account == NULL)
		return;

	if (account->receipt_policy == E_ACCOUNT_RECEIPT_NEVER)
		return;

	if (account->receipt_policy == E_ACCOUNT_RECEIPT_ASK) {
		GtkWindow *window;
		const gchar *subject;
		gint response;

		/* FIXME Parent window should be passed in. */
		window = e_shell_get_active_window (NULL);
		subject = camel_mime_message_get_subject (message);

		response = e_alert_run_dialog_for_args (
			window, "mail:ask-receipt", addr, subject, NULL);

		if (response != GTK_RESPONSE_YES)
			return;
	}

	em_utils_send_receipt (backend, folder, message);
}

static void
em_utils_receipt_done (CamelFolder *folder,
                       GAsyncResult *result,
                       EMailBackend *backend)
{
	/* FIXME Poor error handling. */
	if (!e_mail_folder_append_message_finish (folder, result, NULL, NULL))
		return;

	mail_send (backend);
}

void
em_utils_send_receipt (EMailBackend *backend,
                       CamelFolder *folder,
                       CamelMimeMessage *message)
{
	/* See RFC #3798 for a description of message receipts */
	EAccount *account = em_utils_guess_account_with_recipients (message, folder);
	CamelMimeMessage *receipt = camel_mime_message_new ();
	CamelMultipart *body = camel_multipart_new ();
	CamelMimePart *part;
	CamelDataWrapper *receipt_text, *receipt_data;
	CamelContentType *type;
	CamelInternetAddress *addr;
	CamelStream *stream;
	CamelFolder *out_folder;
	CamelMessageInfo *info;
	const gchar *message_id;
	const gchar *message_date;
	const gchar *message_subject;
	const gchar *receipt_address;
	gchar *fake_msgid;
	gchar *hostname;
	gchar *self_address, *receipt_subject;
	gchar *ua, *recipient;
	gchar *transport_uid;
	gchar *content;

	message_id = camel_medium_get_header (
		CAMEL_MEDIUM (message), "Message-ID");
	message_date = camel_medium_get_header (
		CAMEL_MEDIUM (message), "Date");
	message_subject = camel_mime_message_get_subject (message);
	receipt_address = camel_medium_get_header (
		CAMEL_MEDIUM (message), "Disposition-Notification-To");

	if (!receipt_address)
		return;

	/* the 'account' should be always set */
	g_return_if_fail (account != NULL);

	/* Collect information for the receipt */

	/* We use camel_header_msgid_generate () to get a canonical
	 * hostname, then skip the part leading to '@' */
	hostname = strchr ((fake_msgid = camel_header_msgid_generate ()), '@');
	hostname++;

	self_address = account->id->address;

	if (!message_id)
		message_id = "";
	if (!message_date)
		message_date ="";

	/* Create toplevel container */
	camel_data_wrapper_set_mime_type (
		CAMEL_DATA_WRAPPER (body),
		"multipart/report;"
		"report-type=\"disposition-notification\"");
	camel_multipart_set_boundary (body, NULL);

	/* Create textual receipt */
	receipt_text = camel_data_wrapper_new ();
	type = camel_content_type_new ("text", "plain");
	camel_content_type_set_param (type, "format", "flowed");
	camel_content_type_set_param (type, "charset", "UTF-8");
	camel_data_wrapper_set_mime_type_field (receipt_text, type);
	camel_content_type_unref (type);
	content = g_strdup_printf (
		/* Translators: First %s is an email address, second %s
		 * is the subject of the email, third %s is the date. */
		_("Your message to %s about \"%s\" on %s has been read."),
		self_address, message_subject, message_date);
	stream = camel_stream_mem_new ();
	camel_stream_write_string (stream, content, NULL, NULL);
	camel_data_wrapper_construct_from_stream_sync (
		receipt_text, stream, NULL, NULL);
	g_object_unref (stream);
	g_free (content);

	part = camel_mime_part_new ();
	camel_medium_set_content (CAMEL_MEDIUM (part), receipt_text);
	camel_mime_part_set_encoding (
		part, CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE);
	g_object_unref (receipt_text);
	camel_multipart_add_part (body, part);
	g_object_unref (part);

	/* Create the machine-readable receipt */
	receipt_data = camel_data_wrapper_new ();
	part = camel_mime_part_new ();

	ua = g_strdup_printf (
		"%s; %s", hostname, "Evolution "
		VERSION SUB_VERSION " " VERSION_COMMENT);
	recipient = g_strdup_printf ("rfc822; %s", self_address);

	type = camel_content_type_new ("message", "disposition-notification");
	camel_data_wrapper_set_mime_type_field (receipt_data, type);
	camel_content_type_unref (type);

	content = g_strdup_printf (
		"Reporting-UA: %s\n"
		"Final-Recipient: %s\n"
		"Original-Message-ID: %s\n"
		"Disposition: manual-action/MDN-sent-manually; displayed\n",
		ua, recipient, message_id);
	stream = camel_stream_mem_new ();
	camel_stream_write_string (stream, content, NULL, NULL);
	camel_data_wrapper_construct_from_stream_sync (
		receipt_data, stream, NULL, NULL);
	g_object_unref (stream);
	g_free (content);

	g_free (ua);
	g_free (recipient);
	g_free (fake_msgid);

	camel_medium_set_content (CAMEL_MEDIUM (part), receipt_data);
	camel_mime_part_set_encoding (part, CAMEL_TRANSFER_ENCODING_7BIT);
	g_object_unref (receipt_data);
	camel_multipart_add_part (body, part);
	g_object_unref (part);

	/* Finish creating the message */
	camel_medium_set_content (
		CAMEL_MEDIUM (receipt), CAMEL_DATA_WRAPPER (body));
	g_object_unref (body);

	/* Translators: %s is the subject of the email message */
	receipt_subject = g_strdup_printf (
		_("Delivery Notification for: \"%s\""), message_subject);
	camel_mime_message_set_subject (receipt, receipt_subject);
	g_free (receipt_subject);

	addr = camel_internet_address_new ();
	camel_address_decode (CAMEL_ADDRESS (addr), self_address);
	camel_mime_message_set_from (receipt, addr);
	g_object_unref (addr);

	addr = camel_internet_address_new ();
	camel_address_decode (CAMEL_ADDRESS (addr), receipt_address);
	camel_mime_message_set_recipients (
		receipt, CAMEL_RECIPIENT_TYPE_TO, addr);
	g_object_unref (addr);

	transport_uid = g_strconcat (account->uid, "-transport", NULL);

	camel_medium_set_header (
		CAMEL_MEDIUM (receipt),
		"Return-Path", "<>");
	camel_medium_set_header (
		CAMEL_MEDIUM (receipt),
		"X-Evolution-Account",
		account->uid);
	camel_medium_set_header (
		CAMEL_MEDIUM (receipt),
		"X-Evolution-Transport",
		transport_uid);
	camel_medium_set_header (
		CAMEL_MEDIUM (receipt),
		"X-Evolution-Fcc",
		account->sent_folder_uri);

	g_free (transport_uid);

	/* Send the receipt */
	info = camel_message_info_new (NULL);
	out_folder = e_mail_local_get_folder (E_MAIL_LOCAL_FOLDER_OUTBOX);
	camel_message_info_set_flags (
		info, CAMEL_MESSAGE_SEEN, CAMEL_MESSAGE_SEEN);

	/* FIXME Pass a GCancellable. */
	e_mail_folder_append_message (
		out_folder, receipt, info, G_PRIORITY_DEFAULT, NULL,
		(GAsyncReadyCallback) em_utils_receipt_done, backend);

	camel_message_info_free (info);
}

/* Replying to messages... */

EDestination **
em_utils_camel_address_to_destination (CamelInternetAddress *iaddr)
{
	EDestination *dest, **destv;
	gint n, i, j;

	if (iaddr == NULL)
		return NULL;

	if ((n = camel_address_length ((CamelAddress *) iaddr)) == 0)
		return NULL;

	destv = g_malloc (sizeof (EDestination *) * (n + 1));
	for (i = 0, j = 0; i < n; i++) {
		const gchar *name, *addr;

		if (camel_internet_address_get (iaddr, i, &name, &addr)) {
			dest = e_destination_new ();
			e_destination_set_name (dest, name);
			e_destination_set_email (dest, addr);

			destv[j++] = dest;
		}
	}

	if (j == 0) {
		g_free (destv);
		return NULL;
	}

	destv[j] = NULL;

	return destv;
}

static EMsgComposer *
reply_get_composer (EShell *shell,
                    CamelMimeMessage *message,
                    EAccount *account,
                    CamelInternetAddress *to,
                    CamelInternetAddress *cc,
                    CamelFolder *folder,
                    CamelNNTPAddress *postto)
{
	const gchar *message_id, *references;
	EDestination **tov, **ccv;
	EMsgComposer *composer;
	EComposerHeaderTable *table;
	CamelMedium *medium;
	gchar *subject;

	g_return_val_if_fail (E_IS_SHELL (shell), NULL);
	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	if (to != NULL)
		g_return_val_if_fail (CAMEL_IS_INTERNET_ADDRESS (to), NULL);

	if (cc != NULL)
		g_return_val_if_fail (CAMEL_IS_INTERNET_ADDRESS (cc), NULL);

	composer = e_msg_composer_new (shell);

	/* construct the tov/ccv */
	tov = em_utils_camel_address_to_destination (to);
	ccv = em_utils_camel_address_to_destination (cc);

	/* Set the subject of the new message. */
	if ((subject = (gchar *) camel_mime_message_get_subject (message))) {
		if (g_ascii_strncasecmp (subject, "Re: ", 4) != 0 &&
			g_ascii_strncasecmp (subject, "Re : ", 5) != 0)
			subject = g_strdup_printf ("Re: %s", subject);
		else if (g_ascii_strncasecmp (subject, "Re : ", 5) == 0)
			subject = g_strdup_printf ("Re: %s", subject + 5);
		else
			subject = g_strdup (subject);
	} else {
		subject = g_strdup ("");
	}

	table = e_msg_composer_get_header_table (composer);
	e_composer_header_table_set_account (table, account);
	e_composer_header_table_set_subject (table, subject);
	e_composer_header_table_set_destinations_to (table, tov);

	/* Add destinations instead of setting, so we don't remove
	 * automatic CC addresses that have already been added. */
	e_composer_header_table_add_destinations_cc (table, ccv);

	e_destination_freev (tov);
	e_destination_freev (ccv);
	g_free (subject);

	/* add post-to, if nessecary */
	if (postto && camel_address_length ((CamelAddress *) postto)) {
		gchar *store_url = NULL;
		gchar *post;

		if (folder) {
			CamelStore *parent_store;
			CamelService *service;
			CamelURL *url;

			parent_store = camel_folder_get_parent_store (folder);

			service = CAMEL_SERVICE (parent_store);
			url = camel_service_get_camel_url (service);

			store_url = camel_url_to_string (
				url, CAMEL_URL_HIDE_ALL);
			if (store_url[strlen (store_url) - 1] == '/')
				store_url[strlen (store_url) - 1] = '\0';
		}

		post = camel_address_encode ((CamelAddress *) postto);
		e_composer_header_table_set_post_to_base (
			table, store_url ? store_url : "", post);
		g_free (post);
		g_free (store_url);
	}

	/* Add In-Reply-To and References. */

	medium = CAMEL_MEDIUM (message);
	message_id = camel_medium_get_header (medium, "Message-ID");
	references = camel_medium_get_header (medium, "References");

	if (message_id != NULL) {
		gchar *reply_refs;

		e_msg_composer_add_header (
			composer, "In-Reply-To", message_id);

		if (references)
			reply_refs = g_strdup_printf (
				"%s %s", references, message_id);
		else
			reply_refs = g_strdup (message_id);

		e_msg_composer_add_header (
			composer, "References", reply_refs);
		g_free (reply_refs);

	} else if (references != NULL) {
		e_msg_composer_add_header (
			composer, "References", references);
	}

	return composer;
}

static gboolean
get_reply_list (CamelMimeMessage *message,
                CamelInternetAddress *to)
{
	const gchar *header, *p;
	gchar *addr;

	/* Examples:
	 *
	 * List-Post: <mailto:list@host.com>
	 * List-Post: <mailto:moderator@host.com?subject=list%20posting>
	 * List-Post: NO (posting not allowed on this list)
	 */
	if (!(header = camel_medium_get_header ((CamelMedium *) message, "List-Post")))
		return FALSE;

	while (*header == ' ' || *header == '\t')
		header++;

	/* check for NO */
	if (!g_ascii_strncasecmp (header, "NO", 2))
		return FALSE;

	/* Search for the first mailto angle-bracket enclosed URL.
	 * (See rfc2369, Section 2, paragraph 3 for details) */
	if (!(header = camel_strstrcase (header, "<mailto:")))
		return FALSE;

	header += 8;

	p = header;
	while (*p && !strchr ("?>", *p))
		p++;

	addr = g_strndup (header, p - header);
	camel_internet_address_add (to, NULL, addr);
	g_free (addr);

	return TRUE;
}

gboolean
em_utils_is_munged_list_message (CamelMimeMessage *message)
{
	CamelInternetAddress *reply_to, *list;
	gboolean result = FALSE;

	reply_to = camel_mime_message_get_reply_to (message);
	if (reply_to) {
		list = camel_internet_address_new ();

		if (get_reply_list (message, list) &&
		    camel_address_length (CAMEL_ADDRESS (list)) ==
		    camel_address_length (CAMEL_ADDRESS (reply_to))) {
			gint i;
			const gchar *r_name, *r_addr;
			const gchar *l_name, *l_addr;

			for (i = 0; i < camel_address_length (CAMEL_ADDRESS (list)); i++) {
				if (!camel_internet_address_get (reply_to, i, &r_name, &r_addr))
					break;
				if (!camel_internet_address_get (list, i, &l_name, &l_addr))
					break;
				if (strcmp (l_addr, r_addr))
					break;
			}
			if (i == camel_address_length (CAMEL_ADDRESS (list)))
				result = TRUE;
		}
		g_object_unref (list);
	}
	return result;
}

static CamelInternetAddress *
get_reply_to (CamelMimeMessage *message)
{
	CamelInternetAddress *reply_to;

	reply_to = camel_mime_message_get_reply_to (message);
	if (reply_to) {
		GConfClient *client;
		const gchar *key;
		gboolean ignore_list_reply_to;

		client = gconf_client_get_default ();
		key = "/apps/evolution/mail/composer/ignore_list_reply_to";
		ignore_list_reply_to = gconf_client_get_bool (client, key, NULL);
		g_object_unref (client);

		if (ignore_list_reply_to && em_utils_is_munged_list_message (message))
			reply_to = NULL;
	}
	if (!reply_to)
		reply_to = camel_mime_message_get_from (message);

	return reply_to;
}

static void
get_reply_sender (CamelMimeMessage *message,
                  CamelInternetAddress *to,
                  CamelNNTPAddress *postto)
{
	CamelInternetAddress *reply_to;
	CamelMedium *medium;
	const gchar *posthdr = NULL;

	medium = CAMEL_MEDIUM (message);

	/* check whether there is a 'Newsgroups: ' header in there */
	if (postto != NULL && posthdr == NULL)
		posthdr = camel_medium_get_header (medium, "Followup-To");

	if (postto != NULL && posthdr == NULL)
		posthdr = camel_medium_get_header (medium, "Newsgroups");

	if (postto != NULL && posthdr != NULL) {
		camel_address_decode (CAMEL_ADDRESS (postto), posthdr);
		return;
	}

	reply_to = get_reply_to (message);

	if (reply_to != NULL) {
		const gchar *name;
		const gchar *addr;
		gint ii = 0;

		while (camel_internet_address_get (reply_to, ii++, &name, &addr))
			camel_internet_address_add (to, name, addr);
	}
}

void
em_utils_get_reply_sender (CamelMimeMessage *message,
                           CamelInternetAddress *to,
                           CamelNNTPAddress *postto)
{
	get_reply_sender (message, to, postto);
}

static void
get_reply_from (CamelMimeMessage *message,
                CamelInternetAddress *to,
                CamelNNTPAddress *postto)
{
	CamelInternetAddress *from;
	CamelMedium *medium;
	const gchar *name, *addr;
	const gchar *posthdr = NULL;

	medium = CAMEL_MEDIUM (message);

	/* check whether there is a 'Newsgroups: ' header in there */
	if (postto != NULL && posthdr == NULL)
		posthdr = camel_medium_get_header (medium, "Followup-To");

	if (postto != NULL && posthdr == NULL)
		posthdr = camel_medium_get_header (medium, "Newsgroups");

	if (postto != NULL && posthdr != NULL) {
		camel_address_decode (CAMEL_ADDRESS (postto), posthdr);
		return;
	}

	from = camel_mime_message_get_from (message);

	if (from != NULL) {
		gint ii = 0;

		while (camel_internet_address_get (from, ii++, &name, &addr))
			camel_internet_address_add (to, name, addr);
	}
}

static void
get_reply_recipient (CamelMimeMessage *message,
                     CamelInternetAddress *to,
                     CamelNNTPAddress *postto,
                     CamelInternetAddress *address)
{
	CamelMedium *medium;
	const gchar *posthdr =  NULL;

	medium = CAMEL_MEDIUM (message);

	/* check whether there is a 'Newsgroups: ' header in there */
	if (postto != NULL && posthdr == NULL)
		posthdr = camel_medium_get_header (medium, "Followup-To");

	if (postto != NULL && posthdr == NULL)
		 posthdr = camel_medium_get_header (medium, "Newsgroups");

	if (postto != NULL && posthdr != NULL) {
		camel_address_decode (CAMEL_ADDRESS (postto), posthdr);
		return;
	}

	if (address != NULL) {
		const gchar *name;
		const gchar *addr;
		gint ii = 0;

		while (camel_internet_address_get (address, ii++, &name, &addr))
			camel_internet_address_add (to, name, addr);
	}

}

static void
concat_unique_addrs (CamelInternetAddress *dest,
                     CamelInternetAddress *src,
                     GHashTable *rcpt_hash)
{
	const gchar *name, *addr;
	gint i;

	for (i = 0; camel_internet_address_get (src, i, &name, &addr); i++) {
		if (!g_hash_table_lookup (rcpt_hash, addr)) {
			camel_internet_address_add (dest, name, addr);
			g_hash_table_insert (rcpt_hash, (gchar *) addr, GINT_TO_POINTER (1));
		}
	}
}

void
em_utils_get_reply_all (CamelMimeMessage *message,
                        CamelInternetAddress *to,
                        CamelInternetAddress *cc,
                        CamelNNTPAddress *postto)
{
	CamelInternetAddress *reply_to;
	CamelInternetAddress *to_addrs;
	CamelInternetAddress *cc_addrs;
	CamelMedium *medium;
	const gchar *name, *addr;
	const gchar *posthdr = NULL;
	GHashTable *rcpt_hash;

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));
	g_return_if_fail (CAMEL_IS_INTERNET_ADDRESS (to));
	g_return_if_fail (CAMEL_IS_INTERNET_ADDRESS (cc));

	medium = CAMEL_MEDIUM (message);

	/* check whether there is a 'Newsgroups: ' header in there */
	if (postto != NULL && posthdr == NULL)
		posthdr = camel_medium_get_header (medium, "Followup-To");

	if (postto != NULL && posthdr == NULL)
		posthdr = camel_medium_get_header (medium, "Newsgroups");

	if (postto != NULL && posthdr != NULL)
		camel_address_decode (CAMEL_ADDRESS (postto), posthdr);

	rcpt_hash = em_utils_generate_account_hash ();

	reply_to = get_reply_to (message);
	to_addrs = camel_mime_message_get_recipients (
		message, CAMEL_RECIPIENT_TYPE_TO);
	cc_addrs = camel_mime_message_get_recipients (
		message, CAMEL_RECIPIENT_TYPE_CC);

	if (reply_to != NULL) {
		gint ii = 0;

		while (camel_internet_address_get (reply_to, ii++, &name, &addr)) {
			/* Ignore references to the Reply-To address
			 * in the To and Cc lists. */
			if (addr && !g_hash_table_lookup (rcpt_hash, addr)) {
				/* In the case we are doing a Reply-To-All,
				 * we do not want to include the user's email
				 * address because replying to oneself is
				 * kinda silly. */
				camel_internet_address_add (to, name, addr);
				g_hash_table_insert (
					rcpt_hash, (gchar *) addr,
					GINT_TO_POINTER (1));
			}
		}
	}

	concat_unique_addrs (cc, to_addrs, rcpt_hash);
	concat_unique_addrs (cc, cc_addrs, rcpt_hash);

	/* Promote the first Cc: address to To: if To: is empty. */
	if (camel_address_length ((CamelAddress *) to) == 0 &&
			camel_address_length ((CamelAddress *) cc) > 0) {
		camel_internet_address_get (cc, 0, &name, &addr);
		camel_internet_address_add (to, name, addr);
		camel_address_remove ((CamelAddress *) cc, 0);
	}

	/* If To: is still empty, may we removed duplicates (i.e. ourself),
	 * so add the original To if it was set. */
	if (camel_address_length ((CamelAddress *) to) == 0
	    && (camel_internet_address_get (to_addrs, 0, &name, &addr)
		|| camel_internet_address_get (cc_addrs, 0, &name, &addr))) {
		camel_internet_address_add (to, name, addr);
	}

	g_hash_table_destroy (rcpt_hash);
}

enum {
	ATTRIB_UNKNOWN,
	ATTRIB_CUSTOM,
	ATTRIB_TIMEZONE,
	ATTRIB_STRFTIME,
	ATTRIB_TM_SEC,
	ATTRIB_TM_MIN,
	ATTRIB_TM_24HOUR,
	ATTRIB_TM_12HOUR,
	ATTRIB_TM_MDAY,
	ATTRIB_TM_MON,
	ATTRIB_TM_YEAR,
	ATTRIB_TM_2YEAR,
	ATTRIB_TM_WDAY, /* not actually used */
	ATTRIB_TM_YDAY
};

typedef void		(*AttribFormatter)	(GString *str,
						 const gchar *attr,
						 CamelMimeMessage *message);

static void
format_sender (GString *str,
               const gchar *attr,
               CamelMimeMessage *message)
{
	CamelInternetAddress *sender;
	const gchar *name, *addr = NULL;

	sender = camel_mime_message_get_from (message);
	if (sender != NULL && camel_address_length (CAMEL_ADDRESS (sender)) > 0) {
		camel_internet_address_get (sender, 0, &name, &addr);
	} else {
		name = _("an unknown sender");
	}

	if (name && !strcmp (attr, "{SenderName}")) {
		g_string_append (str, name);
	} else if (addr && !strcmp (attr, "{SenderEMail}")) {
		g_string_append (str, addr);
	} else if (name && *name) {
		g_string_append (str, name);
	} else if (addr) {
		g_string_append (str, addr);
	}
}

static struct {
	const gchar *name;
	gint type;
	struct {
		const gchar *format;         /* strftime or printf format */
		AttribFormatter formatter;  /* custom formatter */
	} v;
} attribvars[] = {
	{ "{Sender}", ATTRIB_CUSTOM, { NULL, format_sender } },
	{ "{SenderName}", ATTRIB_CUSTOM, { NULL, format_sender } },
	{ "{SenderEMail}", ATTRIB_CUSTOM, { NULL, format_sender } },
	{ "{AbbrevWeekdayName}", ATTRIB_STRFTIME, { "%a", NULL } },
	{ "{WeekdayName}", ATTRIB_STRFTIME, { "%A", NULL } },
	{ "{AbbrevMonthName}", ATTRIB_STRFTIME, { "%b", NULL } },
	{ "{MonthName}", ATTRIB_STRFTIME, { "%B", NULL } },
	{ "{AmPmUpper}", ATTRIB_STRFTIME, { "%p", NULL } },
	{ "{AmPmLower}", ATTRIB_STRFTIME, { "%P", NULL } },
	{ "{Day}", ATTRIB_TM_MDAY, { "%02d", NULL } },  /* %d  01-31 */
	{ "{ Day}", ATTRIB_TM_MDAY, { "% 2d", NULL } },  /* %e   1-31 */
	{ "{24Hour}", ATTRIB_TM_24HOUR, { "%02d", NULL } },  /* %H  00-23 */
	{ "{12Hour}", ATTRIB_TM_12HOUR, { "%02d", NULL } },  /* %I  00-12 */
	{ "{DayOfYear}", ATTRIB_TM_YDAY, { "%d", NULL } },  /* %j  1-366 */
	{ "{Month}", ATTRIB_TM_MON, { "%02d", NULL } },  /* %m  01-12 */
	{ "{Minute}", ATTRIB_TM_MIN, { "%02d", NULL } },  /* %M  00-59 */
	{ "{Seconds}", ATTRIB_TM_SEC, { "%02d", NULL } },  /* %S  00-61 */
	{ "{2DigitYear}", ATTRIB_TM_2YEAR, { "%02d", NULL } },  /* %y */
	{ "{Year}", ATTRIB_TM_YEAR, { "%04d", NULL } },  /* %Y */
	{ "{TimeZone}", ATTRIB_TIMEZONE, { "%+05d", NULL } }
};

static gchar *
attribution_format (CamelMimeMessage *message)
{
	register const gchar *inptr;
	const gchar *start;
	gint tzone, len, i;
	gchar buf[64], *s;
	GString *str;
	struct tm tm;
	time_t date;
	gint type;
	gchar *format = quoting_text (QUOTING_ATTRIBUTION);

	str = g_string_new ("");

	date = camel_mime_message_get_date (message, &tzone);

	if (date == CAMEL_MESSAGE_DATE_CURRENT) {
		/* The message has no Date: header, look at Received: */
		date = camel_mime_message_get_date_received (message, &tzone);
	}
	if (date == CAMEL_MESSAGE_DATE_CURRENT) {
		/* That didn't work either, use current time */
		time (&date);
		tzone = 0;
	}

	/* Convert to UTC */
	date += (tzone / 100) * 60 * 60;
	date += (tzone % 100) * 60;

	gmtime_r (&date, &tm);

	inptr = format;
	while (*inptr != '\0') {
		start = inptr;
		while (*inptr && strncmp (inptr, "${", 2) != 0)
			inptr++;

		g_string_append_len (str, start, inptr - start);

		if (*inptr == '\0')
			break;

		start = ++inptr;
		while (*inptr && *inptr != '}')
			inptr++;

		if (*inptr != '}') {
			/* broken translation */
			g_string_append_len (str, "${", 2);
			inptr = start + 1;
			continue;
		}

		inptr++;
		len = inptr - start;
		type = ATTRIB_UNKNOWN;
		for (i = 0; i < G_N_ELEMENTS (attribvars); i++) {
			if (!strncmp (attribvars[i].name, start, len)) {
				type = attribvars[i].type;
				break;
			}
		}

		switch (type) {
		case ATTRIB_CUSTOM:
			attribvars[i].v.formatter (
				str, attribvars[i].name, message);
			break;
		case ATTRIB_TIMEZONE:
			g_string_append_printf (
				str, attribvars[i].v.format, tzone);
			break;
		case ATTRIB_STRFTIME:
			e_utf8_strftime (
				buf, sizeof (buf), attribvars[i].v.format, &tm);
			g_string_append (str, buf);
			break;
		case ATTRIB_TM_SEC:
			g_string_append_printf (
				str, attribvars[i].v.format, tm.tm_sec);
			break;
		case ATTRIB_TM_MIN:
			g_string_append_printf (
				str, attribvars[i].v.format, tm.tm_min);
			break;
		case ATTRIB_TM_24HOUR:
			g_string_append_printf (
				str, attribvars[i].v.format, tm.tm_hour);
			break;
		case ATTRIB_TM_12HOUR:
			g_string_append_printf (
				str, attribvars[i].v.format,
				(tm.tm_hour + 1) % 13);
			break;
		case ATTRIB_TM_MDAY:
			g_string_append_printf (
				str, attribvars[i].v.format, tm.tm_mday);
			break;
		case ATTRIB_TM_MON:
			g_string_append_printf (
				str, attribvars[i].v.format, tm.tm_mon + 1);
			break;
		case ATTRIB_TM_YEAR:
			g_string_append_printf (
				str, attribvars[i].v.format, tm.tm_year + 1900);
			break;
		case ATTRIB_TM_2YEAR:
			g_string_append_printf (
				str, attribvars[i].v.format, tm.tm_year % 100);
			break;
		case ATTRIB_TM_WDAY:
			/* not actually used */
			g_string_append_printf (
				str, attribvars[i].v.format, tm.tm_wday);
			break;
		case ATTRIB_TM_YDAY:
			g_string_append_printf (
				str, attribvars[i].v.format, tm.tm_yday + 1);
			break;
		default:
			/* Misspelled variable?  Drop the
			 * format argument and continue. */
			break;
		}
	}

	s = str->str;
	g_string_free (str, FALSE);
	g_free (format);

	return s;
}

static void
composer_set_body (EMsgComposer *composer,
                   CamelMimeMessage *message,
                   EMailReplyStyle style,
                   EMFormat *source)
{
	gchar *text, *credits, *original;
	CamelMimePart *part;
	GConfClient *client;
	gboolean start_bottom, has_body_text = FALSE;
	guint32 validity_found = 0;
	const gchar *key;

	client = gconf_client_get_default ();

	key = "/apps/evolution/mail/composer/reply_start_bottom";
	start_bottom = gconf_client_get_bool (client, key, NULL);

	switch (style) {
	case E_MAIL_REPLY_STYLE_DO_NOT_QUOTE:
		/* do nothing */
		break;
	case E_MAIL_REPLY_STYLE_ATTACH:
		/* attach the original message as an attachment */
		part = mail_tool_make_message_attachment (message);
		e_msg_composer_attach (composer, part);
		g_object_unref (part);
		break;
	case E_MAIL_REPLY_STYLE_OUTLOOK:
		original = quoting_text (QUOTING_ORIGINAL);
		text = em_utils_message_to_html (
			message, original, EM_FORMAT_QUOTE_HEADERS, source,
			start_bottom ? "<BR>" : NULL, &validity_found);
		e_msg_composer_set_body_text (composer, text, TRUE);
		has_body_text = text && *text;
		g_free (text);
		g_free (original);
		emu_update_composers_security (composer, validity_found);
		break;

	case E_MAIL_REPLY_STYLE_QUOTED:
	default:
		/* do what any sane user would want when replying... */
		credits = attribution_format (message);
		text = em_utils_message_to_html (
			message, credits, EM_FORMAT_QUOTE_CITE, source,
			start_bottom ? "<BR>" : NULL, &validity_found);
		g_free (credits);
		e_msg_composer_set_body_text (composer, text, TRUE);
		has_body_text = text && *text;
		g_free (text);
		emu_update_composers_security (composer, validity_found);
		break;
	}

	if (has_body_text && start_bottom) {
		GtkhtmlEditor *editor = GTKHTML_EDITOR (composer);
		gboolean move_cursor_to_end;
		gboolean top_signature;
		const gchar *key;

		/* If we are placing signature on top, then move cursor to the end,
		 * otherwise try to find the signature place and place cursor just
		 * before the signature. We added there an empty line already. */
		gtkhtml_editor_run_command (editor, "block-selection");
		gtkhtml_editor_run_command (editor, "cursor-bod");

		key = "/apps/evolution/mail/composer/top_signature";
		top_signature = gconf_client_get_bool (client, key, NULL);

		move_cursor_to_end = top_signature ||
			!gtkhtml_editor_search_by_data (
				editor, 1, "ClueFlow", "signature", "1");

		if (move_cursor_to_end)
			gtkhtml_editor_run_command (editor, "cursor-eod");
		else
			gtkhtml_editor_run_command (editor, "selection-move-left");
		gtkhtml_editor_run_command (editor, "unblock-selection");
	}

	g_object_unref (client);
}

gchar *
em_utils_construct_composer_text (CamelMimeMessage *message,
                                  EMFormat *source)
{
	gchar *text, *credits;
	gboolean start_bottom = 0;

	credits = attribution_format (message);
	text = em_utils_message_to_html (
		message, credits, EM_FORMAT_QUOTE_CITE, source,
		start_bottom ? "<BR>" : NULL, NULL);

	g_free (credits);
	return text;
}

/**
 * em_utils_reply_to_message:
 * @shell: an #EShell
 * @message: a #CamelMimeMessage
 * @folder: a #CamelFolder, or %NULL
 * @message_uid: the UID of @message, or %NULL
 * @type: the type of reply to create
 * @style: the reply style to use
 * @source: source to inherit view settings from
 * @address: used for E_MAIL_REPLY_TO_RECIPIENT @type
 *
 * Creates a new composer ready to reply to @message.
 *
 * @folder and @message_uid may be supplied in order to update the message
 * flags once it has been replied to.
 **/
EMsgComposer *
em_utils_reply_to_message (EShell *shell,
                           CamelMimeMessage *message,
                           CamelFolder *folder,
                           const gchar *message_uid,
                           EMailReplyType type,
                           EMailReplyStyle style,
                           EMFormat *source_formatter,
                           CamelInternetAddress *address)
{
	CamelInternetAddress *to, *cc;
	CamelNNTPAddress *postto = NULL;
	EMsgComposer *composer;
	EAccount *account;
	guint32 flags;

	g_return_val_if_fail (E_IS_SHELL (shell), NULL);
	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	to = camel_internet_address_new ();
	cc = camel_internet_address_new ();

	account = em_utils_guess_account_with_recipients (message, folder);
	flags = CAMEL_MESSAGE_ANSWERED | CAMEL_MESSAGE_SEEN;

	switch (type) {
	case E_MAIL_REPLY_TO_FROM:
		if (folder)
			postto = camel_nntp_address_new ();

		get_reply_from (message, to, postto);
		break;
	case E_MAIL_REPLY_TO_RECIPIENT:
		if (folder)
			postto = camel_nntp_address_new ();

		get_reply_recipient (message, to, postto, address);
		break;
	case E_MAIL_REPLY_TO_SENDER:
		if (folder)
			postto = camel_nntp_address_new ();

		get_reply_sender (message, to, postto);
		break;
	case E_MAIL_REPLY_TO_LIST:
		flags |= CAMEL_MESSAGE_ANSWERED_ALL;
		if (get_reply_list (message, to))
			break;
		/* falls through */
	case E_MAIL_REPLY_TO_ALL:
		flags |= CAMEL_MESSAGE_ANSWERED_ALL;
		if (folder)
			postto = camel_nntp_address_new ();

		em_utils_get_reply_all (message, to, cc, postto);
		break;
	}

	composer = reply_get_composer (
		shell, message, account, to, cc, folder, postto);
	e_msg_composer_add_message_attachments (composer, message, TRUE);

	if (postto)
		g_object_unref (postto);
	g_object_unref (to);
	g_object_unref (cc);

	composer_set_body (composer, message, style, source_formatter);

	if (folder != NULL) {
		gchar *folder_uri;

		folder_uri = e_mail_folder_uri_from_folder (folder);

		e_msg_composer_set_source_headers (
			composer, folder_uri, message_uid, flags);

		g_free (folder_uri);
	}

	composer_set_no_change (composer);

	gtk_widget_show (GTK_WIDGET (composer));

	return composer;
}

static void
post_header_clicked_cb (EComposerPostHeader *header,
                        EMsgComposer *composer)
{
	EShell *shell;
	EShellBackend *shell_backend;
	GtkTreeSelection *selection;
	EMFolderSelector *selector;
	EMFolderTreeModel *model;
	EMFolderTree *folder_tree;
	GtkWidget *dialog;
	GList *list;

	/* FIXME Figure out a way to pass the mail backend in. */
	shell = e_msg_composer_get_shell (composer);
	shell_backend = e_shell_get_backend_by_name (shell, "mail");

	/* FIXME Limit the folder tree to the NNTP account? */
	model = em_folder_tree_model_get_default ();

	dialog = em_folder_selector_new (
		GTK_WINDOW (composer),
		E_MAIL_BACKEND (shell_backend),
		model, EM_FOLDER_SELECTOR_CAN_CREATE,
		_("Posting destination"),
		_("Choose folders to post the message to."),
		NULL);

	selector = EM_FOLDER_SELECTOR (dialog);
	folder_tree = em_folder_selector_get_folder_tree (selector);

	em_folder_tree_set_excluded (
		folder_tree,
		EMFT_EXCLUDE_NOSELECT |
		EMFT_EXCLUDE_VIRTUAL |
		EMFT_EXCLUDE_VTRASH);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (folder_tree));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);

	list = e_composer_post_header_get_folders (header);
	em_folder_tree_set_selected_list (folder_tree, list, FALSE);
	g_list_foreach (list, (GFunc) g_free, NULL);
	g_list_free (list);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) != GTK_RESPONSE_OK) {
		/* Prevent the header's "custom" flag from being reset,
		 * which is what the default method will do next. */
		g_signal_stop_emission_by_name (header, "clicked");
		goto exit;
	}

	list = em_folder_tree_get_selected_uris (folder_tree);
	e_composer_post_header_set_folders (header, list);
	g_list_foreach (list, (GFunc) g_free, NULL);
	g_list_free (list);

exit:
	gtk_widget_destroy (dialog);
}

/**
 * em_configure_new_composer:
 * @composer: a newly created #EMsgComposer
 *
 * Integrates a newly created #EMsgComposer into the mail backend.  The
 * composer can't link directly to the mail backend without introducing
 * circular library dependencies, so this function finishes configuring
 * things the #EMsgComposer instance can't do itself.
 **/
void
em_configure_new_composer (EMsgComposer *composer)
{
	EComposerHeaderTable *table;
	EComposerHeaderType header_type;
	EComposerHeader *header;

	g_return_if_fail (E_IS_MSG_COMPOSER (composer));

	header_type = E_COMPOSER_HEADER_POST_TO;
	table = e_msg_composer_get_header_table (composer);
	header = e_composer_header_table_get_header (table, header_type);

	g_signal_connect (
		composer, "presend",
		G_CALLBACK (composer_presend_check_recipients), NULL);

	g_signal_connect (
		composer, "presend",
		G_CALLBACK (composer_presend_check_account), NULL);

	g_signal_connect (
		composer, "presend",
		G_CALLBACK (composer_presend_check_downloads), NULL);

	g_signal_connect (
		composer, "presend",
		G_CALLBACK (composer_presend_check_plugins), NULL);

	g_signal_connect (
		composer, "presend",
		G_CALLBACK (composer_presend_check_subject), NULL);

	g_signal_connect (
		composer, "presend",
		G_CALLBACK (composer_presend_check_unwanted_html), NULL);

	g_signal_connect (
		composer, "send",
		G_CALLBACK (em_utils_composer_send_cb), NULL);

	g_signal_connect (
		composer, "save-to-drafts",
		G_CALLBACK (em_utils_composer_save_to_drafts_cb), NULL);

	g_signal_connect (
		composer, "save-to-outbox",
		G_CALLBACK (em_utils_composer_save_to_outbox_cb), NULL);

	g_signal_connect (
		composer, "print",
		G_CALLBACK (em_utils_composer_print_cb), NULL);

	/* Handle "Post To:" button clicks, which displays a folder tree
	 * widget.  The composer doesn't know about folder tree widgets,
	 * so it can't handle this itself.
	 *
	 * Note: This is a G_SIGNAL_RUN_LAST signal, which allows us to
	 *       stop the signal emission if the user cancels or closes
	 *       the folder selector dialog.  See the handler function. */
	g_signal_connect (
		header, "clicked",
		G_CALLBACK (post_header_clicked_cb), composer);
}

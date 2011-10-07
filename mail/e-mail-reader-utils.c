/*
 * e-mail-reader-utils.c
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

/* Miscellaneous utility functions used by EMailReader actions. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "e-mail-reader-utils.h"

#include <glib/gi18n.h>
#include <libxml/tree.h>
#include <gtkhtml/gtkhtml.h>
#include <camel/camel.h>

#include "e-util/e-alert-dialog.h"
#include "filter/e-filter-rule.h"
#include "misc/e-web-view.h"
#include "shell/e-shell-utils.h"

#include "mail/e-mail-backend.h"
#include "mail/e-mail-browser.h"
#include "mail/e-mail-folder-utils.h"
#include "mail/e-mail-local.h"
#include "mail/em-composer-utils.h"
#include "mail/em-format-html-print.h"
#include "mail/em-utils.h"
#include "mail/mail-autofilter.h"
#include "mail/mail-ops.h"
#include "mail/mail-tools.h"
#include "mail/mail-vfolder.h"
#include "mail/message-list.h"

typedef struct _AsyncContext AsyncContext;

struct _AsyncContext {
	EActivity *activity;
	CamelFolder *folder;
	EMailReader *reader;
	CamelInternetAddress *address;
	gchar *message_uid;

	EMailReplyType reply_type;
	EMailReplyStyle reply_style;
	GtkPrintOperationAction print_action;
	const gchar *filter_source;
	gint filter_type;
};

static void
async_context_free (AsyncContext *context)
{
	if (context->activity != NULL)
		g_object_unref (context->activity);

	if (context->folder != NULL)
		g_object_unref (context->folder);

	if (context->reader != NULL)
		g_object_unref (context->reader);

	if (context->address != NULL)
		g_object_unref (context->address);

	g_free (context->message_uid);

	g_slice_free (AsyncContext, context);
}

static gboolean
mail_reader_is_special_local_folder (const gchar *name)
{
	return (strcmp (name, "Drafts") == 0 ||
		strcmp (name, "Inbox") == 0 ||
		strcmp (name, "Outbox") == 0 ||
		strcmp (name, "Sent") == 0 ||
		strcmp (name, "Templates") == 0);
}

gboolean
e_mail_reader_confirm_delete (EMailReader *reader)
{
	EShell *shell;
	EMailBackend *backend;
	EShellBackend *shell_backend;
	EShellSettings *shell_settings;
	CamelFolder *folder;
	CamelStore *parent_store;
	GtkWidget *check_button;
	GtkWidget *container;
	GtkWidget *dialog;
	GtkWindow *window;
	const gchar *label;
	gboolean prompt_delete_in_vfolder;
	gint response;

	/* Remind users what deleting from a search folder does. */

	g_return_val_if_fail (E_IS_MAIL_READER (reader), FALSE);

	backend = e_mail_reader_get_backend (reader);
	folder = e_mail_reader_get_folder (reader);
	window = e_mail_reader_get_window (reader);

	shell_backend = E_SHELL_BACKEND (backend);
	shell = e_shell_backend_get_shell (shell_backend);
	shell_settings = e_shell_get_shell_settings (shell);

	prompt_delete_in_vfolder = e_shell_settings_get_boolean (
		shell_settings, "mail-prompt-delete-in-vfolder");

	parent_store = camel_folder_get_parent_store (folder);

	if (!CAMEL_IS_VEE_STORE (parent_store))
		return TRUE;

	if (!prompt_delete_in_vfolder)
		return TRUE;

	dialog = e_alert_dialog_new_for_args (
		window, "mail:ask-delete-vfolder-msg",
		camel_folder_get_full_name (folder), NULL);

	container = e_alert_dialog_get_content_area (E_ALERT_DIALOG (dialog));

	label = _("Do not warn me again");
	check_button = gtk_check_button_new_with_label (label);
	gtk_box_pack_start (GTK_BOX (container), check_button, TRUE, TRUE, 6);
	gtk_widget_show (check_button);

	response = gtk_dialog_run (GTK_DIALOG (dialog));

	if (response != GTK_RESPONSE_DELETE_EVENT)
		e_shell_settings_set_boolean (
			shell_settings,
			"mail-prompt-delete-in-vfolder",
			!gtk_toggle_button_get_active (
			GTK_TOGGLE_BUTTON (check_button)));

	gtk_widget_destroy (dialog);

	return (response == GTK_RESPONSE_OK);
}

static void
mail_reader_delete_folder_cb (CamelFolder *folder,
                              GAsyncResult *result,
                              AsyncContext *context)
{
	EAlertSink *alert_sink;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	e_mail_folder_remove_finish (folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_error_free (error);

	} else if (error != NULL) {
		e_alert_submit (
			alert_sink, "mail:no-delete-folder",
			camel_folder_get_full_name (folder),
			error->message, NULL);
		g_error_free (error);
	}

	async_context_free (context);
}

void
e_mail_reader_delete_folder (EMailReader *reader,
                             CamelFolder *folder)
{
	CamelStore *local_store;
	CamelStore *parent_store;
	EMailBackend *backend;
	EMailSession *session;
	EAlertSink *alert_sink;
	MailFolderCache *folder_cache;
	GtkWindow *parent = e_shell_get_active_window (NULL);
	GtkWidget *dialog;
	const gchar *full_name;
	CamelFolderInfoFlags flags = 0;
	gboolean have_flags;

	g_return_if_fail (E_IS_MAIL_READER (reader));
	g_return_if_fail (CAMEL_IS_FOLDER (folder));

	full_name = camel_folder_get_full_name (folder);
	parent_store = camel_folder_get_parent_store (folder);

	backend = e_mail_reader_get_backend (reader);
	session = e_mail_backend_get_session (backend);

	local_store = e_mail_local_get_store ();
	alert_sink = e_mail_reader_get_alert_sink (reader);
	folder_cache = e_mail_session_get_folder_cache (session);

	if (parent_store == local_store &&
		mail_reader_is_special_local_folder (full_name)) {
		e_mail_backend_submit_alert (
			backend, "mail:no-delete-special-folder",
			full_name, NULL);
		return;
	}

	have_flags = mail_folder_cache_get_folder_info_flags (
		folder_cache, folder, &flags);

	if (have_flags && (flags & CAMEL_FOLDER_SYSTEM)) {
		e_alert_submit (
			alert_sink, "mail:no-delete-special-folder",
			camel_folder_get_display_name (folder), NULL);
		return;
	}

	if (have_flags && (flags & CAMEL_FOLDER_CHILDREN)) {
		if (CAMEL_IS_VEE_STORE (parent_store))
			dialog = e_alert_dialog_new_for_args (
				parent, "mail:ask-delete-vfolder",
				full_name, NULL);
		else
			dialog = e_alert_dialog_new_for_args (
				parent, "mail:ask-delete-folder",
				full_name, NULL);
	} else {
		if (CAMEL_IS_VEE_STORE (parent_store))
			dialog = e_alert_dialog_new_for_args (
				parent, "mail:ask-delete-vfolder-nochild",
				full_name, NULL);
		else
			dialog = e_alert_dialog_new_for_args (
				parent, "mail:ask-delete-folder-nochild",
				full_name, NULL);
	}

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		EActivity *activity;
		AsyncContext *context;
		GCancellable *cancellable;

		activity = e_mail_reader_new_activity (reader);
		cancellable = e_activity_get_cancellable (activity);

		context = g_slice_new0 (AsyncContext);
		context->activity = activity;
		context->reader = g_object_ref (reader);

		/* Disable the dialog until the activity finishes. */
		gtk_widget_set_sensitive (dialog, FALSE);

		/* Destroy the dialog once the activity finishes. */
		g_object_set_data_full (
			G_OBJECT (activity), "delete-dialog",
			dialog, (GDestroyNotify) gtk_widget_destroy);

		e_mail_folder_remove (
			folder, G_PRIORITY_DEFAULT,
			cancellable, (GAsyncReadyCallback)
			mail_reader_delete_folder_cb, context);
	} else
		gtk_widget_destroy (dialog);
}

void
e_mail_reader_mark_as_read (EMailReader *reader,
                            const gchar *uid)
{
	EMailBackend *backend;
	EMFormatHTML *formatter;
	EMailDisplay *display;
	CamelFolder *folder;
	guint32 mask, set;
	guint32 flags;

	g_return_if_fail (E_IS_MAIL_READER (reader));
	g_return_if_fail (uid != NULL);

	folder = e_mail_reader_get_folder (reader);
	backend = e_mail_reader_get_backend (reader);
	display = e_mail_reader_get_mail_display (reader);
	formatter = e_mail_display_get_formatter (display);

	flags = camel_folder_get_message_flags (folder, uid);

	if (!(flags & CAMEL_MESSAGE_SEEN)) {
		CamelMimeMessage *message;

		message = EM_FORMAT (formatter)->message;
		em_utils_handle_receipt (backend, folder, uid, message);
	}

	mask = CAMEL_MESSAGE_SEEN;
	set  = CAMEL_MESSAGE_SEEN;

	camel_folder_set_message_flags (folder, uid, mask, set);
}

guint
e_mail_reader_mark_selected (EMailReader *reader,
                             guint32 mask,
                             guint32 set)
{
	CamelFolder *folder;
	GPtrArray *uids;
	guint ii;

	g_return_val_if_fail (E_IS_MAIL_READER (reader), 0);

	folder = e_mail_reader_get_folder (reader);

	if (folder == NULL)
		return 0;

	camel_folder_freeze (folder);
	uids = e_mail_reader_get_selected_uids (reader);

	for (ii = 0; ii < uids->len; ii++)
		camel_folder_set_message_flags (
			folder, uids->pdata[ii], mask, set);

	em_utils_uids_free (uids);

	camel_folder_thaw (folder);

	return ii;
}
static void
copy_tree_state (EMailReader *src_reader,
                 EMailReader *des_reader)
{
	GtkWidget *src_mlist, *des_mlist;
	gchar *state;

	g_return_if_fail (src_reader != NULL);
	g_return_if_fail (des_reader != NULL);

	src_mlist = e_mail_reader_get_message_list (src_reader);
	if (!src_mlist)
		return;

	des_mlist = e_mail_reader_get_message_list (des_reader);
	if (!des_mlist)
		return;

	state = e_tree_get_state (E_TREE (src_mlist));
	if (state)
		e_tree_set_state (E_TREE (des_mlist), state);
	g_free (state);
}

guint
e_mail_reader_open_selected (EMailReader *reader)
{
	EMailBackend *backend;
	CamelFolder *folder;
	GtkWindow *window;
	GPtrArray *views;
	GPtrArray *uids;
	guint ii;

	g_return_val_if_fail (E_IS_MAIL_READER (reader), 0);

	backend = e_mail_reader_get_backend (reader);
	folder = e_mail_reader_get_folder (reader);
	uids = e_mail_reader_get_selected_uids (reader);
	window = e_mail_reader_get_window (reader);

	if (!em_utils_ask_open_many (window, uids->len)) {
		em_utils_uids_free (uids);
		return 0;
	}

	if (em_utils_folder_is_drafts (folder) ||
		em_utils_folder_is_outbox (folder) ||
		em_utils_folder_is_templates (folder)) {
		em_utils_edit_messages (reader, folder, uids, TRUE);
		return uids->len;
	}

	views = g_ptr_array_new ();

	/* For vfolders we need to edit the original, not the vfolder copy. */
	for (ii = 0; ii < uids->len; ii++) {
		const gchar *uid = uids->pdata[ii];
		CamelFolder *real_folder;
		CamelMessageInfo *info;
		gchar *real_uid;

		if (!CAMEL_IS_VEE_FOLDER (folder)) {
			g_ptr_array_add (views, g_strdup (uid));
			continue;
		}

		info = camel_folder_get_message_info (folder, uid);
		if (info == NULL)
			continue;

		real_folder = camel_vee_folder_get_location (
			CAMEL_VEE_FOLDER (folder),
			(CamelVeeMessageInfo *) info, &real_uid);

		if (em_utils_folder_is_drafts (real_folder) ||
			em_utils_folder_is_outbox (real_folder)) {
			GPtrArray *edits;

			edits = g_ptr_array_new ();
			g_ptr_array_add (edits, real_uid);
			em_utils_edit_messages (
				reader, real_folder, edits, TRUE);
		} else {
			g_free (real_uid);
			g_ptr_array_add (views, g_strdup (uid));
		}

		camel_folder_free_message_info (folder, info);
	}

	for (ii = 0; ii < views->len; ii++) {
		const gchar *uid = views->pdata[ii];
		GtkWidget *browser;

		browser = e_mail_browser_new (backend, folder, uid,
				E_MAIL_DISPLAY_MODE_NORMAL);
		e_mail_reader_set_folder (E_MAIL_READER (browser), folder);
		e_mail_reader_set_message (E_MAIL_READER (browser), uid);
		copy_tree_state (reader, E_MAIL_READER (browser));
		e_mail_reader_set_group_by_threads (
			E_MAIL_READER (browser),
			e_mail_reader_get_group_by_threads (reader));
		gtk_widget_show (browser);
	}

	g_ptr_array_foreach (views, (GFunc) g_free, NULL);
	g_ptr_array_free (views, TRUE);

	em_utils_uids_free (uids);

	return ii;
}

/* Helper for e_mail_reader_print() */
static void
mail_reader_print_cb (CamelFolder *folder,
                      GAsyncResult *result,
                      AsyncContext *context)
{
	EAlertSink *alert_sink;
	CamelMimeMessage *message;
	EMailDisplay *display;
	EMFormatHTML *formatter;
	EMFormatHTMLPrint *html_print;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	message = camel_folder_get_message_finish (folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (message == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (message == NULL);
		e_alert_submit (
			alert_sink, "mail:no-retrieve-message",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	display = e_mail_reader_get_mail_display (context->reader);
	formatter = e_mail_display_get_formatter (display);

	html_print = em_format_html_print_new (
		formatter, context->print_action);
	/* FIXME WEBKIT
	em_format_merge_handler (
		EM_FORMAT (html_print), EM_FORMAT (formatter));
	*/
	em_format_html_print_message (
		html_print, message, folder, context->message_uid);
	g_object_unref (html_print);

	g_object_unref (message);

	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);

	async_context_free (context);
}

void
e_mail_reader_print (EMailReader *reader,
                     GtkPrintOperationAction action)
{
	EActivity *activity;
	AsyncContext *context;
	GCancellable *cancellable;
	CamelFolder *folder;
	GPtrArray *uids;
	const gchar *message_uid;

	g_return_if_fail (E_IS_MAIL_READER (reader));

	folder = e_mail_reader_get_folder (reader);
	g_return_if_fail (CAMEL_IS_FOLDER (folder));

	/* XXX Learn to handle len > 1. */
	uids = e_mail_reader_get_selected_uids (reader);
	g_return_if_fail (uids != NULL && uids->len == 1);
	message_uid = g_ptr_array_index (uids, 0);

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->reader = g_object_ref (reader);
	context->message_uid = g_strdup (message_uid);
	context->print_action = action;

	camel_folder_get_message (
		folder, message_uid, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		mail_reader_print_cb, context);

	em_utils_uids_free (uids);
}

static void
mail_reader_remove_attachments_cb (CamelFolder *folder,
                                   GAsyncResult *result,
                                   AsyncContext *context)
{
	EAlertSink *alert_sink;
	GError *error = NULL;

	alert_sink = e_mail_reader_get_alert_sink (context->reader);

	e_mail_folder_remove_attachments_finish (folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_error_free (error);

	} else if (error != NULL) {
		e_alert_submit (
			alert_sink,
			"mail:remove-attachments",
			error->message, NULL);
		g_error_free (error);
	}

	async_context_free (context);
}

void
e_mail_reader_remove_attachments (EMailReader *reader)
{
	EActivity *activity;
	AsyncContext *context;
	GCancellable *cancellable;
	CamelFolder *folder;
	GPtrArray *uids;

	g_return_if_fail (E_IS_MAIL_READER (reader));

	folder = e_mail_reader_get_folder (reader);
	uids = e_mail_reader_get_selected_uids (reader);
	g_return_if_fail (uids != NULL);

	/* XXX Either e_mail_reader_get_selected_uids()
	 *     or MessageList should do this itself. */
	g_ptr_array_set_free_func (uids, (GDestroyNotify) g_free);

	/* Remove attachments asynchronously. */

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->reader = g_object_ref (reader);

	e_mail_folder_remove_attachments (
		folder, uids, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		mail_reader_remove_attachments_cb,
		context);

	g_ptr_array_unref (uids);
}

static void
mail_reader_remove_duplicates_cb (CamelFolder *folder,
                                  GAsyncResult *result,
                                  AsyncContext *context)
{
	EAlertSink *alert_sink;
	GHashTable *duplicates;
	GtkWindow *parent_window;
	guint n_duplicates;
	GError *error = NULL;

	alert_sink = e_mail_reader_get_alert_sink (context->reader);
	parent_window = e_mail_reader_get_window (context->reader);

	duplicates = e_mail_folder_find_duplicate_messages_finish (
		folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (duplicates == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (duplicates == NULL);
		e_alert_submit (
			alert_sink,
			"mail:find-duplicate-messages",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (duplicates != NULL);

	/* Finalize the activity here so we don't leave a message in
	 * the task bar while prompting the user for confirmation. */
	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);
	g_object_unref (context->activity);
	context->activity = NULL;

	n_duplicates = g_hash_table_size (duplicates);

	if (n_duplicates == 0) {
		em_utils_prompt_user (
			parent_window, NULL,
			"mail:info-no-remove-duplicates",
			camel_folder_get_display_name (folder), NULL);
	} else {
		gchar *confirmation;
		gboolean proceed;

		confirmation = g_strdup_printf (ngettext (
			/* Translators: %s is replaced with a folder
			 * name %u with count of duplicate messages. */
			"Folder '%s' contains %u duplicate message. "
			"Are you sure you want to delete it?",
			"Folder '%s' contains %u duplicate messages. "
			"Are you sure you want to delete them?",
			n_duplicates),
			camel_folder_get_display_name (folder),
			n_duplicates);

		proceed = em_utils_prompt_user (
			parent_window, NULL,
			"mail:ask-remove-duplicates",
			confirmation, NULL);

		if (proceed) {
			GHashTableIter iter;
			gpointer key;

			camel_folder_freeze (folder);

			g_hash_table_iter_init (&iter, duplicates);

			/* Mark duplicate messages for deletion. */
			while (g_hash_table_iter_next (&iter, &key, NULL))
				camel_folder_delete_message (folder, key);

			camel_folder_thaw (folder);
		}

		g_free (confirmation);
	}

	g_hash_table_destroy (duplicates);

	async_context_free (context);
}

void
e_mail_reader_remove_duplicates (EMailReader *reader)
{
	EActivity *activity;
	AsyncContext *context;
	GCancellable *cancellable;
	CamelFolder *folder;
	GPtrArray *uids;

	g_return_if_fail (E_IS_MAIL_READER (reader));

	folder = e_mail_reader_get_folder (reader);
	uids = e_mail_reader_get_selected_uids (reader);
	g_return_if_fail (uids != NULL);

	/* XXX Either e_mail_reader_get_selected_uids()
	 *     or MessageList should do this itself. */
	g_ptr_array_set_free_func (uids, (GDestroyNotify) g_free);

	/* Find duplicate messages asynchronously. */

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->reader = g_object_ref (reader);

	e_mail_folder_find_duplicate_messages (
		folder, uids, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		mail_reader_remove_duplicates_cb,
		context);

	g_ptr_array_unref (uids);
}

/* Helper for e_mail_reader_reply_to_message()
 * XXX This function belongs in e-html-utils.c */
static gboolean
html_contains_nonwhitespace (const gchar *html,
                             gint len)
{
	const gchar *cp;
	gunichar uc = 0;

	if (html == NULL || len <= 0)
		return FALSE;

	cp = html;

	while (cp != NULL && cp - html < len) {
		uc = g_utf8_get_char (cp);
		if (uc == 0)
			break;

		if (uc == '<') {
			/* skip until next '>' */
			uc = g_utf8_get_char (cp);
			while (uc != 0 && uc != '>' && cp - html < len) {
				cp = g_utf8_next_char (cp);
				uc = g_utf8_get_char (cp);
			}
			if (uc == 0)
				break;
		} else if (uc == '&') {
			/* sequence '&nbsp;' is a space */
			if (g_ascii_strncasecmp (cp, "&nbsp;", 6) == 0)
				cp = cp + 5;
			else
				break;
		} else if (!g_unichar_isspace (uc))
			break;

		cp = g_utf8_next_char (cp);
	}

	return cp - html < len - 1 && uc != 0;
}

static void
mail_reader_get_message_ready_cb (CamelFolder *folder,
                                  GAsyncResult *result,
                                  AsyncContext *context)
{
	EShell *shell;
	EMailBackend *backend;
	EAlertSink *alert_sink;
	EMFormatHTML *formatter;
	CamelMimeMessage *message;
	GError *error = NULL;

	alert_sink = e_mail_reader_get_alert_sink (context->reader);

	message = camel_folder_get_message_finish (folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (message == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (message == NULL);
		e_alert_submit (
			alert_sink, "mail:no-retrieve-message",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	backend = e_mail_reader_get_backend (context->reader);
	shell = e_shell_backend_get_shell (E_SHELL_BACKEND (backend));

	formatter = e_mail_reader_get_formatter (context->reader);

	em_utils_reply_to_message (
		shell, message,
		context->folder, context->message_uid,
		context->reply_type, context->reply_style,
		EM_FORMAT (formatter), context->address);

	g_object_unref (message);

	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);

	async_context_free (context);
}

void
e_mail_reader_reply_to_message (EMailReader *reader,
                                CamelMimeMessage *src_message,
                                EMailReplyType reply_type)
{
	EShell *shell;
	EMailBackend *backend;
	EShellBackend *shell_backend;
	EMailDisplay *display;
	EMFormatHTML *formatter;
	GtkWidget *message_list;
	CamelMimeMessage *new_message;
	CamelInternetAddress *address = NULL;
	CamelFolder *folder;
	EMailReplyStyle reply_style;
	EWebView *web_view;
	struct _camel_header_raw *header;
	const gchar *uid;
	gchar *selection = NULL;
	gint length;

	/* This handles quoting only selected text in the reply.  If
	 * nothing is selected or only whitespace is selected, fall
	 * back to the normal em_utils_reply_to_message(). */

	g_return_if_fail (E_IS_MAIL_READER (reader));

	backend = e_mail_reader_get_backend (reader);
	folder = e_mail_reader_get_folder (reader);
	display = e_mail_reader_get_mail_display (reader);
	formatter = e_mail_display_get_formatter (display);
	message_list = e_mail_reader_get_message_list (reader);
	reply_style = e_mail_reader_get_reply_style (reader);

	shell_backend = E_SHELL_BACKEND (backend);
	shell = e_shell_backend_get_shell (shell_backend);

	web_view = e_mail_display_get_current_web_view (display);

	if (reply_type == E_MAIL_REPLY_TO_RECIPIENT) {
		const gchar *uri;

		uri = e_web_view_get_selected_uri (web_view);

		if (uri) {
			CamelURL *curl;

			curl = camel_url_new (uri, NULL);

			if (curl && curl->path && *curl->path) {
				address = camel_internet_address_new ();
				if (camel_address_decode (CAMEL_ADDRESS (address), curl->path) < 0) {
					g_object_unref (address);
					address = NULL;
				}
			}

			if (curl)
				camel_url_free (curl);
		}
	}

	uid = MESSAGE_LIST (message_list)->cursor_uid;
	g_return_if_fail (uid != NULL);

	if (!gtk_widget_get_mapped (GTK_WIDGET (web_view)))
		goto whole_message;

	if (src_message == NULL) {
		src_message = EM_FORMAT (formatter)->message;
		if (src_message != NULL)
			g_object_ref (src_message);

		g_return_if_fail (src_message != NULL);
	}

	if (!e_web_view_is_selection_active (web_view))
		goto whole_message;

	selection = gtk_html_get_selection_html (GTK_HTML (web_view), &length);
	if (selection == NULL || *selection == '\0')
		goto whole_message;

	if (!html_contains_nonwhitespace (selection, length))
		goto whole_message;

	new_message = camel_mime_message_new ();

	/* Filter out "content-*" headers. */
	header = CAMEL_MIME_PART (src_message)->headers;
	while (header != NULL) {
		if (g_ascii_strncasecmp (header->name, "content-", 8) != 0)
			camel_medium_add_header (
				CAMEL_MEDIUM (new_message),
				header->name, header->value);

		header = header->next;
	}

	camel_mime_part_set_encoding (
		CAMEL_MIME_PART (new_message),
		CAMEL_TRANSFER_ENCODING_8BIT);

	camel_mime_part_set_content (
		CAMEL_MIME_PART (new_message),
		selection, length, "text/html");

	g_object_unref (src_message);

	em_utils_reply_to_message (
		shell, new_message, folder, uid,
		reply_type, reply_style, NULL, address);

	if (address)
		g_object_unref (address);

	g_object_unref (new_message);

	g_free (selection);

	return;

whole_message:
	if (src_message == NULL) {
		EActivity *activity;
		AsyncContext *context;
		GCancellable *cancellable;

		activity = e_mail_reader_new_activity (reader);
		cancellable = e_activity_get_cancellable (activity);

		context = g_slice_new0 (AsyncContext);
		context->activity = activity;
		context->folder = g_object_ref (folder);
		context->reader = g_object_ref (reader);
		context->address = address; /* takes ownership of it, if set */
		context->message_uid = g_strdup (uid);
		context->reply_type = reply_type;
		context->reply_style = reply_style;

		camel_folder_get_message (
			context->folder, context->message_uid,
			G_PRIORITY_DEFAULT, cancellable,
			(GAsyncReadyCallback) mail_reader_get_message_ready_cb,
			context);

		return;
	}

	em_utils_reply_to_message (
		shell, src_message, folder, uid,
		reply_type, reply_style, EM_FORMAT (formatter), address);

	if (address)
		g_object_unref (address);
}

static void
mail_reader_save_messages_cb (CamelFolder *folder,
                              GAsyncResult *result,
                              AsyncContext *context)
{
	EAlertSink *alert_sink;
	GError *error = NULL;

	alert_sink = e_mail_reader_get_alert_sink (context->reader);

	e_mail_folder_save_messages_finish (folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_error_free (error);

	} else if (error != NULL) {
		e_alert_submit (
			alert_sink,
			"mail:save-messages",
			error->message, NULL);
		g_error_free (error);
	}

	async_context_free (context);
}

void
e_mail_reader_save_messages (EMailReader *reader)
{
	EShell *shell;
	EActivity *activity;
	AsyncContext *context;
	EMailBackend *backend;
	GCancellable *cancellable;
	EShellBackend *shell_backend;
	CamelMessageInfo *info;
	CamelFolder *folder;
	GFile *destination;
	GPtrArray *uids;
	const gchar *message_uid;
	const gchar *title;
	gchar *suggestion = NULL;

	folder = e_mail_reader_get_folder (reader);
	backend = e_mail_reader_get_backend (reader);

	uids = e_mail_reader_get_selected_uids (reader);
	g_return_if_fail (uids != NULL && uids->len > 0);
	message_uid = g_ptr_array_index (uids, 0);

	/* XXX Either e_mail_reader_get_selected_uids()
	 *     or MessageList should do this itself. */
	g_ptr_array_set_free_func (uids, (GDestroyNotify) g_free);

	title = ngettext ("Save Message", "Save Messages", uids->len);

	/* Suggest as a filename the subject of the first message. */
	info = camel_folder_get_message_info (folder, message_uid);
	if (info != NULL) {
		const gchar *subject;

		subject = camel_message_info_subject (info);
		if (subject != NULL)
			suggestion = g_strconcat (subject, ".mbox", NULL);
		camel_folder_free_message_info (folder, info);
	}

	if (suggestion == NULL) {
		const gchar *basename;

		/* Translators: This is part of a suggested file name
		 * used when saving a message or multiple messages to
		 * mbox format, when the first message doesn't have a
		 * subject.  The extension ".mbox" is appended to the
		 * string; for example "Message.mbox". */
		basename = ngettext ("Message", "Messages", uids->len);
		suggestion = g_strconcat (basename, ".mbox", NULL);
	}

	shell_backend = E_SHELL_BACKEND (backend);
	shell = e_shell_backend_get_shell (shell_backend);

	destination = e_shell_run_save_dialog (
		shell, title, suggestion,
		"*.mbox:application/mbox,message/rfc822", NULL, NULL);

	if (destination == NULL) {
		g_ptr_array_unref (uids);
		return;
	}

	/* Save messages asynchronously. */

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->reader = g_object_ref (reader);

	e_mail_folder_save_messages (
		folder, uids,
		destination, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		mail_reader_save_messages_cb, context);

	g_object_unref (destination);
	g_ptr_array_unref (uids);
}

void
e_mail_reader_select_next_message (EMailReader *reader,
                                   gboolean or_else_previous)
{
	GtkWidget *message_list;
	gboolean hide_deleted;
	gboolean success;

	g_return_if_fail (E_IS_MAIL_READER (reader));

	hide_deleted = e_mail_reader_get_hide_deleted (reader);
	message_list = e_mail_reader_get_message_list (reader);

	success = message_list_select (
		MESSAGE_LIST (message_list),
		MESSAGE_LIST_SELECT_NEXT, 0, 0);

	if (!success && (hide_deleted || or_else_previous))
		message_list_select (
			MESSAGE_LIST (message_list),
			MESSAGE_LIST_SELECT_PREVIOUS, 0, 0);
}

/* Helper for e_mail_reader_create_filter_from_selected() */
static void
mail_reader_create_filter_cb (CamelFolder *folder,
                              GAsyncResult *result,
                              AsyncContext *context)
{
	EMailBackend *backend;
	EAlertSink *alert_sink;
	CamelMimeMessage *message;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	message = camel_folder_get_message_finish (folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (message == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (message == NULL);
		e_alert_submit (
			alert_sink, "mail:no-retrieve-message",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	/* Finalize the activity here so we don't leave a message
	 * in the task bar while displaying the filter editor. */
	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);
	g_object_unref (context->activity);
	context->activity = NULL;

	backend = e_mail_reader_get_backend (context->reader);

	filter_gui_add_from_message (
		backend, message,
		context->filter_source,
		context->filter_type);

	g_object_unref (message);

	async_context_free (context);
}

void
e_mail_reader_create_filter_from_selected (EMailReader *reader,
                                           gint filter_type)
{
	EActivity *activity;
	AsyncContext *context;
	GCancellable *cancellable;
	CamelFolder *folder;
	GPtrArray *uids;
	const gchar *filter_source;
	const gchar *message_uid;

	g_return_if_fail (E_IS_MAIL_READER (reader));

	folder = e_mail_reader_get_folder (reader);
	g_return_if_fail (CAMEL_IS_FOLDER (folder));

	if (em_utils_folder_is_sent (folder))
		filter_source = E_FILTER_SOURCE_OUTGOING;
	else if (em_utils_folder_is_outbox (folder))
		filter_source = E_FILTER_SOURCE_OUTGOING;
	else
		filter_source = E_FILTER_SOURCE_INCOMING;

	uids = e_mail_reader_get_selected_uids (reader);
	g_return_if_fail (uids != NULL && uids->len == 1);
	message_uid = g_ptr_array_index (uids, 0);

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->reader = g_object_ref (reader);
	context->filter_source = filter_source;
	context->filter_type = filter_type;

	camel_folder_get_message (
		folder, message_uid, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		mail_reader_create_filter_cb, context);

	em_utils_uids_free (uids);
}

/* Helper for e_mail_reader_create_vfolder_from_selected() */
static void
mail_reader_create_vfolder_cb (CamelFolder *folder,
                               GAsyncResult *result,
                               AsyncContext *context)
{
	EMailBackend *backend;
	EMailSession *session;
	EAlertSink *alert_sink;
	CamelMimeMessage *message;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	message = camel_folder_get_message_finish (folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (message == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (message == NULL);
		e_alert_submit (
			alert_sink, "mail:no-retrieve-message",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	/* Finalize the activity here so we don't leave a message
	 * in the task bar while displaying the vfolder editor. */
	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);
	g_object_unref (context->activity);
	context->activity = NULL;

	backend = e_mail_reader_get_backend (context->reader);
	session = e_mail_backend_get_session (backend);

	vfolder_gui_add_from_message (
		session, message,
		context->filter_type,
		context->folder);

	g_object_unref (message);

	async_context_free (context);
}

void
e_mail_reader_create_vfolder_from_selected (EMailReader *reader,
                                            gint vfolder_type)
{
	EActivity *activity;
	AsyncContext *context;
	GCancellable *cancellable;
	CamelFolder *folder;
	GPtrArray *uids;
	const gchar *message_uid;

	g_return_if_fail (E_IS_MAIL_READER (reader));

	folder = e_mail_reader_get_folder (reader);
	g_return_if_fail (CAMEL_IS_FOLDER (folder));

	uids = e_mail_reader_get_selected_uids (reader);
	g_return_if_fail (uids != NULL && uids->len == 1);
	message_uid = g_ptr_array_index (uids, 0);

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->folder = g_object_ref (folder);
	context->reader = g_object_ref (reader);
	context->filter_type = vfolder_type;

	camel_folder_get_message (
		folder, message_uid, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		mail_reader_create_vfolder_cb, context);

	em_utils_uids_free (uids);
}

static EMailReaderHeader *
emr_header_from_xmldoc (xmlDocPtr doc)
{
	EMailReaderHeader *h;
	xmlNodePtr root;
	xmlChar *name;

	if (doc == NULL)
		return NULL;

	root = doc->children;
	if (strcmp ((gchar *)root->name, "header") != 0)
		return NULL;

	name = xmlGetProp (root, (const guchar *)"name");
	if (name == NULL)
		return NULL;

	h = g_malloc0 (sizeof (EMailReaderHeader));
	h->name = g_strdup ((gchar *) name);
	xmlFree (name);

	if (xmlHasProp (root, (const guchar *)"enabled"))
		h->enabled = 1;
	else
		h->enabled = 0;

	return h;
}

/**
 * e_mail_reader_header_from_xml
 * @xml: XML configuration data
 *
 * Parses passed XML data, which should be of
 * the format <header name="foo" enabled />, and
 * returns a EMailReaderHeader structure, or NULL if there
 * is an error.
 **/
EMailReaderHeader *
e_mail_reader_header_from_xml (const gchar *xml)
{
	EMailReaderHeader *header;
	xmlDocPtr doc;

	if (!(doc = xmlParseDoc ((guchar *) xml)))
		return NULL;

	header = emr_header_from_xmldoc (doc);
	xmlFreeDoc (doc);

	return header;
}

/**
 * e_mail_reader_header_to_xml
 * @header: header from which to generate XML
 *
 * Returns the passed header as a XML structure,
 * or NULL on error
 */
gchar *
e_mail_reader_header_to_xml (EMailReaderHeader *header)
{
	xmlDocPtr doc;
	xmlNodePtr root;
	xmlChar *xml;
	gchar *out;
	gint size;

	g_return_val_if_fail (header != NULL, NULL);
	g_return_val_if_fail (header->name != NULL, NULL);

	doc = xmlNewDoc ((const guchar *)"1.0");

	root = xmlNewDocNode (doc, NULL, (const guchar *)"header", NULL);
	xmlSetProp (root, (const guchar *)"name", (guchar *)header->name);
	if (header->enabled)
		xmlSetProp (root, (const guchar *)"enabled", NULL);

	xmlDocSetRootElement (doc, root);
	xmlDocDumpMemory (doc, &xml, &size);
	xmlFreeDoc (doc);

	out = g_malloc (size + 1);
	memcpy (out, xml, size);
	out[size] = '\0';
	xmlFree (xml);

	return out;
}

/**
 * e_mail_reader_header_free
 * @header: header to free
 *
 * Frees the memory associated with the passed header
 * structure.
 */
void
e_mail_reader_header_free (EMailReaderHeader *header)
{
	if (header == NULL)
		return;

	g_free (header->name);
	g_free (header);
}

static void
headers_changed_cb (GConfClient *client,
                    guint cnxn_id,
                    GConfEntry *entry,
                    EMailReader *reader)
{
	EMailDisplay *display;
	EMFormatHTML *formatter;
	GSList *header_config_list, *p;

	g_return_if_fail (client != NULL);
	g_return_if_fail (reader != NULL);

	display = e_mail_reader_get_mail_display (reader);
	formatter = e_mail_display_get_formatter (display);

	if (!formatter)
		return;

	header_config_list = gconf_client_get_list (
		client, "/apps/evolution/mail/display/headers",
		GCONF_VALUE_STRING, NULL);
	/* FIXME WEBKIT
	em_format_clear_headers (EM_FORMAT (formatter));
	for (p = header_config_list; p; p = g_slist_next (p)) {
		EMailReaderHeader *h;
		gchar *xml = (gchar *) p->data;

		h = e_mail_reader_header_from_xml (xml);
		if (h && h->enabled)
			em_format_add_header (
				EM_FORMAT (formatter),
				h->name, EM_FORMAT_HEADER_BOLD);

		e_mail_reader_header_free (h);
	}

	if (!header_config_list)
		em_format_default_headers (EM_FORMAT (formatter));

	g_slist_foreach (header_config_list, (GFunc) g_free, NULL);
	g_slist_free (header_config_list);
	*/

	/* force a redraw */
	if (EM_FORMAT (formatter)->message) {
		e_mail_display_reload (display);
	}
}

static void
remove_header_notify_cb (gpointer data)
{
	GConfClient *client;
	guint notify_id;

	notify_id = GPOINTER_TO_INT (data);
	g_return_if_fail (notify_id != 0);

	client = gconf_client_get_default ();
	gconf_client_notify_remove (client, notify_id);
	gconf_client_remove_dir (client, "/apps/evolution/mail/display", NULL);
	g_object_unref (client);
}

/**
 * e_mail_reader_connect_headers
 * @reader: an #EMailReader
 *
 * Connects @reader to listening for changes in headers and
 * updates the EMFormat whenever it changes and on this call too.
 **/
void
e_mail_reader_connect_headers (EMailReader *reader)
{
	GConfClient *client;
	guint notify_id;

	client = gconf_client_get_default ();

	gconf_client_add_dir (
		client, "/apps/evolution/mail/display",
		GCONF_CLIENT_PRELOAD_NONE, NULL);
	notify_id = gconf_client_notify_add (
		client, "/apps/evolution/mail/display/headers",
		(GConfClientNotifyFunc) headers_changed_cb,
		reader, NULL, NULL);

	g_object_set_data_full (
		G_OBJECT (reader), "reader-header-notify-id",
		GINT_TO_POINTER (notify_id), remove_header_notify_cb);

	headers_changed_cb (client, 0, NULL, reader);

	g_object_unref (client);
}

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
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 * Copyright (C) 2004 Meilof Veeningen <meilof@wanadoo.nl>
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib/gi18n-lib.h>
#include <stdio.h>
#include <string.h>
#include <gtk/gtk.h>
#include <gconf/gconf-client.h>

#include "composer/e-msg-composer.h"
#include "mail/e-mail-browser.h"
#include "mail/e-mail-reader.h"
#include "mail/em-composer-utils.h"
#include "mail/em-format-hook.h"
#include "mail/em-config.h"
#include "mail/em-utils.h"
#include "mail/mail-ops.h"
#include "mail/mail-mt.h"
#include "mail/message-list.h"
#include "e-util/e-util.h"
#include "e-util/e-account-utils.h"
#include "e-util/e-alert-dialog.h"
#include "shell/e-shell-view.h"
#include "shell/e-shell-window.h"
#include "shell/e-shell-window-actions.h"

/* EAlert Message IDs */
#define MESSAGE_PREFIX			"org.gnome.mailing-list-actions:"
#define MESSAGE_NO_ACTION		MESSAGE_PREFIX "no-action"
#define MESSAGE_NO_HEADER		MESSAGE_PREFIX "no-header"
#define MESSAGE_ASK_SEND_MESSAGE	MESSAGE_PREFIX "ask-send-message"
#define MESSAGE_MALFORMED_HEADER	MESSAGE_PREFIX "malformed-header"
#define MESSAGE_POSTING_NOT_ALLOWED	MESSAGE_PREFIX "posting-not-allowed"

typedef enum {
	EMLA_ACTION_HELP,
	EMLA_ACTION_UNSUBSCRIBE,
	EMLA_ACTION_SUBSCRIBE,
	EMLA_ACTION_POST,
	EMLA_ACTION_OWNER,
	EMLA_ACTION_ARCHIVE
} EmlaAction;

typedef struct {
	/* action enumeration */
	EmlaAction action;

	/* whether the user needs to edit a mailto:
	 * message (e.g. for post action) */
	gboolean interactive;

	/* header representing the action */
	const gchar *header;
} EmlaActionHeader;

const EmlaActionHeader emla_action_headers[] = {
	{ EMLA_ACTION_HELP,        FALSE, "List-Help" },
	{ EMLA_ACTION_UNSUBSCRIBE, TRUE,  "List-Unsubscribe" },
	{ EMLA_ACTION_SUBSCRIBE,   FALSE, "List-Subscribe" },
	{ EMLA_ACTION_POST,        TRUE,  "List-Post" },
	{ EMLA_ACTION_OWNER,       TRUE,  "List-Owner" },
	{ EMLA_ACTION_ARCHIVE,     FALSE, "List-Archive" },
};

gboolean	mail_browser_init		(GtkUIManager *ui_manager,
						 EMailBrowser *browser);
gboolean	mail_shell_view_init		(GtkUIManager *ui_manager,
						 EShellView *shell_view);
gint e_plugin_lib_enable (EPlugin *ep, gint enable);

gint
e_plugin_lib_enable (EPlugin *ep,
                     gint enable)
{
	return 0;
}

typedef struct _AsyncContext AsyncContext;

struct _AsyncContext {
	EActivity *activity;
	EMailReader *reader;
	EmlaAction action;
};

static void
async_context_free (AsyncContext *context)
{
	if (context->activity != NULL)
		g_object_unref (context->activity);

	if (context->reader != NULL)
		g_object_unref (context->reader);

	g_slice_free (AsyncContext, context);
}

static void
emla_list_action_cb (CamelFolder *folder,
                     GAsyncResult *result,
                     AsyncContext *context)
{
	const gchar *header = NULL, *headerpos;
	gchar *end, *url = NULL;
	gint t;
	EMsgComposer *composer;
	EAlertSink *alert_sink;
	CamelMimeMessage *message;
	gint send_message_response;
	EShell *shell;
	EMailBackend *backend;
	EShellBackend *shell_backend;
	EAccount *account;
	GtkWindow *window;
	CamelStore *store;
	const gchar *uid;
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

	/* Finalize the activity here so we don't leave a
	 * message in the task bar while display a dialog. */
	e_activity_set_state (context->activity, E_ACTIVITY_COMPLETED);
	g_object_unref (context->activity);
	context->activity = NULL;

	store = camel_folder_get_parent_store (folder);
	uid = camel_service_get_uid (CAMEL_SERVICE (store));
	account = e_get_account_by_uid (uid);

	backend = e_mail_reader_get_backend (context->reader);

	shell_backend = E_SHELL_BACKEND (backend);
	shell = e_shell_backend_get_shell (shell_backend);

	window = e_mail_reader_get_window (context->reader);

	for (t = 0; t < G_N_ELEMENTS (emla_action_headers); t++) {
		if (emla_action_headers[t].action == context->action &&
		    (header = camel_medium_get_header (CAMEL_MEDIUM (message),
			emla_action_headers[t].header)) != NULL)
			break;
	}

	if (!header) {
		/* there was no header matching the action */
		e_alert_run_dialog_for_args (window, MESSAGE_NO_HEADER, NULL);
		goto exit;
	}

	headerpos = header;

	if (context->action == EMLA_ACTION_POST) {
		while (*headerpos == ' ') headerpos++;
		if (g_ascii_strcasecmp (headerpos, "NO") == 0) {
			e_alert_run_dialog_for_args (
				window, MESSAGE_POSTING_NOT_ALLOWED, NULL);
			goto exit;
		}
	}

	/* parse the action value */
	while (*headerpos) {
		/* skip whitespace */
		while (*headerpos == ' ') headerpos++;
		if (*headerpos != '<' || (end = strchr (headerpos++, '>')) == NULL) {
			e_alert_run_dialog_for_args (
				window, MESSAGE_MALFORMED_HEADER,
				emla_action_headers[t].header, header, NULL);
			goto exit;
		}

		/* get URL portion */
		url = g_strndup (headerpos, end - headerpos);

		if (strncmp (url, "mailto:", 6) == 0) {
			if (emla_action_headers[t].interactive)
				send_message_response = GTK_RESPONSE_NO;
			else
				send_message_response = e_alert_run_dialog_for_args (
					window, MESSAGE_ASK_SEND_MESSAGE,
					url, NULL);

			if (send_message_response == GTK_RESPONSE_YES) {
				/* directly send message */
				composer = e_msg_composer_new_from_url (shell, url);
				if (account != NULL)
					e_composer_header_table_set_account (
						e_msg_composer_get_header_table (composer),
						account);
				e_msg_composer_send (composer);
			} else if (send_message_response == GTK_RESPONSE_NO) {
				/* show composer */
				em_utils_compose_new_message_with_mailto (shell, url, folder);
			}

			goto exit;
		} else {
			e_show_uri (window, url);
			goto exit;
		}
		g_free (url);
		url = NULL;
		headerpos = end++;

		/* ignore everything 'till next comma */
		headerpos = strchr (headerpos, ',');
		if (!headerpos)
			break;
		headerpos++;
	}

	/* if we got here, there's no valid action */
	e_alert_run_dialog_for_args (window, MESSAGE_NO_ACTION, header, NULL);

exit:
	g_object_unref (message);
	g_free (url);

	async_context_free (context);
}

static void
emla_list_action (EMailReader *reader,
                  EmlaAction action)
{
	EActivity *activity;
	AsyncContext *context;
	GCancellable *cancellable;
	CamelFolder *folder;
	GPtrArray *uids;
	const gchar *message_uid;

	folder = e_mail_reader_get_folder (reader);
	g_return_if_fail (CAMEL_IS_FOLDER (folder));

	uids = e_mail_reader_get_selected_uids (reader);
	g_return_if_fail (uids != NULL && uids->len == 1);
	message_uid = g_ptr_array_index (uids, 0);

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->reader = g_object_ref (reader);
	context->action = action;

	camel_folder_get_message (
		folder, message_uid, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		emla_list_action_cb, context);

	em_utils_uids_free (uids);
}

static void
action_mailing_list_archive_cb (GtkAction *action,
                                EMailReader *reader)
{
	emla_list_action (reader, EMLA_ACTION_ARCHIVE);
}

static void
action_mailing_list_help_cb (GtkAction *action,
                             EMailReader *reader)
{
	emla_list_action (reader, EMLA_ACTION_HELP);
}

static void
action_mailing_list_owner_cb (GtkAction *action,
                              EMailReader *reader)
{
	emla_list_action (reader, EMLA_ACTION_OWNER);
}

static void
action_mailing_list_post_cb (GtkAction *action,
                             EMailReader *reader)
{
	emla_list_action (reader, EMLA_ACTION_POST);
}

static void
action_mailing_list_subscribe_cb (GtkAction *action,
                                  EMailReader *reader)
{
	emla_list_action (reader, EMLA_ACTION_SUBSCRIBE);
}

static void
action_mailing_list_unsubscribe_cb (GtkAction *action,
                                    EMailReader *reader)
{
	emla_list_action (reader, EMLA_ACTION_UNSUBSCRIBE);
}

static GtkActionEntry mailing_list_entries[] = {

	{ "mailing-list-archive",
	  NULL,
	  N_("Get List _Archive"),
	  NULL,
	  N_("Get an archive of the list this message belongs to"),
	  G_CALLBACK (action_mailing_list_archive_cb) },

	{ "mailing-list-help",
	  NULL,
	  N_("Get List _Usage Information"),
	  NULL,
	  N_("Get information about the usage of the list this message belongs to"),
	  G_CALLBACK (action_mailing_list_help_cb) },

	{ "mailing-list-owner",
	  NULL,
	  N_("Contact List _Owner"),
	  NULL,
	  N_("Contact the owner of the mailing list this message belongs to"),
	  G_CALLBACK (action_mailing_list_owner_cb) },

	{ "mailing-list-post",
	  NULL,
	  N_("_Post Message to List"),
	  NULL,
	  N_("Post a message to the mailing list this message belongs to"),
	  G_CALLBACK (action_mailing_list_post_cb) },

	{ "mailing-list-subscribe",
	  NULL,
	  N_("_Subscribe to List"),
	  NULL,
	  N_("Subscribe to the mailing list this message belongs to"),
	  G_CALLBACK (action_mailing_list_subscribe_cb) },

	{ "mailing-list-unsubscribe",
	  NULL,
	  N_("_Unsubscribe from List"),
	  NULL,
	  N_("Unsubscribe from the mailing list this message belongs to"),
	  G_CALLBACK (action_mailing_list_unsubscribe_cb) },

	/*** Menus ***/

	{ "mailing-list-menu",
	  NULL,
	  N_("Mailing _List"),
	  NULL,
	  NULL,
	  NULL }
};

static void
update_actions_cb (EMailReader *reader,
                   guint32 state,
                   GtkActionGroup *action_group)
{
	gboolean sensitive;

	sensitive = (state & E_MAIL_READER_SELECTION_IS_MAILING_LIST) != 0
		 && (state & E_MAIL_READER_SELECTION_SINGLE) != 0;
	gtk_action_group_set_sensitive (action_group, sensitive);
}

static void
setup_actions (EMailReader *reader,
               GtkUIManager *ui_manager)
{
	GtkActionGroup *action_group;
	const gchar *domain = GETTEXT_PACKAGE;

	action_group = gtk_action_group_new ("mailing-list");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_action_group_add_actions (
		action_group, mailing_list_entries,
		G_N_ELEMENTS (mailing_list_entries), reader);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	/* GtkUIManager now owns the action group reference.
	 * The signal we're connecting to will only be emitted
	 * during the GtkUIManager's lifetime, so the action
	 * group will not disappear on us. */

	g_signal_connect (
		reader, "update-actions",
		G_CALLBACK (update_actions_cb), action_group);
}

gboolean
mail_browser_init (GtkUIManager *ui_manager,
                   EMailBrowser *browser)
{
	setup_actions (E_MAIL_READER (browser), ui_manager);

	return TRUE;
}

gboolean
mail_shell_view_init (GtkUIManager *ui_manager,
                      EShellView *shell_view)
{
	EShellContent *shell_content;

	shell_content = e_shell_view_get_shell_content (shell_view);

	setup_actions (E_MAIL_READER (shell_content), ui_manager);

	return TRUE;
}

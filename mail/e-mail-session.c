/*
 * e-mail-session.c
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
 *   Jonathon Jongsma <jonathon.jongsma@collabora.co.uk>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 * Copyright (C) 2009 Intel Corporation
 *
 */

/* mail-session.c: handles the session information and resource manipulation */

#include <config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <gtk/gtk.h>

#include <gconf/gconf-client.h>

#ifdef HAVE_CANBERRA
#include <canberra-gtk.h>
#endif

#include <libedataserverui/e-passwords.h>
#include <libedataserver/e-flag.h>

#include "e-util/e-util.h"
#include "e-util/e-alert-dialog.h"
#include "e-util/e-util-private.h"

#include "e-mail-local.h"
#include "e-mail-session.h"
#include "em-composer-utils.h"
#include "em-filter-context.h"
#include "em-filter-rule.h"
#include "em-utils.h"
#include "mail-config.h"
#include "mail-folder-cache.h"
#include "mail-mt.h"
#include "mail-ops.h"
#include "mail-send-recv.h"
#include "mail-tools.h"

#define E_MAIL_SESSION_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_MAIL_SESSION, EMailSessionPrivate))

static guint session_check_junk_notify_id;
static guint session_gconf_proxy_id;

typedef struct _AsyncContext AsyncContext;

struct _EMailSessionPrivate {
	FILE *filter_logfile;
	GList *junk_plugins;
};

struct _AsyncContext {
	/* arguments */
	CamelStoreGetFolderFlags flags;
	gchar *uri;

	/* results */
	CamelFolder *folder;
};

static gchar *mail_data_dir;
static gchar *mail_config_dir;

static MailMsgInfo ms_thread_info_dummy = { sizeof (MailMsg) };

G_DEFINE_TYPE (
	EMailSession,
	e_mail_session,
	CAMEL_TYPE_SESSION)

/* Support for CamelSession.alert_user() *************************************/

static gpointer user_message_dialog;
static GQueue user_message_queue = { NULL, NULL, 0 };

struct _user_message_msg {
	MailMsg base;

	CamelSessionAlertType type;
	gchar *prompt;
	EFlag *done;

	guint allow_cancel:1;
	guint result:1;
	guint ismain:1;
};

static void user_message_exec (struct _user_message_msg *m);

static void
user_message_response_free (GtkDialog *dialog,
                            gint button,
                            struct _user_message_msg *m)
{
	gtk_widget_destroy ((GtkWidget *) dialog);

	user_message_dialog = NULL;

	/* check for pendings */
	if (!g_queue_is_empty (&user_message_queue)) {
		m = g_queue_pop_head (&user_message_queue);
		user_message_exec (m);
		mail_msg_unref (m);
	}
}

/* clicked, send back the reply */
static void
user_message_response (GtkDialog *dialog,
                       gint button,
                       struct _user_message_msg *m)
{
	/* if !allow_cancel, then we've already replied */
	if (m->allow_cancel) {
		m->result = button == GTK_RESPONSE_OK;
		e_flag_set (m->done);
	}

	user_message_response_free (dialog, button, m);
}

static void
user_message_exec (struct _user_message_msg *m)
{
	GtkWindow *parent;
	const gchar *error_type;

	if (!m->ismain && user_message_dialog != NULL) {
		g_queue_push_tail (&user_message_queue, mail_msg_ref (m));
		return;
	}

	switch (m->type) {
		case CAMEL_SESSION_ALERT_INFO:
			error_type = m->allow_cancel ?
				"mail:session-message-info-cancel" :
				"mail:session-message-info";
			break;
		case CAMEL_SESSION_ALERT_WARNING:
			error_type = m->allow_cancel ?
				"mail:session-message-warning-cancel" :
				"mail:session-message-warning";
			break;
		case CAMEL_SESSION_ALERT_ERROR:
			error_type = m->allow_cancel ?
				"mail:session-message-error-cancel" :
				"mail:session-message-error";
			break;
		default:
			error_type = NULL;
			g_return_if_reached ();
	}

	/* Pull in the active window from the shell to get a parent window */
	parent = e_shell_get_active_window (e_shell_get_default ());
	user_message_dialog = e_alert_dialog_new_for_args (
		parent, error_type, m->prompt, NULL);
	g_object_set (
		user_message_dialog, "allow_shrink", TRUE,
		"allow_grow", TRUE, NULL);

	/* Use the number of dialog buttons as a heuristic for whether to
	 * emit a status bar message or present the dialog immediately, the
	 * thought being if there's more than one button then something is
	 * probably blocked until the user responds. */
	if (e_alert_dialog_count_buttons (user_message_dialog) > 1) {
		if (m->ismain) {
			gint response;

			response = gtk_dialog_run (user_message_dialog);
			user_message_response (
				user_message_dialog, response, m);
		} else {
			g_signal_connect (
				user_message_dialog, "response",
				G_CALLBACK (user_message_response), m);
			gtk_widget_show (user_message_dialog);
		}
	} else {
		g_signal_connect (
			user_message_dialog, "response",
			G_CALLBACK (user_message_response_free), m);
		g_object_set_data (
			user_message_dialog, "response-handled",
			GINT_TO_POINTER (TRUE));
		em_utils_show_error_silent (user_message_dialog);
	}
}

static void
user_message_free (struct _user_message_msg *m)
{
	g_free (m->prompt);
	e_flag_free (m->done);
}

static MailMsgInfo user_message_info = {
	sizeof (struct _user_message_msg),
	(MailMsgDescFunc) NULL,
	(MailMsgExecFunc) user_message_exec,
	(MailMsgDoneFunc) NULL,
	(MailMsgFreeFunc) user_message_free
};

/* Support for CamelSession.get_filter_driver () *****************************/

static CamelFolder *
get_folder (CamelFilterDriver *d,
            const gchar *uri,
            gpointer user_data,
            GError **error)
{
	EMailSession *session = E_MAIL_SESSION (user_data);

	/* FIXME Not passing a GCancellable here. */
	/* FIXME Need a camel_filter_driver_get_session(). */
	return e_mail_session_uri_to_folder_sync (
		session, uri, 0, NULL, error);
}

static gboolean
session_play_sound_cb (const gchar *filename)
{
#ifdef HAVE_CANBERRA
	if (filename != NULL && *filename != '\0')
		ca_context_play (
			ca_gtk_context_get (), 0,
			CA_PROP_MEDIA_FILENAME, filename,
			NULL);
	else
#endif
		gdk_beep ();

	return FALSE;
}

static void
session_play_sound (CamelFilterDriver *driver,
                    const gchar *filename,
                    gpointer user_data)
{
	g_idle_add_full (
		G_PRIORITY_DEFAULT_IDLE,
		(GSourceFunc) session_play_sound_cb,
		g_strdup (filename), (GDestroyNotify) g_free);
}

static void
session_system_beep (CamelFilterDriver *driver,
                     gpointer user_data)
{
	g_idle_add ((GSourceFunc) session_play_sound_cb, NULL);
}

static CamelFilterDriver *
main_get_filter_driver (CamelSession *session,
                        const gchar *type,
                        GError **error)
{
	EMailSession *ms = E_MAIL_SESSION (session);
	CamelFilterDriver *driver;
	EFilterRule *rule = NULL;
	const gchar *config_dir;
	gchar *user, *system;
	GConfClient *gconf;
	ERuleContext *fc;

	gconf = mail_config_get_gconf_client ();

	config_dir = mail_session_get_config_dir ();
	user = g_build_filename (config_dir, "filters.xml", NULL);
	system = g_build_filename (EVOLUTION_PRIVDATADIR, "filtertypes.xml", NULL);
	fc = (ERuleContext *) em_filter_context_new (ms);
	e_rule_context_load (fc, system, user);
	g_free (system);
	g_free (user);

	driver = camel_filter_driver_new (session);
	camel_filter_driver_set_folder_func (driver, get_folder, session);

	if (gconf_client_get_bool (gconf, "/apps/evolution/mail/filters/log", NULL)) {
		if (ms->priv->filter_logfile == NULL) {
			gchar *filename;

			filename = gconf_client_get_string (gconf, "/apps/evolution/mail/filters/logfile", NULL);
			if (filename) {
				ms->priv->filter_logfile = g_fopen (filename, "a+");
				g_free (filename);
			}
		}

		if (ms->priv->filter_logfile)
			camel_filter_driver_set_logfile (driver, ms->priv->filter_logfile);
	}

	camel_filter_driver_set_shell_func (driver, mail_execute_shell_command, NULL);
	camel_filter_driver_set_play_sound_func (driver, session_play_sound, NULL);
	camel_filter_driver_set_system_beep_func (driver, session_system_beep, NULL);

	if ((!strcmp (type, E_FILTER_SOURCE_INCOMING) || !strcmp (type, E_FILTER_SOURCE_JUNKTEST))
	    && camel_session_get_check_junk (session)) {
		/* implicit junk check as 1st rule */
		camel_filter_driver_add_rule (driver, "Junk check", "(junk-test)", "(begin (set-system-flag \"junk\"))");
	}

	if (strcmp (type, E_FILTER_SOURCE_JUNKTEST) != 0) {
		GString *fsearch, *faction;

		fsearch = g_string_new ("");
		faction = g_string_new ("");

		if (!strcmp (type, E_FILTER_SOURCE_DEMAND))
			type = E_FILTER_SOURCE_INCOMING;

		/* add the user-defined rules next */
		while ((rule = e_rule_context_next_rule (fc, rule, type))) {
			g_string_truncate (fsearch, 0);
			g_string_truncate (faction, 0);

			/* skip disabled rules */
			if (!rule->enabled)
				continue;

			e_filter_rule_build_code (rule, fsearch);
			em_filter_rule_build_action ((EMFilterRule *) rule, faction);
			camel_filter_driver_add_rule (driver, rule->name, fsearch->str, faction->str);
		}

		g_string_free (fsearch, TRUE);
		g_string_free (faction, TRUE);
	}

	g_object_unref (fc);

	return driver;
}

/* Support for CamelSession.forward_to () ************************************/

static guint preparing_flush = 0;

static gboolean
forward_to_flush_outbox_cb (EMailSession *session)
{
	g_return_val_if_fail (preparing_flush != 0, FALSE);

	preparing_flush = 0;
	mail_send (session);

	return FALSE;
}

static void
ms_forward_to_cb (CamelFolder *folder,
                  CamelMimeMessage *msg,
                  CamelMessageInfo *info,
                  gint queued,
                  const gchar *appended_uid,
                  gpointer data)
{
	EMailSession *session = E_MAIL_SESSION (data);

	camel_message_info_free (info);

	/* do not call mail send immediately, just pile them all in the outbox */
	if (preparing_flush ||
	    gconf_client_get_bool (mail_config_get_gconf_client (), "/apps/evolution/mail/filters/flush-outbox", NULL)) {
		if (preparing_flush)
			g_source_remove (preparing_flush);

		preparing_flush = g_timeout_add_seconds (
			60, (GSourceFunc)
			forward_to_flush_outbox_cb, session);
	}
}

/* Support for SOCKS proxy ***************************************************/

#define DIR_PROXY "/system/proxy"
#define MODE_PROXY "/system/proxy/mode"
#define KEY_SOCKS_HOST "/system/proxy/socks_host"
#define KEY_SOCKS_PORT "/system/proxy/socks_port"

static void
set_socks_proxy_from_gconf (CamelSession *session)
{
	GConfClient *client;
	gchar *mode, *host;
	gint port;

	client = mail_config_get_gconf_client ();

	mode = gconf_client_get_string (client, MODE_PROXY, NULL);
	if (!g_strcmp0(mode, "manual")) {
		host = gconf_client_get_string (client, KEY_SOCKS_HOST, NULL); /* NULL-GError */
		port = gconf_client_get_int (client, KEY_SOCKS_PORT, NULL); /* NULL-GError */
		camel_session_set_socks_proxy (session, host, port);
		g_free (host);
	}
	g_free (mode);
}

static void
proxy_gconf_notify_cb (GConfClient* client,
                       guint cnxn_id,
                       GConfEntry *entry,
                       gpointer user_data)
{
	CamelSession *session = CAMEL_SESSION (user_data);
	const gchar *key;

	key = gconf_entry_get_key (entry);

	if (strcmp (entry->key, KEY_SOCKS_HOST) == 0
	    || strcmp (entry->key, KEY_SOCKS_PORT) == 0)
		set_socks_proxy_from_gconf (session);
}

static void
set_socks_proxy_gconf_watch (CamelSession *session)
{
	GConfClient *client;

	client = mail_config_get_gconf_client ();

	gconf_client_add_dir (
		client, DIR_PROXY,
		GCONF_CLIENT_PRELOAD_ONELEVEL, NULL); /* NULL-GError */
	session_gconf_proxy_id = gconf_client_notify_add (
		client, DIR_PROXY, proxy_gconf_notify_cb,
		session, NULL, NULL); /* NULL-GError */
}

static void
init_socks_proxy (CamelSession *session)
{
	g_return_if_fail (CAMEL_IS_SESSION (session));

	set_socks_proxy_gconf_watch (session);
	set_socks_proxy_from_gconf (session);
}

/*****************************************************************************/

static void
async_context_free (AsyncContext *context)
{
	if (context->folder != NULL)
		g_object_unref (context->folder);

	g_free (context->uri);

	g_slice_free (AsyncContext, context);
}

static gchar *
mail_session_make_key (CamelService *service,
                       const gchar *item)
{
	gchar *key;

	if (service != NULL)
		key = camel_url_to_string (
			service->url,
			CAMEL_URL_HIDE_PASSWORD |
			CAMEL_URL_HIDE_PARAMS);
	else
		key = g_strdup (item);

	return key;
}

static void
mail_session_check_junk_notify (GConfClient *gconf,
                                guint id,
                                GConfEntry *entry,
                                CamelSession *session)
{
	gchar *key;

	g_return_if_fail (gconf_entry_get_key (entry) != NULL);
	g_return_if_fail (gconf_entry_get_value (entry) != NULL);

	key = strrchr (gconf_entry_get_key (entry), '/');
	if (key) {
		key++;
		if (strcmp (key, "check_incoming") == 0)
			camel_session_set_check_junk (
				session, gconf_value_get_bool (
				gconf_entry_get_value (entry)));
	}
}

static void
mail_session_finalize (GObject *object)
{
	GConfClient *client;

	client = mail_config_get_gconf_client ();

	if (session_check_junk_notify_id != 0) {
		gconf_client_notify_remove (client, session_check_junk_notify_id);
		session_check_junk_notify_id = 0;
	}

	if (session_gconf_proxy_id != 0) {
		gconf_client_notify_remove (client, session_gconf_proxy_id);
		session_gconf_proxy_id = 0;
	}

	g_free (mail_data_dir);
	g_free (mail_config_dir);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_mail_session_parent_class)->finalize (object);
}

static gchar *
mail_session_get_password (CamelSession *session,
                           CamelService *service,
                           const gchar *domain,
                           const gchar *prompt,
                           const gchar *item,
                           guint32 flags,
                           GError **error)
{
	gchar *url;
	gchar *ret = NULL;
	EAccount *account = NULL;

	url = service?camel_url_to_string (service->url, CAMEL_URL_HIDE_ALL):NULL;

	if (!strcmp(item, "popb4smtp_uri")) {
		/* not 100% mt safe, but should be ok */
		if (url
		    && (account = mail_config_get_account_by_transport_url (url)))
			ret = g_strdup (account->source->url);
		else
			ret = g_strdup (url);
	} else {
		gchar *key = mail_session_make_key (service, item);
		EAccountService *config_service = NULL;

		if (domain == NULL)
			domain = "Mail";

		ret = e_passwords_get_password (domain, key);
		if (ret == NULL || (flags & CAMEL_SESSION_PASSWORD_REPROMPT)) {
			gboolean remember;

			if (url) {
				if  ((account = mail_config_get_account_by_source_url (url)))
					config_service = account->source;
				else if ((account = mail_config_get_account_by_transport_url (url)))
					config_service = account->transport;
			}

			remember = config_service?config_service->save_passwd:FALSE;

			if (!config_service || (config_service && !config_service->get_password_canceled)) {
				guint32 eflags;
				gchar *title;

				if (flags & CAMEL_SESSION_PASSPHRASE) {
					if (account)
						title = g_strdup_printf (_("Enter Passphrase for %s"), account->name);
					else
						title = g_strdup (_("Enter Passphrase"));
				} else {
					if (account)
						title = g_strdup_printf (_("Enter Password for %s"), account->name);
					else
						title = g_strdup (_("Enter Password"));
				}
				if ((flags & CAMEL_SESSION_PASSWORD_STATIC) != 0)
					eflags = E_PASSWORDS_REMEMBER_NEVER;
				else if (config_service == NULL)
					eflags = E_PASSWORDS_REMEMBER_SESSION;
				else
					eflags = E_PASSWORDS_REMEMBER_FOREVER;

				if (flags & CAMEL_SESSION_PASSWORD_REPROMPT)
					eflags |= E_PASSWORDS_REPROMPT;

				if (flags & CAMEL_SESSION_PASSWORD_SECRET)
					eflags |= E_PASSWORDS_SECRET;

				if (flags & CAMEL_SESSION_PASSPHRASE)
					eflags |= E_PASSWORDS_PASSPHRASE;

				/* HACK: breaks abstraction ...
				   e_account_writable doesn't use the eaccount, it also uses the same writable key for
				   source and transport */
				if (!e_account_writable (NULL, E_ACCOUNT_SOURCE_SAVE_PASSWD))
					eflags |= E_PASSWORDS_DISABLE_REMEMBER;

				ret = e_passwords_ask_password (title, domain, key, prompt, eflags, &remember, NULL);

				g_free (title);

				if (ret && config_service)
					mail_config_service_set_save_passwd (config_service, remember);

				if (config_service)
					config_service->get_password_canceled = ret == NULL;
			}
		}

		g_free (key);
	}

	g_free (url);

	if (ret == NULL)
		g_set_error (
			error, G_IO_ERROR,
			G_IO_ERROR_CANCELLED,
			_("User canceled operation."));

	return ret;
}

static gboolean
mail_session_forget_password (CamelSession *session,
                              CamelService *service,
                              const gchar *domain,
                              const gchar *item,
                              GError **error)
{
	gchar *key;

	domain = (domain != NULL) ? domain : "Mail";
	key = mail_session_make_key (service, item);

	e_passwords_forget_password (domain, key);

	g_free (key);

	return TRUE;
}

static gboolean
mail_session_alert_user (CamelSession *session,
                         CamelSessionAlertType type,
                         const gchar *prompt,
                         gboolean cancel)
{
	struct _user_message_msg *m;
	gboolean result = TRUE;

	m = mail_msg_new (&user_message_info);
	m->ismain = mail_in_main_thread ();
	m->type = type;
	m->prompt = g_strdup (prompt);
	m->done = e_flag_new ();
	m->allow_cancel = cancel;

	if (cancel)
		mail_msg_ref (m);

	if (m->ismain)
		user_message_exec (m);
	else
		mail_msg_main_loop_push (m);

	if (cancel) {
		e_flag_wait (m->done);
		result = m->result;
		mail_msg_unref (m);
	} else if (m->ismain)
		mail_msg_unref (m);

	return result;
}

static CamelFilterDriver *
mail_session_get_filter_driver (CamelSession *session,
                                const gchar *type,
                                GError **error)
{
	return (CamelFilterDriver *) mail_call_main (
		MAIL_CALL_p_ppp, (MailMainFunc) main_get_filter_driver,
		session, type, error);
}

static gboolean
mail_session_lookup_addressbook (CamelSession *session,
                                 const gchar *name)
{
	CamelInternetAddress *addr;
	gboolean ret;

	if (!mail_config_get_lookup_book ())
		return FALSE;

	addr = camel_internet_address_new ();
	camel_address_decode ((CamelAddress *)addr, name);
	ret = em_utils_in_addressbook (
		addr, mail_config_get_lookup_book_local_only ());
	g_object_unref (addr);

	return ret;
}

static gpointer
mail_session_thread_msg_new (CamelSession *session,
                             CamelSessionThreadOps *ops,
                             guint size)
{
	CamelSessionThreadMsg *msg;
	CamelSessionClass *session_class;

	/* TODO This is very temporary, until we have a better way to do
	 *      the progress reporting, we just borrow a dummy mail-mt
	 *      thread message and hook it onto out camel thread message. */

	/* Chain up to parent's thread_msg_new() method. */
	session_class = CAMEL_SESSION_CLASS (e_mail_session_parent_class);
	msg = session_class->thread_msg_new (session, ops, size);

	/* We create a dummy mail_msg, and then copy its cancellation
	 * port over to ours, so we get cancellation and progress in
	 * common with hte existing mail code, for free. */
	if (msg) {
		MailMsg *m = mail_msg_new (&ms_thread_info_dummy);

		msg->data = m;
		g_object_unref (msg->cancellable);
		msg->cancellable = g_object_ref (m->cancellable);
	}

	return msg;
}

static void
mail_session_thread_msg_free (CamelSession *session,
                              CamelSessionThreadMsg *msg)
{
	CamelSessionClass *session_class;

	mail_msg_unref (msg->data);

	/* Chain up to parent's thread_msg_free() method. */
	session_class = CAMEL_SESSION_CLASS (e_mail_session_parent_class);
	session_class->thread_msg_free (session, msg);
}

static void
mail_session_thread_status (CamelSession *session,
                            CamelSessionThreadMsg *msg,
                            const gchar *text,
                            gint pc)
{
	/* This should never be called since we bypass it in alloc! */
	g_warn_if_reached ();
}

static gboolean
mail_session_forward_to (CamelSession *session,
                         CamelFolder *folder,
                         CamelMimeMessage *message,
                         const gchar *address,
                         GError **error)
{
	EAccount *account;
	CamelMimeMessage *forward;
	CamelStream *mem;
	CamelInternetAddress *addr;
	CamelFolder *out_folder;
	CamelMessageInfo *info;
	struct _camel_header_raw *xev;
	gchar *subject;

	g_return_val_if_fail (folder != NULL, FALSE);
	g_return_val_if_fail (message != NULL, FALSE);
	g_return_val_if_fail (address != NULL, FALSE);

	if (!*address) {
		g_set_error (
			error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
			_("No destination address provided, forward "
			  "of the message has been cancelled."));
		return FALSE;
	}

	account = em_utils_guess_account_with_recipients (message, folder);
	if (!account) {
		g_set_error (
			error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
			_("No account found to use, forward of the "
			  "message has been cancelled."));
		return FALSE;
	}

	forward = camel_mime_message_new ();

	/* make copy of the message, because we are going to modify it */
	mem = camel_stream_mem_new ();
	camel_data_wrapper_write_to_stream_sync ((CamelDataWrapper *)message, mem, NULL, NULL);
	camel_seekable_stream_seek (CAMEL_SEEKABLE_STREAM (mem), 0, CAMEL_STREAM_SET, NULL);
	camel_data_wrapper_construct_from_stream_sync ((CamelDataWrapper *)forward, mem, NULL, NULL);
	g_object_unref (mem);

	/* clear previous recipients */
	camel_mime_message_set_recipients (forward, CAMEL_RECIPIENT_TYPE_TO, NULL);
	camel_mime_message_set_recipients (forward, CAMEL_RECIPIENT_TYPE_CC, NULL);
	camel_mime_message_set_recipients (forward, CAMEL_RECIPIENT_TYPE_BCC, NULL);
	camel_mime_message_set_recipients (forward, CAMEL_RECIPIENT_TYPE_RESENT_TO, NULL);
	camel_mime_message_set_recipients (forward, CAMEL_RECIPIENT_TYPE_RESENT_CC, NULL);
	camel_mime_message_set_recipients (forward, CAMEL_RECIPIENT_TYPE_RESENT_BCC, NULL);

	/* remove all delivery and notification headers */
	while (camel_medium_get_header (CAMEL_MEDIUM (forward), "Disposition-Notification-To"))
		camel_medium_remove_header (CAMEL_MEDIUM (forward), "Disposition-Notification-To");

	while (camel_medium_get_header (CAMEL_MEDIUM (forward), "Delivered-To"))
		camel_medium_remove_header (CAMEL_MEDIUM (forward), "Delivered-To");

	/* remove any X-Evolution-* headers that may have been set */
	xev = mail_tool_remove_xevolution_headers (forward);
	camel_header_raw_clear (&xev);

	/* from */
	addr = camel_internet_address_new ();
	camel_internet_address_add (addr, account->id->name, account->id->address);
	camel_mime_message_set_from (forward, addr);
	g_object_unref (addr);

	/* to */
	addr = camel_internet_address_new ();
	camel_address_decode (CAMEL_ADDRESS (addr), address);
	camel_mime_message_set_recipients (forward, CAMEL_RECIPIENT_TYPE_TO, addr);
	g_object_unref (addr);

	/* subject */
	subject = mail_tool_generate_forward_subject (message);
	camel_mime_message_set_subject (forward, subject);
	g_free (subject);

	/* and send it */
	info = camel_message_info_new (NULL);
	out_folder = e_mail_local_get_folder (E_MAIL_FOLDER_OUTBOX);
	camel_message_info_set_flags (
		info, CAMEL_MESSAGE_SEEN, CAMEL_MESSAGE_SEEN);
	mail_append_mail (
		out_folder, forward, info, ms_forward_to_cb, session);

	return TRUE;
}

static void
e_mail_session_class_init (EMailSessionClass *class)
{
	GObjectClass *object_class;
	CamelSessionClass *session_class;

	g_type_class_add_private (class, sizeof (EMailSessionPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->finalize = mail_session_finalize;

	session_class = CAMEL_SESSION_CLASS (class);
	session_class->get_password = mail_session_get_password;
	session_class->forget_password = mail_session_forget_password;
	session_class->alert_user = mail_session_alert_user;
	session_class->get_filter_driver = mail_session_get_filter_driver;
	session_class->lookup_addressbook = mail_session_lookup_addressbook;
	session_class->thread_msg_new = mail_session_thread_msg_new;
	session_class->thread_msg_free = mail_session_thread_msg_free;
	session_class->thread_status = mail_session_thread_status;
	session_class->forward_to = mail_session_forward_to;
}

static void
e_mail_session_init (EMailSession *session)
{
	GConfClient *client;

	session->priv = E_MAIL_SESSION_GET_PRIVATE (session);

	/* Initialize the EAccount setup. */
	e_account_writable (NULL, E_ACCOUNT_SOURCE_SAVE_PASSWD);

	camel_session_construct (
		CAMEL_SESSION (session),
		mail_session_get_data_dir ());

	client = gconf_client_get_default ();

	gconf_client_add_dir (
		client, "/apps/evolution/mail/junk",
		GCONF_CLIENT_PRELOAD_ONELEVEL, NULL);
	camel_session_set_check_junk (
		CAMEL_SESSION (session), gconf_client_get_bool (
		client, "/apps/evolution/mail/junk/check_incoming", NULL));
	session_check_junk_notify_id = gconf_client_notify_add (
		client, "/apps/evolution/mail/junk",
		(GConfClientNotifyFunc) mail_session_check_junk_notify,
		session, NULL, NULL);
	CAMEL_SESSION (session)->junk_plugin = NULL;

	mail_config_reload_junk_headers (CAMEL_SESSION (session));

	init_socks_proxy (CAMEL_SESSION (session));

	g_object_unref (client);
}

EMailSession *
e_mail_session_new (void)
{
	return g_object_new (E_TYPE_MAIL_SESSION, NULL);
}

static void
mail_session_get_inbox_thread (GSimpleAsyncResult *simple,
                               EMailSession *session,
                               GCancellable *cancellable)
{
	AsyncContext *context;
	GError *error = NULL;

	context = g_simple_async_result_get_op_res_gpointer (simple);

	context->folder = e_mail_session_get_inbox_sync (
		session, context->uri, cancellable, &error);

	if (error != NULL) {
		g_simple_async_result_set_from_error (simple, error);
		g_error_free (error);
	}
}

CamelFolder *
e_mail_session_get_inbox_sync (EMailSession *session,
                               const gchar *service_uri,
                               GCancellable *cancellable,
                               GError **error)
{
	CamelStore *store;
	CamelFolder *folder;

	g_return_val_if_fail (E_IS_MAIL_SESSION (session), NULL);
	g_return_val_if_fail (service_uri != NULL, NULL);

	store = camel_session_get_store (
		CAMEL_SESSION (session), service_uri, error);

	if (store == NULL)
		return NULL;

	folder = camel_store_get_inbox_folder_sync (store, cancellable, error);

	g_object_unref (store);

	return folder;
}

void
e_mail_session_get_inbox (EMailSession *session,
                          const gchar *service_uri,
                          gint io_priority,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
	GSimpleAsyncResult *simple;
	AsyncContext *context;

	g_return_if_fail (E_IS_MAIL_SESSION (session));
	g_return_if_fail (service_uri != NULL);

	context = g_slice_new0 (AsyncContext);
	context->uri = g_strdup (service_uri);

	simple = g_simple_async_result_new (
		G_OBJECT (session), callback,
		user_data, e_mail_session_get_inbox);

	g_simple_async_result_set_op_res_gpointer (
		simple, context, (GDestroyNotify) async_context_free);

	g_simple_async_result_run_in_thread (
		simple, (GSimpleAsyncThreadFunc)
		mail_session_get_inbox_thread,
		io_priority, cancellable);

	g_object_unref (simple);
}

CamelFolder *
e_mail_session_get_inbox_finish (EMailSession *session,
                                 GAsyncResult *result,
                                 GError **error)
{
	GSimpleAsyncResult *simple;
	AsyncContext *context;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (session),
		e_mail_session_get_inbox), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	context = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	g_return_val_if_fail (CAMEL_IS_FOLDER (context->folder), NULL);

	return g_object_ref (context->folder);
}

static void
mail_session_get_trash_thread (GSimpleAsyncResult *simple,
                               EMailSession *session,
                               GCancellable *cancellable)
{
	AsyncContext *context;
	GError *error = NULL;

	context = g_simple_async_result_get_op_res_gpointer (simple);

	context->folder = e_mail_session_get_trash_sync (
		session, context->uri, cancellable, &error);

	if (error != NULL) {
		g_simple_async_result_set_from_error (simple, error);
		g_error_free (error);
	}
}

CamelFolder *
e_mail_session_get_trash_sync (EMailSession *session,
                               const gchar *service_uri,
                               GCancellable *cancellable,
                               GError **error)
{
	CamelStore *store;
	CamelFolder *folder;

	g_return_val_if_fail (E_IS_MAIL_SESSION (session), NULL);
	g_return_val_if_fail (service_uri != NULL, NULL);

	store = camel_session_get_store (
		CAMEL_SESSION (session), service_uri, error);

	if (store == NULL)
		return NULL;

	folder = camel_store_get_trash_folder_sync (store, cancellable, error);

	g_object_unref (store);

	return folder;
}

void
e_mail_session_get_trash (EMailSession *session,
                          const gchar *service_uri,
                          gint io_priority,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
	GSimpleAsyncResult *simple;
	AsyncContext *context;

	g_return_if_fail (E_IS_MAIL_SESSION (session));
	g_return_if_fail (service_uri != NULL);

	context = g_slice_new0 (AsyncContext);
	context->uri = g_strdup (service_uri);

	simple = g_simple_async_result_new (
		G_OBJECT (session), callback,
		user_data, e_mail_session_get_trash);

	g_simple_async_result_set_op_res_gpointer (
		simple, context, (GDestroyNotify) async_context_free);

	g_simple_async_result_run_in_thread (
		simple, (GSimpleAsyncThreadFunc)
		mail_session_get_trash_thread,
		io_priority, cancellable);

	g_object_unref (simple);
}

CamelFolder *
e_mail_session_get_trash_finish (EMailSession *session,
                                 GAsyncResult *result,
                                 GError **error)
{
	GSimpleAsyncResult *simple;
	AsyncContext *context;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (session),
		e_mail_session_get_trash), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	context = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	g_return_val_if_fail (CAMEL_IS_FOLDER (context->folder), NULL);

	return g_object_ref (context->folder);
}

static void
mail_session_uri_to_folder_thread (GSimpleAsyncResult *simple,
                                   EMailSession *session,
                                   GCancellable *cancellable)
{
	AsyncContext *context;
	GError *error = NULL;

	context = g_simple_async_result_get_op_res_gpointer (simple);

	context->folder = e_mail_session_uri_to_folder_sync (
		session, context->uri, context->flags,
		cancellable, &error);

	if (error != NULL) {
		g_simple_async_result_set_from_error (simple, error);
		g_error_free (error);
	}
}

CamelFolder *
e_mail_session_uri_to_folder_sync (EMailSession *session,
                                   const gchar *folder_uri,
                                   CamelStoreGetFolderFlags flags,
                                   GCancellable *cancellable,
                                   GError **error)
{
	CamelURL *url;
	CamelStore *store;
	CamelFolder *folder = NULL;
	gchar *camel_uri = NULL;
	gboolean vtrash = FALSE;
	gboolean vjunk = FALSE;

	g_return_val_if_fail (E_IS_MAIL_SESSION (session), NULL);
	g_return_val_if_fail (folder_uri != NULL, NULL);

	camel_operation_push_message (
		cancellable, _("Opening folder '%s'"), folder_uri);

	/* FIXME vtrash and vjunk are no longer used for these URI's. */
	if (g_str_has_prefix (folder_uri, "vtrash:")) {
		folder_uri += 7;
		vtrash = TRUE;
	} else if (g_str_has_prefix (folder_uri, "vjunk:")) {
		folder_uri += 6;
		vjunk = TRUE;
	} else if (g_str_has_prefix (folder_uri, "email:")) {
		/* FIXME Shouldn't the filter:get_folder
		 *       callback do this itself? */
		camel_uri = em_uri_to_camel (folder_uri);
		if (camel_uri == NULL) {
			g_set_error (
				error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
				_("Invalid folder: %s"), folder_uri);
			goto exit;
		}
		folder_uri = camel_uri;
	}

	url = camel_url_new (folder_uri, error);

	if (url == NULL) {
		g_free (camel_uri);
		goto exit;
	}

	store = (CamelStore *) camel_session_get_service (
		CAMEL_SESSION (session), folder_uri,
		CAMEL_PROVIDER_STORE, error);

	if (store != NULL) {
		const gchar *name = "";

		/* If we have a fragment, then the path is actually
		 * used by the store, so the fragment is the path to
		 * the folder instead. */
		if (url->fragment != NULL)
			name = url->fragment;
		else if (url->path != NULL && *url->path != '\0')
			name = url->path + 1;

		if (vtrash)
			folder = camel_store_get_trash_folder_sync (
				store, cancellable, error);
		else if (vjunk)
			folder = camel_store_get_junk_folder_sync (
				store, cancellable, error);
		else
			folder = camel_store_get_folder_sync (
				store, name, flags, cancellable, error);

		g_object_unref (store);
	}

	if (folder != NULL) {
		MailFolderCache *cache;

		cache = mail_folder_cache_get_default ();
		mail_folder_cache_note_folder (cache, folder);
	}

	camel_url_free (url);
	g_free (camel_uri);

exit:
	camel_operation_pop_message (cancellable);

	return folder;
}

void
e_mail_session_uri_to_folder (EMailSession *session,
                              const gchar *folder_uri,
                              CamelStoreGetFolderFlags flags,
                              gint io_priority,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
	GSimpleAsyncResult *simple;
	AsyncContext *context;

	g_return_if_fail (E_IS_MAIL_SESSION (session));
	g_return_if_fail (folder_uri != NULL);

	context = g_slice_new0 (AsyncContext);
	context->uri = g_strdup (folder_uri);
	context->flags = flags;

	simple = g_simple_async_result_new (
		G_OBJECT (session), callback,
		user_data, e_mail_session_uri_to_folder);

	g_simple_async_result_set_op_res_gpointer (
		simple, context, (GDestroyNotify) async_context_free);

	g_simple_async_result_run_in_thread (
		simple, (GSimpleAsyncThreadFunc)
		mail_session_uri_to_folder_thread,
		io_priority, cancellable);

	g_object_unref (simple);
}

CamelFolder *
e_mail_session_uri_to_folder_finish (EMailSession *session,
                                     GAsyncResult *result,
                                     GError **error)
{
	GSimpleAsyncResult *simple;
	AsyncContext *context;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (session),
		e_mail_session_uri_to_folder), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	context = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	g_return_val_if_fail (CAMEL_IS_FOLDER (context->folder), NULL);

	return g_object_ref (context->folder);
}

/******************************** Legacy API *********************************/

void
mail_session_flush_filter_log (EMailSession *session)
{
	g_return_if_fail (E_IS_MAIL_SESSION (session));

	if (session->priv->filter_logfile)
		fflush (session->priv->filter_logfile);
}

void
mail_session_add_junk_plugin (EMailSession *session,
                              const gchar *plugin_name,
                              CamelJunkPlugin *junk_plugin)
{
	GConfClient *gconf;
	gchar *def_plugin;

	g_return_if_fail (E_IS_MAIL_SESSION (session));

	gconf = mail_config_get_gconf_client ();
	def_plugin = gconf_client_get_string (
		gconf, "/apps/evolution/mail/junk/default_plugin", NULL);

	session->priv->junk_plugins = g_list_append (
		session->priv->junk_plugins, junk_plugin);
	if (def_plugin && plugin_name) {
		if (!strcmp (def_plugin, plugin_name)) {
			CAMEL_SESSION (session)->junk_plugin = junk_plugin;
			camel_junk_plugin_init (junk_plugin);
		}
	}

	g_free (def_plugin);
}

const GList *
mail_session_get_junk_plugins (EMailSession *session)
{
	g_return_val_if_fail (E_IS_MAIL_SESSION (session), NULL);

	return session->priv->junk_plugins;
}

const gchar *
mail_session_get_data_dir (void)
{
	if (G_UNLIKELY (mail_data_dir == NULL))
		mail_data_dir = g_build_filename (
			e_get_user_data_dir (), "mail", NULL);

	return mail_data_dir;
}

const gchar *
mail_session_get_config_dir (void)
{
	if (G_UNLIKELY (mail_config_dir == NULL))
		mail_config_dir = g_build_filename (
			e_get_user_config_dir (), "mail", NULL);

	return mail_config_dir;
}

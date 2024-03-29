/*
 * e-mail-shell-settings.c
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "e-mail-shell-settings.h"

#include <gconf/gconf-client.h>

#include <mail/e-mail-backend.h>
#include <mail/e-mail-label-list-store.h>

#include <shell/e-shell.h>

static gboolean
transform_no_folder_dots_to_ellipsize (GBinding *binding,
                                       const GValue *source_value,
                                       GValue *target_value,
                                       gpointer user_data)
{
	PangoEllipsizeMode ellipsize;

	if (g_value_get_boolean (source_value))
		ellipsize = PANGO_ELLIPSIZE_NONE;
	else
		ellipsize = PANGO_ELLIPSIZE_END;

	g_value_set_enum (target_value, ellipsize);

	return TRUE;
}

void
e_mail_shell_settings_init (EShellBackend *shell_backend)
{
	EShell *shell;
	EShellSettings *shell_settings;
	EMailBackend *backend;
	EMailSession *session;
	gpointer object;

	shell = e_shell_backend_get_shell (shell_backend);
	shell_settings = e_shell_get_shell_settings (shell);

	backend = E_MAIL_BACKEND (shell_backend);
	session = e_mail_backend_get_session (backend);

	/*** Global Objects ***/

	e_shell_settings_install_property (
		g_param_spec_object (
			"mail-label-list-store",
			NULL,
			NULL,
			E_TYPE_MAIL_LABEL_LIST_STORE,
			G_PARAM_READWRITE));

	object = e_mail_label_list_store_new ();
	e_shell_settings_set_object (
		shell_settings, "mail-label-list-store", object);
	g_object_unref (object);

	e_shell_settings_install_property (
		g_param_spec_pointer (
			"mail-session",
			NULL,
			NULL,
			G_PARAM_READWRITE));

	e_shell_settings_set_pointer (
		shell_settings, "mail-session",
		g_object_ref (session));

	/*** Mail Preferences ***/

	e_shell_settings_install_property_for_key (
		"mail-address-compress",
		"/apps/evolution/mail/display/address_compress");

	e_shell_settings_install_property_for_key (
		"mail-address-count",
		"/apps/evolution/mail/display/address_count");

	e_shell_settings_install_property_for_key (
		"mail-charset",
		"/apps/evolution/mail/display/charset");

	e_shell_settings_install_property_for_key (
		"mail-check-for-junk",
		"/apps/evolution/mail/junk/check_incoming");

	e_shell_settings_install_property_for_key (
		"mail-check-on-start",
		"/apps/evolution/mail/send_recv_on_start");

	e_shell_settings_install_property_for_key (
		"mail-check-all-on-start",
		"/apps/evolution/mail/send_recv_all_on_start");

	e_shell_settings_install_property_for_key (
		"mail-citation-color",
		"/apps/evolution/mail/display/citation_colour");

	e_shell_settings_install_property_for_key (
		"mail-confirm-expunge",
		"/apps/evolution/mail/prompts/expunge");

	e_shell_settings_install_property_for_key (
		"mail-confirm-unwanted-html",
		"/apps/evolution/mail/prompts/unwanted_html");

	e_shell_settings_install_property_for_key (
		"mail-empty-junk-on-exit",
		"/apps/evolution/mail/junk/empty_on_exit");

	e_shell_settings_install_property_for_key (
		"mail-empty-trash-on-exit",
		"/apps/evolution/mail/trash/empty_on_exit");

	e_shell_settings_install_property_for_key (
		"mail-enable-search-folders",
		"/apps/evolution/mail/display/enable_vfolders");

	e_shell_settings_install_property_for_key (
		"mail-enable-local-folders",
		"/apps/evolution/mail/display/enable_local");

	e_shell_settings_install_property_for_key (
		"mail-font-monospace",
		"/apps/evolution/mail/display/fonts/monospace");

	e_shell_settings_install_property_for_key (
		"mail-font-variable",
		"/apps/evolution/mail/display/fonts/variable");

	e_shell_settings_install_property_for_key (
		"mail-force-message-limit",
		"/apps/evolution/mail/display/force_message_limit");

	/* This value corresponds to the EMailForwardStyle enum. */
	e_shell_settings_install_property_for_key (
		"mail-forward-style",
		"/apps/evolution/mail/format/forward_style");

	/* This value corresponds to MailConfigHTTPMode enum. */
	e_shell_settings_install_property_for_key (
		"mail-image-loading-policy",
		"/apps/evolution/mail/display/load_http_images");

	e_shell_settings_install_property_for_key (
		"mail-magic-spacebar",
		"/apps/evolution/mail/display/magic_spacebar");

	e_shell_settings_install_property_for_key (
		"mail-global-view-setting",
		"/apps/evolution/mail/display/global_view_setting");

	e_shell_settings_install_property_for_key (
		"mail-mark-citations",
		"/apps/evolution/mail/display/mark_citations");

	e_shell_settings_install_property_for_key (
		"mail-mark-seen",
		"/apps/evolution/mail/display/mark_seen");

	e_shell_settings_install_property_for_key (
		"mail-mark-seen-timeout",
		"/apps/evolution/mail/display/mark_seen_timeout");

	e_shell_settings_install_property_for_key (
		"mail-message-text-part-limit",
		"/apps/evolution/mail/display/message_text_part_limit");

	/* Do not bind to this.  Use "mail-sidebar-ellipsize" instead. */
	e_shell_settings_install_property_for_key (
		"mail-no-folder-dots",
		"/apps/evolution/mail/display/no_folder_dots");

	e_shell_settings_install_property_for_key (
		"mail-only-local-photos",
		"/apps/evolution/mail/display/photo_local");

	e_shell_settings_install_property_for_key (
		"mail-show-real-date",
		"/apps/evolution/mail/display/show_real_date");

	e_shell_settings_install_property_for_key (
		"mail-sort-accounts-alpha",
		"/apps/evolution/mail/display/sort_accounts_alpha");

	e_shell_settings_install_property_for_key (
		"mail-prompt-delete-in-vfolder",
		"/apps/evolution/mail/prompts/delete_in_vfolder");

	/* This value corresponds to the EMailReplyStyle enum,
	 * but the ordering of the combo box items in preferences
	 * has changed.  We use transformation functions there. */
	e_shell_settings_install_property_for_key (
		"mail-reply-style",
		"/apps/evolution/mail/format/reply_style");

	e_shell_settings_install_property_for_key (
		"mail-safe-list",
		"/apps/evolution/mail/display/safe_list");

	e_shell_settings_install_property_for_key (
		"mail-show-animated-images",
		"/apps/evolution/mail/display/animated_images");

	e_shell_settings_install_property_for_key (
		"mail-show-sender-photo",
		"/apps/evolution/mail/display/sender_photo");

	e_shell_settings_install_property_for_key (
		"mail-sidebar-search",
		"/apps/evolution/mail/display/side_bar_search");

	e_shell_settings_install_property_for_key (
		"mail-thread-by-subject",
		"/apps/evolution/mail/display/thread_subject");

	e_shell_settings_install_property_for_key (
		"mail-use-custom-fonts",
		"/apps/evolution/mail/display/fonts/use_custom");

	/*** Composer Preferences ***/

	e_shell_settings_install_property_for_key (
		"composer-charset",
		"/apps/evolution/mail/composer/charset");

	e_shell_settings_install_property_for_key (
		"composer-format-html",
		"/apps/evolution/mail/composer/send_html");

	e_shell_settings_install_property_for_key (
		"composer-inline-spelling",
		"/apps/evolution/mail/composer/inline_spelling");

	e_shell_settings_install_property_for_key (
		"composer-magic-links",
		"/apps/evolution/mail/composer/magic_links");

	e_shell_settings_install_property_for_key (
		"composer-magic-smileys",
		"/apps/evolution/mail/composer/magic_smileys");

	e_shell_settings_install_property_for_key (
		"composer-outlook-filenames",
		"/apps/evolution/mail/composer/outlook_filenames");

	e_shell_settings_install_property_for_key (
		"composer-ignore-list-reply-to",
		"/apps/evolution/mail/composer/ignore_list_reply_to");

	e_shell_settings_install_property_for_key (
		"composer-group-reply-to-list",
		"/apps/evolution/mail/composer/group_reply_to_list");

	e_shell_settings_install_property_for_key (
		"composer-prompt-only-bcc",
		"/apps/evolution/mail/prompts/only_bcc");

	e_shell_settings_install_property_for_key (
		"composer-prompt-private-list-reply",
		"/apps/evolution/mail/prompts/private_list_reply");

	e_shell_settings_install_property_for_key (
		"composer-prompt-reply-many-recips",
		"/apps/evolution/mail/prompts/reply_many_recips");

	e_shell_settings_install_property_for_key (
		"composer-prompt-list-reply-to",
		"/apps/evolution/mail/prompts/list_reply_to");

	e_shell_settings_install_property_for_key (
		"composer-prompt-empty-subject",
		"/apps/evolution/mail/prompts/empty_subject");

	e_shell_settings_install_property_for_key (
		"composer-prompt-send-invalid-recip",
		"/apps/evolution/mail/prompts/send_invalid_recip");

	e_shell_settings_install_property_for_key (
		"composer-reply-start-bottom",
		"/apps/evolution/mail/composer/reply_start_bottom");

	e_shell_settings_install_property_for_key (
		"composer-request-receipt",
		"/apps/evolution/mail/composer/request_receipt");

	e_shell_settings_install_property_for_key (
		"composer-spell-color",
		"/apps/evolution/mail/composer/spell_color");

	e_shell_settings_install_property_for_key (
		"composer-top-signature",
		"/apps/evolution/mail/composer/top_signature");

	e_shell_settings_install_property_for_key (
		"composer-no-signature-delim",
		"/apps/evolution/mail/composer/no_signature_delim");

	e_shell_settings_install_property_for_key (
		"composer-gallery-path",
		"/apps/evolution/mail/composer/gallery_path");

	e_shell_settings_install_property_for_key (
		"paned-view-headers-state",
		"/apps/evolution/mail/display/paned_view_headers_state");

	/* These properties use transform functions to convert
	 * GConf values to forms more useful to Evolution.  We
	 * have to use separate properties because GConfBridge
	 * does not support transform functions.  Much of this
	 * is backward-compatibility cruft for poorly designed
	 * GConf schemas. */

	e_shell_settings_install_property (
		g_param_spec_enum (
			"mail-sidebar-ellipsize",
			NULL,
			NULL,
			PANGO_TYPE_ELLIPSIZE_MODE,
			PANGO_ELLIPSIZE_NONE,
			G_PARAM_READWRITE));

	g_object_bind_property_full (
		shell_settings, "mail-no-folder-dots",
		shell_settings, "mail-sidebar-ellipsize",
		G_BINDING_SYNC_CREATE,
		transform_no_folder_dots_to_ellipsize,
		NULL,
		g_object_ref (shell_settings),
		(GDestroyNotify) g_object_unref);
}

/*
 * e-mail-config-format-html.c
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
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "e-mail-config-format-html.h"

#include <libebackend/e-extension.h>

#include <shell/e-shell.h>
#include <e-util/e-util.h>
#include <mail/em-format-html.h>

static gpointer parent_class;

static void
mail_config_format_html_constructed (GObject *object)
{
	EExtension *extension;
	EExtensible *extensible;
	EShellSettings *shell_settings;
	EShell *shell;

	extension = E_EXTENSION (object);
	extensible = e_extension_get_extensible (extension);

	shell = e_shell_get_default ();
	shell_settings = e_shell_get_shell_settings (shell);

	g_object_bind_property_full (
		shell_settings, "mail-citation-color",
		extensible, "citation-color",
		G_BINDING_SYNC_CREATE,
		e_binding_transform_string_to_color,
		NULL, NULL, (GDestroyNotify) NULL);

	g_object_bind_property (
		shell_settings, "mail-image-loading-policy",
		extensible, "image-loading-policy",
		G_BINDING_SYNC_CREATE);

	g_object_bind_property (
		shell_settings, "mail-only-local-photos",
		extensible, "only-local-photos",
		G_BINDING_SYNC_CREATE);

	g_object_bind_property (
		shell_settings, "mail-show-sender-photo",
		extensible, "show-sender-photo",
		G_BINDING_SYNC_CREATE);

	g_object_bind_property (
		shell_settings, "mail-show-real-date",
		extensible, "show-real-date",
		G_BINDING_SYNC_CREATE);

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (parent_class)->constructed (object);
}

static void
mail_config_format_html_class_init (EExtensionClass *class)
{
	GObjectClass *object_class;

	parent_class = g_type_class_peek_parent (class);

	object_class = G_OBJECT_CLASS (class);
	object_class->constructed = mail_config_format_html_constructed;

	class->extensible_type = EM_TYPE_FORMAT_HTML;
}

void
e_mail_config_format_html_register_type (GTypeModule *type_module)
{
	static const GTypeInfo type_info = {
		sizeof (EExtensionClass),
		(GBaseInitFunc) NULL,
		(GBaseFinalizeFunc) NULL,
		(GClassInitFunc) mail_config_format_html_class_init,
		(GClassFinalizeFunc) NULL,
		NULL,  /* class_data */
		sizeof (EExtension),
		0,     /* n_preallocs */
		(GInstanceInitFunc) NULL,
		NULL   /* value_table */
	};

	g_type_module_register_type (
		type_module, E_TYPE_EXTENSION,
		"EMailConfigFormatHTML", &type_info, 0);
}

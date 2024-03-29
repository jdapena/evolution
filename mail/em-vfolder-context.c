/*
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
 *		Not Zed <notzed@lostzed.mmc.com.au>
 *      Jeffrey Stedfast <fejj@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include "em-vfolder-context.h"
#include "em-vfolder-rule.h"
#include "filter/e-filter-option.h"
#include "filter/e-filter-int.h"

#include "em-filter-folder-element.h"

struct _EMVFolderContextPrivate {
	EMailBackend *backend;
};

enum {
	PROP_0,
	PROP_BACKEND
};

G_DEFINE_TYPE (
	EMVFolderContext,
	em_vfolder_context,
	E_TYPE_RULE_CONTEXT)

static void
vfolder_context_set_backend (EMVFolderContext *context,
                             EMailBackend *backend)
{
	g_return_if_fail (E_IS_MAIL_BACKEND (backend));
	g_return_if_fail (context->priv->backend == NULL);

	context->priv->backend = g_object_ref (backend);
}

static void
vfolder_context_set_property (GObject *object,
                              guint property_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_BACKEND:
			vfolder_context_set_backend (
				EM_VFOLDER_CONTEXT (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
vfolder_context_get_property (GObject *object,
                              guint property_id,
                              GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_BACKEND:
			g_value_set_object (
				value,
				em_vfolder_context_get_backend (
				EM_VFOLDER_CONTEXT (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
vfolder_context_dispose (GObject *object)
{
	EMVFolderContextPrivate *priv;

	priv = EM_VFOLDER_CONTEXT (object)->priv;

	if (priv->backend != NULL) {
		g_object_unref (priv->backend);
		priv->backend = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (em_vfolder_context_parent_class)->dispose (object);
}

static EFilterElement *
vfolder_context_new_element (ERuleContext *context,
                             const gchar *type)
{
	EMVFolderContextPrivate *priv;

	priv = EM_VFOLDER_CONTEXT (context)->priv;

	if (strcmp (type, "system-flag") == 0)
		return e_filter_option_new ();

	if (strcmp (type, "score") == 0)
		return e_filter_int_new_type("score", -3, 3);

	if (strcmp (type, "folder") == 0)
		return em_filter_folder_element_new (priv->backend);

	/* XXX Legacy type name.  Same as "folder" now. */
	if (strcmp (type, "folder-curi") == 0)
		return em_filter_folder_element_new (priv->backend);

	return E_RULE_CONTEXT_CLASS (em_vfolder_context_parent_class)->
		new_element (context, type);
}

static void
em_vfolder_context_class_init (EMVFolderContextClass *class)
{
	GObjectClass *object_class;
	ERuleContextClass *rule_context_class;

	g_type_class_add_private (class, sizeof (EMVFolderContextPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = vfolder_context_set_property;
	object_class->get_property = vfolder_context_get_property;
	object_class->dispose = vfolder_context_dispose;

	rule_context_class = E_RULE_CONTEXT_CLASS (class);
	rule_context_class->new_element = vfolder_context_new_element;

	g_object_class_install_property (
		object_class,
		PROP_BACKEND,
		g_param_spec_object (
			"backend",
			NULL,
			NULL,
			E_TYPE_MAIL_BACKEND,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY));
}

static void
em_vfolder_context_init (EMVFolderContext *context)
{
	context->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		context, EM_TYPE_VFOLDER_CONTEXT, EMVFolderContextPrivate);

	e_rule_context_add_part_set (
		E_RULE_CONTEXT (context), "partset", E_TYPE_FILTER_PART,
		(ERuleContextPartFunc) e_rule_context_add_part,
		(ERuleContextNextPartFunc) e_rule_context_next_part);

	e_rule_context_add_rule_set (
		E_RULE_CONTEXT (context), "ruleset", EM_TYPE_VFOLDER_RULE,
		(ERuleContextRuleFunc) e_rule_context_add_rule,
		(ERuleContextNextRuleFunc) e_rule_context_next_rule);

	E_RULE_CONTEXT (context)->flags =
		E_RULE_CONTEXT_THREADING | E_RULE_CONTEXT_GROUPING;
}

EMVFolderContext *
em_vfolder_context_new (EMailBackend *backend)
{
	g_return_val_if_fail (E_IS_MAIL_BACKEND (backend), NULL);

	return g_object_new (
		EM_TYPE_VFOLDER_CONTEXT, "backend", backend, NULL);
}

EMailBackend *
em_vfolder_context_get_backend (EMVFolderContext *context)
{
	g_return_val_if_fail (EM_IS_VFOLDER_CONTEXT (context), NULL);

	return context->priv->backend;
}

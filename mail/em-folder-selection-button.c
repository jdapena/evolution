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
#include <glib/gi18n.h>
#include <e-util/e-util.h>

#include "e-mail-folder-utils.h"
#include "em-folder-tree.h"
#include "em-folder-selector.h"
#include "em-utils.h"

#include "em-folder-selection-button.h"

#define EM_FOLDER_SELECTION_BUTTON_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), EM_TYPE_FOLDER_SELECTION_BUTTON, EMFolderSelectionButtonPrivate))

struct _EMFolderSelectionButtonPrivate {
	EMailBackend *backend;
	GtkWidget *icon;
	GtkWidget *label;
	CamelStore *store;

	gchar *title;
	gchar *caption;
	gchar *folder_uri;
};

enum {
	PROP_0,
	PROP_BACKEND,
	PROP_CAPTION,
	PROP_FOLDER_URI,
	PROP_STORE,
	PROP_TITLE
};

enum {
	SELECTED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (
	EMFolderSelectionButton,
	em_folder_selection_button,
	GTK_TYPE_BUTTON)

static void
folder_selection_button_unselected (EMFolderSelectionButton *button)
{
	const gchar *text;

	text = _("<click here to select a folder>");
	gtk_image_set_from_pixbuf (GTK_IMAGE (button->priv->icon), NULL);
	gtk_label_set_text (GTK_LABEL (button->priv->label), text);
}

static void
folder_selection_button_set_contents (EMFolderSelectionButton *button)
{
	EMailBackend *backend;
	CamelStore *store = NULL;
	CamelService *service;
	GtkLabel *label;
	const gchar *display_name;
	gchar *folder_name = NULL;

	label = GTK_LABEL (button->priv->label);
	backend = em_folder_selection_button_get_backend (button);

	if (backend != NULL && button->priv->folder_uri != NULL) {
		EMailSession *session;

		session = e_mail_backend_get_session (backend);
		e_mail_folder_uri_parse (
			CAMEL_SESSION (session),
			button->priv->folder_uri,
			&store, &folder_name, NULL);
	}

	if (store == NULL || folder_name == NULL) {
		folder_selection_button_unselected (button);
		return;
	}

	service = CAMEL_SERVICE (store);
	display_name = camel_service_get_display_name (service);

	if (display_name != NULL) {
		gchar *text;

		text = g_strdup_printf (
			"%s/%s", display_name, _(folder_name));
		gtk_label_set_text (label, text);
		g_free (text);
	} else
		gtk_label_set_text (label, _(folder_name));

	g_object_unref (store);
	g_free (folder_name);
}

static void
folder_selection_button_set_property (GObject *object,
                                      guint property_id,
                                      const GValue *value,
                                      GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_BACKEND:
			em_folder_selection_button_set_backend (
				EM_FOLDER_SELECTION_BUTTON (object),
				g_value_get_object (value));
			return;

		case PROP_CAPTION:
			em_folder_selection_button_set_caption (
				EM_FOLDER_SELECTION_BUTTON (object),
				g_value_get_string (value));
			return;

		case PROP_FOLDER_URI:
			em_folder_selection_button_set_folder_uri (
				EM_FOLDER_SELECTION_BUTTON (object),
				g_value_get_string (value));
			return;

		case PROP_STORE:
			em_folder_selection_button_set_store (
				EM_FOLDER_SELECTION_BUTTON (object),
				g_value_get_object (value));
			return;

		case PROP_TITLE:
			em_folder_selection_button_set_title (
				EM_FOLDER_SELECTION_BUTTON (object),
				g_value_get_string (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
folder_selection_button_get_property (GObject *object,
                                      guint property_id,
                                      GValue *value,
                                      GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_BACKEND:
			g_value_set_object (
				value,
				em_folder_selection_button_get_backend (
				EM_FOLDER_SELECTION_BUTTON (object)));
			return;

		case PROP_CAPTION:
			g_value_set_string (
				value,
				em_folder_selection_button_get_caption (
				EM_FOLDER_SELECTION_BUTTON (object)));
			return;

		case PROP_FOLDER_URI:
			g_value_set_string (
				value,
				em_folder_selection_button_get_folder_uri (
				EM_FOLDER_SELECTION_BUTTON (object)));
			return;

		case PROP_STORE:
			g_value_set_object (
				value,
				em_folder_selection_button_get_store (
				EM_FOLDER_SELECTION_BUTTON (object)));
			return;

		case PROP_TITLE:
			g_value_set_string (
				value,
				em_folder_selection_button_get_title (
				EM_FOLDER_SELECTION_BUTTON (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
folder_selection_button_dispose (GObject *object)
{
	EMFolderSelectionButtonPrivate *priv;

	priv = EM_FOLDER_SELECTION_BUTTON_GET_PRIVATE (object);

	if (priv->backend != NULL) {
		g_object_unref (priv->backend);
		priv->backend = NULL;
	}

	if (priv->store != NULL) {
		g_object_unref (priv->store);
		priv->store = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (em_folder_selection_button_parent_class)->
		dispose (object);
}

static void
folder_selection_button_finalize (GObject *object)
{
	EMFolderSelectionButtonPrivate *priv;

	priv = EM_FOLDER_SELECTION_BUTTON_GET_PRIVATE (object);

	g_free (priv->title);
	g_free (priv->caption);
	g_free (priv->folder_uri);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (em_folder_selection_button_parent_class)->
		finalize (object);
}

static void
folder_selection_button_clicked (GtkButton *button)
{
	EMFolderSelectionButtonPrivate *priv;
	EMFolderSelector *selector;
	EMFolderTree *folder_tree;
	EMFolderTreeModel *model = NULL;
	GtkWidget *dialog;
	GtkTreeSelection *selection;
	gpointer parent;

	priv = EM_FOLDER_SELECTION_BUTTON (button)->priv;

	parent = gtk_widget_get_toplevel (GTK_WIDGET (button));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	if (priv->store != NULL) {
		model = em_folder_tree_model_new ();
		em_folder_tree_model_set_backend (model, priv->backend);
		em_folder_tree_model_add_store (model, priv->store);
	}

	if (model == NULL)
		model = g_object_ref (em_folder_tree_model_get_default ());

	dialog = em_folder_selector_new (
		parent, priv->backend, model,
		EM_FOLDER_SELECTOR_CAN_CREATE,
		priv->title, priv->caption, NULL);

	g_object_unref (model);

	selector = EM_FOLDER_SELECTOR (dialog);
	folder_tree = em_folder_selector_get_folder_tree (selector);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (folder_tree));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);

	em_folder_tree_set_excluded (
		folder_tree,
		EMFT_EXCLUDE_NOSELECT |
		EMFT_EXCLUDE_VIRTUAL |
		EMFT_EXCLUDE_VTRASH);

	em_folder_tree_set_selected (folder_tree, priv->folder_uri, FALSE);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		const gchar *uri;

		uri = em_folder_selector_get_selected_uri (selector);
		em_folder_selection_button_set_folder_uri (
			EM_FOLDER_SELECTION_BUTTON (button), uri);

		g_signal_emit (button, signals[SELECTED], 0);
	}

	gtk_widget_destroy (dialog);
}

static void
em_folder_selection_button_class_init (EMFolderSelectionButtonClass *class)
{
	GObjectClass *object_class;
	GtkButtonClass *button_class;

	g_type_class_add_private (class, sizeof (EMFolderSelectionButtonPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = folder_selection_button_set_property;
	object_class->get_property = folder_selection_button_get_property;
	object_class->dispose = folder_selection_button_dispose;
	object_class->finalize = folder_selection_button_finalize;

	button_class = GTK_BUTTON_CLASS (class);
	button_class->clicked = folder_selection_button_clicked;

	g_object_class_install_property (
		object_class,
		PROP_BACKEND,
		g_param_spec_object (
			"backend",
			NULL,
			NULL,
			E_TYPE_MAIL_BACKEND,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_CAPTION,
		g_param_spec_string (
			"caption",
			NULL,
			NULL,
			NULL,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_FOLDER_URI,
		g_param_spec_string (
			"folder-uri",
			NULL,
			NULL,
			NULL,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_STORE,
		g_param_spec_object (
			"store",
			NULL,
			NULL,
			CAMEL_TYPE_STORE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_TITLE,
		g_param_spec_string (
			"title",
			NULL,
			NULL,
			NULL,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	signals[SELECTED] = g_signal_new (
		"selected",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET (EMFolderSelectionButtonClass, selected),
		NULL, NULL,
		g_cclosure_marshal_VOID__VOID,
		G_TYPE_NONE, 0);
}

static void
em_folder_selection_button_init (EMFolderSelectionButton *emfsb)
{
	GtkWidget *box;

	emfsb->priv = EM_FOLDER_SELECTION_BUTTON_GET_PRIVATE (emfsb);

	box = gtk_hbox_new (FALSE, 4);
	gtk_container_add (GTK_CONTAINER (emfsb), box);
	gtk_widget_show (box);

	emfsb->priv->icon = gtk_image_new ();
	gtk_box_pack_start (GTK_BOX (box), emfsb->priv->icon, FALSE, TRUE, 0);
	gtk_widget_show (emfsb->priv->icon);

	emfsb->priv->label = gtk_label_new ("");
	gtk_label_set_justify (GTK_LABEL (emfsb->priv->label), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment (GTK_MISC (emfsb->priv->label), 0.0, 0.5);
	gtk_box_pack_start (GTK_BOX (box), emfsb->priv->label, TRUE, TRUE, 0);
	gtk_widget_show (emfsb->priv->label);
}

GtkWidget *
em_folder_selection_button_new (EMailBackend *backend,
                                const gchar *title,
                                const gchar *caption)
{
	g_return_val_if_fail (E_IS_MAIL_BACKEND (backend), NULL);

	return g_object_new (
		EM_TYPE_FOLDER_SELECTION_BUTTON,
		"backend", backend, "title", title,
		"caption", caption, NULL);
}

EMailBackend *
em_folder_selection_button_get_backend (EMFolderSelectionButton *button)
{
	g_return_val_if_fail (EM_IS_FOLDER_SELECTION_BUTTON (button), NULL);

	return button->priv->backend;
}

void
em_folder_selection_button_set_backend (EMFolderSelectionButton *button,
                                        EMailBackend *backend)
{
	g_return_if_fail (EM_IS_FOLDER_SELECTION_BUTTON (button));

	if (backend != NULL) {
		g_return_if_fail (E_IS_MAIL_BACKEND (backend));
		g_object_ref (backend);
	}

	if (button->priv->backend != NULL)
		g_object_unref (button->priv->backend);

	button->priv->backend = backend;

	g_object_notify (G_OBJECT (button), "backend");
}

const gchar *
em_folder_selection_button_get_caption (EMFolderSelectionButton *button)
{
	g_return_val_if_fail (EM_IS_FOLDER_SELECTION_BUTTON (button), NULL);

	return button->priv->caption;
}

void
em_folder_selection_button_set_caption (EMFolderSelectionButton *button,
                                        const gchar *caption)
{
	g_return_if_fail (EM_IS_FOLDER_SELECTION_BUTTON (button));

	g_free (button->priv->caption);
	button->priv->caption = g_strdup (caption);

	g_object_notify (G_OBJECT (button), "caption");
}

const gchar *
em_folder_selection_button_get_folder_uri (EMFolderSelectionButton *button)
{
	g_return_val_if_fail (EM_IS_FOLDER_SELECTION_BUTTON (button), NULL);

	return button->priv->folder_uri;
}

void
em_folder_selection_button_set_folder_uri (EMFolderSelectionButton *button,
                                           const gchar *folder_uri)
{
	g_return_if_fail (EM_IS_FOLDER_SELECTION_BUTTON (button));

	/* An empty string is equivalent to NULL. */
	if (folder_uri != NULL && *folder_uri == '\0')
		folder_uri = NULL;

	g_free (button->priv->folder_uri);
	button->priv->folder_uri = g_strdup (folder_uri);

	folder_selection_button_set_contents (button);

	g_object_notify (G_OBJECT (button), "folder-uri");
}

CamelStore *
em_folder_selection_button_get_store (EMFolderSelectionButton *button)
{
	g_return_val_if_fail (EM_IS_FOLDER_SELECTION_BUTTON (button), NULL);

	return button->priv->store;
}

void
em_folder_selection_button_set_store (EMFolderSelectionButton *button,
                                      CamelStore *store)
{
	g_return_if_fail (EM_IS_FOLDER_SELECTION_BUTTON (button));

	if (store != NULL) {
		g_return_if_fail (CAMEL_IS_STORE (store));
		g_object_ref (store);
	}

	if (button->priv->store != NULL)
		g_object_unref (button->priv->store);

	button->priv->store = store;

	g_object_notify (G_OBJECT (button), "store");
}

const gchar *
em_folder_selection_button_get_title (EMFolderSelectionButton *button)
{
	g_return_val_if_fail (EM_FOLDER_SELECTION_BUTTON (button), NULL);

	return button->priv->title;
}

void
em_folder_selection_button_set_title (EMFolderSelectionButton *button,
                                      const gchar *title)
{
	g_return_if_fail (EM_IS_FOLDER_SELECTION_BUTTON (button));

	g_free (button->priv->title);
	button->priv->title = g_strdup (title);

	g_object_notify (G_OBJECT (button), "title");
}

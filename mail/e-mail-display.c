/*
 * e-mail-display.c
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

#define LIBSOUP_USE_UNSTABLE_REQUEST_API

#include "e-mail-display.h"

#include <glib/gi18n.h>

#include "e-util/e-util.h"
#include "e-util/e-plugin-ui.h"
#include "mail/em-composer-utils.h"
#include "mail/em-utils.h"
#include "mail/e-mail-request.h"
#include "mail/em-format-html-display.h"
#include "mail/e-mail-attachment-bar.h"
#include "widgets/misc/e-attachment-button.h"

#include <libsoup/soup.h>
#include <libsoup/soup-requester.h>

struct _EMailDisplayPrivate {
	GtkWidget *grid;

	ESearchBar *searchbar;
	EMFormatHTML *formatter;

	EMFormatWriteMode mode;
	gboolean headers_collapsable;
	gboolean headers_collapsed;

	GList *webviews;
};

enum {
	PROP_0,
	PROP_FORMATTER,
	PROP_MODE,
	PROP_HEADERS_COLLAPSABLE,
	PROP_HEADERS_COLLAPSED
};

static gpointer parent_class;

typedef void (*WebViewActionFunc) (EWebView *web_view);

static const gchar *ui =
"<ui>"
"  <popup name='context'>"
"    <placeholder name='custom-actions-1'>"
"      <menuitem action='add-to-address-book'/>"
"      <menuitem action='send-reply'/>"
"    </placeholder>"
"    <placeholder name='custom-actions-3'>"
"      <menu action='search-folder-menu'>"
"        <menuitem action='search-folder-recipient'/>"
"        <menuitem action='search-folder-sender'/>"
"      </menu>"
"    </placeholder>"
"    <placeholder name='inspect' />"
"  </popup>"
"</ui>";

static GtkActionEntry mailto_entries[] = {

	{ "add-to-address-book",
	  "contact-new",
	  N_("_Add to Address Book..."),
	  NULL,
	  NULL,  /* XXX Add a tooltip! */
	  NULL   /* Handled by EMailReader */ },

	{ "search-folder-recipient",
	  NULL,
	  N_("_To This Address"),
	  NULL,
	  NULL,  /* XXX Add a tooltip! */
	  NULL   /* Handled by EMailReader */ },

	{ "search-folder-sender",
	  NULL,
	  N_("_From This Address"),
	  NULL,
	  NULL,  /* XXX Add a tooltip! */
	  NULL   /* Handled by EMailReader */ },

	{ "send-reply",
	  NULL,
	  N_("Send _Reply To..."),
	  NULL,
	  N_("Send a reply message to this address"),
	  NULL   /* Handled by EMailReader */ },

	/*** Menus ***/

	{ "search-folder-menu",
	  "folder-saved-search",
	  N_("Create Search _Folder"),
	  NULL,
	  NULL,
	  NULL }
};

static void
mail_display_update_formatter_colors (EMailDisplay *display)
{
	EMFormatHTMLColorType type;
	EMFormatHTML *formatter;
	GdkColor *color;
	GtkStateType state;
	GtkStyle *style;

	state = gtk_widget_get_state (GTK_WIDGET (display));
	formatter = display->priv->formatter;

	if (!display->priv->formatter)
		return;

	style = gtk_widget_get_style (GTK_WIDGET (display));
	if (style == NULL)
		return;

	g_object_freeze_notify (G_OBJECT (formatter));

	color = &style->bg[state];
	type = EM_FORMAT_HTML_COLOR_BODY;
	em_format_html_set_color (formatter, type, color);

	color = &style->base[GTK_STATE_NORMAL];
	type = EM_FORMAT_HTML_COLOR_CONTENT;
	em_format_html_set_color (formatter, type, color);

	color = &style->dark[state];
	type = EM_FORMAT_HTML_COLOR_FRAME;
	em_format_html_set_color (formatter, type, color);

	color = &style->fg[state];
	type = EM_FORMAT_HTML_COLOR_HEADER;
	em_format_html_set_color (formatter, type, color);

	color = &style->text[state];
	type = EM_FORMAT_HTML_COLOR_TEXT;
	em_format_html_set_color (formatter, type, color);

	g_object_thaw_notify (G_OBJECT (formatter));
}

static void
mail_display_set_property (GObject *object,
                           guint property_id,
                           const GValue *value,
                           GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_FORMATTER:
			e_mail_display_set_formatter (
				E_MAIL_DISPLAY (object),
				g_value_get_object (value));
			return;
		case PROP_MODE:
			e_mail_display_set_mode (
				E_MAIL_DISPLAY (object),
				g_value_get_int (value));
			return;
		case PROP_HEADERS_COLLAPSABLE:
			e_mail_display_set_headers_collapsable (
				E_MAIL_DISPLAY (object),
				g_value_get_boolean (value));
			return;
		case PROP_HEADERS_COLLAPSED:
			e_mail_display_set_headers_collapsed (
				E_MAIL_DISPLAY (object),
				g_value_get_boolean (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_display_get_property (GObject *object,
                           guint property_id,
                           GValue *value,
                           GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_FORMATTER:
			g_value_set_object (
				value, e_mail_display_get_formatter (
				E_MAIL_DISPLAY (object)));
			return;
		case PROP_MODE:
			g_value_set_int (
				value, e_mail_display_get_mode (
				E_MAIL_DISPLAY (object)));
			return;
		case PROP_HEADERS_COLLAPSABLE:
			g_value_set_boolean (
				value, e_mail_display_get_headers_collapsable (
				E_MAIL_DISPLAY (object)));
			return;
		case PROP_HEADERS_COLLAPSED:
			g_value_set_boolean (
				value, e_mail_display_get_headers_collapsed (
				E_MAIL_DISPLAY (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_display_dispose (GObject *object)
{
	EMailDisplayPrivate *priv;

	priv = E_MAIL_DISPLAY (object)->priv;

	if (priv->formatter) {
		g_object_unref (priv->formatter);
		priv->formatter = NULL;
	}

	if (priv->searchbar) {
		g_object_unref (priv->searchbar);
		priv->searchbar = NULL;
	}

	if (priv->webviews) {
		g_list_free (priv->webviews);
		priv->webviews = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
mail_display_realize (GtkWidget *widget)
{
	/* Chain up to parent's realize() method. */
	GTK_WIDGET_CLASS (parent_class)->realize (widget);

	mail_display_update_formatter_colors (E_MAIL_DISPLAY (widget));
}

static void
mail_display_style_set (GtkWidget *widget,
                        GtkStyle *previous_style)
{
	EMailDisplay *display = E_MAIL_DISPLAY (widget);

	/* Chain up to parent's style_set() method. */
	GTK_WIDGET_CLASS (parent_class)->style_set (widget, previous_style);

	mail_display_update_formatter_colors (display);

	e_mail_display_reload (display);
}

static gboolean
mail_display_process_mailto (EWebView *web_view,
                             const gchar *mailto_uri,
                             gpointer user_data)
{
	EMailDisplay *display = user_data;

	g_return_val_if_fail (web_view != NULL, FALSE);
	g_return_val_if_fail (mailto_uri != NULL, FALSE);
	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display), FALSE);

	if (g_ascii_strncasecmp (mailto_uri, "mailto:", 7) == 0) {
		EMailDisplayPrivate *priv;
		EMFormat *format;
		CamelFolder *folder = NULL;
		EShell *shell;

		priv = display->priv;
		g_return_val_if_fail (priv->formatter != NULL, FALSE);

		format = EM_FORMAT (priv->formatter);

		if (format != NULL && format->folder != NULL)
			folder = format->folder;

		shell = e_shell_get_default ();
		em_utils_compose_new_message_with_mailto (
			shell, mailto_uri, folder);

		return TRUE;
	}

	return FALSE;
}

static gboolean
mail_display_link_clicked (WebKitWebView *web_view,
			   WebKitWebFrame *frame,
			   WebKitNetworkRequest *request,
			   WebKitWebNavigationAction *navigation_action,
			   WebKitWebPolicyDecision *policy_decision,
			   gpointer user_data)
{
	EMailDisplay *display = user_data;
	EMailDisplayPrivate *priv;
	const gchar *uri = webkit_network_request_get_uri (request);

	priv = display->priv;
	g_return_val_if_fail (priv->formatter != NULL, FALSE);

	if (mail_display_process_mailto (E_WEB_VIEW (web_view), uri, display)) {
		/* do nothing, function handled the "mailto:" uri already */
		webkit_web_policy_decision_ignore (policy_decision);
		return TRUE;

	} else if (g_ascii_strncasecmp (uri, "thismessage:", 12) == 0) {
		/* ignore */ ;
		webkit_web_policy_decision_ignore (policy_decision);
		return TRUE;

	} else if (g_ascii_strncasecmp (uri, "cid:", 4) == 0) {
		/* ignore */ ;
		webkit_web_policy_decision_ignore (policy_decision);
		return TRUE;
	}

	/* Let webkit handle it */
	return FALSE;
}

static void
mail_display_resource_requested (WebKitWebView *web_view,
				 WebKitWebFrame *frame,
				 WebKitWebResource *resource,
				 WebKitNetworkRequest *request,
				 WebKitNetworkResponse *response,
				 gpointer user_data)
{
	EMailDisplay *display = user_data;
	EMFormatHTML *formatter = display->priv->formatter;
	const gchar *uri = webkit_network_request_get_uri (request);

        /* Redirect cid:part_id to mail://mail_id/cid:part_id */
        if (g_str_has_prefix (uri, "cid:")) {
		gchar *new_uri = em_format_build_mail_uri (EM_FORMAT (formatter)->folder, 
			EM_FORMAT (formatter)->message_uid,
			"part_id", G_TYPE_STRING, uri, NULL);

                webkit_network_request_set_uri (request, new_uri);

                g_free (new_uri);

        /* WebKit won't allow to load a local file when displaing "remote" mail://,
           protocol, so we need to handle this manually */
        } else if (g_str_has_prefix (uri, "file:")) {
                gchar *data = NULL;
                gsize length = 0;
                gboolean status;
                gchar *path;

                path = g_filename_from_uri (uri, NULL, NULL);
                if (!path)
                        return;

		status = g_file_get_contents (path, &data, &length, NULL);
                if (status) {
                        gchar *b64, *new_uri;
                        gchar *ct;

                        b64 = g_base64_encode ((guchar*) data, length);
                        ct = g_content_type_guess (path, NULL, 0, NULL);

                        new_uri =  g_strdup_printf ("data:%s;base64,%s", ct, b64);
                        webkit_network_request_set_uri (request, new_uri);

                        g_free (b64);
                        g_free (new_uri);
                        g_free (ct);
                }
                g_free (data);
                g_free (path);
        }
}

static void
mail_display_headers_collapsed_state_changed (EWebView *web_view,
					      size_t arg_count,
					      const JSValueRef args[],
					      gpointer user_data)
{
	EMailDisplay *display = user_data;
	JSGlobalContextRef ctx = e_web_view_get_global_context (web_view);

	display->priv->headers_collapsed = JSValueToBoolean (ctx, args[0]);
}

static void
mail_display_install_js_callbacks (WebKitWebView *web_view,
			           WebKitWebFrame *frame,
				   gpointer context,
				   gpointer window_object,
				   gpointer user_data)
{
	if (frame != webkit_web_view_get_main_frame (web_view))
		return;

	e_web_view_install_js_callback (E_WEB_VIEW (web_view), "headers_collapsed",
		(EWebViewJSFunctionCallback) mail_display_headers_collapsed_state_changed, user_data);
}

static EWebView*
mail_display_setup_webview (EMailDisplay *display)
{
	EWebView *web_view;
	GtkUIManager *ui_manager;
	GtkActionGroup *action_group;
	GError *error = NULL;

	web_view = E_WEB_VIEW (e_web_view_new ());

	g_signal_connect (web_view, "navigation-policy-decision-requested",
		G_CALLBACK (mail_display_link_clicked), display);
	g_signal_connect (web_view, "window-object-cleared",
		G_CALLBACK (mail_display_install_js_callbacks), display);
	g_signal_connect (web_view, "resource-request-starting",
		G_CALLBACK (mail_display_resource_requested), display);
	g_signal_connect (web_view, "process-mailto",
		G_CALLBACK (mail_display_process_mailto), display);

	/* EWebView's action groups are added during its instance
	 * initialization function (like what we're in now), so it
	 * is safe to fetch them this early in construction. */
	action_group = e_web_view_get_action_group (web_view, "mailto");

	/* We don't actually handle the actions we're adding.
	 * EMailReader handles them.  How devious is that? */
	gtk_action_group_add_actions (
		action_group, mailto_entries,
		G_N_ELEMENTS (mailto_entries), display);

	/* Because we are loading from a hard-coded string, there is
	 * no chance of I/O errors.  Failure here implies a malformed
	 * UI definition.  Full stop. */
	ui_manager = e_web_view_get_ui_manager (web_view);
	gtk_ui_manager_add_ui_from_string (ui_manager, ui, -1, &error);
	if (error != NULL)
		g_error ("%s", error->message);

	return web_view;
}

static void
mail_display_on_web_view_vadjustment_changed (GtkAdjustment* adjustment,
					      gpointer user_data)
{
	GtkWidget *widget = user_data;
	int upper = gtk_adjustment_get_upper (GTK_ADJUSTMENT (adjustment));
	int page_size = gtk_adjustment_get_page_size (GTK_ADJUSTMENT (adjustment));
	int widget_height = gtk_widget_get_allocated_height (widget);

	if (page_size < upper) {
		gtk_widget_set_size_request (GTK_WIDGET (widget), -1, upper + (widget_height - page_size));
		gtk_widget_queue_resize (GTK_WIDGET (widget));
	}
	
}

static void
mail_display_insert_web_view (EMailDisplay *display,
			      EWebView *web_view)
{
	GtkWidget *scrolled_window;
	GtkAdjustment *web_view_vadjustment;

	display->priv->webviews = g_list_append (display->priv->webviews, web_view);
	scrolled_window = gtk_scrolled_window_new (NULL, NULL);
	g_object_set (G_OBJECT (scrolled_window),
		"hscrollbar-policy", GTK_POLICY_AUTOMATIC,
		"vscrollbar-policy", GTK_POLICY_AUTOMATIC,
		"shadow-type", GTK_SHADOW_NONE,
		"hexpand", TRUE,
		      "vexpand", FALSE,
		      "vexpand-set", TRUE,
		NULL);
	g_object_set (G_OBJECT (web_view),
		      "vscroll-policy", GTK_SCROLL_NATURAL,
		      NULL);
		//"min-content-height", 300, NULL);
	web_view_vadjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scrolled_window));
	g_signal_connect (G_OBJECT (web_view_vadjustment), "changed",
			  G_CALLBACK (mail_display_on_web_view_vadjustment_changed), scrolled_window);
	gtk_container_add (GTK_CONTAINER (scrolled_window), GTK_WIDGET (web_view));

	gtk_container_add (GTK_CONTAINER (display->priv->grid), scrolled_window);
	gtk_widget_show_all (scrolled_window);

#if 0
	g_object_set (G_OBJECT (web_view),
		"expand", TRUE, NULL);
	gtk_container_add (GTK_CONTAINER (display->priv->grid), GTK_WIDGET (web_view));
	gtk_widget_show_all (GTK_WIDGET (web_view));
#endif
}

static void
mail_display_load_as_source (EMailDisplay *display,
			     const gchar *msg_uid)
{
	EWebView *web_view;
	EMFormat *emf = (EMFormat *) display->priv->formatter;
	gchar *uri;

	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	e_mail_display_clear (display);

	web_view = mail_display_setup_webview (display);
	mail_display_insert_web_view (display, web_view);

	uri = em_format_build_mail_uri (emf->folder, emf->message_uid,
		"part_id", G_TYPE_STRING, ".message",
		"mode", G_TYPE_INT, display->priv->mode,
		NULL);
	e_web_view_load_uri (web_view, uri);

	gtk_widget_show_all (display->priv->grid);
}

static void
mail_display_load_normal (EMailDisplay *display,
			  const gchar *msg_uid)
{
	EWebView *web_view;
	EMFormatPURI *puri;
	EMFormat *emf = (EMFormat *) display->priv->formatter;
	EMFormatHTMLDisplay *efhd = (EMFormatHTMLDisplay *) emf;
	EAttachmentView *attachment_view;
	gchar *uri;
	GList *iter;
	GtkContainer *grid;

	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	/* Don't use gtk_widget_show_all() to display all widgets at once,
	   it makes all parts of EMailAttachmentBar visible and that's not
	   what we want.
	   FIXME: Maybe using gtk_widget_set_no_show_all() in EAttachmentView
	          could help...
	*/

	/* First remove all widgets left after previous message */
	e_mail_display_clear (display);


	grid = GTK_CONTAINER (display->priv->grid);
	gtk_widget_show (display->priv->grid);

	attachment_view = NULL;
	for (iter = emf->mail_part_list; iter; iter = iter->next) {
		GtkWidget *widget = NULL;

		puri = iter->data;
		uri = em_format_build_mail_uri (emf->folder, emf->message_uid,
			"part_id", G_TYPE_STRING, puri->uri,
			"mode", G_TYPE_INT, display->priv->mode,
			"headers_collapsable", G_TYPE_BOOLEAN, display->priv->headers_collapsable,
			"headers_collapsed", G_TYPE_BOOLEAN, display->priv->headers_collapsed,
			NULL);

		if (puri->widget_func) {

			widget = puri->widget_func (emf, puri, NULL);
			if (!GTK_IS_WIDGET (widget)) {
				g_message ("Part %s didn't provide a valid widget, skipping!", puri->uri);
				continue;
			}

			if (E_IS_ATTACHMENT_BUTTON (widget) && attachment_view)
				e_attachment_button_set_view (E_ATTACHMENT_BUTTON (widget),
					attachment_view);

			gtk_container_add (grid, widget);

			if (E_IS_ATTACHMENT_VIEW (widget)) {
				EAttachmentStore *store;

				attachment_view = E_ATTACHMENT_VIEW (widget);
				store = e_attachment_view_get_store (attachment_view);

				if (e_attachment_store_get_num_attachments (store) > 0)
					gtk_widget_show (widget);
				else
					gtk_widget_hide (widget);
			} else {
				gtk_widget_show (widget);
			}
		}

		if ((!puri->is_attachment && puri->write_func) || (puri->is_attachment && puri->write_func && puri->widget_func)) {
			web_view = mail_display_setup_webview (display);
			mail_display_insert_web_view (display, web_view);
			e_web_view_load_uri (web_view, uri);

			if (widget) {
				g_object_bind_property (widget, "expanded",
					web_view, "visible", G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);
			}

		}

		g_free (uri);
	}
}

static void
mail_display_class_init (EMailDisplayClass *class)
{
	GObjectClass *object_class;
	GtkWidgetClass *widget_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EMailDisplayPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = mail_display_set_property;
	object_class->get_property = mail_display_get_property;
	object_class->dispose = mail_display_dispose;

	widget_class = GTK_WIDGET_CLASS (class);
	widget_class->realize = mail_display_realize;
	widget_class->style_set = mail_display_style_set;

	g_object_class_install_property (
		object_class,
		PROP_FORMATTER,
		g_param_spec_object (
			"formatter",
			"HTML Formatter",
			NULL,
			EM_TYPE_FORMAT_HTML,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_MODE,
		g_param_spec_int (
			"mode",
			"Display Mode",
			NULL,
			EM_FORMAT_WRITE_MODE_NORMAL,
			EM_FORMAT_WRITE_MODE_SOURCE,
			EM_FORMAT_WRITE_MODE_NORMAL,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_HEADERS_COLLAPSABLE,
		g_param_spec_boolean (
			"headers-collapsable",
			"Headers Collapsable",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_HEADERS_COLLAPSED,
		g_param_spec_boolean (
			"header-collapsed",
			"Headers Collapsed",
			NULL,
			FALSE,
			G_PARAM_READWRITE));
}

static void
mail_display_init (EMailDisplay *display)
{
	SoupSession *session;
	SoupSessionFeature *feature;

	display->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		display, E_TYPE_MAIL_DISPLAY, EMailDisplayPrivate);

	display->priv->grid = g_object_new (GTK_TYPE_GRID,
		"orientation", GTK_ORIENTATION_VERTICAL,
		"margin", 10, NULL);
	gtk_container_add (GTK_CONTAINER (display), display->priv->grid);

	display->priv->webviews = NULL;

	/* WEBKIT TODO: ESearchBar */


	/* Register our own handler for our own mail:// protocol */
	session = webkit_get_default_session ();
	feature = SOUP_SESSION_FEATURE (soup_requester_new ());
	soup_session_feature_add_feature (feature, E_TYPE_MAIL_REQUEST);
	soup_session_add_feature (session, feature);
	g_object_unref (feature);
}

GType
e_mail_display_get_type (void)
{
	static GType type = 0;

	if (G_UNLIKELY (type == 0)) {
		static const GTypeInfo type_info = {
			sizeof (EMailDisplayClass),
			(GBaseInitFunc) NULL,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) mail_display_class_init,
			(GClassFinalizeFunc) NULL,
			NULL,  /* class_data */
			sizeof (EMailDisplay),
			0,     /* n_preallocs */
			(GInstanceInitFunc) mail_display_init,
			NULL   /* value_table */
		};

		type = g_type_register_static (
			GTK_TYPE_VIEWPORT, "EMailDisplay", &type_info, 0);
	}

	return type;
}

EMFormatHTML *
e_mail_display_get_formatter (EMailDisplay *display)
{
	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display), NULL);

	return display->priv->formatter;
}

void
e_mail_display_set_formatter (EMailDisplay *display,
                              EMFormatHTML *formatter)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));
	g_return_if_fail (EM_IS_FORMAT_HTML (formatter));

	if (display->priv->formatter != NULL)
		g_object_unref (display->priv->formatter);

	display->priv->formatter = g_object_ref (formatter);

	mail_display_update_formatter_colors (display);

	g_object_notify (G_OBJECT (display), "formatter");
}

EMFormatWriteMode
e_mail_display_get_mode (EMailDisplay *display)
{
	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display),
			EM_FORMAT_WRITE_MODE_NORMAL);

	return display->priv->mode;
}

void
e_mail_display_set_mode (EMailDisplay *display,
			 EMFormatWriteMode mode)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	if (display->priv->mode == mode)
		return;

	display->priv->mode = mode;
	if (mode == EM_FORMAT_WRITE_MODE_SOURCE)
		mail_display_load_as_source (display, NULL);
	else
		e_mail_display_reload (display);

	g_object_notify (G_OBJECT (display), "mode");
}

gboolean
e_mail_display_get_headers_collapsable (EMailDisplay *display)
{
	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display), FALSE);

	return display->priv->headers_collapsable;
}

void
e_mail_display_set_headers_collapsable (EMailDisplay *display,
					gboolean collapsable)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	if (display->priv->headers_collapsable == collapsable)
		return;

	display->priv->headers_collapsable = collapsable;
	e_mail_display_reload (display);

	g_object_notify (G_OBJECT (display), "header-collapsable");
}

gboolean
e_mail_display_get_headers_collapsed (EMailDisplay *display)
{
	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display), FALSE);

	if (display->priv->headers_collapsable)
		return display->priv->headers_collapsed;

	return FALSE;
}

void
e_mail_display_set_headers_collapsed (EMailDisplay *display,
				      gboolean collapsed)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	if (display->priv->headers_collapsed == collapsed)
		return;

	display->priv->headers_collapsed = collapsed;
	e_mail_display_reload (display);

	g_object_notify (G_OBJECT (display), "headers-collapsed");
}

void
e_mail_display_load (EMailDisplay *display,
		     const gchar *msg_uri)
{
	if (display->priv->mode == EM_FORMAT_WRITE_MODE_SOURCE)
		mail_display_load_as_source  (display, msg_uri);
	else
		mail_display_load_normal (display, msg_uri);
}

void
e_mail_display_reload (EMailDisplay *display)
{
	GList *iter;

	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	/* We can't just call e_web_view_reload() here, we need the URI queries
	   to reflect possible changes in write mode and headers properties.
	   Unfortunatelly, nothing provides API good enough to do this more
	   simple way... */

	for (iter = display->priv->webviews; iter; iter = iter->next) {
		EWebView *web_view;
		const gchar *uri;
		gchar *base;
		GString *new_uri;
		GHashTable *table;
		GHashTableIter table_iter;
		gpointer key, val;
		char separator;

		web_view = (EWebView *) iter->data;
		uri = e_web_view_get_uri (web_view);

		if (!uri)
			continue;

		base = g_strndup (uri, strstr (uri, "?") - uri);
		new_uri = g_string_new (base);
		g_free (base);

		table = soup_form_decode (strstr (uri, "?") + 1);
		g_hash_table_insert (table, g_strdup ("mode"), g_strdup_printf ("%d", display->priv->mode));
		g_hash_table_insert (table, g_strdup ("headers_collapsable"), g_strdup_printf ("%d", display->priv->headers_collapsable));
		g_hash_table_insert (table, g_strdup ("headers_collapsed"), g_strdup_printf ("%d", display->priv->headers_collapsed));

		g_hash_table_iter_init (&table_iter, table);
		separator = '?';
		while (g_hash_table_iter_next (&table_iter, &key, &val)) {
			g_string_append_printf (new_uri, "%c%s=%s", separator,
				(gchar *) key, (gchar *) val);

			if (separator == '?')
				separator = '&';
		}

		e_web_view_load_uri (web_view, new_uri->str);

		g_string_free (new_uri, TRUE);
		g_hash_table_destroy (table);
	}
}

EWebView*
e_mail_display_get_current_web_view (EMailDisplay *display)
{
	GtkWidget *widget;

	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display), NULL);

	widget = gtk_container_get_focus_child (GTK_CONTAINER (display));

	if (E_IS_WEB_VIEW (widget))
		return E_WEB_VIEW (widget);
	else
		return NULL;
}

void
e_mail_display_set_status (EMailDisplay *display,
			   const gchar *status)
{
	GtkWidget *label;

	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	e_mail_display_clear (display);

	label = gtk_label_new (status);
	gtk_container_add (GTK_CONTAINER (display->priv->grid), label);
	gtk_widget_show_all (display->priv->grid);
}

static void
remove_widget (GtkWidget *widget, gpointer user_data)
{
	EMailDisplay *display = user_data;

	if (!GTK_IS_WIDGET (widget))
		return;

	gtk_container_remove  (GTK_CONTAINER (display->priv->grid), widget);
}

void
e_mail_display_clear (EMailDisplay *display)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	gtk_widget_hide (display->priv->grid);

	gtk_container_foreach (GTK_CONTAINER (display->priv->grid),
		(GtkCallback) remove_widget, display);

	g_list_free (display->priv->webviews);
	display->priv->webviews = NULL;
}

ESearchBar*
e_mail_display_get_search_bar (EMailDisplay *display)
{
	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display), NULL);

	return display->priv->searchbar;
}

gboolean
e_mail_display_is_selection_active (EMailDisplay *display)
{
	EWebView *web_view;

	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display), FALSE);

	web_view = e_mail_display_get_current_web_view (display);
	if (!web_view)
		return FALSE;
	else
		return e_web_view_is_selection_active (web_view);
}

gchar*
e_mail_display_get_selection_plain_text (EMailDisplay *display,
					 gint *len)
{
	EWebView *web_view;
	WebKitWebFrame *frame;
	const gchar *frame_name;
	GValue value = {0};
	GType type;
	const gchar *str;

	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display), NULL);

	web_view = e_mail_display_get_current_web_view (display);
	if (!web_view)
		return NULL;

	frame = webkit_web_view_get_focused_frame (WEBKIT_WEB_VIEW (web_view));
	frame_name = webkit_web_frame_get_name (frame);

	type = e_web_view_frame_exec_script (web_view, frame_name, "window.getSelection().toString()", &value);
	g_return_val_if_fail (type == G_TYPE_STRING, NULL);

	str = g_value_get_string (&value);

	if (len)
		*len = strlen (str);

	return g_strdup (str);
}

static void
webview_action (GtkWidget *widget, WebViewActionFunc func)
{
	/*
	 * It's not a critical error to pass other then EWebView
	 * widgets.
	 */
	if (E_IS_WEB_VIEW (widget)) {
		func (E_WEB_VIEW (widget));
	}
}

void
e_mail_display_zoom_100 (EMailDisplay *display)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	gtk_container_foreach (GTK_CONTAINER (display),
			(GtkCallback) webview_action, e_web_view_zoom_100);
}

void
e_mail_display_zoom_in (EMailDisplay *display)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	gtk_container_foreach (GTK_CONTAINER (display),
			(GtkCallback) webview_action, e_web_view_zoom_in);
}

void
e_mail_display_zoom_out (EMailDisplay *display)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	gtk_container_foreach (GTK_CONTAINER (display),
			(GtkCallback) webview_action, e_web_view_zoom_out);
}

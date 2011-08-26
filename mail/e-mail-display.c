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

#include <libsoup/soup.h>
#include <libsoup/soup-requester.h>

struct _EMailDisplayPrivate {
	GtkWidget *vbox;

	ESearchBar *searchbar;
	EMFormatHTML *formatter;

	EMailDisplayMode display_mode;
};

enum {
	PROP_0,
	PROP_FORMATTER,
	PROP_DISPLAY_MODE
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
		case PROP_DISPLAY_MODE:
			e_mail_display_set_mode (
				E_MAIL_DISPLAY (object),
				g_value_get_int (value));
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
		case PROP_DISPLAY_MODE:
			g_value_set_int (
				value, e_mail_display_get_mode (
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
		gchar *new_uri = em_format_build_mail_uri (EM_FORMAT (formatter)->folder, EM_FORMAT (formatter)->message_uid, uri);

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

       	//g_signal_stop_emission_by_name (web_view, "resource-request-starting");
}

static void
mail_display_headers_collapsed_state_changed (EWebView *web_view,
					      size_t arg_count,
					      const JSValueRef args[],
					      gpointer user_data)
{
	EMailDisplay *display = user_data;
	EMFormatHTML *formatter = display->priv->formatter;
	JSGlobalContextRef ctx = e_web_view_get_global_context (web_view);

	gboolean collapsed = JSValueToBoolean (ctx, args[0]);

	if (collapsed) {
		em_format_html_set_headers_state (formatter, EM_FORMAT_HTML_HEADERS_STATE_COLLAPSED);
	} else {
		em_format_html_set_headers_state (formatter, EM_FORMAT_HTML_HEADERS_STATE_EXPANDED);
	}
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
		PROP_DISPLAY_MODE,
		g_param_spec_int (
			"display-mode",
			"Display Mode",
			NULL,
			E_MAIL_DISPLAY_MODE_NORMAL,
			E_MAIL_DISPLAY_MODE_SOURCE,
			E_MAIL_DISPLAY_MODE_NORMAL,
			G_PARAM_READWRITE));
}

static void
mail_display_init (EMailDisplay *display)
{
	GtkScrolledWindow *scrolled_window;
	SoupSession *session;
	SoupSessionFeature *feature;
	GValue margin = {0};

	display->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		display, E_TYPE_MAIL_DISPLAY, EMailDisplayPrivate);

	scrolled_window = GTK_SCROLLED_WINDOW (display);
	gtk_scrolled_window_set_policy (scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (scrolled_window, GTK_SHADOW_NONE);

	display->priv->vbox = gtk_vbox_new (FALSE, 0);
	gtk_scrolled_window_add_with_viewport (scrolled_window, display->priv->vbox);
	g_value_init (&margin, G_TYPE_INT);
	g_value_set_int (&margin, 10);
	g_object_set_property (G_OBJECT (display->priv->vbox), "margin", &margin);


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
			GTK_TYPE_SCROLLED_WINDOW, "EMailDisplay", &type_info, 0);
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

EMailDisplayMode
e_mail_display_get_mode (EMailDisplay *display)
{
	g_return_val_if_fail (E_IS_MAIL_DISPLAY (display),
			E_MAIL_DISPLAY_MODE_NORMAL);

	return display->priv->display_mode;
}

void
e_mail_display_set_mode (EMailDisplay *display,
			 EMailDisplayMode mode)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	if (display->priv->display_mode == mode)
		return;

	display->priv->display_mode = mode;
	e_mail_display_reload (display);

	g_object_notify (G_OBJECT (display), "display-mode");
}

void
e_mail_display_load (EMailDisplay *display,
		     const gchar *msg_uri)
{
	EWebView *web_view;
	EMFormatPURI *puri;
	EMFormat *emf = (EMFormat *) display->priv->formatter;
	gchar *uri;
	GList *iter;
	GtkBox *box;

	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	/* First remove all widgets left after previous message */
	e_mail_display_clear (display);

	box = GTK_BOX (display->priv->vbox);

	/* Headers webview */
	web_view = mail_display_setup_webview (display);
	display->widgets = g_list_append (display->widgets, GTK_WIDGET (web_view));
	gtk_box_pack_start (box, GTK_WIDGET (web_view),
			TRUE, TRUE, 0);
	uri = em_format_build_mail_uri (emf->folder, emf->message_uid, "headers");
	e_web_view_load_uri (web_view, uri);
	g_free (uri);

	/* Attachment bar */
	puri = g_hash_table_lookup (emf->mail_part_table, "attachment-bar:");
	if (puri && puri->widget_func) {
		GtkWidget *widget = puri->widget_func (emf, puri, NULL);
		display->widgets = g_list_append (display->widgets, widget);
		gtk_box_pack_start (box, widget, TRUE, TRUE, 0);
	}

	for (iter = emf->mail_part_list; iter; iter = iter->next) {
		puri = iter->data;
		uri = em_format_build_mail_uri (emf->folder, emf->message_uid, puri->uri);

		if (puri->widget_func && strcmp (puri->uri, "attachment-bar:") != 0) {
			GtkWidget *widget;

			widget = puri->widget_func (emf, puri, NULL);
			if (!widget) {
				g_message ("Part %s didn't provide a valid widget, skipping!", puri->uri);
				continue;
			}
			display->widgets = g_list_append (display->widgets, widget);
			gtk_box_pack_start (box, widget,
					TRUE, TRUE, 0);

		} else if (puri->write_func) {
			web_view = mail_display_setup_webview (display);
			display->widgets = g_list_append (display->widgets, GTK_WIDGET (web_view));
			gtk_box_pack_start (box, GTK_WIDGET (web_view),
					TRUE, TRUE, 0);
			e_web_view_load_uri (web_view, uri);
		}

		g_free (uri);
	}

	gtk_widget_show_all (display->priv->vbox);
}

void
e_mail_display_reload (EMailDisplay *display)
{
	GList *iter;

	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	for (iter = display->widgets; iter; iter = iter->next) {
		if (E_IS_WEB_VIEW (iter->data))
			e_web_view_reload (E_WEB_VIEW (iter->data));
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
	gtk_box_pack_start (GTK_BOX (display->priv->vbox), label, TRUE, TRUE, 0);
	gtk_widget_show_all (display->priv->vbox);

	display->widgets = g_list_append (display->widgets, label);
}

void
e_mail_display_clear (EMailDisplay *display)
{
	g_return_if_fail (E_IS_MAIL_DISPLAY (display));

	gtk_widget_hide (display->priv->vbox);

	g_list_free_full (display->widgets,
			(GDestroyNotify) gtk_widget_destroy);
	display->widgets = NULL;
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

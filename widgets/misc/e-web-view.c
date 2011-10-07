/*
 * e-web-view.c
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

#include "e-web-view.h"

#include <JavaScriptCore/JavaScript.h>

#include <string.h>
#include <glib/gi18n-lib.h>

#include <camel/camel.h>
#include <libebackend/e-extensible.h>

#include <e-util/e-util.h>
#include <e-util/e-alert-dialog.h>
#include <e-util/e-alert-sink.h>
#include <e-util/e-plugin-ui.h>

#include <mail/e-mail-request.h>

#include "e-popup-action.h"
#include "e-selectable.h"

#define E_WEB_VIEW_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_WEB_VIEW, EWebViewPrivate))

typedef struct _EWebViewRequest EWebViewRequest;

struct _EWebViewPrivate {
	GList *requests;
	GtkUIManager *ui_manager;
	gchar *selected_uri;
	GdkPixbufAnimation *cursor_image;
	gchar *cursor_image_src;

	GHashTable *js_callbacks;

	GtkAction *open_proxy;
	GtkAction *print_proxy;
	GtkAction *save_as_proxy;

	/* Lockdown Options */
	guint disable_printing     : 1;
	guint disable_save_to_disk : 1;

	guint caret_mode : 1;
};

struct _EWebViewRequest {
	GFile *file;
	EWebView *web_view;
	GCancellable *cancellable;
	GInputStream *input_stream;
	GString *output_buffer;
	gchar buffer[4096];
};

enum {
	PROP_0,
	PROP_ANIMATE,
	PROP_CARET_MODE,
	PROP_CURSOR_IMAGE,
	PROP_CURSOR_IMAGE_SRC,
	PROP_DISABLE_PRINTING,
	PROP_DISABLE_SAVE_TO_DISK,
	PROP_INLINE_SPELLING,
	PROP_MAGIC_LINKS,
	PROP_MAGIC_SMILEYS,
	PROP_OPEN_PROXY,
	PROP_PRINT_PROXY,
	PROP_SAVE_AS_PROXY,
	PROP_SELECTED_URI
};

enum {
	POPUP_EVENT,
	STATUS_MESSAGE,
	STOP_LOADING,
	UPDATE_ACTIONS,
	PROCESS_MAILTO,
	LAST_SIGNAL
};

static gpointer parent_class;
static guint signals[LAST_SIGNAL];

static const gchar *ui =
"<ui>"
"  <popup name='context'>"
"    <menuitem action='copy-clipboard'/>"
"    <separator/>"
"    <placeholder name='custom-actions-1'>"
"      <menuitem action='open'/>"
"      <menuitem action='save-as'/>"
"      <menuitem action='http-open'/>"
"      <menuitem action='send-message'/>"
"      <menuitem action='print'/>"
"    </placeholder>"
"    <placeholder name='custom-actions-2'>"
"      <menuitem action='uri-copy'/>"
"      <menuitem action='mailto-copy'/>"
"      <menuitem action='image-copy'/>"
"    </placeholder>"
"    <placeholder name='custom-actions-3'/>"
"    <separator/>"
"    <menuitem action='select-all'/>"
"  </popup>"
"</ui>";

/* Forward Declarations */
static void e_web_view_alert_sink_init (EAlertSinkInterface *interface);
static void e_web_view_selectable_init (ESelectableInterface *interface);

G_DEFINE_TYPE_WITH_CODE (
	EWebView,
	e_web_view,
	WEBKIT_TYPE_WEB_VIEW,
	G_IMPLEMENT_INTERFACE (
		E_TYPE_EXTENSIBLE, NULL)
	G_IMPLEMENT_INTERFACE (
		E_TYPE_ALERT_SINK,
		e_web_view_alert_sink_init)
	G_IMPLEMENT_INTERFACE (
		E_TYPE_SELECTABLE,
		e_web_view_selectable_init))

static void
action_copy_clipboard_cb (GtkAction *action,
                          EWebView *web_view)
{
	e_web_view_copy_clipboard (web_view);
}

static void
action_http_open_cb (GtkAction *action,
                     EWebView *web_view)
{
	const gchar *uri;
	gpointer parent;

	parent = gtk_widget_get_toplevel (GTK_WIDGET (web_view));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	e_show_uri (parent, uri);
}

static void
action_mailto_copy_cb (GtkAction *action,
                       EWebView *web_view)
{
	CamelURL *curl;
	CamelInternetAddress *inet_addr;
	GtkClipboard *clipboard;
	const gchar *uri;
	gchar *text;

	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	/* This should work because we checked it in update_actions(). */
	curl = camel_url_new (uri, NULL);
	g_return_if_fail (curl != NULL);

	inet_addr = camel_internet_address_new ();
	camel_address_decode (CAMEL_ADDRESS (inet_addr), curl->path);
	text = camel_address_format (CAMEL_ADDRESS (inet_addr));
	if (text == NULL || *text == '\0')
		text = g_strdup (uri + strlen ("mailto:"));

	g_object_unref (inet_addr);
	camel_url_free (curl);

	clipboard = gtk_clipboard_get (GDK_SELECTION_PRIMARY);
	gtk_clipboard_set_text (clipboard, text, -1);
	gtk_clipboard_store (clipboard);

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_text (clipboard, text, -1);
	gtk_clipboard_store (clipboard);

	g_free (text);
}

static void
action_select_all_cb (GtkAction *action,
                      EWebView *web_view)
{
	e_web_view_select_all (web_view);
}

static void
action_send_message_cb (GtkAction *action,
                        EWebView *web_view)
{
	const gchar *uri;
	gpointer parent;
	gboolean handled;

	parent = gtk_widget_get_toplevel (GTK_WIDGET (web_view));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	handled = FALSE;
	g_signal_emit (web_view, signals[PROCESS_MAILTO], 0, uri, &handled);

	if (!handled)
		e_show_uri (parent, uri);
}

static void
action_uri_copy_cb (GtkAction *action,
                    EWebView *web_view)
{
	GtkClipboard *clipboard;
	const gchar *uri;

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	gtk_clipboard_set_text (clipboard, uri, -1);
	gtk_clipboard_store (clipboard);
}

static void
action_image_copy_cb (GtkAction *action,
                    EWebView *web_view)
{
	GtkClipboard *clipboard;
	GdkPixbufAnimation *animation;
	GdkPixbuf *pixbuf;

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
	animation = e_web_view_get_cursor_image (web_view);
	g_return_if_fail (animation != NULL);

	pixbuf = gdk_pixbuf_animation_get_static_image (animation);
	if (!pixbuf)
		return;

	gtk_clipboard_set_image (clipboard, pixbuf);
	gtk_clipboard_store (clipboard);
}

static GtkActionEntry uri_entries[] = {

	{ "uri-copy",
	  GTK_STOCK_COPY,
	  N_("_Copy Link Location"),
	  NULL,
	  N_("Copy the link to the clipboard"),
	  G_CALLBACK (action_uri_copy_cb) }
};

static GtkActionEntry http_entries[] = {

	{ "http-open",
	  "emblem-web",
	  N_("_Open Link in Browser"),
	  NULL,
	  N_("Open the link in a web browser"),
	  G_CALLBACK (action_http_open_cb) }
};

static GtkActionEntry mailto_entries[] = {

	{ "mailto-copy",
	  GTK_STOCK_COPY,
	  N_("_Copy Email Address"),
	  NULL,
	  N_("Copy the email address to the clipboard"),
	  G_CALLBACK (action_mailto_copy_cb) },

	{ "send-message",
	  "mail-message-new",
	  N_("_Send New Message To..."),
	  NULL,
	  N_("Send a mail message to this address"),
	  G_CALLBACK (action_send_message_cb) }
};

static GtkActionEntry image_entries[] = {

	{ "image-copy",
	  GTK_STOCK_COPY,
	  N_("_Copy Image"),
	  NULL,
	  N_("Copy the image to the clipboard"),
	  G_CALLBACK (action_image_copy_cb) }
};

static GtkActionEntry selection_entries[] = {

	{ "copy-clipboard",
	  GTK_STOCK_COPY,
	  NULL,
	  NULL,
	  N_("Copy the selection"),
	  G_CALLBACK (action_copy_clipboard_cb) },
};

static GtkActionEntry standard_entries[] = {

	{ "select-all",
	  GTK_STOCK_SELECT_ALL,
	  NULL,
	  NULL,
	  N_("Select all text and images"),
	  G_CALLBACK (action_select_all_cb) }
};

static void
web_view_menu_item_select_cb (EWebView *web_view,
                              GtkWidget *widget)
{
	GtkAction *action;
	GtkActivatable *activatable;
	const gchar *tooltip;

	activatable = GTK_ACTIVATABLE (widget);
	action = gtk_activatable_get_related_action (activatable);
	tooltip = gtk_action_get_tooltip (action);

	if (tooltip == NULL)
		return;

	e_web_view_status_message (web_view, tooltip);
}

static void
web_view_menu_item_deselect_cb (EWebView *web_view)
{
	e_web_view_status_message (web_view, NULL);
}

static void
web_view_connect_proxy_cb (EWebView *web_view,
                           GtkAction *action,
                           GtkWidget *proxy)
{
	if (!GTK_IS_MENU_ITEM (proxy))
		return;

	g_signal_connect_swapped (
		proxy, "select",
		G_CALLBACK (web_view_menu_item_select_cb), web_view);

	g_signal_connect_swapped (
		proxy, "deselect",
		G_CALLBACK (web_view_menu_item_deselect_cb), web_view);
}

static GtkWidget *
web_view_create_plugin_widget_cb (EWebView *web_view,
                                  const gchar *mime_type,
                                  const gchar *uri,
                                  GHashTable *param)
{
	EWebViewClass *class;

	/* XXX WebKitWebView does not provide a class method for
	 *     this signal, so we do so we can override the default
	 *     behavior from subclasses for special URI types. */

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_val_if_fail (class->create_plugin_widget != NULL, NULL);

	return class->create_plugin_widget (web_view, mime_type, uri, param);
}

static void
web_view_hovering_over_link_cb (EWebView *web_view,
                                const gchar *title,
                                const gchar *uri)
{
	EWebViewClass *class;

	/* XXX WebKitWebView does not provide a class method for
	 *     this signal, so we do so we can override the default
	 *     behavior from subclasses for special URI types. */

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_if_fail (class->hovering_over_link != NULL);

	class->hovering_over_link (web_view, title, uri);
}

static gboolean
web_view_navigation_policy_decision_requested_cb (EWebView *web_view,
                                                  WebKitWebFrame *frame,
                                                  WebKitNetworkRequest *request,
                                                  WebKitWebNavigationAction *navigation_action,
                                                  WebKitWebPolicyDecision *policy_decision)
{
	EWebViewClass *class;
	WebKitWebNavigationReason reason;
	const gchar *uri;

	reason = webkit_web_navigation_action_get_reason (navigation_action);
	if (reason != WEBKIT_WEB_NAVIGATION_REASON_LINK_CLICKED)
		return FALSE;

	/* XXX WebKitWebView does not provide a class method for
	 *     this signal, so we do so we can override the default
	 *     behavior from subclasses for special URI types. */

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_val_if_fail (class->link_clicked != NULL, FALSE);

	webkit_web_policy_decision_ignore (policy_decision);

	uri = webkit_network_request_get_uri (request);

	class->link_clicked (web_view, uri);

	return TRUE;
}

static void
web_view_set_property (GObject *object,
                       guint property_id,
                       const GValue *value,
                       GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ANIMATE:
			e_web_view_set_animate (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_CARET_MODE:
			e_web_view_set_caret_mode (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_CURSOR_IMAGE:
			e_web_view_set_cursor_image (
				E_WEB_VIEW (object),
				g_value_get_object (value));
			return;

		case PROP_CURSOR_IMAGE_SRC:
			e_web_view_set_cursor_image_src (
				E_WEB_VIEW (object),
				g_value_get_string (value));
			return;

		case PROP_DISABLE_PRINTING:
			e_web_view_set_disable_printing (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_DISABLE_SAVE_TO_DISK:
			e_web_view_set_disable_save_to_disk (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_INLINE_SPELLING:
			e_web_view_set_inline_spelling (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_MAGIC_LINKS:
			e_web_view_set_magic_links (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_MAGIC_SMILEYS:
			e_web_view_set_magic_smileys (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_OPEN_PROXY:
			e_web_view_set_open_proxy (
				E_WEB_VIEW (object),
				g_value_get_object (value));
			return;

		case PROP_PRINT_PROXY:
			e_web_view_set_print_proxy (
				E_WEB_VIEW (object),
				g_value_get_object (value));
			return;

		case PROP_SAVE_AS_PROXY:
			e_web_view_set_save_as_proxy (
				E_WEB_VIEW (object),
				g_value_get_object (value));
			return;

		case PROP_SELECTED_URI:
			e_web_view_set_selected_uri (
				E_WEB_VIEW (object),
				g_value_get_string (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
web_view_get_property (GObject *object,
                       guint property_id,
                       GValue *value,
                       GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ANIMATE:
			g_value_set_boolean (
				value, e_web_view_get_animate (
				E_WEB_VIEW (object)));
			return;

		case PROP_CARET_MODE:
			g_value_set_boolean (
				value, e_web_view_get_caret_mode (
				E_WEB_VIEW (object)));
			return;

		case PROP_CURSOR_IMAGE:
			g_value_set_object (
				value, e_web_view_get_cursor_image (
				E_WEB_VIEW (object)));
			return;

		case PROP_CURSOR_IMAGE_SRC:
			g_value_set_string (
				value, e_web_view_get_cursor_image_src (
				E_WEB_VIEW (object)));
			return;

		case PROP_DISABLE_PRINTING:
			g_value_set_boolean (
				value, e_web_view_get_disable_printing (
				E_WEB_VIEW (object)));
			return;

		case PROP_DISABLE_SAVE_TO_DISK:
			g_value_set_boolean (
				value, e_web_view_get_disable_save_to_disk (
				E_WEB_VIEW (object)));
			return;

		case PROP_INLINE_SPELLING:
			g_value_set_boolean (
				value, e_web_view_get_inline_spelling (
				E_WEB_VIEW (object)));
			return;

		case PROP_MAGIC_LINKS:
			g_value_set_boolean (
				value, e_web_view_get_magic_links (
				E_WEB_VIEW (object)));
			return;

		case PROP_MAGIC_SMILEYS:
			g_value_set_boolean (
				value, e_web_view_get_magic_smileys (
				E_WEB_VIEW (object)));
			return;

		case PROP_OPEN_PROXY:
			g_value_set_object (
				value, e_web_view_get_open_proxy (
				E_WEB_VIEW (object)));
			return;

		case PROP_PRINT_PROXY:
			g_value_set_object (
				value, e_web_view_get_print_proxy (
				E_WEB_VIEW (object)));
			return;

		case PROP_SAVE_AS_PROXY:
			g_value_set_object (
				value, e_web_view_get_save_as_proxy (
				E_WEB_VIEW (object)));
			return;

		case PROP_SELECTED_URI:
			g_value_set_string (
				value, e_web_view_get_selected_uri (
				E_WEB_VIEW (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
web_view_dispose (GObject *object)
{
	EWebViewPrivate *priv;

	priv = E_WEB_VIEW_GET_PRIVATE (object);

	if (priv->ui_manager != NULL) {
		g_object_unref (priv->ui_manager);
		priv->ui_manager = NULL;
	}

	if (priv->open_proxy != NULL) {
		g_object_unref (priv->open_proxy);
		priv->open_proxy = NULL;
	}

	if (priv->print_proxy != NULL) {
		g_object_unref (priv->print_proxy);
		priv->print_proxy = NULL;
	}

	if (priv->save_as_proxy != NULL) {
		g_object_unref (priv->save_as_proxy);
		priv->save_as_proxy = NULL;
	}

	if (priv->cursor_image != NULL) {
		g_object_unref (priv->cursor_image);
		priv->cursor_image = NULL;
	}

	if (priv->cursor_image_src != NULL) {
		g_free (priv->cursor_image_src);
		priv->cursor_image_src = NULL;
	}

	if (priv->js_callbacks != NULL) {
		g_hash_table_destroy (priv->js_callbacks);
		priv->js_callbacks = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
web_view_finalize (GObject *object)
{
	EWebViewPrivate *priv;

	priv = E_WEB_VIEW_GET_PRIVATE (object);

	/* All URI requests should be complete or cancelled by now. */
	if (priv->requests != NULL)
		g_warning ("Finalizing EWebView with active URI requests");

	g_free (priv->selected_uri);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
web_view_constructed (GObject *object)
{
#ifndef G_OS_WIN32
	GSettings *settings;

	settings = g_settings_new ("org.gnome.desktop.lockdown");

	g_settings_bind (
		settings, "disable-printing",
		object, "disable-printing",
		G_SETTINGS_BIND_GET);

	g_settings_bind (
		settings, "disable-save-to-disk",
		object, "disable-save-to-disk",
		G_SETTINGS_BIND_GET);

	g_object_unref (settings);
#endif

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (parent_class)->constructed (object);
}

static gboolean
web_view_button_press_event (GtkWidget *widget,
                             GdkEventButton *event)
{
	GtkWidgetClass *widget_class;
	EWebView *web_view;
	gboolean event_handled = FALSE;
	gchar *uri;

	web_view = E_WEB_VIEW (widget);

	if (event != NULL && event->button != 3)
		goto chainup;

	uri = e_web_view_extract_uri (web_view, event);

	if (uri != NULL && g_str_has_prefix (uri, "##")) {
		g_free (uri);
		goto chainup;
	}

	g_signal_emit (
		web_view, signals[POPUP_EVENT], 0,
		event, uri, &event_handled);

	g_free (uri);

	if (event_handled)
		return TRUE;

chainup:
	/* Chain up to parent's button_press_event() method. */
	widget_class = GTK_WIDGET_CLASS (parent_class);
	return widget_class->button_press_event (widget, event);
}

static gboolean
web_view_scroll_event (GtkWidget *widget,
                       GdkEventScroll *event)
{
	if (event->state & GDK_CONTROL_MASK) {
		switch (event->direction) {
			case GDK_SCROLL_UP:
				e_web_view_zoom_in (E_WEB_VIEW (widget));
				return TRUE;
			case GDK_SCROLL_DOWN:
				e_web_view_zoom_out (E_WEB_VIEW (widget));
				return TRUE;
			default:
				break;
		}
	}

	return FALSE;
}

static GtkWidget *
web_view_create_plugin_widget (EWebView *web_view,
                               const gchar *mime_type,
                               const gchar *uri,
                               GHashTable *param)
{
	GtkWidget *widget = NULL;

	if (g_strcmp0 (mime_type, "image/x-themed-icon") == 0) {
		GtkIconTheme *icon_theme;
		GdkPixbuf *pixbuf;
		gpointer data;
		glong size = 0;
		GError *error = NULL;

		icon_theme = gtk_icon_theme_get_default ();

		if (size == 0) {
			data = g_hash_table_lookup (param, "width");
			if (data != NULL)
				size = MAX (size, strtol (data, NULL, 10));
		}

		if (size == 0) {
			data = g_hash_table_lookup (param, "height");
			if (data != NULL)
				size = MAX (size, strtol (data, NULL, 10));
		}

		if (size == 0)
			size = 32;  /* arbitrary default */

		pixbuf = gtk_icon_theme_load_icon (
			icon_theme, uri, size, 0, &error);
		if (pixbuf != NULL) {
			widget = gtk_image_new_from_pixbuf (pixbuf);
			g_object_unref (pixbuf);
		} else if (error != NULL) {
			g_warning ("%s", error->message);
			g_error_free (error);
		}
	}

	return widget;
}

static gchar *
web_view_extract_uri (EWebView *web_view,
                      GdkEventButton *event)
{
	WebKitHitTestResult *result;
	WebKitHitTestResultContext context;
	gchar *uri = NULL;

	result = webkit_web_view_get_hit_test_result (
		WEBKIT_WEB_VIEW (web_view), event);

	g_object_get (result, "context", &context, "link-uri", &uri, NULL);

	if (context & WEBKIT_HIT_TEST_RESULT_CONTEXT_LINK)
		return uri;

	g_free (uri);

	return NULL;
}

static void
web_view_hovering_over_link (EWebView *web_view,
                             const gchar *title,
                             const gchar *uri)
{
	CamelInternetAddress *address;
	CamelURL *curl;
	const gchar *format = NULL;
	gchar *message = NULL;
	gchar *who;

	if (uri == NULL || *uri == '\0')
		goto exit;

	if (g_str_has_prefix (uri, "mailto:"))
		format = _("Click to mail %s");
	else if (g_str_has_prefix (uri, "callto:"))
		format = _("Click to call %s");
	else if (g_str_has_prefix (uri, "h323:"))
		format = _("Click to call %s");
	else if (g_str_has_prefix (uri, "sip:"))
		format = _("Click to call %s");
	else if (g_str_has_prefix (uri, "##"))
		message = g_strdup (_("Click to hide/unhide addresses"));
	else
		message = g_strdup_printf (_("Click to open %s"), uri);

	if (format == NULL)
		goto exit;

	/* XXX Use something other than Camel here.  Surely
	 *     there's other APIs around that can do this. */
	curl = camel_url_new (uri, NULL);
	address = camel_internet_address_new ();
	camel_address_decode (CAMEL_ADDRESS (address), curl->path);
	who = camel_address_format (CAMEL_ADDRESS (address));
	g_object_unref (address);
	camel_url_free (curl);

	if (who == NULL)
		who = g_strdup (strchr (uri, ':') + 1);

	message = g_strdup_printf (format, who);

	g_free (who);

exit:
	e_web_view_status_message (web_view, message);

	g_free (message);
}

static void
web_view_link_clicked (EWebView *web_view,
                       const gchar *uri)
{
	gpointer parent;

	parent = gtk_widget_get_toplevel (GTK_WIDGET (web_view));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	e_show_uri (parent, uri);
}

static void
web_view_load_string (EWebView *web_view,
                      const gchar *string)
{
	if (string == NULL)
		string = "";

	webkit_web_view_load_string (
		WEBKIT_WEB_VIEW (web_view),
		string, "text/html", "UTF-8", "file://");
}

static void
web_view_load_uri (EWebView *web_view,
		   const gchar *uri)
{
	if (uri == NULL)
		uri = "about:blank";

	webkit_web_view_load_uri (
		WEBKIT_WEB_VIEW (web_view), uri);
}

static void
web_view_frame_load_string (EWebView *web_view,
			    const gchar *frame_name,
			    const gchar *string)
{
	WebKitWebFrame *main_frame, *frame;

	if (string == NULL)
		string = "";

	main_frame = webkit_web_view_get_main_frame (WEBKIT_WEB_VIEW (web_view));
	if (main_frame) {
		frame = webkit_web_frame_find_frame (main_frame, frame_name);

		if (frame)
			webkit_web_frame_load_string (
				frame, string, "text/html", "UTF-8", "file://");
	}
}

static void
web_view_frame_load_uri (EWebView *web_view,
			 const gchar *frame_name,
			 const gchar *uri)
{
	WebKitWebFrame *main_frame, *frame;

	if (uri == NULL)
		uri = "about:blank";

	main_frame = webkit_web_view_get_main_frame (WEBKIT_WEB_VIEW (web_view));
	if (main_frame) {
		frame = webkit_web_frame_find_frame (main_frame, frame_name);

		if (frame)
			webkit_web_frame_load_uri (frame, uri);
	}
}

static gboolean
web_view_popup_event (EWebView *web_view,
                      GdkEventButton *event,
                      const gchar *uri)
{
	e_web_view_set_selected_uri (web_view, uri);
	e_web_view_show_popup_menu (web_view, event, NULL, NULL);

	return TRUE;
}

static void
web_view_stop_loading (EWebView *web_view)
{
	webkit_web_view_stop_loading (WEBKIT_WEB_VIEW (web_view));
}

static void
web_view_update_actions (EWebView *web_view)
{
	GtkActionGroup *action_group;
	gboolean have_selection;
	gboolean scheme_is_http = FALSE;
	gboolean scheme_is_mailto = FALSE;
	gboolean uri_is_valid = FALSE;
	gboolean has_cursor_image;
	gboolean visible;
	const gchar *group_name;
	const gchar *uri;

	uri = e_web_view_get_selected_uri (web_view);
	have_selection = e_web_view_is_selection_active (web_view);
	has_cursor_image = e_web_view_get_cursor_image (web_view) != NULL;

	/* Parse the URI early so we know if the actions will work. */
	if (uri != NULL) {
		CamelURL *curl;

		curl = camel_url_new (uri, NULL);
		uri_is_valid = (curl != NULL);
		camel_url_free (curl);

		scheme_is_http =
			(g_ascii_strncasecmp (uri, "http:", 5) == 0) ||
			(g_ascii_strncasecmp (uri, "https:", 6) == 0);

		scheme_is_mailto =
			(g_ascii_strncasecmp (uri, "mailto:", 7) == 0);
	}

	/* Allow copying the URI even if it's malformed. */
	group_name = "uri";
	visible = (uri != NULL) && !scheme_is_mailto;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "http";
	visible = uri_is_valid && scheme_is_http;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "mailto";
	visible = uri_is_valid && scheme_is_mailto;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "image";
	visible = has_cursor_image;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "selection";
	visible = have_selection;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "standard";
	visible = (uri == NULL);
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "lockdown-printing";
	visible = (uri == NULL) && !web_view->priv->disable_printing;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "lockdown-save-to-disk";
	visible = (uri == NULL) && !web_view->priv->disable_save_to_disk;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);
}

static void
web_view_submit_alert (EAlertSink *alert_sink,
                       EAlert *alert)
{
	GtkIconInfo *icon_info;
	EWebView *web_view;
	GtkWidget *dialog;
	GString *buffer;
	const gchar *icon_name = NULL;
	const gchar *filename;
	gpointer parent;
	gchar *icon_uri;
	gint size = 0;
	GError *error = NULL;

	web_view = E_WEB_VIEW (alert_sink);

	parent = gtk_widget_get_toplevel (GTK_WIDGET (web_view));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	/* We use equivalent named icons instead of stock IDs,
	 * since it's easier to get the filename of the icon. */
	switch (e_alert_get_message_type (alert)) {
		case GTK_MESSAGE_INFO:
			icon_name = "dialog-information";
			break;

		case GTK_MESSAGE_WARNING:
			icon_name = "dialog-warning";
			break;

		case GTK_MESSAGE_ERROR:
			icon_name = "dialog-error";
			break;

		default:
			dialog = e_alert_dialog_new (parent, alert);
			gtk_dialog_run (GTK_DIALOG (dialog));
			gtk_widget_destroy (dialog);
			return;
	}

	gtk_icon_size_lookup (GTK_ICON_SIZE_DIALOG, &size, NULL);

	icon_info = gtk_icon_theme_lookup_icon (
		gtk_icon_theme_get_default (),
		icon_name, size, GTK_ICON_LOOKUP_NO_SVG);
	g_return_if_fail (icon_info != NULL);

	filename = gtk_icon_info_get_filename (icon_info);
	icon_uri = g_filename_to_uri (filename, NULL, &error);

	if (error != NULL) {
		g_warning ("%s", error->message);
		g_clear_error (&error);
	}

	buffer = g_string_sized_new (512);

	g_string_append (
		buffer,
		"<html>"
		"<head>"
		"<meta http-equiv=\"content-type\""
		" content=\"text/html; charset=utf-8\">"
		"</head>"
		"<body>");

	g_string_append (
		buffer,
		"<table bgcolor='#000000' width='100%'"
		" cellpadding='1' cellspacing='0'>"
		"<tr>"
		"<td>"
		"<table bgcolor='#dddddd' width='100%' cellpadding='6'>"
		"<tr>");

	g_string_append_printf (
		buffer,
		"<tr>"
		"<td valign='top'>"
		"<img src='%s'/>"
		"</td>"
		"<td align='left' width='100%%'>"
		"<h3>%s</h3>"
		"%s"
		"</td>"
		"</tr>",
		icon_uri,
		e_alert_get_primary_text (alert),
		e_alert_get_secondary_text (alert));

	g_string_append (
		buffer,
		"</table>"
		"</td>"
		"</tr>"
		"</table>"
		"</body>"
		"</html>");

	e_web_view_load_string (web_view, buffer->str);

	g_string_free (buffer, TRUE);

	gtk_icon_info_free (icon_info);
	g_free (icon_uri);
}

static void
web_view_selectable_update_actions (ESelectable *selectable,
                                    EFocusTracker *focus_tracker,
                                    GdkAtom *clipboard_targets,
                                    gint n_clipboard_targets)
{
	WebKitWebView *web_view;
	GtkAction *action;
	gboolean sensitive;
	const gchar *tooltip;

	web_view = WEBKIT_WEB_VIEW (selectable);

	action = e_focus_tracker_get_cut_clipboard_action (focus_tracker);
	sensitive = webkit_web_view_can_cut_clipboard (web_view);
	tooltip = _("Cut the selection");
	gtk_action_set_sensitive (action, sensitive);
	gtk_action_set_tooltip (action, tooltip);

	action = e_focus_tracker_get_copy_clipboard_action (focus_tracker);
	sensitive = webkit_web_view_can_copy_clipboard (web_view);
	tooltip = _("Copy the selection");
	gtk_action_set_sensitive (action, sensitive);
	gtk_action_set_tooltip (action, tooltip);

	action = e_focus_tracker_get_paste_clipboard_action (focus_tracker);
	sensitive = webkit_web_view_can_paste_clipboard (web_view);
	tooltip = _("Paste the clipboard");
	gtk_action_set_sensitive (action, sensitive);
	gtk_action_set_tooltip (action, tooltip);

	action = e_focus_tracker_get_select_all_action (focus_tracker);
	sensitive = TRUE;
	tooltip = _("Select all text and images");
	gtk_action_set_sensitive (action, sensitive);
	gtk_action_set_tooltip (action, tooltip);
}

static void
web_view_selectable_cut_clipboard (ESelectable *selectable)
{
	e_web_view_cut_clipboard (E_WEB_VIEW (selectable));
}

static void
web_view_selectable_copy_clipboard (ESelectable *selectable)
{
	e_web_view_copy_clipboard (E_WEB_VIEW (selectable));
}

static void
web_view_selectable_paste_clipboard (ESelectable *selectable)
{
	e_web_view_paste_clipboard (E_WEB_VIEW (selectable));
}

static void
web_view_selectable_select_all (ESelectable *selectable)
{
	e_web_view_select_all (E_WEB_VIEW (selectable));
}

static void
e_web_view_class_init (EWebViewClass *class)
{
	GObjectClass *object_class;
	GtkWidgetClass *widget_class;
#if 0  /* WEBKIT */
	GtkHTMLClass *html_class;
#endif

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EWebViewPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = web_view_set_property;
	object_class->get_property = web_view_get_property;
	object_class->dispose = web_view_dispose;
	object_class->finalize = web_view_finalize;
	object_class->constructed = web_view_constructed;

	widget_class = GTK_WIDGET_CLASS (class);
	widget_class->button_press_event = web_view_button_press_event;
	widget_class->scroll_event = web_view_scroll_event;

#if 0  /* WEBKIT */
	html_class = GTK_HTML_CLASS (class);
	html_class->url_requested = web_view_url_requested;
#endif

	class->create_plugin_widget = web_view_create_plugin_widget;
	class->extract_uri = web_view_extract_uri;
	class->hovering_over_link = web_view_hovering_over_link;
	class->link_clicked = web_view_link_clicked;
	class->load_string = web_view_load_string;
	class->load_uri = web_view_load_uri;
	class->frame_load_string = web_view_frame_load_string;
	class->frame_load_uri = web_view_frame_load_uri;
	class->popup_event = web_view_popup_event;
	class->stop_loading = web_view_stop_loading;
	class->update_actions = web_view_update_actions;

	g_object_class_install_property (
		object_class,
		PROP_ANIMATE,
		g_param_spec_boolean (
			"animate",
			"Animate Images",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_CARET_MODE,
		g_param_spec_boolean (
			"caret-mode",
			"Caret Mode",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	/* Inherited from ESelectableInterface */
	g_object_class_override_property (
		object_class,
		PROP_COPY_TARGET_LIST,
		"copy-target-list");

	g_object_class_install_property (
		object_class,
		PROP_CURSOR_IMAGE,
		g_param_spec_object (
			"cursor-image",
			"Image animation at the mouse cursor",
			NULL,
			GDK_TYPE_PIXBUF_ANIMATION,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_CURSOR_IMAGE_SRC,
		g_param_spec_string (
			"cursor-image-src",
			"Image source uri at the mouse cursor",
			NULL,
			NULL,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_DISABLE_PRINTING,
		g_param_spec_boolean (
			"disable-printing",
			"Disable Printing",
			NULL,
			FALSE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_DISABLE_SAVE_TO_DISK,
		g_param_spec_boolean (
			"disable-save-to-disk",
			"Disable Save-to-Disk",
			NULL,
			FALSE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_INLINE_SPELLING,
		g_param_spec_boolean (
			"inline-spelling",
			"Inline Spelling",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_MAGIC_LINKS,
		g_param_spec_boolean (
			"magic-links",
			"Magic Links",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_MAGIC_SMILEYS,
		g_param_spec_boolean (
			"magic-smileys",
			"Magic Smileys",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_OPEN_PROXY,
		g_param_spec_object (
			"open-proxy",
			"Open Proxy",
			NULL,
			GTK_TYPE_ACTION,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_PRINT_PROXY,
		g_param_spec_object (
			"print-proxy",
			"Print Proxy",
			NULL,
			GTK_TYPE_ACTION,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_SAVE_AS_PROXY,
		g_param_spec_object (
			"save-as-proxy",
			"Save As Proxy",
			NULL,
			GTK_TYPE_ACTION,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_SELECTED_URI,
		g_param_spec_string (
			"selected-uri",
			"Selected URI",
			NULL,
			NULL,
			G_PARAM_READWRITE));

	signals[POPUP_EVENT] = g_signal_new (
		"popup-event",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, popup_event),
		g_signal_accumulator_true_handled, NULL,
		e_marshal_BOOLEAN__BOXED_STRING,
		G_TYPE_BOOLEAN, 2,
		GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE,
		G_TYPE_STRING);

	signals[STATUS_MESSAGE] = g_signal_new (
		"status-message",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, status_message),
		NULL, NULL,
		g_cclosure_marshal_VOID__STRING,
		G_TYPE_NONE, 1,
		G_TYPE_STRING);

	signals[STOP_LOADING] = g_signal_new (
		"stop-loading",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, stop_loading),
		NULL, NULL,
		g_cclosure_marshal_VOID__VOID,
		G_TYPE_NONE, 0);

	signals[UPDATE_ACTIONS] = g_signal_new (
		"update-actions",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, update_actions),
		NULL, NULL,
		g_cclosure_marshal_VOID__VOID,
		G_TYPE_NONE, 0);

	/* return TRUE when a signal handler processed the mailto URI */
	signals[PROCESS_MAILTO] = g_signal_new (
		"process-mailto",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, process_mailto),
		NULL, NULL,
		e_marshal_BOOLEAN__STRING,
		G_TYPE_BOOLEAN, 1, G_TYPE_STRING);
}

static void
e_web_view_alert_sink_init (EAlertSinkInterface *interface)
{
	interface->submit_alert = web_view_submit_alert;
}

static void
e_web_view_selectable_init (ESelectableInterface *interface)
{
	interface->update_actions = web_view_selectable_update_actions;
	interface->cut_clipboard = web_view_selectable_cut_clipboard;
	interface->copy_clipboard = web_view_selectable_copy_clipboard;
	interface->paste_clipboard = web_view_selectable_paste_clipboard;
	interface->select_all = web_view_selectable_select_all;
}

static void
e_web_view_init (EWebView *web_view)
{
	GtkUIManager *ui_manager;
	GtkActionGroup *action_group;
	EPopupAction *popup_action;
	WebKitWebSettings *web_settings;
	const gchar *domain = GETTEXT_PACKAGE;
	const gchar *id;
	GtkStyleContext *context;
	const PangoFontDescription *font;
	GError *error = NULL;

	web_view->priv = E_WEB_VIEW_GET_PRIVATE (web_view);

	g_signal_connect (
		web_view, "create-plugin-widget",
		G_CALLBACK (web_view_create_plugin_widget_cb), NULL);

	g_signal_connect (
		web_view, "hovering-over-link",
		G_CALLBACK (web_view_hovering_over_link_cb), NULL);

	g_signal_connect (
		web_view, "navigation-policy-decision-requested",
		G_CALLBACK (web_view_navigation_policy_decision_requested_cb),
		NULL);

	ui_manager = gtk_ui_manager_new ();
	web_view->priv->ui_manager = ui_manager;

	web_view->priv->js_callbacks = g_hash_table_new_full (g_str_hash, g_str_equal,
		(GDestroyNotify) g_free, NULL);

	g_signal_connect_swapped (
		ui_manager, "connect-proxy",
		G_CALLBACK (web_view_connect_proxy_cb), web_view);

	web_settings = webkit_web_view_get_settings (
		WEBKIT_WEB_VIEW (web_view));

	/* Use same font-size as rest of Evolution */
	context = gtk_widget_get_style_context (GTK_WIDGET (web_view));
	font = gtk_style_context_get_font (context, GTK_STATE_FLAG_NORMAL);
	g_object_set (G_OBJECT (web_settings),
		"default-font-size", (pango_font_description_get_size (font) / PANGO_SCALE),
		"default-monospace-font-size", (pango_font_description_get_size (font) / PANGO_SCALE),
		NULL);

	/* Force frames to be as high as their content (e.g. no scrolling) */
	g_object_set (G_OBJECT (web_settings), "enable-frame-flattening",
		TRUE, NULL);

	g_object_bind_property (
		web_view, "caret-mode",
		web_settings, "enable-caret-browsing",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE);

	action_group = gtk_action_group_new ("uri");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, uri_entries,
		G_N_ELEMENTS (uri_entries), web_view);

	action_group = gtk_action_group_new ("http");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, http_entries,
		G_N_ELEMENTS (http_entries), web_view);

	action_group = gtk_action_group_new ("mailto");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, mailto_entries,
		G_N_ELEMENTS (mailto_entries), web_view);

	action_group = gtk_action_group_new ("image");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, image_entries,
		G_N_ELEMENTS (image_entries), web_view);

	action_group = gtk_action_group_new ("selection");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, selection_entries,
		G_N_ELEMENTS (selection_entries), web_view);

	action_group = gtk_action_group_new ("standard");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, standard_entries,
		G_N_ELEMENTS (standard_entries), web_view);

	popup_action = e_popup_action_new ("open");
	gtk_action_group_add_action (action_group, GTK_ACTION (popup_action));
	g_object_unref (popup_action);

	g_object_bind_property (
		web_view, "open-proxy",
		popup_action, "related-action",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE);

	/* Support lockdown. */

	action_group = gtk_action_group_new ("lockdown-printing");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	popup_action = e_popup_action_new ("print");
	gtk_action_group_add_action (action_group, GTK_ACTION (popup_action));
	g_object_unref (popup_action);

	g_object_bind_property (
		web_view, "print-proxy",
		popup_action, "related-action",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE);

	action_group = gtk_action_group_new ("lockdown-save-to-disk");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	popup_action = e_popup_action_new ("save-as");
	gtk_action_group_add_action (action_group, GTK_ACTION (popup_action));
	g_object_unref (popup_action);

	g_object_bind_property (
		web_view, "save-as-proxy",
		popup_action, "related-action",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE);

	/* Because we are loading from a hard-coded string, there is
	 * no chance of I/O errors.  Failure here implies a malformed
	 * UI definition.  Full stop. */
	gtk_ui_manager_add_ui_from_string (ui_manager, ui, -1, &error);
	if (error != NULL)
		g_error ("%s", error->message);

	id = "org.gnome.evolution.webview";
	e_plugin_ui_register_manager (ui_manager, id, web_view);
	e_plugin_ui_enable_manager (ui_manager, id);

	e_extensible_load_extensions (E_EXTENSIBLE (web_view));

}

GtkWidget *
e_web_view_new (void)
{
	return g_object_new (E_TYPE_WEB_VIEW, NULL);
}

void
e_web_view_clear (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	e_web_view_load_string (web_view, NULL);
}

void
e_web_view_load_string (EWebView *web_view,
                        const gchar *string)
{
	EWebViewClass *class;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_if_fail (class->load_string != NULL);

	class->load_string (web_view, string);
}

void
e_web_view_load_uri (EWebView *web_view,
		     const gchar *uri)
{
	EWebViewClass *class;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_if_fail (class->load_uri != NULL);

	class->load_uri (web_view, uri);
}

void
e_web_view_reload (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_reload (WEBKIT_WEB_VIEW (web_view));
}

const gchar*
e_web_view_get_uri (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return webkit_web_view_get_uri (WEBKIT_WEB_VIEW (web_view));
}

void
e_web_view_frame_load_string (EWebView *web_view,
			      const gchar *frame_name,
			      const gchar *string)
{
	EWebViewClass *class;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));
	g_return_if_fail (frame_name != NULL);

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_if_fail (class->frame_load_string != NULL);

	class->frame_load_string (web_view, frame_name, string);
}

void
e_web_view_frame_load_uri (EWebView *web_view,
			   const gchar *frame_name,
			   const gchar *uri)
{
	EWebViewClass *class;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));
	g_return_if_fail (frame_name != NULL);

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_if_fail (class->frame_load_uri != NULL);

	class->frame_load_uri (web_view, frame_name, uri);
}

const gchar*
e_web_view_frame_get_uri (EWebView *web_view,
			  const gchar *frame_name)
{
	WebKitWebFrame *main_frame, *frame;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
	g_return_val_if_fail (frame_name != NULL, NULL);

	main_frame = webkit_web_view_get_main_frame (WEBKIT_WEB_VIEW (web_view));
	if (main_frame) {
		frame = webkit_web_frame_find_frame (main_frame, frame_name);

		if (frame)
			return webkit_web_frame_get_uri (frame);
	}

	return NULL;
}

gchar*
e_web_view_get_html (EWebView *web_view)
{
	GValue html = {0};

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	if (e_web_view_exec_script (web_view, "return document.documentElement.innerHTML;", &html) == G_TYPE_STRING)
		return g_strdup (g_value_get_string (&html));
	else
		return NULL;
}

JSGlobalContextRef
e_web_view_get_global_context (EWebView *web_view)
{
	WebKitWebFrame *main_frame;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), 0);

	main_frame = webkit_web_view_get_main_frame (WEBKIT_WEB_VIEW (web_view));
	return webkit_web_frame_get_global_context (main_frame);
}

GType
e_web_view_exec_script (EWebView *web_view, const gchar *script, GValue *value)
{
	WebKitWebFrame *main_frame;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), G_TYPE_INVALID);
	g_return_val_if_fail (script != NULL, G_TYPE_INVALID);

	main_frame = webkit_web_view_get_main_frame (WEBKIT_WEB_VIEW (web_view));

	return e_web_view_frame_exec_script (web_view,
		webkit_web_frame_get_name (main_frame),
		script, value);
}

GType
e_web_view_frame_exec_script (EWebView *web_view, const gchar *frame_name, const gchar *script, GValue *value)
{
	WebKitWebFrame *main_frame, *frame;
	JSGlobalContextRef context;
	JSValueRef js_value, error = NULL;
	JSType js_type;
	JSStringRef js_script;
	JSStringRef js_str;
	size_t str_len;
	gchar *str;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), G_TYPE_INVALID);
	g_return_val_if_fail (script != NULL, G_TYPE_INVALID);

	main_frame = webkit_web_view_get_main_frame (WEBKIT_WEB_VIEW (web_view));
	frame = webkit_web_frame_find_frame (main_frame, frame_name);

	context = webkit_web_frame_get_global_context (frame);

	js_script = JSStringCreateWithUTF8CString (script);
	js_value = JSEvaluateScript (context, js_script, NULL, NULL, 0, &error);
	JSStringRelease (js_script);

	if (error) {
		gchar *msg;
		js_str = JSValueToStringCopy (context, error, NULL);
		str_len = JSStringGetLength (js_str);

		msg = g_malloc (str_len + 1);
		JSStringGetUTF8CString (js_str, msg, str_len + 1);
		JSStringRelease (js_str);

		g_message ("JavaScript Execution Failed: %s", msg);
		g_free (msg);

		return G_TYPE_INVALID;
	}

	if (!value)
		return G_TYPE_NONE;

	js_type = JSValueGetType (context, js_value);
	switch (js_type) {
		case kJSTypeBoolean:
			g_value_init (value, G_TYPE_BOOLEAN);
			g_value_set_boolean (value, JSValueToBoolean (context, js_value));
			break;
		case kJSTypeNumber:
			g_value_init (value, G_TYPE_DOUBLE);
			g_value_set_double(value, JSValueToNumber (context, js_value, NULL));
			break;
		case kJSTypeString:
			js_str = JSValueToStringCopy (context, js_value, NULL);
			str_len = JSStringGetLength (js_str);
			str = g_malloc (str_len + 1);
			JSStringGetUTF8CString (js_str, str, str_len + 1);
			JSStringRelease (js_str);
			g_value_init (value, G_TYPE_STRING);
			g_value_set_string (value, str);
			g_free (str);
			break;
		case kJSTypeObject:
			g_value_init (value, G_TYPE_OBJECT);
			g_value_set_object (value, JSValueToObject (context, js_value, NULL));
			break;
		case kJSTypeNull:
			g_value_init (value, G_TYPE_POINTER);
			g_value_set_pointer (value, NULL);
			break;
		case kJSTypeUndefined:
			break;
	}

	return G_VALUE_TYPE (value);
}

static JSValueRef
web_view_handle_js_callback (JSContextRef ctx, JSObjectRef function, JSObjectRef this_object,
			     size_t argument_count, const JSValueRef arguments[], JSValueRef *exception)
{
	gpointer web_view;
	gpointer user_data;
	gchar *fnc_name;
	size_t fnc_name_len;

	EWebViewJSFunctionCallback callback;

	JSStringRef js_webview_prop = JSStringCreateWithUTF8CString ("webview");
	JSStringRef js_userdata_prop = JSStringCreateWithUTF8CString ("user-data");
	JSStringRef js_fncname_prop = JSStringCreateWithUTF8CString ("fnc-name");
	JSStringRef js_fncname_str;

	JSValueRef js_webview = JSObjectGetProperty (ctx, function, js_webview_prop, NULL);
	JSValueRef js_userdata = JSObjectGetProperty (ctx, function, js_userdata_prop, NULL);
	JSValueRef js_fncname = JSObjectGetProperty (ctx, function, js_fncname_prop, NULL);

	web_view = GINT_TO_POINTER ((int) JSValueToNumber (ctx, js_webview, NULL));
	user_data = GINT_TO_POINTER ((int) JSValueToNumber (ctx, js_userdata, NULL));
	js_fncname_str = JSValueToStringCopy (ctx, js_fncname, NULL);
	fnc_name_len = JSStringGetLength (js_fncname_str);

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), 0);

	/* Convert fncname to gchar* and lookup the callback in hashtable */
	fnc_name = g_malloc (fnc_name_len + 1);
	JSStringGetUTF8CString (js_fncname_str, fnc_name, fnc_name_len+1);
	callback = g_hash_table_lookup (E_WEB_VIEW (web_view)->priv->js_callbacks, fnc_name);

	g_return_val_if_fail (callback != NULL, 0);

	/* Call the callback function */
	callback (E_WEB_VIEW (web_view), argument_count, arguments, user_data);

	JSStringRelease (js_fncname_str);
	JSStringRelease (js_fncname_prop);
	JSStringRelease (js_webview_prop);
	JSStringRelease (js_userdata_prop);

	g_free (fnc_name);

	return 0;
}

void
e_web_view_install_js_callback (EWebView *web_view, const gchar *fnc_name, EWebViewJSFunctionCallback callback, gpointer user_data)
{
	WebKitWebFrame *frame;
	JSGlobalContextRef ctx;
	JSObjectRef global_obj, js_func;
	JSStringRef js_fnc_name, js_webview_prop, js_userdata_prop, js_fncname_prop;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));
	g_return_if_fail (fnc_name != NULL);
	g_return_if_fail (callback != NULL);

	frame = webkit_web_view_get_main_frame (WEBKIT_WEB_VIEW (web_view));
	ctx = webkit_web_frame_get_global_context (frame);
	global_obj = JSContextGetGlobalObject (ctx);
	js_fnc_name = JSStringCreateWithUTF8CString (fnc_name);
	js_func = JSObjectMakeFunctionWithCallback (ctx, NULL,
		(JSObjectCallAsFunctionCallback) web_view_handle_js_callback);
	js_webview_prop = JSStringCreateWithUTF8CString ("webview");
	js_userdata_prop = JSStringCreateWithUTF8CString ("user-data");
	js_fncname_prop = JSStringCreateWithUTF8CString ("fnc-name");

	/* Set some properties to the function */
	JSObjectSetProperty (ctx, js_func, js_webview_prop, JSValueMakeNumber (ctx, GPOINTER_TO_INT (web_view)), 0, NULL);
	JSObjectSetProperty (ctx, js_func, js_userdata_prop, JSValueMakeNumber (ctx, GPOINTER_TO_INT (user_data)), 0, NULL);
	JSObjectSetProperty (ctx, js_func, js_fncname_prop, JSValueMakeString (ctx, js_fnc_name), 0, NULL);

	/* Set the function as a property of global object */
	JSObjectSetProperty (ctx, global_obj, js_fnc_name, js_func, 0, NULL);

	JSStringRelease (js_fncname_prop);
	JSStringRelease (js_userdata_prop);
	JSStringRelease (js_webview_prop);
	JSStringRelease (js_fnc_name);

	g_hash_table_insert (web_view->priv->js_callbacks, g_strdup (fnc_name), callback);
}

gboolean
e_web_view_get_animate (EWebView *web_view)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX This is just here to maintain symmetry
	 *     with e_web_view_set_animate(). */

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return gtk_html_get_animate (GTK_HTML (web_view));
#endif

	return FALSE;
}

void
e_web_view_set_animate (EWebView *web_view,
                        gboolean animate)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX GtkHTML does not utilize GObject properties as well
	 *     as it could.  This just wraps gtk_html_set_animate()
	 *     so we can get a "notify::animate" signal. */

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	gtk_html_set_animate (GTK_HTML (web_view), animate);

	g_object_notify (G_OBJECT (web_view), "animate");
#endif
}

gboolean
e_web_view_get_caret_mode (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->caret_mode;
}

void
e_web_view_set_caret_mode (EWebView *web_view,
                           gboolean caret_mode)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	web_view->priv->caret_mode = caret_mode;

	g_object_notify (G_OBJECT (web_view), "caret-mode");
}

GtkTargetList *
e_web_view_get_copy_target_list (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return webkit_web_view_get_copy_target_list (
		WEBKIT_WEB_VIEW (web_view));
}

gboolean
e_web_view_get_disable_printing (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->disable_printing;
}

void
e_web_view_set_disable_printing (EWebView *web_view,
                                 gboolean disable_printing)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	web_view->priv->disable_printing = disable_printing;

	g_object_notify (G_OBJECT (web_view), "disable-printing");
}

gboolean
e_web_view_get_disable_save_to_disk (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->disable_save_to_disk;
}

void
e_web_view_set_disable_save_to_disk (EWebView *web_view,
                                     gboolean disable_save_to_disk)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	web_view->priv->disable_save_to_disk = disable_save_to_disk;

	g_object_notify (G_OBJECT (web_view), "disable-save-to-disk");
}

gboolean
e_web_view_get_editable (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return webkit_web_view_get_editable (WEBKIT_WEB_VIEW (web_view));
}

void
e_web_view_set_editable (EWebView *web_view,
                         gboolean editable)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_set_editable (WEBKIT_WEB_VIEW (web_view), editable);
}

gboolean
e_web_view_get_inline_spelling (EWebView *web_view)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX This is just here to maintain symmetry
	 *     with e_web_view_set_inline_spelling(). */

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return gtk_html_get_inline_spelling (GTK_HTML (web_view));
#endif

	return FALSE;
}

void
e_web_view_set_inline_spelling (EWebView *web_view,
                                gboolean inline_spelling)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX GtkHTML does not utilize GObject properties as well
	 *     as it could.  This just wraps gtk_html_set_inline_spelling()
	 *     so we get a "notify::inline-spelling" signal. */

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	gtk_html_set_inline_spelling (GTK_HTML (web_view), inline_spelling);

	g_object_notify (G_OBJECT (web_view), "inline-spelling");
#endif
}

gboolean
e_web_view_get_magic_links (EWebView *web_view)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX This is just here to maintain symmetry
	 *     with e_web_view_set_magic_links(). */

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return gtk_html_get_magic_links (GTK_HTML (web_view));
#endif

	return FALSE;
}

void
e_web_view_set_magic_links (EWebView *web_view,
                            gboolean magic_links)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX GtkHTML does not utilize GObject properties as well
	 *     as it could.  This just wraps gtk_html_set_magic_links()
	 *     so we can get a "notify::magic-links" signal. */

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	gtk_html_set_magic_links (GTK_HTML (web_view), magic_links);

	g_object_notify (G_OBJECT (web_view), "magic-links");
#endif
}

gboolean
e_web_view_get_magic_smileys (EWebView *web_view)
{
#if 0  /* WEBKIT - No equivalent property? */
	/* XXX This is just here to maintain symmetry
	 *     with e_web_view_set_magic_smileys(). */

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return gtk_html_get_magic_smileys (GTK_HTML (web_view));
#endif

	return FALSE;
}

void
e_web_view_set_magic_smileys (EWebView *web_view,
                              gboolean magic_smileys)
{
#if 0  /* WEBKIT - No equivalent property? */
	/* XXX GtkHTML does not utilize GObject properties as well
	 *     as it could.  This just wraps gtk_html_set_magic_smileys()
	 *     so we can get a "notify::magic-smileys" signal. */

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	gtk_html_set_magic_smileys (GTK_HTML (web_view), magic_smileys);

	g_object_notify (G_OBJECT (web_view), "magic-smileys");
#endif
}

const gchar *
e_web_view_get_selected_uri (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return web_view->priv->selected_uri;
}

void
e_web_view_set_selected_uri (EWebView *web_view,
                             const gchar *selected_uri)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	g_free (web_view->priv->selected_uri);
	web_view->priv->selected_uri = g_strdup (selected_uri);

	g_object_notify (G_OBJECT (web_view), "selected-uri");
}

GdkPixbufAnimation *
e_web_view_get_cursor_image (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return web_view->priv->cursor_image;
}

void
e_web_view_set_cursor_image (EWebView *web_view,
                             GdkPixbufAnimation *image)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (image != NULL)
		g_object_ref (image);

	if (web_view->priv->cursor_image != NULL)
		g_object_unref (web_view->priv->cursor_image);

	web_view->priv->cursor_image = image;

	g_object_notify (G_OBJECT (web_view), "cursor-image");
}

const gchar *
e_web_view_get_cursor_image_src (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return web_view->priv->cursor_image_src;
}

void
e_web_view_set_cursor_image_src (EWebView *web_view,
                                 const gchar *src_uri)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	g_free (web_view->priv->cursor_image_src);
	web_view->priv->cursor_image_src = g_strdup (src_uri);

	g_object_notify (G_OBJECT (web_view), "cursor-image-src");
}

GtkAction *
e_web_view_get_open_proxy (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->open_proxy;
}

void
e_web_view_set_open_proxy (EWebView *web_view,
                           GtkAction *open_proxy)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (open_proxy != NULL) {
		g_return_if_fail (GTK_IS_ACTION (open_proxy));
		g_object_ref (open_proxy);
	}

	if (web_view->priv->open_proxy != NULL)
		g_object_unref (web_view->priv->open_proxy);

	web_view->priv->open_proxy = open_proxy;

	g_object_notify (G_OBJECT (web_view), "open-proxy");
}

GtkTargetList *
e_web_view_get_paste_target_list (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return webkit_web_view_get_paste_target_list (
		WEBKIT_WEB_VIEW (web_view));
}

GtkAction *
e_web_view_get_print_proxy (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->print_proxy;
}

void
e_web_view_set_print_proxy (EWebView *web_view,
                            GtkAction *print_proxy)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (print_proxy != NULL) {
		g_return_if_fail (GTK_IS_ACTION (print_proxy));
		g_object_ref (print_proxy);
	}

	if (web_view->priv->print_proxy != NULL)
		g_object_unref (web_view->priv->print_proxy);

	web_view->priv->print_proxy = print_proxy;

	g_object_notify (G_OBJECT (web_view), "print-proxy");
}

GtkAction *
e_web_view_get_save_as_proxy (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->save_as_proxy;
}

void
e_web_view_set_save_as_proxy (EWebView *web_view,
                              GtkAction *save_as_proxy)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (save_as_proxy != NULL) {
		g_return_if_fail (GTK_IS_ACTION (save_as_proxy));
		g_object_ref (save_as_proxy);
	}

	if (web_view->priv->save_as_proxy != NULL)
		g_object_unref (web_view->priv->save_as_proxy);

	web_view->priv->save_as_proxy = save_as_proxy;

	g_object_notify (G_OBJECT (web_view), "save-as-proxy");
}

GtkAction *
e_web_view_get_action (EWebView *web_view,
                       const gchar *action_name)
{
	GtkUIManager *ui_manager;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
	g_return_val_if_fail (action_name != NULL, NULL);

	ui_manager = e_web_view_get_ui_manager (web_view);

	return e_lookup_action (ui_manager, action_name);
}

GtkActionGroup *
e_web_view_get_action_group (EWebView *web_view,
                             const gchar *group_name)
{
	GtkUIManager *ui_manager;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
	g_return_val_if_fail (group_name != NULL, NULL);

	ui_manager = e_web_view_get_ui_manager (web_view);

	return e_lookup_action_group (ui_manager, group_name);
}

gchar *
e_web_view_extract_uri (EWebView *web_view,
                        GdkEventButton *event)
{
	EWebViewClass *class;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_val_if_fail (class->extract_uri != NULL, NULL);

	return class->extract_uri (web_view, event);
}

void
e_web_view_copy_clipboard (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_copy_clipboard (WEBKIT_WEB_VIEW (web_view));
}

void
e_web_view_cut_clipboard (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_cut_clipboard (WEBKIT_WEB_VIEW (web_view));
}

gboolean
e_web_view_is_selection_active (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return webkit_web_view_has_selection (WEBKIT_WEB_VIEW (web_view));
}

void
e_web_view_paste_clipboard (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_paste_clipboard (WEBKIT_WEB_VIEW (web_view));
}

gboolean
e_web_view_scroll_forward (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	webkit_web_view_move_cursor (
		WEBKIT_WEB_VIEW (web_view), GTK_MOVEMENT_PAGES, 1);

	return TRUE;  /* XXX This means nothing. */
}

gboolean
e_web_view_scroll_backward (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	webkit_web_view_move_cursor (
		WEBKIT_WEB_VIEW (web_view), GTK_MOVEMENT_PAGES, -1);

	return TRUE;  /* XXX This means nothing. */
}

void
e_web_view_select_all (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_select_all (WEBKIT_WEB_VIEW (web_view));
}

void
e_web_view_unselect_all (EWebView *web_view)
{
#if 0  /* WEBKIT */
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	gtk_html_command (GTK_HTML (web_view), "unselect-all");
#endif
}

void
e_web_view_zoom_100 (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_set_zoom_level (WEBKIT_WEB_VIEW (web_view), 1.0);
}

void
e_web_view_zoom_in (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_zoom_in (WEBKIT_WEB_VIEW (web_view));
}

void
e_web_view_zoom_out (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_zoom_out (WEBKIT_WEB_VIEW (web_view));
}

GtkUIManager *
e_web_view_get_ui_manager (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return web_view->priv->ui_manager;
}

GtkWidget *
e_web_view_get_popup_menu (EWebView *web_view)
{
	GtkUIManager *ui_manager;
	GtkWidget *menu;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	ui_manager = e_web_view_get_ui_manager (web_view);
	menu = gtk_ui_manager_get_widget (ui_manager, "/context");
	g_return_val_if_fail (GTK_IS_MENU (menu), NULL);

	return menu;
}

void
e_web_view_show_popup_menu (EWebView *web_view,
                            GdkEventButton *event,
                            GtkMenuPositionFunc func,
                            gpointer user_data)
{
	GtkWidget *menu;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	e_web_view_update_actions (web_view);

	menu = e_web_view_get_popup_menu (web_view);

	if (event != NULL)
		gtk_menu_popup (
			GTK_MENU (menu), NULL, NULL, func,
			user_data, event->button, event->time);
	else
		gtk_menu_popup (
			GTK_MENU (menu), NULL, NULL, func,
			user_data, 0, gtk_get_current_event_time ());
}

void
e_web_view_status_message (EWebView *web_view,
                           const gchar *status_message)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	g_signal_emit (web_view, signals[STATUS_MESSAGE], 0, status_message);
}

void
e_web_view_stop_loading (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	g_signal_emit (web_view, signals[STOP_LOADING], 0);
}

void
e_web_view_update_actions (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	g_signal_emit (web_view, signals[UPDATE_ACTIONS], 0);
}

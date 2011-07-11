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
 *		Chris Toshok <toshok@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "eab-contact-display.h"

#include "eab-gui-util.h"
#include "e-util/e-util.h"
#include "e-util/e-util-private.h"
#include "e-util/e-html-utils.h"
#include "e-util/e-icon-factory.h"
#include "e-util/e-plugin-ui.h"

#ifdef WITH_CONTACT_MAPS
#include "widgets/misc/e-contact-map.h"
#endif

#include <string.h>
#include <glib/gi18n.h>

#define TEXT_IS_RIGHT_TO_LEFT \
	(gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL)

struct _EABContactDisplayPrivate {
	EContact *contact;
	EABContactDisplayMode mode;
	gboolean show_maps;
};

enum {
	PROP_0,
	PROP_CONTACT,
	PROP_MODE,
	PROP_SHOW_MAPS
};

enum {
	SEND_MESSAGE,
	LAST_SIGNAL
};

static struct {
	const gchar *name;
	const gchar *pretty_name;
}
common_location[] =
{
	{ "WORK",  N_ ("Work")  },
	{ "HOME",  N_ ("Home")  },
	{ "OTHER", N_ ("Other") }
};

#define IMAGE_COL_WIDTH   "20"
#define CONTACT_LIST_ICON "stock_contact-list"
#define AIM_ICON          "im-aim"
#define GROUPWISE_ICON    "im-nov"
#define ICQ_ICON          "im-icq"
#define JABBER_ICON       "im-jabber"
#define MSN_ICON          "im-msn"
#define YAHOO_ICON        "im-yahoo"
#define GADUGADU_ICON	  "im-gadugadu"
#define SKYPE_ICON	  "stock_people"
#define VIDEOCONF_ICON    "stock_video-conferencing"

#define MAX_COMPACT_IMAGE_DIMENSION 48


#define HTML_HEADER "<!doctype html public \"-//W3C//DTD HTML 4.0 TRANSITIONAL//EN\">\n<html>\n"  \
                    "<head>\n<meta name=\"generator\" content=\"Evolution Addressbook Component\">\n" \
                    "<link type=\"text/css\" rel=\"stylesheet\" href=\"file://" EVOLUTION_PRIVDATADIR "/theme/webview.css\">" \
                    "<style type=\"text/css\">\n" \
		    "  div#header { width:100%; clear: both; }\n" \
		    "  div#columns { width: 100%; clear: both; }\n" \
		    "  div#footer { width: 100%; clear: both; }\n" \
		    "  div.column { width: auto; float: left; margin-right: 15px; }\n" \
		    "  img#contact-photo { float: left; }\n" \
		    "  div#contact-name { float: left; margin-left: 20px; }\n" \
		    "</style>\n" \
		    "<script type=\"text/javascript\">\n" \
		    "function collapse_list (obj, listId) {\n" \
		    "	var l = document.getElementById (listId);\n" \
		    "	if (l.style.display == \"none\") {\n" \
		    "		l.style.display = \"block\"; obj.src = obj.src.substr (0, obj.src.lastIndexOf (\"/\")) + \"/minus.png\";\n" \
		    "	} else {\n" \
		    "		l.style.display = \"none\"; obj.src = obj.src.substr (0, obj.src.lastIndexOf (\"/\")) + \"/plus.png\";\n" \
		    "   }\n" \
		    "}\n" \
		    "</script>\n" \
                    "</head>\n"


static const gchar *ui =
"<ui>"
"  <popup name='context'>"
"    <placeholder name='custom-actions-1'>"
"      <menuitem action='contact-send-message'/>"
"    </placeholder>"
"    <placeholder name='custom-actions-2'>"
"      <menuitem action='contact-mailto-copy'/>"
"    </placeholder>"
"  </popup>"
"</ui>";

static gpointer parent_class;
static guint signals[LAST_SIGNAL];

static gchar*
get_icon_uri (const gchar *icon_name)
{
	GtkIconTheme *icon_theme;
	GtkIconInfo *icon_info;
	const gchar *filename;
	gchar *icon_uri;
	GError *error = NULL;

	icon_theme = gtk_icon_theme_get_default ();
	icon_info = gtk_icon_theme_lookup_icon (
		icon_theme, icon_name, GTK_ICON_SIZE_MENU, 0);
	g_return_val_if_fail (icon_info != NULL, NULL);

	filename = gtk_icon_info_get_filename (icon_info);
	icon_uri = g_filename_to_uri (filename, NULL, &error);

	if (error != NULL) {
		g_warning ("%s", error->message);
		g_error_free (error);
	}

	gtk_icon_info_free (icon_info);

	return icon_uri;
}

static void
contact_display_emit_send_message (EABContactDisplay *display,
                                   gint email_num)
{
	EDestination *destination;
	EContact *contact;

	g_return_if_fail (email_num >= 0);

	destination = e_destination_new ();
	contact = eab_contact_display_get_contact (display);
	e_destination_set_contact (destination, contact, email_num);
	g_signal_emit (display, signals[SEND_MESSAGE], 0, destination);
	g_object_unref (destination);
}

static void
action_contact_mailto_copy_cb (GtkAction *action,
                               EABContactDisplay *display)
{
	GtkClipboard *clipboard;
	EWebView *web_view;
	EContact *contact;
	GList *list;
	const gchar *text;
	const gchar *uri;
	gint index;

	web_view = E_WEB_VIEW (display);
	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	index = atoi (uri + strlen ("internal-mailto:"));
	g_return_if_fail (index >= 0);

	contact = eab_contact_display_get_contact (display);
	list = e_contact_get (contact, E_CONTACT_EMAIL);
	text = g_list_nth_data (list, index);

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_text (clipboard, text, -1);
	gtk_clipboard_store (clipboard);

	g_list_foreach (list, (GFunc) g_free, NULL);
	g_list_free (list);
}

static void
action_contact_send_message_cb (GtkAction *action,
                                EABContactDisplay *display)
{
	EWebView *web_view;
	const gchar *uri;
	gint index;

	web_view = E_WEB_VIEW (display);
	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	index = atoi (uri + strlen ("internal-mailto:"));
	contact_display_emit_send_message (display, index);
}

static GtkActionEntry internal_mailto_entries[] = {

	{ "contact-mailto-copy",
	  GTK_STOCK_COPY,
	  N_("Copy _Email Address"),
	  NULL,
	  N_("Copy the email address to the clipboard"),
	  G_CALLBACK (action_contact_mailto_copy_cb) },

	{ "contact-send-message",
	  "mail-message-new",
	  N_("_Send New Message To..."),
	  NULL,
	  N_("Send a mail message to this address"),
	  G_CALLBACK (action_contact_send_message_cb) }
};

static void
render_address_link (GString *buffer,
                     EContact *contact,
                     gint map_type)
{
	EContactAddress *adr;
	GString *link = g_string_new ("");

	adr = e_contact_get (contact, map_type);
	if (adr &&
	    (adr->street || adr->locality || adr->region || adr->country)) {

		if (adr->street && *adr->street) g_string_append_printf (link, "%s, ", adr->street);
		if (adr->locality && *adr->locality) g_string_append_printf (link, "%s, ", adr->locality);
		if (adr->region && *adr->region) g_string_append_printf (link, "%s, ", adr->region);
		if (adr->country && *adr->country) g_string_append_printf (link, "%s", adr->country);

		g_string_assign (link, g_uri_escape_string (link->str, NULL, TRUE));

		g_string_prepend (link, "<a href=\"http://maps.google.com?q=");
		g_string_append_printf (link, "\">%s</a>", _("Open map"));
	}

	if (adr)
		e_contact_address_free (adr);

	g_string_append (buffer, link->str);
	g_string_free (link, TRUE);
}

static void
accum_address (GString *buffer,
               EContact *contact,
               const gchar *html_label,
               EContactField adr_field,
               EContactField label_field)
{
	EContactAddress *adr;
	const gchar *label;
	GString *map_link = g_string_new ("<br>");

	render_address_link (map_link, contact, adr_field);

	label = e_contact_get_const (contact, label_field);
	if (label) {
		gchar *html = e_text_to_html (label, E_TEXT_TO_HTML_CONVERT_NL);

		if (TEXT_IS_RIGHT_TO_LEFT)
			g_string_append_printf (buffer, "<tr><td align=\"right\" valign=\"top\" nowrap>%s</td><th>%s:<br>%s</th><td valign=\"top\" width=\"" IMAGE_COL_WIDTH "\"></td></tr>", html, html_label, map_link->str);
		else
			g_string_append_printf (buffer, "<tr><td width=\"" IMAGE_COL_WIDTH "\"></td><th>%s:<br>%s</th><td valign=\"top\" nowrap>%s</td></tr>", html_label, map_link->str, html);

		g_free (html);
		g_string_free (map_link, TRUE);
		return;
	}

	adr = e_contact_get (contact, adr_field);
	if (adr &&
	    (adr->po || adr->ext || adr->street || adr->locality || adr->region || adr->code || adr->country)) {
		if (TEXT_IS_RIGHT_TO_LEFT)
			g_string_append_printf (buffer, "<tr><td align=\"right\" valign=\"top\" nowrap>");
		else
			g_string_append_printf (buffer, "<tr><td valign=\"top\" width=\"" IMAGE_COL_WIDTH "\"></td><th>%s:<br>%s</th><td valign=\"top\" nowrap>", html_label, map_link->str);

		if (adr->po && *adr->po) g_string_append_printf (buffer, "%s<br>", adr->po);
		if (adr->ext && *adr->ext) g_string_append_printf (buffer, "%s<br>", adr->ext);
		if (adr->street && *adr->street) g_string_append_printf (buffer, "%s<br>", adr->street);
		if (adr->locality && *adr->locality) g_string_append_printf (buffer, "%s<br>", adr->locality);
		if (adr->region && *adr->region) g_string_append_printf (buffer, "%s<br>", adr->region);
		if (adr->code && *adr->code) g_string_append_printf (buffer, "%s<br>", adr->code);
		if (adr->country && *adr->country) g_string_append_printf (buffer, "%s<br>", adr->country);

		if (TEXT_IS_RIGHT_TO_LEFT)
			g_string_append_printf (buffer, "</td><th%s:<br>%s</th><td width=\"" IMAGE_COL_WIDTH "\"></td></tr>", html_label, map_link->str);
		else
			g_string_append_printf (buffer, "</td></tr>");
	}
	if (adr)
		e_contact_address_free (adr);

	g_string_free (map_link, TRUE);
}

static void
render_table_row (GString *buffer,
                  const gchar *label,
                  const gchar *str,
                  const gchar *icon,
                  guint html_flags)
{
	const gchar *icon_html;
	gchar *value;

	if (html_flags)
		value = e_text_to_html (str, html_flags);
	else
		value = (gchar*)str;


	if (icon) {
		gchar *icon_uri = get_icon_uri (icon);
		icon_html = g_strdup_printf ("<img src=\"%s\" width=\"16\" height=\"16\" />", icon_uri);
		g_free (icon_uri);
	} else {
		icon_html = "";
	}

	if (TEXT_IS_RIGHT_TO_LEFT) {
		g_string_append_printf (
			buffer, "<tr>"
			"<td valign=\"top\" align=\"right\">%s</td>"
			"<th align=\"right\" valign=\"top\" width=\"100\" nowrap>:%s</th>"
			"<td valign=\"top\" width=\"" IMAGE_COL_WIDTH "\">%s</td>"
			"</tr>",
			value, label, icon_html);
	} else {
		g_string_append_printf (
			buffer, "<tr>"
			"<td valign=\"top\" width=\"" IMAGE_COL_WIDTH "\">%s</td>"
			"<th valign=\"top\" width=\"100\" nowrap>%s:</th>"
			"<td valign=\"top\">%s</td>"
			"</tr>",
			icon_html, label, value);
	}

	if (html_flags)
		g_free (value);
}

static void
accum_attribute (GString *buffer,
                 EContact *contact,
                 const gchar *html_label,
                 EContactField field,
                 const gchar *icon,
                 guint html_flags)
{
	const gchar *str;

	str = e_contact_get_const (contact, field);

	if (str != NULL && *str != '\0')
		render_table_row (buffer, html_label, str, icon, html_flags);
}

static void
accum_time_attribute (GString *buffer,
                      EContact *contact,
                      const gchar *html_label,
                      EContactField field,
                      const gchar *icon,
                      guint html_flags)
{
	EContactDate *date;
	GDate *gdate = NULL;
	gchar sdate[100];

	date = e_contact_get (contact, field);
	if (date) {
		gdate = g_date_new_dmy ( date->day,
					 date->month,
					 date->year );
		g_date_strftime (sdate, 100, "%x", gdate);
		g_date_free (gdate);
		render_table_row (buffer, html_label, sdate, icon, html_flags);
		e_contact_date_free (date);
	}
}

static void
accum_attribute_multival (GString *buffer,
                          EContact *contact,
                          const gchar *html_label,
                          EContactField field,
                          const gchar *icon,
                          guint html_flags)
{
	GList *val_list, *l;
	GString *val = g_string_new ("");

	val_list = e_contact_get (contact, field);

	for (l = val_list; l; l = l->next) {
		if (l != val_list)
			g_string_append (val, "<br>");

		g_string_append (val, l->data);
	}

	if (val->str && *val->str)
		render_table_row (buffer, html_label, val->str, icon, html_flags);

	g_string_free (val, TRUE);
	g_list_foreach (val_list, (GFunc) g_free, NULL);
	g_list_free (val_list);
}

static const gchar *
get_email_location (EVCardAttribute *attr)
{
	gint i;

	for (i = 0; i < G_N_ELEMENTS (common_location); i++) {
		if (e_vcard_attribute_has_type (attr, common_location[i].name))
			return _(common_location[i].pretty_name);
	}

	return _("Other");
}

static void
render_title_block (GString *buffer,
                    EContact *contact)
{
	const gchar *str;
	gchar *html;
	EContactPhoto *photo;

	g_string_append_printf (
			buffer, "<table border=\"0\"><tr>"
			"<td %s valign=\"middle\">", TEXT_IS_RIGHT_TO_LEFT ?
			"align=\"right\"" : "");
	photo = e_contact_get (contact, E_CONTACT_PHOTO);
	if (!photo)
		photo = e_contact_get (contact, E_CONTACT_LOGO);
	/* Only handle inlined photos for now */
	if (photo && photo->type == E_CONTACT_PHOTO_TYPE_INLINED) {
		gchar *photo_data = g_base64_encode (photo->data.inlined.data, photo->data.inlined.length);
		g_string_append_printf (buffer, "<img border=\"1\" src=\"data:%s;base64,%s\">",
			photo->data.inlined.mime_type,
			photo_data);
		g_free (photo_data);
	}
	if (photo)
		e_contact_photo_free (photo);

	if (e_contact_get (contact, E_CONTACT_IS_LIST)) {
		gchar *icon = get_icon_uri (CONTACT_LIST_ICON);
		g_string_append_printf (buffer, "<img src=\"%s\">", icon);
		g_free (icon);
	}

	g_string_append_printf (
		buffer, "</td><td width=\"20\"></td><td %s valign=\"top\">\n",
		TEXT_IS_RIGHT_TO_LEFT ? "align=\"right\"" : "");

	str = e_contact_get_const (contact, E_CONTACT_FILE_AS);
	if (!str)
		str = e_contact_get_const (contact, E_CONTACT_FULL_NAME);

	if (str) {
		html = e_text_to_html (str, 0);
		if (e_contact_get (contact, E_CONTACT_IS_LIST))
			g_string_append_printf (buffer, "<h2><a href=\"internal-mailto:0\">%s</a></h2>", html);
		else
			g_string_append_printf (buffer, "<h2>%s</h2>", html);
		g_free (html);
	}

	g_string_append (buffer, "</td></tr></table>");

}

static void
render_contact_list_row (GString *buffer,
                         EDestination *destination,
                         EABContactDisplay *display)
{
	gchar *evolution_imagesdir = g_filename_to_uri (EVOLUTION_IMAGESDIR, NULL, NULL);
	gboolean list_collapsed = FALSE;
	const gchar *textrep;
	gchar *name = NULL, *email_addr = NULL;

	textrep = e_destination_get_textrep (destination, TRUE);
	if (!eab_parse_qp_email (textrep, &name, &email_addr))
		email_addr = g_strdup (textrep);

	g_string_append (buffer, "<tr>");
	if (e_destination_is_evolution_list (destination)) {
		g_string_append_printf (buffer,
			"<td width=" IMAGE_COL_WIDTH " valign=\"top\"><img src=\"%s/minus.png\" onClick=\"collapse_list(this, %s);\"></td><td width=\"100%%\">%s",
			evolution_imagesdir,
			e_destination_get_contact_uid (destination),
			name ? name : email_addr);

		if (!list_collapsed) {
			const GList *dest, *dests;
			g_string_append_printf (buffer, "<br><table cellspacing=\"1\" id=\"%s\">",
				e_destination_get_contact_uid (destination));

			dests = e_destination_list_get_root_dests (destination);
			for (dest = dests; dest; dest = dest->next) {
				render_contact_list_row (buffer, dest->data, display);
			}

			g_string_append (buffer, "</table>");
		}

		g_string_append (buffer, "</td>");

	} else {
		if (name && *name) {
			g_string_append_printf (buffer, "<td colspan=\"2\">%s &lt<a href=\"mailto:%s\">%s</a>&gt;</td>", name, email_addr, email_addr);
		} else {
			g_string_append_printf (buffer, "<td colspan=\"2\"><a href=\"mailto:%s\">%s</a></td>", email_addr, email_addr);
		}
	}

	g_string_append (buffer, "</tr>");

	g_free (evolution_imagesdir);
	g_free (name);
	g_free (email_addr);
}

static void
render_contact_list (GString *buffer,
		             EContact *contact,
                     EABContactDisplay *display)
{
	EDestination *destination;
	const GList *dest, *dests;

	destination = e_destination_new ();
	e_destination_set_contact (destination, contact, 0);
	dests = e_destination_list_get_root_dests (destination);

	render_title_block (buffer, contact);

	g_string_append_printf (buffer, "<table border=\"0\"><tr><th colspan=\"2\">%s</th></tr>"
		"<tr><td with=" IMAGE_COL_WIDTH "></td><td>", _("List Members:"));
	g_string_append (buffer, "<table border=\"0\" cellspacing=\"1\">");

	for (dest = dests; dest; dest = dest->next)
		render_contact_list_row (buffer, dest->data, display);

	g_string_append (buffer, "</table>");
	g_string_append (buffer, "</td></tr></table>");

	g_object_unref (destination);
}

static void
render_contact_column (GString *buffer,
                       EContact *contact)
{
	GString *accum, *email;
	GList *email_list, *l, *email_attr_list, *al;
	gint email_num = 0;
	const gchar *nl;

	email = g_string_new ("");
	nl = "";

	email_list = e_contact_get (contact, E_CONTACT_EMAIL);
	email_attr_list = e_contact_get_attributes (contact, E_CONTACT_EMAIL);

	for (l = email_list, al = email_attr_list; l && al; l = l->next, al = al->next) {
		gchar *name = NULL, *mail = NULL;
		gchar *attr_str = (gchar *) get_email_location ((EVCardAttribute *) al->data);

		if (!eab_parse_qp_email (l->data, &name, &mail))
			mail = e_text_to_html (l->data, 0);

		g_string_append_printf (email, "%s%s%s<a href=\"internal-mailto:%d\">%s</a>%s <span class=\"header\">(%s)</span>",
						nl,
						name ? name : "",
						name ? " &lt;" : "",
						email_num,
						mail,
						name ? "&gt;" : "",
						attr_str ? attr_str : "");
		email_num++;
		nl = "<br>";

		g_free (name);
		g_free (mail);
	}
	g_list_foreach (email_list, (GFunc) g_free, NULL);
	g_list_foreach (email_attr_list, (GFunc) e_vcard_attribute_free, NULL);
	g_list_free (email_list);
	g_list_free (email_attr_list);

	accum = g_string_new ("");

	if (email->len)
		render_table_row (accum, _("Email"), email->str, NULL, 0);

	accum_attribute (accum, contact, _("Nickname"), E_CONTACT_NICKNAME, NULL, 0);
	accum_attribute_multival (accum, contact, _("AIM"), E_CONTACT_IM_AIM, AIM_ICON, 0);
	accum_attribute_multival (accum, contact, _("GroupWise"), E_CONTACT_IM_GROUPWISE, GROUPWISE_ICON, 0);
	accum_attribute_multival (accum, contact, _("ICQ"), E_CONTACT_IM_ICQ, ICQ_ICON, 0);
	accum_attribute_multival (accum, contact, _("Jabber"), E_CONTACT_IM_JABBER, JABBER_ICON, 0);
	accum_attribute_multival (accum, contact, _("MSN"), E_CONTACT_IM_MSN, MSN_ICON, 0);
	accum_attribute_multival (accum, contact, _("Yahoo"), E_CONTACT_IM_YAHOO, YAHOO_ICON, 0);
	accum_attribute_multival (accum, contact, _("Gadu-Gadu"), E_CONTACT_IM_GADUGADU, GADUGADU_ICON, 0);
	accum_attribute_multival (accum, contact, _("Skype"), E_CONTACT_IM_SKYPE, SKYPE_ICON, 0);

	if (accum->len)
		g_string_append_printf (buffer,
			"<div class=\"column\" id=\"contact-internet\">"
			"<table border=\"0\" cellspacing=\"5\">%s</table>"
			"</div>", accum->str);

	g_string_free (accum, TRUE);
	g_string_free (email, TRUE);
}

static void
render_work_column (GString *buffer,
                    EContact *contact)
{
	GString *accum = g_string_new ("");

	accum_attribute (accum, contact, _("Company"), E_CONTACT_ORG, NULL, 0);
	accum_attribute (accum, contact, _("Department"), E_CONTACT_ORG_UNIT, NULL, 0);
	accum_attribute (accum, contact, _("Profession"), E_CONTACT_ROLE, NULL, 0);
	accum_attribute (accum, contact, _("Position"), E_CONTACT_TITLE, NULL, 0);
	accum_attribute (accum, contact, _("Manager"), E_CONTACT_MANAGER, NULL, 0);
	accum_attribute (accum, contact, _("Assistant"), E_CONTACT_ASSISTANT, NULL, 0);
	accum_attribute (accum, contact, _("Video Chat"), E_CONTACT_VIDEO_URL, VIDEOCONF_ICON, E_TEXT_TO_HTML_CONVERT_URLS);
	accum_attribute (accum, contact, _("Calendar"), E_CONTACT_CALENDAR_URI, NULL, E_TEXT_TO_HTML_CONVERT_URLS);
	accum_attribute (accum, contact, _("Free/Busy"), E_CONTACT_FREEBUSY_URL, NULL, E_TEXT_TO_HTML_CONVERT_URLS);
	accum_attribute (accum, contact, _("Phone"), E_CONTACT_PHONE_BUSINESS, NULL, 0);
	accum_attribute (accum, contact, _("Fax"), E_CONTACT_PHONE_BUSINESS_FAX, NULL, 0);
	accum_address   (accum, contact, _("Address"), E_CONTACT_ADDRESS_WORK, E_CONTACT_ADDRESS_LABEL_WORK);

	if (accum->len > 0) {
		g_string_append_printf (buffer,
			"<div class=\"column\" id=\"contact-work\">"
			"<h3>%s</h3>"
			"<table border=\"0\" cellspacing=\"5\">%s</table>"
			"</div>", _("Work"), accum->str);
	}

	g_string_free (accum, TRUE);
}

static void
render_personal_column (GString *buffer,
	                    EContact *contact)
{
	GString *accum = g_string_new ("");

	accum_attribute (accum, contact, _("Home Page"), E_CONTACT_HOMEPAGE_URL, NULL, E_TEXT_TO_HTML_CONVERT_URLS);
	accum_attribute (accum, contact, _("Web Log"), E_CONTACT_BLOG_URL, NULL, E_TEXT_TO_HTML_CONVERT_URLS);

	accum_attribute (accum, contact, _("Phone"), E_CONTACT_PHONE_HOME, NULL, 0);
	accum_attribute (accum, contact, _("Mobile Phone"), E_CONTACT_PHONE_MOBILE, NULL, 0);
	accum_address   (accum, contact, _("Address"), E_CONTACT_ADDRESS_HOME, E_CONTACT_ADDRESS_LABEL_HOME);
	accum_time_attribute (accum, contact, _("Birthday"), E_CONTACT_BIRTH_DATE, NULL, 0);
	accum_time_attribute (accum, contact, _("Anniversary"), E_CONTACT_ANNIVERSARY, NULL, 0);
	accum_attribute (accum, contact, _("Spouse"), E_CONTACT_SPOUSE, NULL, 0);

	if (accum->len > 0) {
		g_string_append_printf (buffer,
			"<div class=\"column\" id=\"contact-personal\">"
			"<h3>%s</h3>"
			"<table border=\"0\" cellspacing=\"5\">%s</table>"
			"</div>", _("Personal"), accum->str);
	}

	g_string_free (accum, TRUE);
}

static void
render_footer (GString *buffer,
               EContact *contact)
{
	const gchar *str;

	str = e_contact_get_const (contact, E_CONTACT_NOTE);
	if (!str || !*str)
		return;

	g_string_append (buffer, "<div id=\"footer\">"
		"<table border=\"0\" cellspacing=\"5\">");
	render_table_row (buffer, _("Note"),
		e_contact_get_const (contact, E_CONTACT_NOTE),
		NULL, E_TEXT_TO_HTML_CONVERT_ADDRESSES | E_TEXT_TO_HTML_CONVERT_URLS | E_TEXT_TO_HTML_CONVERT_NL);
	g_string_append (buffer, "</table></div>");
}

static void
render_address_map (GString *buffer,
                    EContact *contact,
                    gint map_type)
{
#ifdef WITH_CONTACT_MAPS
	if (map_type == E_CONTACT_ADDRESS_WORK) {
		g_string_append (buffer, "<object type=\"application/x-map-widget\" data=\"work\"></object>");
	 } else {
 		g_string_append (buffer, "<object type=\"application/x-map-widget\" data=\"home\"></object>");
	 }
#endif
}

static void
render_contact (GString *buffer,
                EContact *contact,
                EABContactDisplay *display)
{
	render_title_block (buffer, contact);

	g_string_append (buffer, "<div id=\"columns\">");
	render_contact_column (buffer, contact);
	render_work_column (buffer, contact);
	render_personal_column (buffer, contact);
	g_string_append (buffer, "</div>");

	render_footer (buffer, contact);
}

static void
eab_contact_display_render_normal (EABContactDisplay *display,
                                   EContact *contact)
{
	GString *buffer;

	/* XXX The initial buffer size is arbitrary.  Tune it. */

	buffer = g_string_sized_new (4096);
	g_string_append (buffer, HTML_HEADER);
	g_string_append (buffer, "<body>");

	if (contact) {
		if (e_contact_get (contact, E_CONTACT_IS_LIST))
			render_contact_list (buffer, contact, display);
		else
			render_contact (buffer, contact, display);
	}

	g_string_append (buffer, "</body></html>\n");

	e_web_view_load_string (E_WEB_VIEW (display), buffer->str);

	g_string_free (buffer, TRUE);
}

static void
eab_contact_display_render_compact (EABContactDisplay *display,
                                    EContact *contact)
{
#warning FIXME: eab_contact_display_render_compact is not WebKit-ready

	GString *buffer;


	/* XXX The initial buffer size is arbitrary.  Tune it. */

	buffer = g_string_sized_new (4096);
	g_string_append (buffer, HTML_HEADER);
	g_string_append (buffer, "<body>\n");

	if (contact) {
		const gchar *str;
		gchar *html;
		EContactPhoto *photo;
		guint bg_frame = 0x000000, bg_body = 0xEEEEEE;
		GtkStyle *style;

		style = gtk_widget_get_style (GTK_WIDGET (display));
		if (style) {
			gushort r, g, b;

			r = style->black.red >> 8;
			g = style->black.green >> 8;
			b = style->black.blue >> 8;
			bg_frame = ((r << 16) | (g << 8) | b) & 0xffffff;

			#define DARKER(a) (((a) >= 0x22) ? ((a) - 0x22) : 0)
			r = DARKER (style->bg[GTK_STATE_NORMAL].red >> 8);
			g = DARKER (style->bg[GTK_STATE_NORMAL].green >> 8);
			b = DARKER (style->bg[GTK_STATE_NORMAL].blue >> 8);
			bg_body = ((r << 16) | (g << 8) | b) & 0xffffff;
			#undef DARKER
		}

		g_string_append_printf (
			buffer,
			"<table width=\"100%%\" cellpadding=1 cellspacing=0 bgcolor=\"#%06X\">"
			"<tr><td valign=\"top\">"
			"<table width=\"100%%\" cellpadding=0 cellspacing=0 bgcolor=\"#%06X\">"
			"<tr><td valign=\"top\">"
			"<table>"
			"<tr><td valign=\"top\">", bg_frame, bg_body);

		photo = e_contact_get (contact, E_CONTACT_PHOTO);
		if (!photo)
			photo = e_contact_get (contact, E_CONTACT_LOGO);
		if (photo) {
			gint calced_width = MAX_COMPACT_IMAGE_DIMENSION, calced_height = MAX_COMPACT_IMAGE_DIMENSION;
			GdkPixbufLoader *loader = gdk_pixbuf_loader_new ();
			GdkPixbuf *pixbuf;

			/* figure out if we need to downscale the
			 * image here.  we don't scale the pixbuf
			 * itself, just insert width/height tags in
			 * the html */
			gdk_pixbuf_loader_write (loader, photo->data.inlined.data, photo->data.inlined.length, NULL);
			gdk_pixbuf_loader_close (loader, NULL);
			pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
			if (pixbuf)
				g_object_ref (pixbuf);
			g_object_unref (loader);
			if (pixbuf) {
				gint max_dimension;

				calced_width = gdk_pixbuf_get_width (pixbuf);
				calced_height = gdk_pixbuf_get_height (pixbuf);

				max_dimension = calced_width;
				if (max_dimension < calced_height)
					max_dimension = calced_height;

				if (max_dimension > MAX_COMPACT_IMAGE_DIMENSION) {
					calced_width *= ((gfloat) MAX_COMPACT_IMAGE_DIMENSION / max_dimension);
					calced_height *= ((gfloat) MAX_COMPACT_IMAGE_DIMENSION / max_dimension);
				}
			}

			g_object_unref (pixbuf);
			g_string_append_printf (
				buffer,
				"<object width=\"%d\" height=\"%d\" "
				"type=\"image/x-contact-photo\"/>",
				calced_width, calced_height);
			e_contact_photo_free (photo);
		}

		g_string_append (buffer, "</td><td valign=\"top\">\n");

		str = e_contact_get_const (contact, E_CONTACT_FILE_AS);
		if (str) {
			html = e_text_to_html (str, 0);
			g_string_append_printf (buffer, "<b>%s</b>", html);
			g_free (html);
		}
		else {
			str = e_contact_get_const (contact, E_CONTACT_FULL_NAME);
			if (str) {
				html = e_text_to_html (str, 0);
				g_string_append_printf (buffer, "<b>%s</b>", html);
				g_free (html);
			}
		}

		g_string_append (buffer, "<hr>");

		if (e_contact_get (contact, E_CONTACT_IS_LIST)) {
			GList *email_list;
			GList *l;

			g_string_append (buffer, "<table border=\"0\" cellspacing=\"0\" cellpadding=\"0\"><tr><td valign=\"top\">");
			g_string_append_printf (buffer, "<b>%s:</b>&nbsp;<td>", _("List Members"));

			email_list = e_contact_get (contact, E_CONTACT_EMAIL);

			for (l = email_list; l; l = l->next) {
				if (l->data) {
					html = e_text_to_html (l->data, 0);
					g_string_append_printf (buffer, "%s, ", html);
					g_free (html);
				}
			}
			g_string_append (buffer, "</td></tr></table>");
		}
		else {
			gboolean comma = FALSE;
			str = e_contact_get_const (contact, E_CONTACT_TITLE);
			if (str) {
				html = e_text_to_html (str, 0);
				g_string_append_printf (buffer, "<b>%s:</b> %s<br>", _("Job Title"), str);
				g_free (html);
			}

			#define print_email() {								\
				html = eab_parse_qp_email_to_html (str);				\
													\
				if (!html)								\
					html = e_text_to_html (str, 0);				\
													\
				g_string_append_printf (buffer, "%s%s", comma ? ", " : "", html);	\
				g_free (html);								\
				comma = TRUE;								\
			}

			g_string_append_printf (buffer, "<b>%s:</b> ", _("Email"));
			str = e_contact_get_const (contact, E_CONTACT_EMAIL_1);
			if (str)
				print_email ();

			str = e_contact_get_const (contact, E_CONTACT_EMAIL_2);
			if (str)
				print_email ();

			str = e_contact_get_const (contact, E_CONTACT_EMAIL_3);
			if (str)
				print_email ();

			g_string_append (buffer, "<br>");

			#undef print_email

			str = e_contact_get_const (contact, E_CONTACT_HOMEPAGE_URL);
			if (str) {
				html = e_text_to_html (str, E_TEXT_TO_HTML_CONVERT_URLS);
				g_string_append_printf (
					buffer, "<b>%s:</b> %s<br>",
					_("Home page"), html);
				g_free (html);
			}

			str = e_contact_get_const (contact, E_CONTACT_BLOG_URL);
			if (str) {
				html = e_text_to_html (str, E_TEXT_TO_HTML_CONVERT_URLS);
				g_string_append_printf (
					buffer, "<b>%s:</b> %s<br>",
					_("Blog"), html);
			}
		}

		g_string_append (buffer, "</td></tr></table></td></tr></table></td></tr></table>\n");
	}

	g_string_append (buffer, "</body></html>\n");

	e_web_view_load_string (E_WEB_VIEW (display), buffer->str);

	g_string_free (buffer, TRUE);
}

static void
contact_display_set_property (GObject *object,
                              guint property_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_CONTACT:
			eab_contact_display_set_contact (
				EAB_CONTACT_DISPLAY (object),
				g_value_get_object (value));
			return;

		case PROP_MODE:
			eab_contact_display_set_mode (
				EAB_CONTACT_DISPLAY (object),
				g_value_get_int (value));
			return;

		case PROP_SHOW_MAPS:
			eab_contact_display_set_show_maps (
				EAB_CONTACT_DISPLAY (object),
				g_value_get_boolean (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
contact_display_get_property (GObject *object,
                              guint property_id,
                              GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_CONTACT:
			g_value_set_object (
				value, eab_contact_display_get_contact (
				EAB_CONTACT_DISPLAY (object)));
			return;

		case PROP_MODE:
			g_value_set_int (
				value, eab_contact_display_get_mode (
				EAB_CONTACT_DISPLAY (object)));
			return;

		case PROP_SHOW_MAPS:
			g_value_set_boolean (
				value, eab_contact_display_get_show_maps (
				EAB_CONTACT_DISPLAY (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
contact_display_dispose (GObject *object)
{
	EABContactDisplayPrivate *priv;

	priv = EAB_CONTACT_DISPLAY (object)->priv;

	if (priv->contact != NULL) {
		g_object_unref (priv->contact);
		priv->contact = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static GtkWidget *
contact_display_create_plugin_widget (EWebView *web_view,
                                      const gchar *mime_type,
                                      const gchar *uri,
                                      GHashTable *param)
{
	EWebViewClass *web_view_class;

#warning FIXME: contact_display_create_plugin_widget is not WebKit-ready


	/* Chain up to parent's create_plugin_widget() method. */
	web_view_class = E_WEB_VIEW_CLASS (parent_class);
	return web_view_class->create_plugin_widget (web_view, mime_type, uri, param);
}

static void
contact_display_hovering_over_link (EWebView *web_view,
                                    const gchar *title,
                                    const gchar *uri)
{
	EWebViewClass *web_view_class;
	EABContactDisplay *display;
	EContact *contact;
	const gchar *name;
	gchar *message;

	if (uri == NULL || *uri == '\0')
		goto chainup;

	if (!g_str_has_prefix (uri, "internal-mailto:"))
		goto chainup;

	display = EAB_CONTACT_DISPLAY (web_view);
	contact = eab_contact_display_get_contact (display);

	name = e_contact_get_const (contact, E_CONTACT_FILE_AS);
	if (name == NULL)
		e_contact_get_const (contact, E_CONTACT_FULL_NAME);
	g_return_if_fail (name != NULL);

	message = g_strdup_printf (_("Click to mail %s"), name);
	e_web_view_status_message (web_view, message);
	g_free (message);

	return;

chainup:
	/* Chain up to parent's hovering_over_link() method. */
	web_view_class = E_WEB_VIEW_CLASS (parent_class);
	web_view_class->hovering_over_link (web_view, title, uri);
}

static void
contact_display_link_clicked (EWebView *web_view,
                              const gchar *uri)
{
	EABContactDisplay *display;
	gsize length;

	display = EAB_CONTACT_DISPLAY (web_view);

	length = strlen ("internal-mailto:");
	if (g_ascii_strncasecmp (uri, "internal-mailto:", length) == 0) {
		gint index;

		index = atoi (uri + length);
		contact_display_emit_send_message (display, index);
		return;
	}

	/* Chain up to parent's link_clicked() method. */
	E_WEB_VIEW_CLASS (parent_class)->link_clicked (web_view, uri);
}

#ifdef WITH_CONTACT_MAPS
/* XXX Clutter event handling workaround. Clutter-gtk propagates events down
 *     to parent widgets.  In this case it leads to GtkHTML scrolling up and
 *     down while user's trying to zoom in the champlain widget. This
 *     workaround stops the propagation from map widget down to GtkHTML. */
static gboolean
handle_map_scroll_event (GtkWidget *widget,
                         GdkEvent *event)
{
	return TRUE;
}

#if 0
static void
contact_display_object_requested (GtkHTML *html,
                                  GtkHTMLEmbedded *eb,
                                  EABContactDisplay *display)
{
	EContact *contact = display->priv->contact;
	const gchar *name = e_contact_get_const (contact, E_CONTACT_FILE_AS);
	const gchar *contact_uid = e_contact_get_const (contact, E_CONTACT_UID);
	gchar *full_name;
	EContactAddress *address;

	if (g_ascii_strcasecmp (eb->classid, "address-map-work") == 0) {
		address = e_contact_get (contact, E_CONTACT_ADDRESS_WORK);
		full_name = g_strconcat (name, " (", _("Work"), ")", NULL);
	} else {
		address = e_contact_get (contact, E_CONTACT_ADDRESS_HOME);
		full_name = g_strconcat (name, " (", _("Home"), ")", NULL);
	}

	if (address) {
		GtkWidget *map = e_contact_map_new ();
		gtk_container_add (GTK_CONTAINER (eb), map);
		gtk_widget_set_size_request (map, 250, 250);
		g_signal_connect (E_CONTACT_MAP (map), "contact-added",
			G_CALLBACK (e_contact_map_zoom_on_marker), NULL);
		g_signal_connect_swapped (E_CONTACT_MAP (map), "contact-added",
			G_CALLBACK (gtk_widget_show_all), map);
		g_signal_connect (GTK_CHAMPLAIN_EMBED (map), "scroll-event",
			G_CALLBACK (handle_map_scroll_event), NULL);

				/* No need to display photo in contact preview ------------------v */
		e_contact_map_add_marker (E_CONTACT_MAP (map), full_name, contact_uid, address, NULL);
	}

	g_free (full_name);
	e_contact_address_free (address);
}
#endif
#endif

static void
contact_display_update_actions (EWebView *web_view)
{
	GtkActionGroup *action_group;
	gboolean scheme_is_internal_mailto;
	gboolean visible;
	const gchar *group_name;
	const gchar *uri;

	/* Chain up to parent's update_actions() method. */
	E_WEB_VIEW_CLASS (parent_class)->update_actions (web_view);

	uri = e_web_view_get_selected_uri (web_view);

	scheme_is_internal_mailto = (uri == NULL) ? FALSE :
		(g_ascii_strncasecmp (uri, "internal-mailto:", 16) == 0);

	/* Override how EWebView treats internal-mailto URIs. */
	group_name = "uri";
	action_group = e_web_view_get_action_group (web_view, group_name);
	visible = gtk_action_group_get_visible (action_group);
	visible &= !scheme_is_internal_mailto;
	gtk_action_group_set_visible (action_group, visible);

	group_name = "internal-mailto";
	visible = scheme_is_internal_mailto;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);
}

static void
eab_contact_display_class_init (EABContactDisplayClass *class)
{
	GObjectClass *object_class;
	EWebViewClass *web_view_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EABContactDisplayPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = contact_display_set_property;
	object_class->get_property = contact_display_get_property;
	object_class->dispose = contact_display_dispose;

	web_view_class = E_WEB_VIEW_CLASS (class);
	web_view_class->create_plugin_widget = contact_display_create_plugin_widget;
	web_view_class->hovering_over_link = contact_display_hovering_over_link;
	web_view_class->link_clicked = contact_display_link_clicked;
	web_view_class->update_actions = contact_display_update_actions;

	g_object_class_install_property (
		object_class,
		PROP_CONTACT,
		g_param_spec_object (
			"contact",
			NULL,
			NULL,
			E_TYPE_CONTACT,
			G_PARAM_READWRITE));

	/* XXX Make this a real enum property. */
	g_object_class_install_property (
		object_class,
		PROP_MODE,
		g_param_spec_int (
			"mode",
			NULL,
			NULL,
			EAB_CONTACT_DISPLAY_RENDER_NORMAL,
			EAB_CONTACT_DISPLAY_RENDER_COMPACT,
			EAB_CONTACT_DISPLAY_RENDER_NORMAL,
			G_PARAM_READWRITE));

	g_object_class_install_property	(
		object_class,
		PROP_SHOW_MAPS,
		g_param_spec_boolean (
			"show-maps",
			NULL,
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	signals[SEND_MESSAGE] = g_signal_new (
		"send-message",
		G_OBJECT_CLASS_TYPE (class),
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET (EABContactDisplayClass, send_message),
		NULL, NULL,
		g_cclosure_marshal_VOID__OBJECT,
		G_TYPE_NONE, 1,
		E_TYPE_DESTINATION);
}

static void
eab_contact_display_init (EABContactDisplay *display)
{
	EWebView *web_view;
	GtkUIManager *ui_manager;
	GtkActionGroup *action_group;
	const gchar *domain = GETTEXT_PACKAGE;
	GError *error = NULL;

	display->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		display, EAB_TYPE_CONTACT_DISPLAY, EABContactDisplayPrivate);
	display->priv->mode = EAB_CONTACT_DISPLAY_RENDER_NORMAL;
	display->priv->show_maps = FALSE;

	web_view = E_WEB_VIEW (display);
	ui_manager = e_web_view_get_ui_manager (web_view);

#ifdef WITH_CONTACT_MAPS
#if 0
	g_signal_connect (web_view, "object-requested",
	G_CALLBACK (contact_display_object_requested), display);
#endif
#endif

	action_group = gtk_action_group_new ("internal-mailto");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, internal_mailto_entries,
		G_N_ELEMENTS (internal_mailto_entries), display);

	/* Because we are loading from a hard-coded string, there is
	 * no chance of I/O errors.  Failure here implies a malformed
	 * UI definition.  Full stop. */
	gtk_ui_manager_add_ui_from_string (ui_manager, ui, -1, &error);
	if (error != NULL)
		g_error ("%s", error->message);
}

GType
eab_contact_display_get_type (void)
{
	static GType type = 0;

	if (G_UNLIKELY (type == 0)) {
		static const GTypeInfo type_info =  {
			sizeof (EABContactDisplayClass),
			(GBaseInitFunc) NULL,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) eab_contact_display_class_init,
			(GClassFinalizeFunc) NULL,
			NULL,  /* class_data */
			sizeof (EABContactDisplay),
			0,     /* n_preallocs */
			(GInstanceInitFunc) eab_contact_display_init,
			NULL   /* value_table */
		};

		type = g_type_register_static (
			E_TYPE_WEB_VIEW, "EABContactDisplay", &type_info, 0);
	}

	return type;
}

GtkWidget *
eab_contact_display_new (void)
{
	return g_object_new (EAB_TYPE_CONTACT_DISPLAY, NULL);
}

EContact *
eab_contact_display_get_contact (EABContactDisplay *display)
{
	g_return_val_if_fail (EAB_IS_CONTACT_DISPLAY (display), NULL);

	return display->priv->contact;
}

void
eab_contact_display_set_contact (EABContactDisplay *display,
                                 EContact *contact)
{
	EABContactDisplayMode mode;

	g_return_if_fail (EAB_IS_CONTACT_DISPLAY (display));

	mode = eab_contact_display_get_mode (display);

	if (contact != NULL)
		g_object_ref (contact);
	if (display->priv->contact != NULL)
		g_object_unref (display->priv->contact);
	display->priv->contact = contact;

	switch (mode) {
		case EAB_CONTACT_DISPLAY_RENDER_NORMAL:
			eab_contact_display_render_normal (display, contact);
			break;

		case EAB_CONTACT_DISPLAY_RENDER_COMPACT:
			eab_contact_display_render_compact (display, contact);
			break;
	}

	g_object_notify (G_OBJECT (display), "contact");
}

EABContactDisplayMode
eab_contact_display_get_mode (EABContactDisplay *display)
{
	g_return_val_if_fail (EAB_IS_CONTACT_DISPLAY (display), 0);

	return display->priv->mode;
}

void
eab_contact_display_set_mode (EABContactDisplay *display,
                              EABContactDisplayMode mode)
{
	EContact *contact;

	g_return_if_fail (EAB_IS_CONTACT_DISPLAY (display));

	display->priv->mode = mode;
	contact = eab_contact_display_get_contact (display);

	switch (mode) {
		case EAB_CONTACT_DISPLAY_RENDER_NORMAL:
			eab_contact_display_render_normal (display, contact);
			break;

		case EAB_CONTACT_DISPLAY_RENDER_COMPACT:
			eab_contact_display_render_compact (display, contact);
			break;
	}

	g_object_notify (G_OBJECT (display), "mode");
}

gboolean
eab_contact_display_get_show_maps (EABContactDisplay *display)
{
	g_return_val_if_fail (EAB_IS_CONTACT_DISPLAY (display), FALSE);

	return display->priv->show_maps;
}

void
eab_contact_display_set_show_maps (EABContactDisplay *display,
                                   gboolean show_maps)
{
	EABContactDisplayMode mode;
	EContact *contact;

	g_return_if_fail (EAB_IS_CONTACT_DISPLAY (display));

	display->priv->show_maps = show_maps;
	contact = eab_contact_display_get_contact (display);
	mode = eab_contact_display_get_mode (display);

	switch (mode) {
		case EAB_CONTACT_DISPLAY_RENDER_NORMAL:
			eab_contact_display_render_normal (display, contact);
			break;

		case EAB_CONTACT_DISPLAY_RENDER_COMPACT:
			eab_contact_display_render_compact (display, contact);
			break;
	}

	g_object_notify (G_OBJECT (display), "show-maps");
}

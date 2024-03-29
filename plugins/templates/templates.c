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
 *		Diego Escalante Urrelo <diegoe@gnome.org>
 *		Bharath Acharya <abharath@novell.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 * Copyright (C) 2008 - Diego Escalante Urrelo
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <string.h>

#include <gconf/gconf-client.h>

#include <e-util/e-config.h>

#include <mail/e-mail-folder-utils.h>
#include <mail/e-mail-local.h>
#include <mail/e-mail-reader.h>
#include <mail/e-mail-session.h>
#include <mail/em-composer-utils.h>
#include <mail/em-utils.h>
#include <mail/mail-ops.h>
#include <mail/message-list.h>
#include <e-util/e-alert-dialog.h>
#include <e-util/e-plugin.h>
#include <e-util/e-util.h>
#include <shell/e-shell-view.h>

#include <composer/e-msg-composer.h>

#define GCONF_KEY_TEMPLATE_PLACEHOLDERS "/apps/evolution/mail/template_placeholders"

typedef struct _AsyncContext AsyncContext;

struct _AsyncContext {
	EActivity *activity;
	EMailReader *reader;
	CamelMimeMessage *message;
	CamelFolder *template_folder;
	gchar *message_uid;
	gchar *template_message_uid;
};

typedef struct {
	GConfClient *gconf;
	GtkWidget   *treeview;
	GtkWidget   *clue_add;
	GtkWidget   *clue_edit;
	GtkWidget   *clue_remove;
	GtkListStore *store;
} UIData;

enum {
	CLUE_KEYWORD_COLUMN,
	CLUE_VALUE_COLUMN,
	CLUE_N_COLUMNS
};

GtkWidget *	e_plugin_lib_get_configure_widget
						(EPlugin *plugin);
gboolean	init_composer_actions		(GtkUIManager *ui_manager,
						 EMsgComposer *composer);
gboolean	init_shell_actions		(GtkUIManager *ui_manager,
						 EShellWindow *shell_window);
gint		e_plugin_lib_enable		(EPlugin *plugin,
						 gboolean enabled);

/* Thanks to attachment reminder plugin for this*/
static void commit_changes (UIData *ui);

static void  key_cell_edited_callback (GtkCellRendererText *cell, gchar *path_string,
				   gchar *new_text,UIData *ui);

static void  value_cell_edited_callback (GtkCellRendererText *cell, gchar *path_string,
				   gchar *new_text,UIData *ui);

static gboolean clue_foreach_check_isempty (GtkTreeModel *model, GtkTreePath
					*path, GtkTreeIter *iter, UIData *ui);

static void templates_folder_msg_changed_cb (CamelFolder *folder,
			      CamelFolderChangeInfo *change_info,
			      EShellWindow *shell_window);

static gboolean plugin_enabled;

static void
async_context_free (AsyncContext *context)
{
	if (context->activity != NULL)
		g_object_unref (context->activity);

	if (context->reader != NULL)
		g_object_unref (context->reader);

	if (context->message != NULL)
		g_object_unref (context->message);

	if (context->template_folder != NULL)
		g_object_unref (context->template_folder);

	g_free (context->message_uid);
	g_free (context->template_message_uid);

	g_slice_free (AsyncContext, context);
}

static void
selection_changed (GtkTreeSelection *selection,
                   UIData *ui)
{
	GtkTreeModel *model;
	GtkTreeIter iter;

	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
		gtk_widget_set_sensitive (ui->clue_edit, TRUE);
		gtk_widget_set_sensitive (ui->clue_remove, TRUE);
	} else {
		gtk_widget_set_sensitive (ui->clue_edit, FALSE);
		gtk_widget_set_sensitive (ui->clue_remove, FALSE);
	}
}

static void
destroy_ui_data (gpointer data)
{
	UIData *ui = (UIData *) data;

	if (!ui)
		return;

	g_object_unref (ui->gconf);
	g_free (ui);
}

static void
commit_changes (UIData *ui)
{
	GtkTreeModel *model = NULL;
	GSList *clue_list = NULL;
	GtkTreeIter iter;
	gboolean valid;

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (ui->treeview));
	valid = gtk_tree_model_get_iter_first (model, &iter);

	while (valid) {
		gchar *keyword, *value;
		gchar *key;

		gtk_tree_model_get (model, &iter, CLUE_KEYWORD_COLUMN, &keyword, -1);
		gtk_tree_model_get (model, &iter, CLUE_VALUE_COLUMN, &value, -1);

		/* Check if the keyword and value are not empty */
		if ((keyword) && (value) && (g_utf8_strlen (g_strstrip (keyword), -1) > 0)
			&& (g_utf8_strlen (g_strstrip (value), -1) > 0)) {
			key = g_strdup_printf("%s=%s", keyword, value);
			clue_list = g_slist_append (clue_list, key);
		}
		valid = gtk_tree_model_iter_next (model, &iter);
	}

	gconf_client_set_list (ui->gconf, GCONF_KEY_TEMPLATE_PLACEHOLDERS, GCONF_VALUE_STRING, clue_list, NULL);

	g_slist_foreach (clue_list, (GFunc) g_free, NULL);
	g_slist_free (clue_list);
}

static void
clue_check_isempty (GtkTreeModel *model,
                    GtkTreePath *path,
                    GtkTreeIter *iter,
                    UIData *ui)
{
	GtkTreeSelection *selection;
	gchar *keyword = NULL;
	gboolean valid;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (ui->treeview));
	/* move to the previous node */
	valid = gtk_tree_path_prev (path);

	gtk_tree_model_get (model, iter, CLUE_KEYWORD_COLUMN, &keyword, -1);
	if ((keyword) && !(g_utf8_strlen (g_strstrip (keyword), -1) > 0))
		gtk_list_store_remove (ui->store, iter);

	/* Check if we have a valid row to select. If not, then select
	 * the previous row */
	if (gtk_list_store_iter_is_valid (GTK_LIST_STORE (model), iter)) {
		gtk_tree_selection_select_iter (selection, iter);
	} else {
		if (path && valid) {
			gtk_tree_model_get_iter (model, iter, path);
			gtk_tree_selection_select_iter (selection, iter);
		}
	}

	gtk_widget_grab_focus (ui->treeview);
	g_free (keyword);
}

static gboolean
clue_foreach_check_isempty (GtkTreeModel *model,
                            GtkTreePath *path,
                            GtkTreeIter *iter,
                            UIData *ui)
{
	gboolean valid;

	valid = gtk_tree_model_get_iter_first (model, iter);
	while (valid && gtk_list_store_iter_is_valid (ui->store, iter)) {
		gchar *keyword = NULL;
		gtk_tree_model_get (model, iter, CLUE_KEYWORD_COLUMN, &keyword, -1);
		/* Check if the keyword is not empty and then emit the row-changed
		signal (if we delete the row, then the iter gets corrupted) */
		if ((keyword) && !(g_utf8_strlen (g_strstrip (keyword), -1) > 0))
			gtk_tree_model_row_changed (model, path, iter);

		g_free (keyword);
		valid = gtk_tree_model_iter_next (model, iter);
	}

	return FALSE;
}

static void
key_cell_edited_callback (GtkCellRendererText *cell,
                          gchar *path_string,
                          gchar *new_text,
                          UIData *ui)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gchar *value;

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (ui->treeview));

	gtk_tree_model_get_iter_from_string (model, &iter, path_string);

	gtk_tree_model_get (model, &iter, CLUE_VALUE_COLUMN, &value, -1);
	gtk_list_store_set (GTK_LIST_STORE (model), &iter,
				    CLUE_KEYWORD_COLUMN, new_text, CLUE_VALUE_COLUMN, value, -1);

	commit_changes (ui);
}

static void
value_cell_edited_callback (GtkCellRendererText *cell,
                            gchar *path_string,
                            gchar *new_text,
                            UIData *ui)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gchar *keyword;

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (ui->treeview));

	gtk_tree_model_get_iter_from_string (model, &iter, path_string);

	gtk_tree_model_get (model, &iter, CLUE_KEYWORD_COLUMN, &keyword, -1);

	gtk_list_store_set (GTK_LIST_STORE (model), &iter,
				    CLUE_KEYWORD_COLUMN, keyword, CLUE_VALUE_COLUMN, new_text, -1);

	commit_changes (ui);
}

static void
clue_add_clicked (GtkButton *button,
                  UIData *ui)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gchar *new_clue = NULL;
	GtkTreeViewColumn *focus_col;
	GtkTreePath *path;

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (ui->treeview));
	gtk_tree_model_foreach (model, (GtkTreeModelForeachFunc) clue_foreach_check_isempty, ui);

	/* Disconnect from signal so that we can create an empty row */
	g_signal_handlers_disconnect_matched (G_OBJECT (model), G_SIGNAL_MATCH_FUNC, 0, 0, NULL, clue_check_isempty, ui);

	/* TODO : Trim and check for blank strings */
	new_clue = g_strdup ("");
	gtk_list_store_append (GTK_LIST_STORE (model), &iter);
	gtk_list_store_set (GTK_LIST_STORE (model), &iter,
				    CLUE_KEYWORD_COLUMN, new_clue, CLUE_VALUE_COLUMN, new_clue, -1);

	focus_col = gtk_tree_view_get_column (GTK_TREE_VIEW (ui->treeview), CLUE_KEYWORD_COLUMN);
	path = gtk_tree_model_get_path (model, &iter);

	if (path) {
		gtk_tree_view_set_cursor (GTK_TREE_VIEW (ui->treeview), path, focus_col, TRUE);
		gtk_tree_view_row_activated (GTK_TREE_VIEW (ui->treeview), path, focus_col);
		gtk_tree_path_free (path);
	}

	/* We have done our job, connect back to the signal */
	g_signal_connect (
		model, "row-changed",
		G_CALLBACK (clue_check_isempty), ui);
}

static void
clue_remove_clicked (GtkButton *button,
                     UIData *ui)
{
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GtkTreePath *path;
	gboolean valid;
	gint len;

	valid = FALSE;
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (ui->treeview));
	if (!gtk_tree_selection_get_selected (selection, &model, &iter))
		return;

	/* Get the path and move to the previous node :) */
	path = gtk_tree_model_get_path (model, &iter);
	if (path)
		valid = gtk_tree_path_prev (path);

	gtk_list_store_remove (GTK_LIST_STORE (model), &iter);

	len = gtk_tree_model_iter_n_children (model, NULL);
	if (len > 0) {
		if (gtk_list_store_iter_is_valid (GTK_LIST_STORE (model), &iter)) {
			gtk_tree_selection_select_iter (selection, &iter);
		} else {
			if (path && valid) {
				gtk_tree_model_get_iter (model, &iter, path);
				gtk_tree_selection_select_iter (selection, &iter);
			}
		}
	} else {
		gtk_widget_set_sensitive (ui->clue_edit, FALSE);
		gtk_widget_set_sensitive (ui->clue_remove, FALSE);
	}

	gtk_widget_grab_focus (ui->treeview);
	gtk_tree_path_free (path);

	commit_changes (ui);
}

static void
clue_edit_clicked (GtkButton *button,
                   UIData *ui)
{
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkTreeIter iter;
	GtkTreeViewColumn *focus_col;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (ui->treeview));
	if (!gtk_tree_selection_get_selected (selection, &model, &iter))
		return;

	focus_col = gtk_tree_view_get_column (GTK_TREE_VIEW (ui->treeview), CLUE_KEYWORD_COLUMN);
	path = gtk_tree_model_get_path (model, &iter);

	if (path) {
		gtk_tree_view_set_cursor (GTK_TREE_VIEW (ui->treeview), path, focus_col, TRUE);
		gtk_tree_path_free (path);
	}
}

GtkWidget *
e_plugin_lib_get_configure_widget (EPlugin *epl)
{
	GtkCellRenderer *renderer_key, *renderer_value;
	GtkTreeSelection *selection;
	GtkTreeIter iter;
	GConfClient *gconf = gconf_client_get_default ();
	GtkWidget *hbox;
	GSList *clue_list = NULL, *list;
	GtkTreeModel *model;
	GtkWidget *templates_configuration_box;
	GtkWidget *clue_container;
	GtkWidget *scrolledwindow1;
	GtkWidget *clue_treeview;
	GtkWidget *vbuttonbox2;
	GtkWidget *clue_add;
	GtkWidget *clue_edit;
	GtkWidget *clue_remove;

	UIData *ui = g_new0 (UIData, 1);

	templates_configuration_box = gtk_vbox_new (FALSE, 5);
	gtk_widget_show (templates_configuration_box);
	gtk_widget_set_size_request (templates_configuration_box, 385, 189);

	clue_container = gtk_hbox_new (FALSE, 6);
	gtk_widget_show (clue_container);
	gtk_box_pack_start (GTK_BOX (templates_configuration_box), clue_container, TRUE, TRUE, 0);

	scrolledwindow1 = gtk_scrolled_window_new (NULL, NULL);
	gtk_widget_show (scrolledwindow1);
	gtk_box_pack_start (GTK_BOX (clue_container), scrolledwindow1, TRUE, TRUE, 0);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow1), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	clue_treeview = gtk_tree_view_new ();
	gtk_widget_show (clue_treeview);
	gtk_container_add (GTK_CONTAINER (scrolledwindow1), clue_treeview);
	gtk_container_set_border_width (GTK_CONTAINER (clue_treeview), 1);

	vbuttonbox2 = gtk_vbutton_box_new ();
	gtk_widget_show (vbuttonbox2);
	gtk_box_pack_start (GTK_BOX (clue_container), vbuttonbox2, FALSE, TRUE, 0);
	gtk_button_box_set_layout (GTK_BUTTON_BOX (vbuttonbox2), GTK_BUTTONBOX_START);
	gtk_box_set_spacing (GTK_BOX (vbuttonbox2), 6);

	clue_add = gtk_button_new_from_stock ("gtk-add");
	gtk_widget_show (clue_add);
	gtk_container_add (GTK_CONTAINER (vbuttonbox2), clue_add);
	gtk_widget_set_can_default (clue_add, TRUE);

	clue_edit = gtk_button_new_from_stock ("gtk-edit");
	gtk_widget_show (clue_edit);
	gtk_container_add (GTK_CONTAINER (vbuttonbox2), clue_edit);
	gtk_widget_set_can_default (clue_edit, TRUE);

	clue_remove = gtk_button_new_from_stock ("gtk-remove");
	gtk_widget_show (clue_remove);
	gtk_container_add (GTK_CONTAINER (vbuttonbox2), clue_remove);
	gtk_widget_set_can_default (clue_remove, TRUE);

	ui->gconf = gconf_client_get_default ();

	ui->treeview = clue_treeview;

	ui->store = gtk_list_store_new (CLUE_N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING);

	gtk_tree_view_set_model (GTK_TREE_VIEW (ui->treeview), GTK_TREE_MODEL (ui->store));

	renderer_key = gtk_cell_renderer_text_new ();
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (ui->treeview), -1, _("Keywords"),
			renderer_key, "text", CLUE_KEYWORD_COLUMN, NULL);
	g_object_set (renderer_key, "editable", TRUE, NULL);
	g_signal_connect (renderer_key, "edited", (GCallback) key_cell_edited_callback, ui);

	renderer_value = gtk_cell_renderer_text_new ();
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (ui->treeview), -1, _("Values"),
			renderer_value, "text", CLUE_VALUE_COLUMN, NULL);
	g_object_set (renderer_value, "editable", TRUE, NULL);
	g_signal_connect (renderer_value, "edited", (GCallback) value_cell_edited_callback, ui);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (ui->treeview));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);
	g_signal_connect (
		selection, "changed",
		G_CALLBACK (selection_changed), ui);
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (ui->treeview), TRUE);

	ui->clue_add = clue_add;
	g_signal_connect (
		ui->clue_add, "clicked",
		G_CALLBACK (clue_add_clicked), ui);

	ui->clue_remove = clue_remove;
	g_signal_connect (
		ui->clue_remove, "clicked",
		G_CALLBACK (clue_remove_clicked), ui);
	gtk_widget_set_sensitive (ui->clue_remove, FALSE);

	ui->clue_edit = clue_edit;
	g_signal_connect (
		ui->clue_edit, "clicked",
		G_CALLBACK (clue_edit_clicked), ui);
	gtk_widget_set_sensitive (ui->clue_edit, FALSE);

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (ui->treeview));
	g_signal_connect (
		model, "row-changed",
		G_CALLBACK (clue_check_isempty), ui);

	/* Populate tree view with values from gconf */
	clue_list = gconf_client_get_list ( gconf, GCONF_KEY_TEMPLATE_PLACEHOLDERS, GCONF_VALUE_STRING, NULL );

	for (list = clue_list; list; list = g_slist_next (list)) {
		gchar **temp = g_strsplit (list->data, "=", 2);
		gtk_list_store_append (ui->store, &iter);
		gtk_list_store_set (ui->store, &iter, CLUE_KEYWORD_COLUMN, temp[0], CLUE_VALUE_COLUMN, temp[1], -1);
		g_strfreev (temp);
	}

	if (clue_list) {
		g_slist_foreach (clue_list, (GFunc) g_free, NULL);
		g_slist_free (clue_list);
	}

	/* Add the list here */

	hbox = gtk_vbox_new (FALSE, 0);

	gtk_box_pack_start (GTK_BOX (hbox), templates_configuration_box, TRUE, TRUE, 0);

	/* to let free data properly on destroy of configuration widget */
	g_object_set_data_full (G_OBJECT (hbox), "myui-data", ui, destroy_ui_data);

	return hbox;
}

/* Case insensitive version of strstr */
static gchar *
strstr_nocase (const gchar *haystack,
               const gchar *needle)
{
/* When _GNU_SOURCE is available, use the nonstandard extension of libc */
#ifdef _GNU_SOURCE
	g_return_val_if_fail (haystack, NULL);
	g_return_Val_if_fail (needle, NULL);

	return strcasestr (haystack, needle)
#else
/* Otherwise convert both, haystack and needle to lowercase and use good old strstr */
	gchar *l_haystack;
	gchar *l_needle;
	gchar *pos;

	g_return_val_if_fail (haystack, NULL);
	g_return_val_if_fail (needle, NULL);

	l_haystack = g_ascii_strdown (haystack, -1);
	l_needle = g_ascii_strdown (needle, -1);
	pos = strstr (l_haystack, l_needle);

	/* Get actual position of the needle in the haystack instead of l_haystack or
	 * leave it NULL */
	if (pos)
		pos = (gchar *)(haystack + (pos - l_haystack));

	g_free (l_haystack);
	g_free (l_needle);

	return pos;
#endif
}

/* Replaces $ORIG[variable] in given template by given replacement from the original message */
static void
replace_template_variable (GString *text,
                           const gchar *variable,
                           const gchar *replacement)
{
	const gchar *p, *next;
	GString *str;
	gint find_len;
	gchar *find;

	g_return_if_fail (text != NULL);
	g_return_if_fail (variable != NULL);
	g_return_if_fail (*variable);

	find = g_strconcat ("$ORIG[", variable, "]", NULL);

	find_len = strlen (find);
	str = g_string_new ("");
	p = text->str;
	while (next = strstr_nocase (p, find), next) {
		if (p < next)
			g_string_append_len (str, p, next - p);
		if (replacement && *replacement)
			g_string_append (str, replacement);
		p = next + find_len;
	}
	g_string_append (str, p);

	g_string_assign (text, str->str);

	g_string_free (str, TRUE);
	g_free (find);
}

static void
replace_email_addresses (GString *template,
                         CamelInternetAddress *internet_address,
                         const gchar *variable)
{
	gint address_index = 0;
	GString *emails = g_string_new ("");
	const gchar *address_name, *address_email;

	g_return_if_fail (template);
	g_return_if_fail (internet_address);
	g_return_if_fail (variable);

	while (camel_internet_address_get (internet_address, address_index, &address_name, &address_email)) {
		gchar *address = camel_internet_address_format_address (address_name, address_email);

		if (address_index > 0)
			g_string_append_printf (emails, ", %s", address);
		else
			g_string_append_printf (emails, "%s", address);

		address_index++;
		g_free (address);
	}
	replace_template_variable (template, variable, emails->str);
	g_string_free (emails, TRUE);
}

static CamelMimePart *
fill_template (CamelMimeMessage *message,
               CamelMimePart *template)
{
	struct _camel_header_raw *header;
	CamelContentType *ct;
	CamelStream *stream;
	CamelMimePart *return_part;
	CamelMimePart *message_part = NULL;
	CamelDataWrapper *dw;

	CamelInternetAddress *internet_address;

	GString *template_body;
	GByteArray *byte_array;

	gint i;
	gboolean message_html, template_html;

	ct = camel_mime_part_get_content_type (template);
	template_html = ct && camel_content_type_is (ct, "text", "html");

	message_html = FALSE;
	/* When template is html, then prefer HTML part of the original message. Otherwise go for plaintext */
	dw = camel_medium_get_content (CAMEL_MEDIUM (message));
	if (CAMEL_IS_MULTIPART (dw)) {
		CamelMultipart *multipart = CAMEL_MULTIPART (dw);

		for (i = 0; i < camel_multipart_get_number (multipart); i++) {
			CamelMimePart *part = camel_multipart_get_part (multipart, i);
			CamelContentType *ct = camel_mime_part_get_content_type (part);

			if (!ct)
				continue;

			if (camel_content_type_is (ct, "text", "html") && template_html) {
				message_part = camel_multipart_get_part (multipart, i);
				message_html = TRUE;
				break;
			} else if (camel_content_type_is (ct, "text", "plain") && message_html == FALSE) {
				message_part = camel_multipart_get_part (multipart, i);
			}
		}
	 } else
		message_part  = CAMEL_MIME_PART (message);

	/* Get content of the template */
	stream = camel_stream_mem_new ();
	camel_data_wrapper_decode_to_stream_sync (camel_medium_get_content (CAMEL_MEDIUM (template)), stream, NULL, NULL);
	camel_stream_flush (stream, NULL, NULL);
	byte_array = camel_stream_mem_get_byte_array (CAMEL_STREAM_MEM (stream));
	template_body = g_string_new_len ((gchar *) byte_array->data, byte_array->len);
	g_object_unref (stream);

	/* Replace all $ORIG[header_name] by respective values */
	header = CAMEL_MIME_PART (message)->headers;
	while (header) {
		if (g_ascii_strncasecmp (header->name, "content-", 8) != 0 &&
		    g_ascii_strncasecmp (header->name, "to", 2) != 0 &&
		    g_ascii_strncasecmp (header->name, "cc", 2) != 0 &&
		    g_ascii_strncasecmp (header->name, "bcc", 3) != 0 &&
		    g_ascii_strncasecmp (header->name, "from", 4) != 0 &&
		    g_ascii_strncasecmp (header->name, "subject", 7) != 0)
			replace_template_variable (template_body, header->name, header->value);

		header = header->next;
	}

	/* Now manually replace the *subject* header. The header->value for subject header could be
	 * base64 encoded, so let camel_mime_message to decode it for us if needed */
	replace_template_variable (template_body, "subject", camel_mime_message_get_subject (message));

	/* Replace TO and FROM modifiers. */
	internet_address = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO);
	replace_email_addresses (template_body, internet_address, "to");

	internet_address = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC);
	replace_email_addresses (template_body, internet_address, "cc");

	internet_address = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_BCC);
	replace_email_addresses (template_body, internet_address, "bcc");

	internet_address = camel_mime_message_get_from (message);
	replace_email_addresses (template_body, internet_address, "from");

	/* Now extract body of the original message and replace the $ORIG[body] modifier in template */
	if (message_part && strstr_nocase (template_body->str, "$ORIG[body]")) {
		GString *message_body;

		stream = camel_stream_mem_new ();
		camel_data_wrapper_decode_to_stream_sync (camel_medium_get_content (CAMEL_MEDIUM (message_part)), stream, NULL, NULL);
		camel_stream_flush (stream, NULL, NULL);
		byte_array = camel_stream_mem_get_byte_array (CAMEL_STREAM_MEM (stream));
		message_body = g_string_new_len ((gchar *) byte_array->data, byte_array->len);
		g_object_unref (stream);

		if (template_html && !message_html) {
			gchar *html = camel_text_to_html (message_body->str, CAMEL_MIME_FILTER_TOHTML_CONVERT_NL |
									     CAMEL_MIME_FILTER_TOHTML_CONVERT_SPACES |
									     CAMEL_MIME_FILTER_TOHTML_CONVERT_URLS |
									     CAMEL_MIME_FILTER_TOHTML_MARK_CITATION |
									     CAMEL_MIME_FILTER_TOHTML_CONVERT_ADDRESSES, 0);
			g_string_assign (message_body, html);
			g_free (html);
		} else if (!template_html && message_html) {
			g_string_prepend (message_body, "<pre>");
			g_string_append (message_body, "</pre>");
		} /* Other cases should not occur. And even if they happen to do, there's nothing we can really do about it */

		replace_template_variable (template_body, "body", message_body->str);
		g_string_free (message_body, TRUE);
	} else {
		replace_template_variable (template_body, "body", "");
	}

	return_part = camel_mime_part_new ();

	if (template_html)
		camel_mime_part_set_content (return_part, template_body->str, template_body->len, "text/html");
	else
		camel_mime_part_set_content (return_part, template_body->str, template_body->len, "text/plain");

	g_string_free (template_body, TRUE);

	return return_part;
}

static void
create_new_message (CamelFolder *folder,
                    GAsyncResult *result,
                    AsyncContext *context)
{
	EAlertSink *alert_sink;
	CamelMimeMessage *new;
	CamelMimeMessage *message;
	CamelMimeMessage *template;
	CamelMultipart *new_multipart;
	CamelContentType *new_content_type = NULL;
	CamelDataWrapper *dw;
	struct _camel_header_raw *header;
	EMailBackend *backend;
	EShell *shell;
	const gchar *message_uid;
	gint i;
	GError *error = NULL;

	CamelMimePart *template_part = NULL;
	CamelMimePart *out_part = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);

	template = camel_folder_get_message_finish (folder, result, &error);

	if (e_activity_handle_cancellation (context->activity, error)) {
		g_warn_if_fail (template == NULL);
		async_context_free (context);
		g_error_free (error);
		return;

	} else if (error != NULL) {
		g_warn_if_fail (template == NULL);
		e_alert_submit (
			alert_sink, "mail:no-retrieve-message",
			error->message, NULL);
		async_context_free (context);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (template));

	message = context->message;
	message_uid = context->message_uid;

	backend = e_mail_reader_get_backend (context->reader);
	shell = e_shell_backend_get_shell (E_SHELL_BACKEND (backend));

	folder = e_mail_local_get_folder (E_MAIL_LOCAL_FOLDER_TEMPLATES);

	new = camel_mime_message_new ();
	new_multipart = camel_multipart_new ();
	camel_data_wrapper_set_mime_type (CAMEL_DATA_WRAPPER (new_multipart), "multipart/alternative");
	camel_multipart_set_boundary (new_multipart, NULL);

	dw = camel_medium_get_content (CAMEL_MEDIUM (template));
	/* If template is a multipart, then try to use HTML. When no HTML part is available, use plaintext. Every other
	 * add as an attachment */
	if (CAMEL_IS_MULTIPART (dw)) {
		for (i = 0; i < camel_multipart_get_number (CAMEL_MULTIPART (dw)); i++) {
			CamelMimePart *part = camel_multipart_get_part (CAMEL_MULTIPART (dw), i);
			CamelContentType *ct = camel_mime_part_get_content_type (part);

			if (ct && camel_content_type_is (ct, "text", "html")) {
				new_content_type = ct;
				template_part = camel_multipart_get_part (CAMEL_MULTIPART (dw), i);
			} else if (ct && camel_content_type_is (ct, "text", "plain") && new_content_type == NULL) {
				new_content_type = ct;
				template_part = camel_multipart_get_part (CAMEL_MULTIPART (dw), i);
			} else {
				/* Copy any other parts (attachments...) to the output message */
				camel_mime_part_set_disposition (part, "attachment");
				camel_multipart_add_part (new_multipart, part);
			}
		}
	} else {
		CamelContentType *ct = camel_mime_part_get_content_type (CAMEL_MIME_PART (template));

		if (ct && (camel_content_type_is (ct, "text", "html") ||
		    camel_content_type_is (ct, "text", "plain"))) {
			template_part = CAMEL_MIME_PART (template);
			new_content_type = ct;
		}
	}

	/* Here replace all the modifiers in template body by values from message and return the newly created part */
	out_part = fill_template (message, template_part);

	/* Assigning part directly to mime_message causes problem with "Content-type" header displaying
	 * in the HTML message (camel parsing bug?) */
	camel_multipart_add_part (new_multipart, out_part);
	g_object_unref (out_part);
	camel_medium_set_content (CAMEL_MEDIUM (new), CAMEL_DATA_WRAPPER (new_multipart));

	/* Add the headers from the message we are replying to, so CC and that
	 * stuff is preserved. Also replace any $ORIG[header-name] modifiers ignoring
	 * 'content-*' headers */
	header = CAMEL_MIME_PART (message)->headers;
	while (header) {
		if (g_ascii_strncasecmp (header->name, "content-", 8) != 0) {

			/* Some special handling of the 'subject' header */
			if (g_ascii_strncasecmp (header->name, "subject", 7) == 0) {
				GString *subject = g_string_new (camel_mime_message_get_subject (template));

				/* Now replace all possible $ORIG[]s in the subject line by values from original message */
				struct _camel_header_raw *m_header = CAMEL_MIME_PART (message)->headers;
				while (m_header) {
					if (g_ascii_strncasecmp (m_header->name, "content-", 8) != 0 &&
					    g_ascii_strncasecmp (m_header->name, "subject", 7) !=0)
						replace_template_variable (subject, m_header->name, m_header->value);

					m_header = m_header->next;
				}
				/* Now replace $ORIG[subject] variable, handling possible base64 encryption */
				replace_template_variable (subject, "subject",
					camel_mime_message_get_subject (message));
				header->value = g_strdup (subject->str);
				g_string_free (subject, TRUE);
			}

			camel_medium_add_header (CAMEL_MEDIUM (new),
						 header->name,
						 header->value);
		}

		header = header->next;
	}

	/* Set the To: field to the same To: field of the message we are replying to. */
	camel_mime_message_set_recipients (new, CAMEL_RECIPIENT_TYPE_TO,
			camel_mime_message_get_from (message));

	/* Copy the CC and BCC from the template.*/
	camel_mime_message_set_recipients (new, CAMEL_RECIPIENT_TYPE_CC,
			camel_mime_message_get_recipients (template, CAMEL_RECIPIENT_TYPE_CC));

	camel_mime_message_set_recipients (new, CAMEL_RECIPIENT_TYPE_BCC,
			camel_mime_message_get_recipients (template, CAMEL_RECIPIENT_TYPE_BCC));

	/* Create the composer */
	em_utils_edit_message (shell, folder, new, message_uid);

	g_object_unref (template);
	g_object_unref (new_multipart);
	g_object_unref (new);

	async_context_free (context);
}

static void
template_got_source_message (CamelFolder *folder,
                             GAsyncResult *result,
                             AsyncContext *context)
{
	EAlertSink *alert_sink;
	GCancellable *cancellable;
	CamelMimeMessage *message;
	GError *error = NULL;

	alert_sink = e_activity_get_alert_sink (context->activity);
	cancellable = e_activity_get_cancellable (context->activity);

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

	context->message = message;

	/* Now fetch the template message. */

	camel_folder_get_message (
		context->template_folder,
		context->template_message_uid,
		G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) create_new_message,
		context);
}

static void
action_reply_with_template_cb (GtkAction *action,
                               EShellView *shell_view)
{
	EActivity *activity;
	AsyncContext *context;
	GCancellable *cancellable;
	CamelFolder *folder, *template_folder;
	EShellContent *shell_content;
	EMailReader *reader;
	GPtrArray *uids;
	const gchar *message_uid;
	const gchar *template_message_uid;

	shell_content = e_shell_view_get_shell_content (shell_view);
	reader = E_MAIL_READER (shell_content);

	folder = e_mail_reader_get_folder (reader);
	g_return_if_fail (CAMEL_IS_FOLDER (folder));

	uids = e_mail_reader_get_selected_uids (reader);
	g_return_if_fail (uids != NULL && uids->len == 1);
	message_uid = g_ptr_array_index (uids, 0);

	template_folder = g_object_get_data (
		G_OBJECT (action), "template-folder");
	template_message_uid = g_object_get_data (
		G_OBJECT (action), "template-uid");

	activity = e_mail_reader_new_activity (reader);
	cancellable = e_activity_get_cancellable (activity);

	context = g_slice_new0 (AsyncContext);
	context->activity = activity;
	context->reader = g_object_ref (reader);
	context->template_folder = g_object_ref (template_folder);
	context->message_uid = g_strdup (message_uid);
	context->template_message_uid = g_strdup (template_message_uid);

	camel_folder_get_message (
		folder, message_uid, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		template_got_source_message, context);

	em_utils_uids_free (uids);
}

static void
build_template_menus_recurse (GtkUIManager *ui_manager,
                              GtkActionGroup *action_group,
                              const gchar *menu_path,
                              guint *action_count,
                              guint merge_id,
                              CamelFolderInfo *folder_info,
                              EShellView *shell_view)
{
	CamelStore *store;
	EShellWindow *shell_window = e_shell_view_get_shell_window (shell_view);

	store = e_mail_local_get_store ();

	while (folder_info != NULL) {
		CamelFolder *folder;
		GPtrArray *uids;
		GtkAction *action;
		const gchar *action_label;
		const gchar *display_name;
		gchar *action_name;
		gchar *path;
		guint ii;

		display_name = folder_info->display_name;
		/* FIXME Not passing a GCancellable or GError here. */
		folder = camel_store_get_folder_sync (
			store, folder_info->full_name, 0, NULL, NULL);

		action_name = g_strdup_printf (
			"templates-menu-%d", *action_count);
		*action_count = *action_count + 1;

		/* To avoid having a Templates dir, we ignore the top level */
		if (g_str_has_suffix (display_name, "Templates"))
			action_label = _("Templates");
		else
			action_label = display_name;

		action = gtk_action_new (
			action_name, action_label, NULL, NULL);

		gtk_action_group_add_action (action_group, action);

		gtk_ui_manager_add_ui (
			ui_manager, merge_id, menu_path, action_name,
			action_name, GTK_UI_MANAGER_MENU, FALSE);

		/* Disconnect previous connection to avoid possible multiple calls because
		 * folder is a persistent structure */
		g_signal_handlers_disconnect_by_func (folder, G_CALLBACK (templates_folder_msg_changed_cb), shell_window);
		g_signal_connect (folder, "changed",
			G_CALLBACK (templates_folder_msg_changed_cb), shell_window);

		path = g_strdup_printf ("%s/%s", menu_path, action_name);

		g_object_unref (action);
		g_free (action_name);

		/* Add submenus, if any. */
		if (folder_info->child != NULL)
			build_template_menus_recurse (
				ui_manager, action_group,
				path, action_count, merge_id,
				folder_info->child, shell_view);

		if (!folder) {
			g_free (path);
			folder_info = folder_info->next;
			continue;
		}

		/* Get the UIDs for this folder and add them to the menu. */
		uids = camel_folder_get_uids (folder);
		for (ii = 0; uids && ii < uids->len; ii++) {
			CamelMimeMessage *template;
			const gchar *uid = uids->pdata[ii];
			guint32 flags;

			/* If the UIDs is marked for deletion, skip it. */
			flags = camel_folder_get_message_flags (folder, uid);
			if (flags & CAMEL_MESSAGE_DELETED)
				continue;

			/* FIXME Not passing a GCancellable or GError here. */
			template = camel_folder_get_message_sync (
				folder, uid, NULL, NULL);

			/* FIXME Do something more intelligent with errors. */
			if (template == NULL)
				continue;

			action_label =
				camel_mime_message_get_subject (template);
			if (action_label == NULL || *action_label == '\0')
				action_label = _("No Title");

			action_name = g_strdup_printf (
				"templates-item-%d", *action_count);
			*action_count = *action_count + 1;

			action = gtk_action_new (
				action_name, action_label, NULL, NULL);

			g_object_set_data(G_OBJECT (action), "template-uid", (gpointer) uid);

			g_object_set_data(G_OBJECT (action), "template-folder", folder);

			g_signal_connect (
				action, "activate",
				G_CALLBACK (action_reply_with_template_cb),
				shell_view);

			gtk_action_group_add_action (action_group, action);

			gtk_ui_manager_add_ui (
				ui_manager, merge_id, path, action_name,
				action_name, GTK_UI_MANAGER_MENUITEM, FALSE);

			g_object_unref (action);
			g_free (action_name);
			g_object_unref (template);
		}

		camel_folder_free_uids (folder, uids);
		g_object_unref (folder);
		g_free (path);

		folder_info = folder_info->next;
	}
}

static void
got_message_draft_cb (EMsgComposer *composer,
                      GAsyncResult *result)
{
	CamelMimeMessage *message;
	CamelMessageInfo *info;
	CamelFolder *folder;
	GError *error = NULL;

	message = e_msg_composer_get_message_draft_finish (
		composer, result, &error);

	/* Ignore cancellations. */
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_warn_if_fail (message == NULL);
		g_error_free (error);
		return;
	}

	if (error != NULL) {
		g_warn_if_fail (message == NULL);
		e_alert_run_dialog_for_args (
			GTK_WINDOW (composer),
			"mail-composer:no-build-message",
			error->message, NULL);
		g_error_free (error);
		return;
	}

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	/* Get the templates folder and all UIDs of the messages there. */
	folder = e_mail_local_get_folder (E_MAIL_LOCAL_FOLDER_TEMPLATES);

	info = camel_message_info_new (NULL);

	/* The last argument is a bit mask which tells the function
	 * which flags to modify.  In this case, ~0 means all flags.
	 * So it clears all the flags and then sets SEEN and DRAFT. */
	camel_message_info_set_flags (
		info, CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_DRAFT, ~0);

	/* FIXME No async callback, so... hope for the best? */
	e_mail_folder_append_message (
		folder, message, info, G_PRIORITY_DEFAULT,
		NULL, (GAsyncReadyCallback) NULL, NULL);

	g_object_unref (message);
}

static void
action_template_cb (GtkAction *action,
                    EMsgComposer *composer)
{
	/* XXX Pass a GCancellable */
	e_msg_composer_get_message_draft (
		composer, G_PRIORITY_DEFAULT, NULL,
		(GAsyncReadyCallback) got_message_draft_cb, NULL);
}

static GtkActionEntry composer_entries[] = {

	{ "template",
	  GTK_STOCK_SAVE,
	  N_("Save as _Template"),
	  "<Shift><Control>t",
	  N_("Save as Template"),
	  G_CALLBACK (action_template_cb) }
};

static void
build_menu (EShellWindow *shell_window,
            GtkActionGroup *action_group)
{
	EShellView *shell_view;
	CamelFolder *folder;
	CamelStore *store;
	CamelFolderInfo *folder_info;
	GtkUIManager *ui_manager;
	guint merge_id;
	guint action_count = 0;
	const gchar *full_name;

	ui_manager = e_shell_window_get_ui_manager (shell_window);
	shell_view = e_shell_window_get_shell_view (shell_window, "mail");

	merge_id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (action_group), "merge-id"));

	/* Now recursively build template submenus in the pop-up menu. */
	store = e_mail_local_get_store ();
	folder = e_mail_local_get_folder (
		E_MAIL_LOCAL_FOLDER_TEMPLATES);
	full_name = camel_folder_get_full_name (folder);

	/* FIXME Not passing a GCancellable or GError here. */
	folder_info = camel_store_get_folder_info_sync (
		store, full_name,
		CAMEL_STORE_FOLDER_INFO_RECURSIVE |
		CAMEL_STORE_FOLDER_INFO_FAST, NULL, NULL);

	build_template_menus_recurse (
		ui_manager, action_group,
		"/mail-message-popup/mail-message-templates",
		&action_count, merge_id, folder_info,
		shell_view);

	camel_store_free_folder_info (store, folder_info);
}

static void
update_actions_cb (EShellView *shell_view,
                   GtkActionGroup *action_group)
{
	GList *list;
	gint length;

	if (!plugin_enabled)
		return;

	list = gtk_action_group_list_actions (action_group);
	length = g_list_length (list);

	if (!length) {
		EShellWindow *shell_window  = e_shell_view_get_shell_window (shell_view);
		build_menu (shell_window, action_group);
	}

	gtk_action_group_set_sensitive (action_group, TRUE);
	gtk_action_group_set_visible (action_group, TRUE);

	g_list_free (list);
}

gboolean
init_composer_actions (GtkUIManager *ui_manager,
                       EMsgComposer *composer)
{
	GtkhtmlEditor *editor;

	editor = GTKHTML_EDITOR (composer);

	/* Add actions to the "composer" action group. */
	gtk_action_group_add_actions (
		gtkhtml_editor_get_action_group (editor, "composer"),
		composer_entries, G_N_ELEMENTS (composer_entries), composer);

	return TRUE;
}

static void
rebuild_template_menu (EShellWindow *shell_window)
{
	GtkUIManager *ui_manager;
	GtkActionGroup *action_group;
	guint merge_id;

	ui_manager = e_shell_window_get_ui_manager (shell_window);

	action_group = e_lookup_action_group (ui_manager, "templates");
	merge_id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (action_group), "merge-id"));

	gtk_ui_manager_remove_ui (ui_manager, merge_id);
	e_action_group_remove_all_actions (action_group);
	gtk_ui_manager_ensure_update (ui_manager);

	build_menu (shell_window, action_group);
}

static void
templates_folder_msg_changed_cb (CamelFolder *folder,
                                 CamelFolderChangeInfo *change_info,
                                 EShellWindow *shell_window)
{
	rebuild_template_menu (shell_window);
}

static void
templates_folder_changed_cb (CamelStore *store,
                             CamelFolderInfo *folder_info,
                             EShellWindow *shell_window)
{
	if (folder_info->full_name && strstr (folder_info->full_name, _("Templates")) != NULL)
		rebuild_template_menu (shell_window);
}

static void
templates_folder_renamed_cb (CamelStore *store,
                             const gchar *old_name,
                             CamelFolderInfo *folder_info,
                             EShellWindow *shell_window)
{
	if (folder_info->full_name && strstr (folder_info->full_name, _("Templates")) != NULL)
		rebuild_template_menu (shell_window);
}

static void
mail_shell_view_created_cb (EShellWindow *shell_window,
                            EShellView *shell_view)
{
	GtkUIManager *ui_manager;
	GtkActionGroup *action_group;
	CamelFolder *folder;
	CamelStore *store;
	guint merge_id;

	ui_manager = e_shell_window_get_ui_manager (shell_window);
	e_shell_window_add_action_group (shell_window, "templates");
	action_group = e_lookup_action_group (ui_manager, "templates");
	merge_id = gtk_ui_manager_new_merge_id (ui_manager);
	g_object_set_data (G_OBJECT (action_group), "merge-id",
			   GUINT_TO_POINTER (merge_id));

	folder = e_mail_local_get_folder (E_MAIL_LOCAL_FOLDER_TEMPLATES);
	store = e_mail_local_get_store ();

	g_signal_connect (folder, "changed",
		G_CALLBACK (templates_folder_msg_changed_cb), shell_window);
	g_signal_connect (store, "folder-created",
		G_CALLBACK (templates_folder_changed_cb), shell_window);
	g_signal_connect (store, "folder-deleted",
		G_CALLBACK (templates_folder_changed_cb), shell_window);
	g_signal_connect (store, "folder-renamed",
		G_CALLBACK (templates_folder_renamed_cb), shell_window);

	g_signal_connect (shell_view, "update-actions",
		G_CALLBACK (update_actions_cb), action_group);
}

gboolean
init_shell_actions (GtkUIManager *ui_manager,
                    EShellWindow *shell_window)
{
	EShellView *shell_view;

	/* Be careful not to instantiate the mail view ourselves. */
	shell_view = e_shell_window_peek_shell_view (shell_window, "mail");
	if (shell_view != NULL)
		mail_shell_view_created_cb (shell_window, shell_view);
	else
		g_signal_connect (
			shell_window, "shell-view-created::mail",
			G_CALLBACK (mail_shell_view_created_cb), NULL);

	return TRUE;
}

gint
e_plugin_lib_enable (EPlugin *plugin,
                     gboolean enabled)
{
	plugin_enabled = enabled;

	return 0;
}

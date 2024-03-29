/*
 * Evolution calendar - Copy source dialog
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
 *		Rodrigo Moya <rodrigo@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib/gi18n.h>
#include <libedataserverui/e-client-utils.h>

#include "copy-source-dialog.h"
#include "select-source-dialog.h"

typedef struct {
	GtkWindow *parent;
	ESource *orig_source;
	EClientSourceType obj_type;
	ESource *selected_source;
	ECalClient *source_client, *dest_client;
} CopySourceDialogData;

static void
show_error (CopySourceDialogData *csdd,
            const gchar *msg,
            const GError *error)
{
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new (
		csdd->parent, 0, GTK_MESSAGE_ERROR,
		GTK_BUTTONS_CLOSE, error ? "%s\n%s" : "%s", msg, error ? error->message : "");
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
}

struct ForeachTzidData
{
	ECalClient *source_client;
	ECalClient *dest_client;
};

static void
add_timezone_to_cal_cb (icalparameter *param,
                        gpointer data)
{
	struct ForeachTzidData *ftd = data;
	icaltimezone *tz = NULL;
	const gchar *tzid;

	g_return_if_fail (ftd != NULL);
	g_return_if_fail (ftd->source_client != NULL);
	g_return_if_fail (ftd->dest_client != NULL);

	tzid = icalparameter_get_tzid (param);
	if (!tzid || !*tzid)
		return;

	if (e_cal_client_get_timezone_sync (ftd->source_client, tzid, &tz, NULL, NULL) && tz)
		e_cal_client_add_timezone_sync (ftd->dest_client, tz, NULL, NULL);
}

static void
free_copy_data (CopySourceDialogData *csdd)
{
	if (!csdd)
		return;

	if (csdd->orig_source)
		g_object_unref (csdd->orig_source);
	if (csdd->selected_source)
		g_object_unref (csdd->selected_source);
	if (csdd->source_client)
		g_object_unref (csdd->source_client);
	if (csdd->dest_client)
		g_object_unref (csdd->dest_client);
	g_free (csdd);
}

static void
dest_source_opened_cb (GObject *source_object,
                       GAsyncResult *result,
                       gpointer user_data)
{
	ESource *source = E_SOURCE (source_object);
	CopySourceDialogData *csdd = user_data;
	EClient *client = NULL;
	GError *error = NULL;

	e_client_utils_open_new_finish (source, result, &client, &error);

	if (error != NULL) {
		g_warn_if_fail (client == NULL);
		show_error (csdd, _("Could not open destination"), error);
		g_error_free (error);
		free_copy_data (csdd);
		return;
	}

	g_return_if_fail (E_IS_CLIENT (client));

	csdd->dest_client = E_CAL_CLIENT (client);

	e_client_utils_open_new (csdd->selected_source, csdd->obj_type, FALSE, NULL,
		e_client_utils_authenticate_handler, csdd->parent,
		dest_source_opened_cb, csdd);

	/* check if the destination is read only */
	if (e_client_is_readonly (E_CLIENT (csdd->dest_client))) {
		show_error (csdd, _("Destination is read only"), NULL);
	} else {
		GSList *obj_list = NULL;
		if (e_cal_client_get_object_list_sync (csdd->source_client, "#t", &obj_list, NULL, NULL)) {
			GSList *l;
			icalcomponent *icalcomp;
			struct ForeachTzidData ftd;

			ftd.source_client = csdd->source_client;
			ftd.dest_client = csdd->dest_client;

			for (l = obj_list; l != NULL; l = l->next) {
				/* FIXME: process recurrences */
				/* FIXME: process errors */
				if (e_cal_client_get_object_sync (csdd->dest_client, icalcomponent_get_uid (l->data), NULL,
						      &icalcomp, NULL, NULL)) {
					e_cal_client_modify_object_sync (csdd->dest_client, l->data, CALOBJ_MOD_ALL, NULL, NULL);
					icalcomponent_free (icalcomp);
				} else {
					gchar *uid = NULL;
					GError *error = NULL;

					icalcomp = l->data;

					/* Add timezone information from source
					 * ECal to the destination ECal. */
					icalcomponent_foreach_tzid (
						icalcomp,
						add_timezone_to_cal_cb, &ftd);

					if (e_cal_client_create_object_sync (csdd->dest_client, icalcomp, &uid, NULL, &error)) {
						g_free (uid);
					} else {
						if (error) {
							show_error (csdd, _("Cannot create object"), error);
							g_error_free (error);
						}
						break;
					}
				}
			}

			e_cal_client_free_icalcomp_slist (obj_list);
		}
	}

	free_copy_data (csdd);
}

static void
orig_source_opened_cb (GObject *source_object,
                       GAsyncResult *result,
                       gpointer user_data)
{
	ESource *source = E_SOURCE (source_object);
	CopySourceDialogData *csdd = user_data;
	EClient *client = NULL;
	GError *error = NULL;

	e_client_utils_open_new_finish (source, result, &client, &error);

	if (error != NULL) {
		g_warn_if_fail (client == NULL);
		show_error (csdd, _("Could not open source"), error);
		g_error_free (error);
		free_copy_data (csdd);
		return;
	}

	g_return_if_fail (E_IS_CLIENT (client));

	csdd->source_client = E_CAL_CLIENT (client);

	e_client_utils_open_new (
		csdd->selected_source, csdd->obj_type, FALSE, NULL,
		e_client_utils_authenticate_handler, csdd->parent,
		dest_source_opened_cb, csdd);
}

static void
copy_source (const CopySourceDialogData *const_csdd)
{
	CopySourceDialogData *csdd;

	if (!const_csdd->selected_source)
		return;

	g_return_if_fail (const_csdd->obj_type != E_CLIENT_SOURCE_TYPE_LAST);

	csdd = g_new0 (CopySourceDialogData, 1);
	csdd->parent = const_csdd->parent;
	csdd->orig_source = g_object_ref (const_csdd->orig_source);
	csdd->obj_type = const_csdd->obj_type;
	csdd->selected_source = g_object_ref (const_csdd->selected_source);

	e_client_utils_open_new (csdd->orig_source, csdd->obj_type, FALSE, NULL,
		e_client_utils_authenticate_handler, csdd->parent,
		orig_source_opened_cb, csdd);
}

/**
 * copy_source_dialog
 *
 * Implements the Copy command for sources, allowing the user to select a target
 * source to copy to.
 */
void
copy_source_dialog (GtkWindow *parent,
                    ESource *source,
                    ECalClientSourceType obj_type)
{
	CopySourceDialogData csdd;

	g_return_if_fail (E_IS_SOURCE (source));
	g_return_if_fail (obj_type == E_CAL_CLIENT_SOURCE_TYPE_EVENTS ||
			  obj_type == E_CAL_CLIENT_SOURCE_TYPE_TASKS ||
			  obj_type == E_CAL_CLIENT_SOURCE_TYPE_MEMOS);

	csdd.parent = parent;
	csdd.orig_source = source;
	csdd.selected_source = NULL;
	csdd.obj_type = obj_type == E_CAL_CLIENT_SOURCE_TYPE_EVENTS ? E_CLIENT_SOURCE_TYPE_EVENTS :
			obj_type == E_CAL_CLIENT_SOURCE_TYPE_TASKS ? E_CLIENT_SOURCE_TYPE_TASKS :
			obj_type == E_CAL_CLIENT_SOURCE_TYPE_MEMOS ? E_CLIENT_SOURCE_TYPE_MEMOS :
			E_CLIENT_SOURCE_TYPE_LAST;

	csdd.selected_source = select_source_dialog (parent, obj_type, source);
	if (csdd.selected_source) {
		copy_source (&csdd);

		/* free memory */
		g_object_unref (csdd.selected_source);
	}
}

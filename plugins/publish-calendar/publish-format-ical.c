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
 *		David Trowbridge <trowbrds@cs.colorado.edu>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <glib/gi18n.h>
#include <gconf/gconf-client.h>
#include <libedataserver/e-source.h>
#include <libedataserver/e-source-list.h>
#include <libedataserverui/e-client-utils.h>
#include <libecal/e-cal-client.h>
#include <libecal/e-cal-util.h>
#include "publish-format-ical.h"

typedef struct {
	GHashTable *zones;
	ECalClient *client;
} CompTzData;

 static void
insert_tz_comps (icalparameter *param,
                 gpointer cb_data)
{
	const gchar *tzid;
	CompTzData *tdata = cb_data;
	icaltimezone *zone = NULL;
	icalcomponent *tzcomp;
	GError *error = NULL;

	tzid = icalparameter_get_tzid (param);

	if (g_hash_table_lookup (tdata->zones, tzid))
		return;

	if (!e_cal_client_get_timezone_sync (tdata->client, tzid, &zone, NULL, &error)) {
		g_warning ("Could not get the timezone information for %s :  %s \n", tzid, error->message);
		g_error_free (error);
		return;
	}

	tzcomp = icalcomponent_new_clone (icaltimezone_get_component (zone));
	g_hash_table_insert (tdata->zones, (gpointer) tzid, (gpointer) tzcomp);
}

static void
append_tz_to_comp (gpointer key,
                   gpointer value,
                   icalcomponent *toplevel)
{
	icalcomponent_add_component (toplevel, (icalcomponent *) value);
}

static gboolean
write_calendar (const gchar *uid,
                ESourceList *source_list,
                GOutputStream *stream,
                GError **error)
{
	ESource *source;
	ECalClient *client = NULL;
	GSList *objects;
	icalcomponent *top_level;
	gboolean res = FALSE;

	source = e_source_list_peek_source_by_uid (source_list, uid);
	if (source)
		client = e_cal_client_new (source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, error);
	if (!client) {
		if (error && !error)
			*error = g_error_new (E_CAL_CLIENT_ERROR, E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR, _("Could not publish calendar: Calendar backend no longer exists"));
		return FALSE;
	}

	g_signal_connect (client, "authenticate", G_CALLBACK (e_client_utils_authenticate_handler), NULL);

	if (!e_client_open_sync (E_CLIENT (client), TRUE, NULL, error)) {
		g_object_unref (client);
		return FALSE;
	}

	top_level = e_cal_util_new_top_level ();

	if (e_cal_client_get_object_list_sync (client, "#t", &objects, NULL, error)) {
		GSList *iter;
		gchar *ical_string;
		CompTzData tdata;

		tdata.zones = g_hash_table_new (g_str_hash, g_str_equal);
		tdata.client = client;

		for (iter = objects; iter; iter = iter->next) {
			icalcomponent *icalcomp = objects->data;
			icalcomponent_foreach_tzid (icalcomp, insert_tz_comps, &tdata);
			icalcomponent_add_component (top_level, icalcomp);
		}

		g_hash_table_foreach (tdata.zones, (GHFunc) append_tz_to_comp, top_level);

		g_hash_table_destroy (tdata.zones);
		tdata.zones = NULL;

		ical_string = icalcomponent_as_ical_string_r (top_level);
		res = g_output_stream_write_all (stream, ical_string, strlen (ical_string), NULL, NULL, error);
		g_free (ical_string);
		e_cal_client_free_icalcomp_slist (objects);
	}

	g_object_unref (client);
	icalcomponent_free (top_level);

	return res;
}

void
publish_calendar_as_ical (GOutputStream *stream,
                          EPublishUri *uri,
                          GError **error)
{
	GSList *l;
	ESourceList *source_list;

	/* events */
	if (!e_cal_client_get_sources (&source_list, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, error))
		return;

	l = uri->events;
	while (l) {
		gchar *uid = l->data;
		if (!write_calendar (uid, source_list, stream, error))
			break;
		l = g_slist_next (l);
	}

	g_object_unref (source_list);
}

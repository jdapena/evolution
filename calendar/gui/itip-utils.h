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
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef ITIP_UTILS_H
#define ITIP_UTILS_H

#include <libical/ical.h>
#include <string.h>
#include <libecal/e-cal-client.h>
#include <libecal/e-cal-component.h>
#include <libedataserver/e-account-list.h>

G_BEGIN_DECLS

typedef enum {
	E_CAL_COMPONENT_METHOD_PUBLISH,
	E_CAL_COMPONENT_METHOD_REQUEST,
	E_CAL_COMPONENT_METHOD_REPLY,
	E_CAL_COMPONENT_METHOD_ADD,
	E_CAL_COMPONENT_METHOD_CANCEL,
	E_CAL_COMPONENT_METHOD_REFRESH,
	E_CAL_COMPONENT_METHOD_COUNTER,
	E_CAL_COMPONENT_METHOD_DECLINECOUNTER
} ECalComponentItipMethod;

struct CalMimeAttach {
	gchar *filename;
	gchar *content_type;
	gchar *content_id;
	gchar *description;
	gchar *encoded_data;
	gboolean disposition;
	guint length;
};

gchar **	itip_get_user_identities	(void);
gchar *		itip_get_fallback_identity	(void);
gboolean	itip_address_is_user		(const gchar *address);
gboolean	itip_organizer_is_user		(ECalComponent *comp,
						 ECalClient *cal_client);
gboolean	itip_organizer_is_user_ex	(ECalComponent *comp,
						 ECalClient *cal_client,
						 gboolean skip_cap_test);
gboolean	itip_sentby_is_user		(ECalComponent *comp,
						 ECalClient *cal_client);
const gchar *	itip_strip_mailto		(const gchar *address);
gchar *		itip_get_comp_attendee		(ECalComponent *comp,
						 ECalClient *cal_client);
gboolean	itip_send_comp			(ECalComponentItipMethod method,
						 ECalComponent *comp,
						 ECalClient *cal_client,
						 icalcomponent *zones,
						 GSList *attachments_list,
						 GSList *users,
						 gboolean strip_alarms,
						 gboolean only_new_attendees);
gboolean	itip_publish_comp		(ECalClient *cal_client,
						 gchar *uri,
						 gchar *username,
						 gchar *password,
						 ECalComponent **pub_comp);
gboolean	itip_publish_begin		(ECalComponent *pub_comp,
						 ECalClient *cal_client,
						 gboolean cloned,
						 ECalComponent **clone);
gboolean	reply_to_calendar_comp		(ECalComponentItipMethod method,
						 ECalComponent *send_comp,
						 ECalClient *cal_client,
						 gboolean reply_all,
						 icalcomponent *zones,
						 GSList *attachments_list);
gboolean	is_icalcomp_valid		(icalcomponent *icalcomp);

G_END_DECLS

#endif /* ITIP_UTILS_H */

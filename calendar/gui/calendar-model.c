/* Evolution calendar - Data model for ETable
 *
 * Copyright (C) 2000 Helix Code, Inc.
 *
 * Authors: Federico Mena-Quintero <federico@helixcode.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#include <config.h>
#include <gtk/gtksignal.h>
#include "calendar-model.h"



/* Private part of the ECalendarModel structure */
typedef struct {
	/* Calendar client we are using */
	CalClient *client;

	/* Types of objects we are dealing with */
	CalObjType type;

	/* Array of pointers to calendar objects */
	GArray *objects;

	/* UID -> array index hash */
	GHashTable *uid_index_hash;
} CalendarModelPrivate;



static void calendar_model_class_init (CalendarModelClass *class);
static void calendar_model_init (CalendarModel *model);
static void calendar_model_destroy (GtkObject *object);

static int calendar_model_column_count (ETableModel *etm);
static int calendar_model_row_count (ETableModel *etm);
static void *calendar_model_value_at (ETableModel *etm, int col, int row);
static void calendar_model_set_value_at (ETableModel *etm, int col, int row, const void *value);
static gboolean calendar_model_is_cell_editable (ETableModel *etm, int col, int row);
static void *calendar_model_duplicate_value (ETableModel *etm, int col, const void *value);
static void calendar_model_free_value (ETableModel *etm, int col, void *value);
static void *calendar_model_initialize_value (ETableModel *etm, int col);
static gboolean calendar_model_value_is_empty (ETableModel *etm, int col, const void *value);
static char * calendar_model_value_to_string (ETableModel *etm, int col, const void *value);

static ETableModelClass *parent_class;



/**
 * calendar_model_get_type:
 * @void:
 *
 * Registers the #CalendarModel class if necessary, and returns the type ID
 * associated to it.
 *
 * Return value: The type ID of the #CalendarModel class.
 **/
GtkType
calendar_model_get_type (void)
{
	static GtkType calendar_model_type = 0;

	if (!calendar_model_type) {
		static GtkTypeInfo calendar_model_info = {
			"CalendarModel",
			sizeof (CalendarModel),
			sizeof (CalendarModelClass),
			(GtkClassInitFunc) calendar_model_class_init,
			(GtkObjectInitFunc) calendar_model_init,
			NULL, /* reserved_1 */
			NULL, /* reserved_2 */
			(GtkClassInitFunc) NULL
		};

		calendar_model_type = gtk_type_unique (E_TABLE_MODEL_TYPE, &calendar_model_info);
	}

	return calendar_model_type;
}

/* Class initialization function for the calendar table model */
static void
calendar_model_class_init (CalendarModelClass *class)
{
	GtkObjectClass *object_class;
	ETableModelClass *etm_class;

	object_class = (GtkObjectClass *) class;
	etm_class = (ETableModelClass *) class;

	parent_class = gtk_type_class (E_TABLE_MODEL_TYPE);

	object_class->destroy = calendar_model_destroy;

	etm_class->column_count = calendar_model_column_count;
	etm_class->row_count = calendar_model_row_count;
	etm_class->value_at = calendar_model_value_at;
	etm_class->set_value_at = calendar_model_set_value_at;
	etm_class->is_cell_editable = calendar_model_is_cell_editable;
	etm_class->duplicate_value = calendar_model_duplicate_value;
	etm_class->free_value = calendar_model_free_value;
	etm_class->initialize_value = calendar_model_initialize_value;
	etm_class->value_is_empty = calendar_model_value_is_empty;
#if 0
	etm_class->value_to_string = calendar_model_value_to_string;
#endif
}

/* Object initialization function for the calendar table model */
static void
calendar_model_init (CalendarModel *model)
{
	CalendarModelPrivate *priv;

	priv = g_new0 (CalendarModelPrivate, 1);
	model->priv = priv;

	priv->objects = g_array_new (FALSE, TRUE, sizeof (iCalObject *));
	priv->uid_index_hash = g_hash_table_new (g_str_hash, g_str_equal);
}

/* Called from g_hash_table_foreach_remove(), frees a stored UID->index
 * mapping.
 */
static gboolean
free_uid_index (gpointer key, gpointer value, gpointer data)
{
	int *idx;

	idx = value;
	g_free (idx);

	return TRUE;
}

/* Frees the objects stored in the calendar model */
static void
free_objects (CalendarModel *model)
{
	CalendarModelPrivate *priv;
	int i;

	priv = model->priv;

	g_hash_table_foreach_remove (priv->uid_index_hash, free_uid_index, NULL);

	for (i = 0; i < priv->objects->len; i++) {
		iCalObject *ico;

		ico = g_array_index (priv->objects, iCalObject *, i);
		g_assert (ico != NULL);
		ical_object_unref (ico);
	}

	g_array_set_size (priv->objects, 0);
}

/* Destroy handler for the calendar table model */
static void
calendar_model_destroy (GtkObject *object)
{
	CalendarModel *model;
	CalendarModelPrivate *priv;

	g_return_if_fail (object != NULL);
	g_return_if_fail (IS_CALENDAR_MODEL (object));

	model = CALENDAR_MODEL (object);
	priv = model->priv;

	/* Free the calendar client interface object */

	if (priv->client) {
		gtk_signal_disconnect_by_data (GTK_OBJECT (priv->client), model);
		gtk_object_unref (GTK_OBJECT (priv->client));
		priv->client = NULL;
	}

	/* Free the uid->index hash data and the array of UIDs */

	free_objects (model);

	g_hash_table_destroy (priv->uid_index_hash);
	priv->uid_index_hash = NULL;

	g_array_free (priv->objects, TRUE);
	priv->objects = NULL;

	/* Free the private structure */

	g_free (priv);
	model->priv = NULL;

	if (GTK_OBJECT_CLASS (parent_class)->destroy)
		(* GTK_OBJECT_CLASS (parent_class)->destroy) (object);
}



/* ETableModel methods */

/* column_count handler for the calendar table model */
static int
calendar_model_column_count (ETableModel *etm)
{
	return ICAL_OBJECT_FIELD_NUM_FIELDS;
}

/* row_count handler for the calendar table model */
static int
calendar_model_row_count (ETableModel *etm)
{
	CalendarModel *model;
	CalendarModelPrivate *priv;

	model = CALENDAR_MODEL (etm);
	priv = model->priv;

	return priv->objects->len;
}

/* value_at handler for the calendar table model */
static void *
calendar_model_value_at (ETableModel *etm, int col, int row)
{
	CalendarModel *model;
	CalendarModelPrivate *priv;
	iCalObject *ico;

	model = CALENDAR_MODEL (etm);
	priv = model->priv;

	g_return_val_if_fail (col >= 0 && col < ICAL_OBJECT_FIELD_NUM_FIELDS, NULL);
	g_return_val_if_fail (row >= 0 && row < priv->objects->len, NULL);

	ico = g_array_index (priv->objects, iCalObject *, row);
	g_assert (ico != NULL);

	switch (col) {
	case ICAL_OBJECT_FIELD_COMMENT:
		return ico->comment ? ico->comment : "";

	case ICAL_OBJECT_FIELD_COMPLETED:
		return &ico->completed;

	case ICAL_OBJECT_FIELD_CREATED:
		return &ico->created;

	case ICAL_OBJECT_FIELD_DESCRIPTION:
		return ico->desc ? ico->desc : "";

	case ICAL_OBJECT_FIELD_DTSTAMP:
		return &ico->dtstamp;

	case ICAL_OBJECT_FIELD_DTSTART:
		return &ico->dtstart;

	case ICAL_OBJECT_FIELD_DTEND:
		return &ico->dtend;

	case ICAL_OBJECT_FIELD_GEO:
		return &ico->geo;

	case ICAL_OBJECT_FIELD_LAST_MOD:
		return &ico->last_mod;

	case ICAL_OBJECT_FIELD_LOCATION:
		return ico->location ? ico->location : "";

	case ICAL_OBJECT_FIELD_ORGANIZER:
		return ico->organizer;

	case ICAL_OBJECT_FIELD_PERCENT:
		return &ico->percent;

	case ICAL_OBJECT_FIELD_PRIORITY:
		return &ico->priority;

	case ICAL_OBJECT_FIELD_SUMMARY:
		return ico->summary ? ico->summary : "";

	case ICAL_OBJECT_FIELD_URL:
		return ico->url ? ico->url : "";

	case ICAL_OBJECT_FIELD_HAS_ALARMS:
		return (gpointer) (ico->dalarm.enabled || ico->aalarm.enabled
				   || ico->palarm.enabled || ico->malarm.enabled);

	default:
		g_message ("calendar_model_value_at(): Requested invalid column %d", col);
		return NULL;
	}
}

/* Replaces a string */
static void
set_string (char **dest, const char *value)
{
	if (*dest)
		g_free (*dest);

	if (value)
		*dest = g_strdup (value);
	else
		*dest = NULL;
}

/* Replaces a time_t value */
static void
set_time_t (time_t *dest, const time_t *value)
{
	*dest = *value;
}

/* Replaces a geo value */
static void
set_geo (iCalGeo *dest, const iCalGeo *value)
{
	*dest = *value;
}

/* Replaces a person value */
static void
set_person (iCalPerson **dest, const iCalPerson *value)
{
	/* FIXME */
}

/* Sets an int value */
static void
set_int (int *dest, const int *value)
{
	*dest = *value;
}

/* set_value_at handler for the calendar table model */
static void
calendar_model_set_value_at (ETableModel *etm, int col, int row, const void *value)
{
	CalendarModel *model;
	CalendarModelPrivate *priv;
	iCalObject *ico;

	model = CALENDAR_MODEL (etm);
	priv = model->priv;

	g_return_if_fail (col >= 0 && col < ICAL_OBJECT_FIELD_NUM_FIELDS);
	g_return_if_fail (row >= 0 && row < priv->objects->len);

	ico = g_array_index (priv->objects, iCalObject *, row);
	g_assert (ico != NULL);

	switch (col) {
	case ICAL_OBJECT_FIELD_COMMENT:
		set_string (&ico->comment, value);
		break;

	case ICAL_OBJECT_FIELD_COMPLETED:
		set_time_t (&ico->completed, value);
		break;

	case ICAL_OBJECT_FIELD_CREATED:
		set_time_t (&ico->created, value);
		break;

	case ICAL_OBJECT_FIELD_DESCRIPTION:
		set_string (&ico->desc, value);
		break;

	case ICAL_OBJECT_FIELD_DTSTAMP:
		set_time_t (&ico->dtstamp, value);
		break;

	case ICAL_OBJECT_FIELD_DTSTART:
		set_time_t (&ico->dtstart, value);
		break;

	case ICAL_OBJECT_FIELD_DTEND:
		set_time_t (&ico->dtend, value);
		break;

	case ICAL_OBJECT_FIELD_GEO:
		set_geo (&ico->geo, value);
		break;

	case ICAL_OBJECT_FIELD_LAST_MOD:
		set_time_t (&ico->last_mod, value);
		break;

	case ICAL_OBJECT_FIELD_LOCATION:
		set_string (&ico->location, value);
		break;

	case ICAL_OBJECT_FIELD_ORGANIZER:
		set_person (&ico->organizer, value);
		break;

	case ICAL_OBJECT_FIELD_PERCENT:
		set_int (&ico->percent, value);
		break;

	case ICAL_OBJECT_FIELD_PRIORITY:
		set_int (&ico->priority, value);
		break;

	case ICAL_OBJECT_FIELD_SUMMARY:
		set_string (&ico->summary, value);
		break;

	case ICAL_OBJECT_FIELD_URL:
		set_string (&ico->url, value);
		break;

	case ICAL_OBJECT_FIELD_HAS_ALARMS:
		g_message ("calendar_model_set_value_at(): HAS_ALARMS is not a settable field!");
		break;

	default:
		g_message ("calendar_model_set_value_at(): Requested invalid column %d", col);
		break;
	}

	if (!cal_client_update_object (priv->client, ico))
		g_message ("calendar_model_set_value_at(): Could not update the object!");
}

/* is_cell_editable handler for the calendar table model */
static gboolean
calendar_model_is_cell_editable (ETableModel *etm, int col, int row)
{
	CalendarModel *model;
	CalendarModelPrivate *priv;

	model = CALENDAR_MODEL (etm);
	priv = model->priv;

	g_return_val_if_fail (col >= 0 && col < ICAL_OBJECT_FIELD_NUM_FIELDS, FALSE);
	g_return_val_if_fail (row >= 0 && row < priv->objects->len, FALSE);

	switch (col) {
	case ICAL_OBJECT_FIELD_HAS_ALARMS:
		return FALSE;

	default:
		return TRUE;
	}
}

/* Duplicates a string value */
static char *
dup_string (const char *value)
{
	return g_strdup (value);
}

/* Duplicates a time_t value */
static time_t *
dup_time_t (const time_t *value)
{
	time_t *t;

	t = g_new (time_t, 1);
	*t = *value;
	return t;
}

/* Duplicates a geo value */
static iCalGeo *
dup_geo (const iCalGeo *value)
{
	iCalGeo *geo;

	geo = g_new (iCalGeo, 1);
	*geo = *value;
	return geo;
}

/* Duplicates a person value */
static iCalPerson *
dup_person (const iCalPerson *value)
{
	/* FIXME */
	return NULL;
}

/* Duplicates an int value */
static int *
dup_int (const int *value)
{
	int *v;

	v = g_new (int, 1);
	*v = *value;
	return v;
}

/* duplicate_value handler for the calendar table model */
static void *
calendar_model_duplicate_value (ETableModel *etm, int col, const void *value)
{
	g_return_val_if_fail (col >= 0 && col < ICAL_OBJECT_FIELD_NUM_FIELDS, NULL);

	switch (col) {
	case ICAL_OBJECT_FIELD_COMMENT:
		return dup_string (value);

	case ICAL_OBJECT_FIELD_COMPLETED:
		return dup_time_t (value);

	case ICAL_OBJECT_FIELD_CREATED:
		return dup_time_t (value);

	case ICAL_OBJECT_FIELD_DESCRIPTION:
		return dup_string (value);

	case ICAL_OBJECT_FIELD_DTSTAMP:
		return dup_time_t (value);

	case ICAL_OBJECT_FIELD_DTSTART:
		return dup_time_t (value);

	case ICAL_OBJECT_FIELD_DTEND:
		return dup_time_t (value);

	case ICAL_OBJECT_FIELD_GEO:
		return dup_geo (value);

	case ICAL_OBJECT_FIELD_LAST_MOD:
		return dup_time_t (value);

	case ICAL_OBJECT_FIELD_LOCATION:
		return dup_string (value);

	case ICAL_OBJECT_FIELD_ORGANIZER:
		return dup_person (value);

	case ICAL_OBJECT_FIELD_PERCENT:
		return dup_int (value);

	case ICAL_OBJECT_FIELD_PRIORITY:
		return dup_int (value);

	case ICAL_OBJECT_FIELD_SUMMARY:
		return dup_string (value);

	case ICAL_OBJECT_FIELD_URL:
		return dup_string (value);

	case ICAL_OBJECT_FIELD_HAS_ALARMS:
		return (void *) value;

	default:
		g_message ("calendar_model_duplicate_value(): Requested invalid column %d", col);
		return NULL;
	}
}

/* free_value handler for the calendar table model */
static void
calendar_model_free_value (ETableModel *etm, int col, void *value)
{
	g_return_if_fail (col >= 0 && col < ICAL_OBJECT_FIELD_NUM_FIELDS);
	g_return_if_fail (value != NULL);

	/* FIXME: this requires special handling for iCalPerson */
	g_free (value);
}

/* Initializes a string value */
static char *
init_string (void)
{
	return g_strdup ("");
}

/* Initializes a time_t value */
static time_t *
init_time_t (void)
{
	time_t *t;

	t = g_new (time_t, 1);
	*t = -1;
	return t;
}

/* Initializes a geo value */
static iCalGeo *
init_geo (void)
{
	iCalGeo *geo;

	geo = g_new (iCalGeo, 1);
	geo->valid = FALSE;
	geo->latitude = 0.0;
	geo->longitude = 0.0;
	return geo;
}

/* Initializes a person value */
static iCalPerson *
init_person (void)
{
	/* FIXME */
	return NULL;
}

/* Initializes an int value */
static int *
init_int (void)
{
	int *v;

	v = g_new (int, 1);
	*v = 0;
	return v;
}

/* initialize_value handler for the calendar table model */
static void *
calendar_model_initialize_value (ETableModel *etm, int col)
{
	g_return_val_if_fail (col >= 0 && col < ICAL_OBJECT_FIELD_NUM_FIELDS, NULL);

	switch (col) {
	case ICAL_OBJECT_FIELD_COMMENT:
		return init_string ();

	case ICAL_OBJECT_FIELD_COMPLETED:
		return init_time_t ();

	case ICAL_OBJECT_FIELD_CREATED:
		return init_time_t ();

	case ICAL_OBJECT_FIELD_DESCRIPTION:
		return init_string ();

	case ICAL_OBJECT_FIELD_DTSTAMP:
		return init_time_t ();

	case ICAL_OBJECT_FIELD_DTSTART:
		return init_time_t ();

	case ICAL_OBJECT_FIELD_DTEND:
		return init_time_t ();

	case ICAL_OBJECT_FIELD_GEO:
		return init_geo ();

	case ICAL_OBJECT_FIELD_LAST_MOD:
		return init_time_t ();

	case ICAL_OBJECT_FIELD_LOCATION:
		return init_string ();

	case ICAL_OBJECT_FIELD_ORGANIZER:
		return init_person ();

	case ICAL_OBJECT_FIELD_PERCENT:
		return init_int ();

	case ICAL_OBJECT_FIELD_PRIORITY:
		return init_int ();

	case ICAL_OBJECT_FIELD_SUMMARY:
		return init_string ();

	case ICAL_OBJECT_FIELD_URL:
		return init_string ();

	case ICAL_OBJECT_FIELD_HAS_ALARMS:
		return NULL; /* "false" */

	default:
		g_message ("calendar_model_initialize_value(): Requested invalid column %d", col);
		return NULL;
	}
}

/* Returns whether a string is empty */
static gboolean
string_is_empty (const char *str)
{
	return !(str && *str);
}

/* Returns whether a time_t is empty */
static gboolean
time_t_is_empty (const time_t *t)
{
	return (*t == -1);
}

/* Returns whether a geo is empty */
static gboolean
geo_is_empty (const iCalGeo *geo)
{
	return !geo->valid;
}

/* Returns whether a person is empty */
static gboolean
person_is_empty (const iCalPerson *person)
{
	/* FIXME */
	return TRUE;
}

/* Returns whether an int is empty */
static gboolean
int_is_empty (const int *value)
{
	return FALSE;
}

/* value_is_empty handler for the calendar model */
static gboolean
calendar_model_value_is_empty (ETableModel *etm, int col, const void *value)
{
	g_return_val_if_fail (col >= 0 && col < ICAL_OBJECT_FIELD_NUM_FIELDS, TRUE);

	switch (col) {
	case ICAL_OBJECT_FIELD_COMMENT:
		return string_is_empty (value);

	case ICAL_OBJECT_FIELD_COMPLETED:
		return time_t_is_empty (value);

	case ICAL_OBJECT_FIELD_CREATED:
		return time_t_is_empty (value);

	case ICAL_OBJECT_FIELD_DESCRIPTION:
		return string_is_empty (value);

	case ICAL_OBJECT_FIELD_DTSTAMP:
		return time_t_is_empty (value);

	case ICAL_OBJECT_FIELD_DTSTART:
		return time_t_is_empty (value);

	case ICAL_OBJECT_FIELD_DTEND:
		return time_t_is_empty (value);

	case ICAL_OBJECT_FIELD_GEO:
		return geo_is_empty (value);

	case ICAL_OBJECT_FIELD_LAST_MOD:
		return time_t_is_empty (value);

	case ICAL_OBJECT_FIELD_LOCATION:
		return string_is_empty (value);

	case ICAL_OBJECT_FIELD_ORGANIZER:
		return person_is_empty (value);

	case ICAL_OBJECT_FIELD_PERCENT:
		return int_is_empty (value);

	case ICAL_OBJECT_FIELD_PRIORITY:
		return int_is_empty (value);

	case ICAL_OBJECT_FIELD_SUMMARY:
		return string_is_empty (value);

	case ICAL_OBJECT_FIELD_URL:
		return string_is_empty (value);

	case ICAL_OBJECT_FIELD_HAS_ALARMS:
		return FALSE;

	default:
		g_message ("calendar_model_value_is_empty(): Requested invalid column %d", col);
		return TRUE;
	}
}



/**
 * calendar_model_new:
 *
 * Creates a new calendar model.  It must be told about the calendar client
 * interface object it will monitor with calendar_model_set_cal_client().
 *
 * Return value: A newly-created calendar model.
 **/
CalendarModel *
calendar_model_new (void)
{
	return CALENDAR_MODEL (gtk_type_new (TYPE_CALENDAR_MODEL));
}

/* Removes an object from the model and updates all the indices that follow.
 * Returns the index of the object that was removed, or -1 if no object with
 * such UID was found.
 */
static int
remove_object (CalendarModel *model, const char *uid)
{
	CalendarModelPrivate *priv;
	int *idx;
	iCalObject *orig_ico;
	int i;
	int n;

	priv = model->priv;

	/* Find the index of the object to be removed */

	idx = g_hash_table_lookup (priv->uid_index_hash, uid);
	if (!idx)
		return -1;

	orig_ico = g_array_index (priv->objects, iCalObject *, *idx);
	g_assert (orig_ico != NULL);

	/* Decrease the indices of all the objects that follow in the array */

	for (i = *idx + 1; i < priv->objects->len; i++) {
		iCalObject *ico;
		int *ico_idx;

		ico = g_array_index (priv->objects, iCalObject *, i);
		g_assert (ico != NULL);

		ico_idx = g_hash_table_lookup (priv->uid_index_hash, ico->uid);
		g_assert (ico_idx != NULL);

		(*ico_idx)--;
		g_assert (*ico_idx >= 0);
	}

	/* Remove this object from the array and hash */

	g_hash_table_remove (priv->uid_index_hash, uid);
	g_array_remove_index (priv->objects, *idx);

	ical_object_unref (orig_ico);

	n = *idx;
	g_free (idx);

	return n;
}

/* Callback used when an object is updated in the server */
static void
obj_updated_cb (CalClient *client, const char *uid, gpointer data)
{
	CalendarModel *model;
	CalendarModelPrivate *priv;
	int orig_idx;
	iCalObject *new_ico;
	int *new_idx;
	CalClientGetStatus status;
	gboolean added;

	model = CALENDAR_MODEL (data);
	priv = model->priv;

	orig_idx = remove_object (model, uid);

	status = cal_client_get_object (priv->client, uid, &new_ico);

	added = FALSE;

	switch (status) {
	case CAL_CLIENT_GET_SUCCESS:
		if (orig_idx == -1) {
			/* The object not in the model originally, so we just append it */

			g_array_append_val (priv->objects, new_ico);

			new_idx = g_new (int, 1);
			*new_idx = priv->objects->len - 1;
			g_hash_table_insert (priv->uid_index_hash, new_ico->uid, new_idx);
		} else {
			int i;

			/* Insert the new version of the object in its old position */

			g_array_insert_val (priv->objects, orig_idx, new_ico);

			new_idx = g_new (int, 1);
			*new_idx = orig_idx;
			g_hash_table_insert (priv->uid_index_hash, new_ico->uid, new_idx);

			/* Increase the indices of all subsequent objects */

			for (i = orig_idx + 1; i < priv->objects->len; i++) {
				iCalObject *ico;
				int *ico_idx;

				ico = g_array_index (priv->objects, iCalObject *, i);
				g_assert (ico != NULL);

				ico_idx = g_hash_table_lookup (priv->uid_index_hash, ico->uid);
				g_assert (ico_idx != NULL);

				(*ico_idx)++;
			}
		}

		e_table_model_row_changed (E_TABLE_MODEL (model), *new_idx);
		break;

	case CAL_CLIENT_GET_NOT_FOUND:
		/* Nothing; the object may have been removed from the server.  We just
		 * notify that the old object was deleted.
		 */
		if (orig_idx != -1)
			e_table_model_row_deleted (E_TABLE_MODEL (model), orig_idx);

		break;

	case CAL_CLIENT_GET_SYNTAX_ERROR:
		g_message ("obj_updated_cb(): Syntax error when getting object `%s'", uid);

		/* Same notification as above */
		if (orig_idx != -1)
			e_table_model_row_deleted (E_TABLE_MODEL (model), orig_idx);

		break;

	default:
		g_assert_not_reached ();
	}
}

/* Callback used when an object is removed in the server */
static void
obj_removed_cb (CalClient *client, const char *uid, gpointer data)
{
	CalendarModel *model;
	int idx;

	model = CALENDAR_MODEL (data);

	idx = remove_object (model, uid);

	if (idx != -1)
		e_table_model_row_deleted (E_TABLE_MODEL (model), idx);
}

/* Loads the required objects from the calendar client */
static void
load_objects (CalendarModel *model)
{
	CalendarModelPrivate *priv;
	GList *uids;
	GList *l;

	priv = model->priv;

	uids = cal_client_get_uids (priv->client, priv->type);

	for (l = uids; l; l = l->next) {
		char *uid;
		iCalObject *ico;
		CalClientGetStatus status;
		int *idx;

		uid = l->data;
		status = cal_client_get_object (priv->client, uid, &ico);

		switch (status) {
		case CAL_CLIENT_GET_SUCCESS:
			break;

		case CAL_CLIENT_GET_NOT_FOUND:
			/* Nothing; the object may have been removed from the server */
			continue;

		case CAL_CLIENT_GET_SYNTAX_ERROR:
			g_message ("load_objects(): Syntax error when getting object `%s'", uid);
			continue;

		default:
			g_assert_not_reached ();
		}

		g_assert (ico->uid != NULL);

		idx = g_new (int, 1);

		g_array_append_val (priv->objects, ico);
		*idx = priv->objects->len - 1;

		g_hash_table_insert (priv->uid_index_hash, ico->uid, idx);
	}

	cal_obj_uid_list_free (uids);
}

/**
 * calendar_model_set_cal_client:
 * @model: A calendar model.
 * @client: A calendar client interface object.
 * @type: Type of objects to present.
 *
 * Sets the calendar client interface object that a calendar model will monitor.
 * It also sets the types of objects this model will present to an #ETable.
 **/
void
calendar_model_set_cal_client (CalendarModel *model, CalClient *client, CalObjType type)
{
	CalendarModelPrivate *priv;

	g_return_if_fail (model != NULL);
	g_return_if_fail (IS_CALENDAR_MODEL (model));

	if (client)
		g_return_if_fail (IS_CAL_CLIENT (client));

	priv = model->priv;

	if (priv->client == client && priv->type == type)
		return;

	if (client)
		gtk_object_ref (GTK_OBJECT (client));

	if (priv->client) {
		gtk_signal_disconnect_by_data (GTK_OBJECT (priv->client), model);
		gtk_object_unref (GTK_OBJECT (priv->client));
	}

	free_objects (model);

	priv->client = client;
	priv->type = type;

	if (priv->client) {
		gtk_signal_connect (GTK_OBJECT (priv->client), "obj_updated",
				    GTK_SIGNAL_FUNC (obj_updated_cb), model);
		gtk_signal_connect (GTK_OBJECT (priv->client), "obj_removed",
				    GTK_SIGNAL_FUNC (obj_removed_cb), model);

		load_objects (model);
	}

	e_table_model_changed (E_TABLE_MODEL (model));
}

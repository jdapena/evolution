/* Evolution calendar - Meeting editor dialog
 *
 * Copyright (C) 2000 Helix Code, Inc.
 *
 * Authors: Jesse Pavel <jpavel@helixcode.com>
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
#include <gnome.h>
#include <glade/glade.h>
#include <icaltypes.h>
#include <ical.h>
#include "e-meeting-edit.h"

#define E_MEETING_GLADE_XML "e-meeting-dialog.glade"

#define E_MEETING_DEBUG

typedef struct _EMeetingEditorPrivate EMeetingEditorPrivate;

struct _EMeetingEditorPrivate {
	/* These are the widgets to be used in the GUI. */
	GladeXML *xml;
	GtkWidget *meeting_window;
	GtkWidget *attendee_list;
	GtkWidget *address_entry;
	GtkWidget *edit_dialog;
	GtkWidget *organizer_entry;
	GtkWidget *role_entry;
	GtkWidget *rsvp_check;
	
	gint changed_signal_id;

	/* Various pieces of information. */
	gint selected_row;
	CalComponent *comp;
	CalClient *client;
	icalcomponent *icalcomp, *vevent;

	gint numentries;  /* How many attendees are there? */
	gboolean dirty;  /* Has anything changed? */
}; 

#define NUM_COLUMNS 4  /* The number of columns in our attendee list. */

enum column_names {ADDRESS_COL, ROLE_COL, RSVP_COL, STATUS_COL};

static gchar *partstat_values[] = {
	"Needs action",
	"Accepted",
	"Declined",
	"Tentative",
	"Delegated",
	"Completed",
	"In Progress",
	"Unknown"
};

static gchar *role_values[] = {
	"Chair",
	"Required Participant",
	"Optional Participant",
	"Non-Participant",
	"Other"
};


/* Note that I have to iterate and check myself because
   ical_property_get_xxx_parameter doesn't take into account the
   kind of parameter for which you wish to search! */
static icalparameter *
get_icalparam_by_type (icalproperty *prop, icalparameter_kind kind)
{
	icalparameter *param;

	for (param = icalproperty_get_first_parameter (prop, ICAL_ANY_PARAMETER);
	     param != NULL && icalparameter_isa (param) != kind;
	     param = icalproperty_get_next_parameter (prop, ICAL_ANY_PARAMETER) );

	return param;
}


static gboolean
window_delete_cb (GtkWidget *widget,
		  GdkEvent *event,
		  gpointer data)
{	
	EMeetingEditorPrivate *priv;
	gchar *text;

	priv = (EMeetingEditorPrivate *) ((EMeetingEditor *)data)->priv;

#ifdef E_MEETING_DEBUG
	g_printerr ("e-meeting-edit.c: The main window received a delete event.\n");
#endif
	
	if (priv->dirty == TRUE) {
		/* FIXME: notify the event editor that our data has changed. 
			For now, I'll just display a dialog box. */
		{
			GtkWidget *dialog;
			icalproperty *prop;
			icalvalue *value;

			/* Save the organizer into the iCAL object. */
        		prop = icalcomponent_get_first_property (priv->vevent, ICAL_ORGANIZER_PROPERTY);

			text = gtk_entry_get_text (GTK_ENTRY (priv->organizer_entry));
#ifdef E_MEETING_DEBUG		
			g_print ("e-meeting-edit.c: The organizer entry is %s.\n", text);
#endif
			if (strlen (text) > 0) {
				gchar buffer[200];
				g_snprintf (buffer, 190, "MAILTO:%s", text);
				
				if (prop == NULL) {
					/* We need to add an ORGANIZER property. */
					prop = icalproperty_new (ICAL_ORGANIZER_PROPERTY);
					icalcomponent_add_property (priv->vevent, prop);
				}
				value = icalvalue_new_text (buffer);
				icalproperty_set_value (prop, value);
			}
		
			dialog = gnome_warning_dialog_parented ("Note that the meeting has changed,\n"
								"and you should save this event.",
								GTK_WINDOW (priv->meeting_window));
			gnome_dialog_run (GNOME_DIALOG(dialog));
		}
	}

	gtk_entry_set_text (GTK_ENTRY (priv->organizer_entry), "");

	return (FALSE);
}

static void 
window_destroy_cb (GtkWidget *widget,
		   gpointer data)
{
	EMeetingEditorPrivate *priv;

#ifdef E_MEETING_DEBUG
	g_printerr ("e-meeting-edit.c: The main window received a destroy event.\n");
#endif

	priv = (EMeetingEditorPrivate *) ((EMeetingEditor *)data)->priv;

	gtk_main_quit ();
	return;
}

/* put_property_in_list() synchronizes the display of row `rownum'
   in our attendee list to the values of `prop'. If rownum < 0,
   then put_property_in_list() will append a new row. 
   If the property doesn't contain certain parameters that we deem
   necessary, it will add them. */
static void
put_property_in_list (icalproperty *prop, gint rownum, gpointer data)
{
	gchar *row_text[NUM_COLUMNS];
	gchar *text, *new_text;
	icalparameter *param;
	icalvalue *value;
	gint enumval;
	gint cntr;

	EMeetingEditorPrivate *priv;

	priv = (EMeetingEditorPrivate *) ((EMeetingEditor *)data)->priv;
	
	value = icalproperty_get_value (prop);

	if (value != NULL) {
		text = strdup (icalvalue_as_ical_string (value));
	
		/* Here I strip off the "MAILTO:" if it is present. */
		new_text = strchr (text, ':');
		if (new_text != NULL)
			new_text++;
		else
			new_text = text;
	
		row_text[ADDRESS_COL] = g_strdup (new_text);
		g_free (text);
	}

	param = get_icalparam_by_type (prop, ICAL_ROLE_PARAMETER);
	if (param == NULL) {
#ifdef E_MEETING_DEBUG		
		g_print ("e-meeting-edit.c: within put_param...(), param is NULL.\n");
#endif
		param = icalparameter_new_role (ICAL_ROLE_REQPARTICIPANT);
		icalproperty_add_parameter (prop, param);
	}
		
	enumval = icalparameter_get_role (param);
	if (enumval < 0 || enumval > 4)
		enumval = 4;
#ifdef E_MEETING_DEBUG		
	g_print ("e-meeting-edit.c: the role value is %d.\n", enumval);
#endif
	
	row_text[ROLE_COL] = role_values [enumval];

	param = get_icalparam_by_type (prop, ICAL_RSVP_PARAMETER);
	if (param == NULL) {
		param = icalparameter_new_rsvp (TRUE);
		icalproperty_add_parameter (prop, param);
	}

	if (icalparameter_get_rsvp (param))
		row_text[RSVP_COL] = "Y";
	else
		row_text[RSVP_COL] = "N";

#ifdef E_MEETING_DEBUG		
	g_print ("e-meeting-edit.c: the RSVP is %c.\n", row_text[RSVP_COL][0]);
#endif

	
	param = get_icalparam_by_type (prop, ICAL_PARTSTAT_PARAMETER);
	if (param == NULL) {
		param = icalparameter_new_partstat (ICAL_PARTSTAT_NEEDSACTION);
		icalproperty_add_parameter (prop, param);
	}

	enumval = icalparameter_get_partstat (param);
	if (enumval < 0 || enumval > 7) {
		enumval = 7;
	}

	row_text[STATUS_COL] = partstat_values [enumval];

	if (rownum < 0) {
		gtk_clist_append (GTK_CLIST (priv->attendee_list), row_text);
		gtk_clist_set_row_data (GTK_CLIST (priv->attendee_list), priv->numentries, prop);
		priv->numentries++;
	}
	else {
		for (cntr = 0; cntr < NUM_COLUMNS; cntr++) {
			gtk_clist_set_text (GTK_CLIST (priv->attendee_list), 
					    rownum,
					    cntr,
					    row_text[cntr]);
		}
	}

	g_free (row_text[ADDRESS_COL]);
}

	

/* edit_attendee() performs the GUI manipulation and interaction for
   editing `prop' and returns TRUE if the user indicated that he wants
   to save the new property information.

   Note that it is necessary that the property have parameters of the types
   RSVP, PARTSTAT, and ROLE already when passed into this function. */
static gboolean
edit_attendee (icalproperty *prop, gpointer data)
{
	EMeetingEditorPrivate *priv;
	gint button_num;
	gchar *new_text, *text;
	icalparameter *param;
	icalvalue *value;
	gchar buffer[200];
	gint cntr;
	gint enumval;
	gboolean retval;

	priv = (EMeetingEditorPrivate *) ((EMeetingEditor *)data)->priv;

	g_return_val_if_fail (prop != NULL, FALSE);

	if (priv->edit_dialog == NULL || priv->address_entry == NULL) {
		priv->edit_dialog = glade_xml_get_widget (priv->xml, "edit_dialog");
		priv->address_entry = glade_xml_get_widget (priv->xml, "address_entry");

		gnome_dialog_set_close (GNOME_DIALOG (priv->edit_dialog), TRUE);
		gnome_dialog_editable_enters (GNOME_DIALOG (priv->edit_dialog), 
					      GTK_EDITABLE (priv->address_entry));
		gnome_dialog_close_hides (GNOME_DIALOG (priv->edit_dialog), TRUE);
		gnome_dialog_set_default (GNOME_DIALOG (priv->edit_dialog), 0);
	}

	g_return_val_if_fail (priv->edit_dialog != NULL, FALSE);
	g_return_val_if_fail (priv->address_entry != NULL, FALSE);

	gtk_widget_realize (priv->edit_dialog);
	
	value = icalproperty_get_value (prop);

	if (value != NULL) {
		text = strdup (icalvalue_as_ical_string (value));
	
		/* Here I strip off the "MAILTO:" if it is present. */
		new_text = strchr (text, ':');
		if (new_text != NULL)
			new_text++;
		else
			new_text = text;
	
		gtk_entry_set_text (GTK_ENTRY (priv->address_entry), new_text);
		g_free (text);
	}
	else {
		gtk_entry_set_text (GTK_ENTRY (priv->address_entry), "");
	}
				

	param = get_icalparam_by_type (prop, ICAL_ROLE_PARAMETER);
	enumval = icalparameter_get_role (param);
	if (enumval < 0 || enumval > 4)
		enumval = 4;
	
	text = role_values [enumval];
	gtk_entry_set_text (GTK_ENTRY (priv->role_entry), text);

	param = get_icalparam_by_type (prop, ICAL_RSVP_PARAMETER);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->rsvp_check),
					icalparameter_get_rsvp (param));

	gtk_widget_show (priv->edit_dialog);

	button_num = gnome_dialog_run (GNOME_DIALOG (priv->edit_dialog));

	if (button_num == 0) {
		/* The user pressed the OK button. */
		new_text = gtk_entry_get_text (GTK_ENTRY (priv->address_entry));

		g_snprintf (buffer, 190, "MAILTO:%s", new_text);
		value = icalvalue_new_text (buffer);
		icalproperty_set_value (prop, value);

		/* Take care of the ROLE. */
		icalproperty_remove_parameter (prop, ICAL_ROLE_PARAMETER);

		param = NULL;
		text = gtk_entry_get_text (GTK_ENTRY(priv->role_entry));
#ifdef E_MEETING_DEBUG		
		g_print ("e-meeting-edit.c: the role entry text is %s.\n", text);
#endif

		for (cntr = 0; cntr < 5; cntr++) {
			if (strncmp (text, role_values[cntr], 3) == 0) {
				param = icalparameter_new_role (cntr);
				break;
			}
		}

		if (param == NULL) {
			g_print ("e-meeting-edit.c: edit_attendee() the ROLE param was null.\n");
			/* Use this as a default case, if none of the others match. */
			param = icalparameter_new_role (ICAL_ROLE_REQPARTICIPANT);
		}

		icalproperty_add_parameter (prop, param);

		/* Now the RSVP. */
		icalproperty_remove_parameter (prop, ICAL_RSVP_PARAMETER);

		param = icalparameter_new_rsvp 
				(gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->rsvp_check)));
		icalproperty_add_parameter (prop, param);

		retval = TRUE;
	}
	else  /* The user didn't say OK. */
		retval = FALSE;
	
	return retval;
}


static void 
add_button_clicked_cb (GtkWidget *widget, gpointer data)
{
	EMeetingEditorPrivate *priv;
	icalproperty *prop;
	icalparameter *param;

#ifdef E_MEETING_DEBUG
	g_printerr ("e-meeting-edit.c: the add button was clicked.\n");
#endif

	priv = (EMeetingEditorPrivate *) ((EMeetingEditor *)data)->priv;

	prop = icalproperty_new (ICAL_ATTENDEE_PROPERTY);
	param = icalparameter_new_role (ICAL_ROLE_REQPARTICIPANT); 
	icalproperty_add_parameter (prop, param);
	param = icalparameter_new_rsvp (TRUE); 
	icalproperty_add_parameter (prop, param);
	param = icalparameter_new_partstat (ICAL_PARTSTAT_NEEDSACTION);
	icalproperty_add_parameter (prop, param);

	if (edit_attendee (prop, data) == TRUE) {
#ifdef E_MEETING_DEBUG		
		g_print ("e-meeting-edit.c: After edit_attendee()");
#endif

		/* Let's add this property to our component and to the CList. */
		icalcomponent_add_property (priv->vevent, prop);

		/* The -1 indicates that we should add a new row. */
		put_property_in_list (prop, -1, data);

#ifdef E_MEETING_DEBUG		
		g_print ("e-meeting-edit.c: After put_property_in_list()");
#endif

		priv->dirty = TRUE;
	}
	else {
		icalproperty_free (prop);
	}
}	

static void 
delete_button_clicked_cb (GtkWidget *widget, gpointer data)
{
	EMeetingEditorPrivate *priv;

	priv = (EMeetingEditorPrivate *) ((EMeetingEditor *)data)->priv;

	if (priv->selected_row < 0) {
		GtkWidget *dialog;

		dialog = gnome_warning_dialog_parented ("You must select an entry to delete.",
							GTK_WINDOW (priv->meeting_window));
		gnome_dialog_run (GNOME_DIALOG(dialog));
	}
	else {
		/* Delete the associated property from the iCAL object. */
		icalproperty *prop;

		prop = (icalproperty *)gtk_clist_get_row_data (GTK_CLIST (priv->attendee_list),
							       priv->selected_row);
		icalcomponent_remove_property (priv->vevent, prop);
		icalproperty_free (prop);

		gtk_clist_remove (GTK_CLIST (priv->attendee_list), priv->selected_row);
		priv->selected_row = -1;
		priv->numentries--;
		priv->dirty = TRUE;
	}
}

static void 
edit_button_clicked_cb (GtkWidget *widget, gpointer data)
{
	EMeetingEditorPrivate *priv;

	priv = (EMeetingEditorPrivate *) ((EMeetingEditor *)data)->priv;

	
	if (priv->selected_row < 0) {
		GtkWidget *dialog;

		dialog = gnome_warning_dialog_parented ("You must select an entry to edit.",
							GTK_WINDOW (priv->meeting_window));
		gnome_dialog_run (GNOME_DIALOG(dialog));
		return;
	}
	else {
		icalproperty *prop, *new_prop;
		icalparameter *param, *new_param;
		icalvalue *value, *new_value;
		
		prop = (icalproperty *)gtk_clist_get_row_data (GTK_CLIST (priv->attendee_list),
							       priv->selected_row);

		g_assert (prop != NULL);

		new_prop = icalproperty_new_clone (prop);

		if (edit_attendee (new_prop, data)) {
			/* The user hit Okay. */
			/*We need to synchronize the old property with the newly edited one.*/
			value = icalvalue_new_clone (icalproperty_get_value (new_prop));
			icalproperty_set_value (prop, value);

			icalproperty_remove_parameter (prop, ICAL_ROLE_PARAMETER);
			icalproperty_remove_parameter (prop, ICAL_RSVP_PARAMETER);
			icalproperty_remove_parameter (prop, ICAL_PARTSTAT_PARAMETER);

			param = icalparameter_new_clone (get_icalparam_by_type (new_prop, ICAL_ROLE_PARAMETER));
			g_assert (param != NULL);
			icalproperty_add_parameter (prop, param);
			param = icalparameter_new_clone (get_icalparam_by_type (new_prop, ICAL_RSVP_PARAMETER));
			g_assert (param != NULL);
			icalproperty_add_parameter (prop, param);
			param = icalparameter_new_clone (get_icalparam_by_type (new_prop, ICAL_PARTSTAT_PARAMETER));
			g_assert (param != NULL);
			icalproperty_add_parameter (prop, param);
			
			put_property_in_list (prop, priv->selected_row, data);
			priv->dirty = TRUE;

		}
		icalproperty_free (new_prop);
	}
}



static void 
list_row_select_cb  (GtkWidget *widget,
                     gint row,
                     gint column,
                     GdkEventButton *event,
                     gpointer data)
{
	EMeetingEditorPrivate *priv;

	priv = (EMeetingEditorPrivate *) ((EMeetingEditor *)data)->priv;
	
	priv->selected_row = row;
}

static void
organizer_changed_cb (GtkWidget *widget, gpointer data)
{
	EMeetingEditorPrivate *priv;

	priv = (EMeetingEditorPrivate *) ((EMeetingEditor *)data)->priv;

	gtk_signal_disconnect (GTK_OBJECT (priv->organizer_entry), priv->changed_signal_id);

	priv->dirty = TRUE;
}


/* ------------------------------------------------------------ */
/* --------------------- Exported Functions ------------------- */
/* ------------------------------------------------------------ */

EMeetingEditor * 
e_meeting_editor_new (CalComponent *comp, CalClient *client)
{
	EMeetingEditor *object;
	EMeetingEditorPrivate *priv;

	object = (EMeetingEditor *)g_new(EMeetingEditor, 1);
	
	priv = (EMeetingEditorPrivate *) g_new0(EMeetingEditorPrivate, 1);
	priv->selected_row = -1;
	priv->comp = comp;
	priv->client = client;
	priv->icalcomp = cal_component_get_icalcomponent (comp);
	
	object->priv = priv;

	return object;	
}

void
e_meeting_editor_free (EMeetingEditor *editor)
{
	if (editor == NULL)
		return;
		
	if (editor->priv != NULL)
		g_free (editor->priv);
	
	g_free (editor);
}
	


void 
e_meeting_edit (EMeetingEditor *editor)
{
	EMeetingEditorPrivate *priv;
	GtkWidget *add_button, *delete_button, *edit_button;
	icalproperty *prop;
	icalvalue *value;
	gchar *text;


	g_return_if_fail (editor != NULL);

	priv = (EMeetingEditorPrivate *)editor->priv;

	g_return_if_fail (priv != NULL);


	priv->xml = glade_xml_new (EVOLUTION_GLADEDIR "/" E_MEETING_GLADE_XML, NULL);
	
	priv->meeting_window =  glade_xml_get_widget (priv->xml, "meeting_window");
	priv->attendee_list = glade_xml_get_widget (priv->xml, "attendee_list");
	priv->role_entry = glade_xml_get_widget (priv->xml, "role_entry");
	priv->rsvp_check = glade_xml_get_widget (priv->xml, "rsvp_check");

	gtk_clist_set_column_justification (GTK_CLIST (priv->attendee_list), ROLE_COL, GTK_JUSTIFY_CENTER);
	gtk_clist_set_column_justification (GTK_CLIST (priv->attendee_list), RSVP_COL, GTK_JUSTIFY_CENTER);
	gtk_clist_set_column_justification (GTK_CLIST (priv->attendee_list), STATUS_COL, GTK_JUSTIFY_CENTER);

        gtk_signal_connect (GTK_OBJECT (priv->meeting_window), "delete_event",
                            GTK_SIGNAL_FUNC (window_delete_cb), editor);

	gtk_signal_connect_after (GTK_OBJECT (priv->meeting_window), "delete_event",
				  GTK_SIGNAL_FUNC (window_destroy_cb), editor);

        gtk_signal_connect (GTK_OBJECT (priv->meeting_window), "destroy_event",
                            GTK_SIGNAL_FUNC (window_destroy_cb), editor);

	gtk_signal_connect (GTK_OBJECT (priv->attendee_list), "select_row",
			    GTK_SIGNAL_FUNC (list_row_select_cb), editor);

	add_button = glade_xml_get_widget (priv->xml, "add_button");
	delete_button = glade_xml_get_widget (priv->xml, "delete_button");
	edit_button = glade_xml_get_widget (priv->xml, "edit_button");

	gtk_signal_connect (GTK_OBJECT (add_button), "clicked",
			    GTK_SIGNAL_FUNC (add_button_clicked_cb), editor);
	
	gtk_signal_connect (GTK_OBJECT (delete_button), "clicked",
			    GTK_SIGNAL_FUNC (delete_button_clicked_cb), editor);

	gtk_signal_connect (GTK_OBJECT (edit_button), "clicked",
			    GTK_SIGNAL_FUNC (edit_button_clicked_cb), editor);

	priv->organizer_entry = glade_xml_get_widget (priv->xml, "organizer_entry");

	if (icalcomponent_isa (priv->icalcomp) != ICAL_VEVENT_COMPONENT)
		priv->vevent = icalcomponent_get_first_component(priv->icalcomp,ICAL_VEVENT_COMPONENT);
	else
		priv->vevent = priv->icalcomp;

	g_assert (priv->vevent != NULL);

	/* Let's extract the organizer, if there is one. */
        prop = icalcomponent_get_first_property (priv->vevent, ICAL_ORGANIZER_PROPERTY);

	if (prop != NULL) {
		gchar *buffer;

#ifdef E_MEETING_DEBUG		
		g_print ("e-meeting-edit.c: The organizer property is not null.\n");
#endif

	        value = icalproperty_get_value (prop);
		buffer = g_strdup (icalvalue_as_ical_string (value));
	        if (buffer != NULL) {
			/* Strip off the MAILTO:, if it is present. */
	        	text = strchr (buffer, ':');
			if (text == NULL)
				text = buffer;
			else
				text++;
				
			gtk_entry_set_text (GTK_ENTRY (priv->organizer_entry), text);
			g_free (buffer);
		}
		
	}
#ifdef E_MEETING_DEBUG		
	else {
		g_print ("e-meeting-edit.c: the organizer property was NULL.\n");
	}
#endif

	priv->changed_signal_id = gtk_signal_connect (GTK_OBJECT (priv->organizer_entry), "changed",
						      GTK_SIGNAL_FUNC (organizer_changed_cb), editor);


	/* Let's go through the iCAL object, and create a list entry
	   for each ATTENDEE property. */
        for (prop = icalcomponent_get_first_property (priv->vevent, ICAL_ATTENDEE_PROPERTY);
             prop != NULL;
             prop = icalcomponent_get_next_property (priv->vevent, ICAL_ATTENDEE_PROPERTY))
	{
		put_property_in_list (prop, -1, editor);
	}
	

	gtk_widget_show (priv->meeting_window);

	gtk_main ();

#ifdef E_MEETING_DEBUG
	g_printerr ("e-meeting-edit.c: We've terminated the subsidiary gtk_main().\n");
#endif

	if (priv->meeting_window != NULL)
		gtk_widget_destroy (priv->meeting_window);

	if (priv->edit_dialog != NULL)
		gtk_widget_destroy (priv->edit_dialog);

	gtk_object_unref (GTK_OBJECT (priv->xml));
}

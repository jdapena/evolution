/*
 *  Copyright (C) 2000 Helix Code Inc.
 *
 *  Authors: Michael Zucchi <notzed@helixcode.com>
 *
 *  Implementations of the filter-arg types.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public
 *  License along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <gnome.h>

#include "filter-arg-types.h"

/* ********************************************************************** */
/*                               String                                   */
/* ********************************************************************** */

/* Use for a superclass of any items which are simple strings */

static void filter_arg_string_class_init (FilterArgStringClass *class);
static void filter_arg_string_init       (FilterArgString      *gspaper);

static FilterArg *string_parent_class;

guint
filter_arg_string_get_type (void)
{
	static guint type = 0;
	
	if (!type) {
		GtkTypeInfo type_info = {
			"FilterArgString",
			sizeof (FilterArgString),
			sizeof (FilterArgStringClass),
			(GtkClassInitFunc) filter_arg_string_class_init,
			(GtkObjectInitFunc) filter_arg_string_init,
			(GtkArgSetFunc) NULL,
			(GtkArgGetFunc) NULL
		};
		
		type = gtk_type_unique (filter_arg_get_type (), &type_info);
	}
	
	return type;
}

static void
arg_string_write_html(FilterArg *argin, GtkHTML *html, GtkHTMLStream *stream)
{
	/* empty */
}

static void
arg_string_write_text(FilterArg *argin, GString *string)
{
	GList *l;
	char *a;

	l = argin->values;
	if (l == NULL) {
		g_string_append(string, "folder");
	}
	while (l) {
		a = l->data;
		g_string_append(string, a);
		if (l->next) {
			g_string_append(string, ", ");
		}
		l = g_list_next(l);
	}
}

static void
arg_string_edit_values(FilterArg *arg)
{
	printf("edit string values!\n");
}

/* pop up a dialogue, asking for a new string value */
static int
arg_string_edit_value (FilterArg *arg, int index)
{
	GnomeDialog *dialogue;
	GtkWidget *hbox;
	GtkWidget *label;
	GtkWidget *entry;
	char *text = NULL;
	char *newtext;

	dialogue = (GnomeDialog *)gnome_dialog_new ("Edit value", "Ok", "Cancel", 0);

	hbox = gtk_hbox_new (FALSE, 0);
	label = gtk_label_new ("Option value");
	gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
	entry = gtk_entry_new();
	gtk_box_pack_start (GTK_BOX (hbox), entry, TRUE, TRUE, 0);
	if (index >= 0) {
		text = filter_arg_get_value (arg, index);
	}
	if (text) {
		gtk_entry_set_text (GTK_ENTRY (entry), text);
	}
	gtk_box_pack_start (GTK_BOX (dialogue->vbox), hbox, TRUE, TRUE, 0);
	gtk_widget_show_all (hbox);
	gtk_object_ref (GTK_OBJECT (entry));   /* so we can get the text back afterwards */
	if (gnome_dialog_run_and_close (dialogue) == 0) {
		GList *node;

		newtext = g_strdup (gtk_entry_get_text (GTK_ENTRY (entry)));
		gtk_object_unref (GTK_OBJECT (entry));
		if (index >= 0 && (node = g_list_find (arg->values, text))) {
			node->data = newtext;
		} else {
			arg->values = g_list_append (arg->values, newtext);
		}
		g_free (text);
		return g_list_index (arg->values, newtext);
	}
	return -1;
}

static xmlNodePtr
arg_string_values_get_xml(FilterArg *argin)
{
	xmlNodePtr value;
	GList *l;
	char *a;

	value = xmlNewNode(NULL, "optionvalue");
	xmlSetProp(value, "name", argin->name);

	l = argin->values;
	while (l) {
		xmlNodePtr cur;

		a = l->data;

		cur = xmlNewChild(value, NULL, "folder", NULL);
		if (a)
			xmlSetProp(cur, "folder", a);
		l = g_list_next(l);
	}

	return value;
}

static void
arg_string_values_add_xml(FilterArg *arg, xmlNodePtr node)
{
	xmlNodePtr n;

	n = node->childs;
	while (n) {
		if (!strcmp(n->name, "folder")) {
			filter_arg_string_add(arg, xmlGetProp(n, "folder"));
		} else {
			g_warning("Loading folders from xml, wrong node encountered: %s\n", n->name);
		}
		n = n->next;
	}
}

static char *
arg_string_get_value_as_string(FilterArg *argin, void *data)
{
	char *a = (char *)data;

	return a;
}

static void
arg_string_free_value(FilterArg *arg, void *a)
{
	g_free(a);
}

static void
filter_arg_string_class_init (FilterArgStringClass *class)
{
	GtkObjectClass *object_class;
	
	object_class = (GtkObjectClass *) class;
	if (string_parent_class == NULL)
		string_parent_class = gtk_type_class (gtk_object_get_type ());

	class->parent_class.write_html = arg_string_write_html;
	class->parent_class.write_text = arg_string_write_text;
	class->parent_class.edit_values = arg_string_edit_values;
	class->parent_class.edit_value = arg_string_edit_value;
	class->parent_class.free_value = arg_string_free_value;
	class->parent_class.get_value_as_string = arg_string_get_value_as_string;

	class->parent_class.values_get_xml = arg_string_values_get_xml;
	class->parent_class.values_add_xml = arg_string_values_add_xml;
}

static void
filter_arg_string_init (FilterArgString *arg)
{
	arg->arg.values = NULL;
}

/**
 * filter_arg_string_new:
 *
 * Create a new FilterArgString widget.
 * 
 * Return value: A new FilterArgString widget.
 **/
FilterArg *
filter_arg_string_new (char *name)
{
	FilterArg *a = FILTER_ARG ( gtk_type_new (filter_arg_string_get_type ()));
	a->name = g_strdup(name);
	return a;
}


void
filter_arg_string_add(FilterArg *arg, char *name)
{
	filter_arg_add(arg, g_strdup(name));
}

void
filter_arg_string_remove(FilterArg *arg, char *name)
{
	/* do it */
}


/* ********************************************************************** */
/*                               Address                                  */
/* ********************************************************************** */

static void filter_arg_address_class_init (FilterArgAddressClass *class);
static void filter_arg_address_init       (FilterArgAddress      *gspaper);

static FilterArg *parent_class;

guint
filter_arg_address_get_type (void)
{
	static guint type = 0;
	
	if (!type) {
		GtkTypeInfo type_info = {
			"FilterArgAddress",
			sizeof (FilterArgAddress),
			sizeof (FilterArgAddressClass),
			(GtkClassInitFunc) filter_arg_address_class_init,
			(GtkObjectInitFunc) filter_arg_address_init,
			(GtkArgSetFunc) NULL,
			(GtkArgGetFunc) NULL
		};
		
		type = gtk_type_unique (filter_arg_get_type (), &type_info);
	}
	
	return type;
}

static void
arg_address_write_html (FilterArg *argin, GtkHTML *html, GtkHTMLStream *stream)
{
	/* empty */
}

static void
arg_address_write_text (FilterArg *argin, GString *string)
{
	GList *l;
	struct filter_arg_address *a;

	l = argin->values;
	if (l == NULL) {
		g_string_append (string, "email address");
	}
	while (l) {
		a = l->data;
		g_string_append (string, a->name);
		if (l->next) {
			g_string_append (string, ", ");
		}
		l = g_list_next (l);
	}
}

static void
arg_address_edit_values(FilterArg *arg)
{
	printf ("edit it!\n");
}

static int
arg_address_edit_value (FilterArg *arg, int index)
{
	GnomeDialog *dialogue;
	GtkWidget *hbox;
	GtkWidget *label;
	GtkWidget *entry;
	char *text = NULL;
	char *newtext;
	struct filter_arg_address *ad = NULL;

	dialogue = (GnomeDialog *)gnome_dialog_new ("Edit Address value", "Ok", "Cancel", 0);

	hbox = gtk_hbox_new (FALSE, 0);
	label = gtk_label_new ("Address");
	gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
	entry = gtk_entry_new ();
	gtk_box_pack_start (GTK_BOX (hbox), entry, TRUE, TRUE, 0);
	if (index >= 0 && (ad = filter_arg_get_value (arg, index))) {
		text = ad->email;
	}
	if (text) {
		gtk_entry_set_text (GTK_ENTRY (entry), text);
	}
	gtk_box_pack_start (GTK_BOX (dialogue->vbox), hbox, TRUE, TRUE, 0);
	gtk_widget_show_all (hbox);
	gtk_object_ref (GTK_OBJECT (entry));   /* so we can get the text back afterwards */
	if (gnome_dialog_run_and_close (dialogue) == 0) {
		GList *node;

		newtext = g_strdup (gtk_entry_get_text (GTK_ENTRY (entry)));
		gtk_object_unref (GTK_OBJECT (entry));
		if (index >= 0 && ad && (node = g_list_find(arg->values, ad))) {
			ad = node->data;
			g_free (ad->email);
			ad->email = newtext;
		} else {
			ad = g_malloc0 (sizeof(*ad));
			ad->email = newtext;
			arg->values = g_list_append (arg->values, ad);
		}
		g_free (text);
		return g_list_index (arg->values, ad);
	}
	return -1;
}

static xmlNodePtr
arg_address_values_get_xml(FilterArg *argin)
{
	xmlNodePtr value;
	GList *l;
	struct filter_arg_address *a;

	/* hmm, perhaps this overhead should be in FilterArg, and this function just returns the base node?? */
	value = xmlNewNode(NULL, "optionvalue");
	xmlSetProp(value, "name", argin->name);

	l = argin->values;
	while (l) {
		xmlNodePtr cur;

		a = l->data;

		cur = xmlNewChild(value, NULL, "address", NULL);
		if (a->name)
			xmlSetProp(cur, "name", a->name);
		if (a->email)
			xmlSetProp(cur, "email", a->email);
		l = g_list_next(l);
	}

	return value;
}

static void
arg_address_values_add_xml(FilterArg *arg, xmlNodePtr node)
{
	xmlNodePtr n;

	n = node->childs;
	while (n) {
		if (!strcmp(n->name, "address")) {
			char *nm, *e;
			nm = xmlGetProp(n, "name");
			e = xmlGetProp(n, "email");
			filter_arg_address_add(arg, nm, e);
			free(nm);
			free(e);
		} else {
			g_warning("Loading address from xml, wrong node encountered: %s\n", n->name);
		}
		n = n->next;
	}
}

/* the search string is just the raw email address */
static char *
arg_address_get_value_as_string(FilterArg *argin, void *data)
{
	struct filter_arg_address *a = (struct filter_arg_address *)data;

	printf("geting address as string : %s %s\n", a->email, a->name);

	if (a->email == NULL
	    || a->email[0] == '\0') {
		if (a->name == NULL
		    || a->name[0] == '\0')
			return "";
		return a->name;
	} else
		return a->email;
}

static void
arg_address_free_value(FilterArg *arg, struct filter_arg_address *a)
{
	g_free(a->name);
	g_free(a->email);
	g_free(a);
}

static void
filter_arg_address_class_init (FilterArgAddressClass *class)
{
	GtkObjectClass *object_class;

	object_class = (GtkObjectClass *) class;
	if (parent_class == NULL)
		parent_class = gtk_type_class (gtk_object_get_type ());

	class->parent_class.write_html = arg_address_write_html;
	class->parent_class.write_text = arg_address_write_text;
	class->parent_class.edit_values= arg_address_edit_values;
	class->parent_class.edit_value= arg_address_edit_value;
	class->parent_class.free_value = arg_address_free_value;

	class->parent_class.values_get_xml = arg_address_values_get_xml;
	class->parent_class.values_add_xml = arg_address_values_add_xml;

	class->parent_class.get_value_as_string = arg_address_get_value_as_string;
}

static void
filter_arg_address_init (FilterArgAddress *arg)
{
}

/**
 * filter_arg_address_new:
 *
 * Create a new FilterArgAddress widget.
 * 
 * Return value: A new FilterArgAddress widget.
 **/
FilterArg *
filter_arg_address_new (char *name)
{
	FilterArg *a = FILTER_ARG ( gtk_type_new (filter_arg_address_get_type ()));
	a->name = g_strdup(name);
	return a;
}

void
filter_arg_address_add(FilterArg *arg, char *name, char *email)
{
	struct filter_arg_address *a;

	a = g_malloc0(sizeof(*a));

	a->name = g_strdup(name);
	a->email = g_strdup(email);

	filter_arg_add(arg, a);
}

void
filter_arg_address_remove(FilterArg *arg, char *name, char *email)
{

}

/* ********************************************************************** */
/*                               Folder                                   */
/* ********************************************************************** */


#include <bonobo/bonobo-object.h>
#include <bonobo/bonobo-control.h>

#include "Evolution.h"

static void filter_arg_folder_class_init (FilterArgFolderClass *class);
static void filter_arg_folder_init       (FilterArgFolder      *gspaper);

static FilterArg *folder_parent_class;
extern Evolution_Shell global_shell_interface;

static PortableServer_ServantBase__epv            FolderSelectionListener_base_epv;
static POA_Evolution_FolderSelectionListener__epv  FolderSelectionListener_epv;
static POA_Evolution_FolderSelectionListener__vepv FolderSelectionListener_vepv;
static gboolean FolderSelectionListener_vepv_initialized = FALSE;

struct _FolderSelectionListenerServant {
	POA_Evolution_FolderSelectionListener servant;
	FilterArg *arg;
	int index;
	/*EvolutionShellComponentClient *component_client;*/
};
typedef struct _FolderSelectionListenerServant FolderSelectionListenerServant;

static void
impl_FolderSelectionListener_selected(PortableServer_Servant listener_servant, char *uri, char *physical, CORBA_Environment *ev)
{
	FolderSelectionListenerServant *servant = listener_servant;
	GList *node;

	/* only if we have a selection */
	if (physical[0]) {
		printf ("user selected; %s, or did they select %s\n", uri, physical);

		/* FIXME: passing arg 2 of `g_list_index' makes pointer from integer without a cast */
		if (servant->index >= 0 && (node = g_list_index (servant->arg->values, servant->index))) {
			node->data = g_strdup (physical);
		} else {
			servant->arg->values = g_list_append (servant->arg->values, g_strdup (physical));
		}

		gtk_signal_emit_by_name (GTK_OBJECT (servant->arg), "changed");
	}
	gtk_object_unref (GTK_OBJECT (servant->arg));

	g_free (servant);
}

static Evolution_FolderSelectionListener
create_listener (FilterArg *arg, int index)
{
	PortableServer_Servant listener_servant;
	Evolution_FolderSelectionListener corba_interface;
	CORBA_Environment ev;
	FolderSelectionListenerServant *servant;

	if (!FolderSelectionListener_vepv_initialized) {
		FolderSelectionListener_base_epv._private = NULL;
		FolderSelectionListener_base_epv.finalize = NULL;
		FolderSelectionListener_base_epv.default_POA = NULL;
		
		FolderSelectionListener_epv.selected = impl_FolderSelectionListener_selected;
		
		FolderSelectionListener_vepv._base_epv = & FolderSelectionListener_base_epv;
		FolderSelectionListener_vepv.Evolution_FolderSelectionListener_epv = & FolderSelectionListener_epv;
		
		FolderSelectionListener_vepv_initialized = TRUE;
	}
	servant = g_malloc0 (sizeof (*servant));
	servant->servant.vepv     = &FolderSelectionListener_vepv;
	servant->arg = arg;
	gtk_object_ref (GTK_OBJECT (arg));
	servant->index = index;

	listener_servant = (PortableServer_Servant) servant;

	CORBA_exception_init (&ev);

	POA_Evolution_FolderSelectionListener__init (listener_servant, &ev);
	if (ev._major != CORBA_NO_EXCEPTION) {
		g_free(servant);
		return CORBA_OBJECT_NIL;
	}

	CORBA_free (PortableServer_POA_activate_object (bonobo_poa (), listener_servant, &ev));

	corba_interface = PortableServer_POA_servant_to_reference (bonobo_poa (), listener_servant, &ev);
	if (ev._major != CORBA_NO_EXCEPTION) {
		corba_interface = CORBA_OBJECT_NIL;
	}

	CORBA_exception_free (&ev);
	return corba_interface;
}

guint
filter_arg_folder_get_type (void)
{
	static guint type = 0;
	
	if (!type) {
		GtkTypeInfo type_info = {
			"FilterArgFolder",
			sizeof (FilterArgFolder),
			sizeof (FilterArgFolderClass),
			(GtkClassInitFunc) filter_arg_folder_class_init,
			(GtkObjectInitFunc) filter_arg_folder_init,
			(GtkArgSetFunc) NULL,
			(GtkArgGetFunc) NULL
		};
		
		type = gtk_type_unique (filter_arg_string_get_type (), &type_info);
	}
	
	return type;
}

static void
arg_folder_write_html(FilterArg *argin, GtkHTML *html, GtkHTMLStream *stream)
{
	/* empty */
}

static void
arg_folder_write_text(FilterArg *argin, GString *string)
{
	GList *l;
	char *a;

	l = argin->values;
	if (l == NULL) {
		g_string_append(string, "folder");
	}
	while (l) {
		a = l->data;
		g_string_append(string, a);
		if (l->next) {
			g_string_append(string, ", ");
		}
		l = g_list_next(l);
	}
}

static int
arg_folder_edit_value (FilterArg *arg, int index)
{
	char *def;
	CORBA_Environment ev;

	printf ("folder edit value %d\n", index);
	if (index < 0) {
		def = "";
	} else {
		def = filter_arg_get_value (arg, index);
	}

	CORBA_exception_init (&ev);
	Evolution_Shell_user_select_folder (global_shell_interface,
					    create_listener (arg, index),
					    "Select Folder", def, &ev);

#warning "What do we really want to return here???"
	return 0;
}

static void
arg_folder_edit_values(FilterArg *argin)
{
	/*FilterArgFolder *arg = (FilterArgFolder *)argin;*/
	GList *l;
	char *a, *start, *ptr, *ptrend, *ptrgap;
	char outbuf[128], *outptr; /* FIXME: dont use a bounded buffer! */
	GtkWidget *dialogue;
	GtkWidget *text;
	guint i;

	dialogue = gnome_dialog_new ("Edit addresses", "Ok", "Cancel", NULL);
	text = gtk_text_new (NULL, NULL);
	gtk_object_ref (GTK_OBJECT (text));

	l = argin->values;
	while (l) {
		a = l->data;
		gtk_text_insert(GTK_TEXT (text), NULL, NULL, NULL, a, strlen(a));
		gtk_text_insert(GTK_TEXT (text), NULL, NULL, NULL, "\n", 1);
		l = g_list_next(l);
	}

	gtk_box_pack_start(GTK_BOX (GNOME_DIALOG(dialogue)->vbox), text, TRUE, TRUE, 2);
	gtk_widget_show(text);
	gtk_text_set_editable(GTK_TEXT (text), TRUE);

	gnome_dialog_run_and_close(GNOME_DIALOG (dialogue));

	for (i = 0; i < g_list_length (argin->values); i++)
		g_free (g_list_nth_data (argin->values, i));
	g_list_free (argin->values);
	
	argin->values = NULL;
	ptr = GTK_TEXT(text)->text.ch;
	ptrend = ptr + GTK_TEXT(text)->text_end;
	ptrgap = ptr + GTK_TEXT(text)->gap_position;

	start = ptr;
	outptr = outbuf;
	while (ptr < ptrend) {
		printf("%c", *ptr);
		if (*ptr == '\n') {
			int len = outptr - outbuf;
			char *new;

			printf("(len = %d)", len);

			if (len>0) {
				new = g_malloc(len+1);
				new[len]=0;
				memcpy(new, outbuf, len);
				printf("(appending '%s')", new);
				argin->values = g_list_append(argin->values, new);
			}
			outptr = outbuf;
		} else {
			*outptr++ = *ptr;
		}
		ptr++;
		if (ptr==ptrgap) {
			ptr += GTK_TEXT(text)->gap_size;
		}
	}
	if (outptr > outbuf) {
		int len = outptr-outbuf;
		char *new;

		printf("(lastlen = %d)", len);
		
		new = g_malloc(len+1);
		new[len] = 0;
		memcpy(new, start, len);
		argin->values = g_list_append(argin->values, new);
	}
	printf("\n");
}

static xmlNodePtr
arg_folder_values_get_xml(FilterArg *argin)
{
	xmlNodePtr value;
	/*FilterArgFolder *arg = (FilterArgFolder *)argin;*/
	GList *l;
	char *a;

	value = xmlNewNode(NULL, "optionvalue");
	xmlSetProp(value, "name", argin->name);

	l = argin->values;
	while (l) {
		xmlNodePtr cur;

		a = l->data;

		cur = xmlNewChild(value, NULL, "folder", NULL);
		if (a)
			xmlSetProp(cur, "name", a);
		l = g_list_next(l);
	}

	return value;
}

static void
arg_folder_values_add_xml(FilterArg *arg, xmlNodePtr node)
{
	xmlNodePtr n;

	printf("adding folder values ...\n");

	n = node->childs;
	while (n) {
		if (!strcmp(n->name, "folder")) {
			char *name = xmlGetProp(n, "name");
			if (name) {
				filter_arg_folder_add(arg, name);
				free(name);
			} else
				g_warning("no xml prop 'name' on '%s'\n", n->name);
		} else {
			g_warning("Loading folders from xml, wrong node encountered: %s\n", n->name);
		}
		n = n->next;
	}
}

static char *
arg_folder_get_value_as_string(FilterArg *argin, void *data)
{
	/*FilterArgFolder *arg = (FilterArgFolder *)argin;*/
	char *a = (char *)data;

	return a;
}

static void
arg_folder_free_value(FilterArg *arg, void *a)
{
	g_free(a);
}

static void
filter_arg_folder_class_init (FilterArgFolderClass *class)
{
	GtkObjectClass *object_class;
	FilterArgClass *filter_class;

	object_class = (GtkObjectClass *) class;
	filter_class = (FilterArgClass *) class;
	if (folder_parent_class == NULL)
		folder_parent_class = gtk_type_class (filter_arg_string_get_type ());

	/* FIXME: only need to over-ride the edit values right? */
	filter_class->edit_value = arg_folder_edit_value;

	filter_class->write_html = arg_folder_write_html;
	filter_class->write_text = arg_folder_write_text;
	filter_class->edit_values = arg_folder_edit_values;
	filter_class->free_value = arg_folder_free_value;

	filter_class->values_get_xml = arg_folder_values_get_xml;
	filter_class->values_add_xml = arg_folder_values_add_xml;
}

static void
filter_arg_folder_init (FilterArgFolder *arg)
{
}

/**
 * filter_arg_folder_new:
 *
 * Create a new FilterArgFolder widget.
 * 
 * Return value: A new FilterArgFolder widget.
 **/
FilterArg *
filter_arg_folder_new (char *name)
{
	FilterArg *a = FILTER_ARG ( gtk_type_new (filter_arg_folder_get_type ()));
	a->name = g_strdup(name);
	return a;
}


void
filter_arg_folder_add(FilterArg *arg, char *name)
{
	filter_arg_add(arg, g_strdup(name));
}

void
filter_arg_folder_remove(FilterArg *arg, char *name)
{
	/* do it */
}

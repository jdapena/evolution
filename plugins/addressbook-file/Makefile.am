@EVO_PLUGIN_RULE@

plugin_DATA = org-gnome-addressbook-file.eplug

plugin_LTLIBRARIES = liborg-gnome-addressbook-file.la

liborg_gnome_addressbook_file_la_CPPFLAGS =	\
	$(AM_CPPFLAGS)				\
	-I$(top_srcdir)				\
	$(EVOLUTION_DATA_SERVER_CFLAGS)		\
	$(GNOME_PLATFORM_CFLAGS)

liborg_gnome_addressbook_file_la_SOURCES = addressbook-file.c

liborg_gnome_addressbook_file_la_LDFLAGS = -module -avoid-version $(NO_UNDEFINED)

liborg_gnome_addressbook_file_la_LIBADD =	\
	$(EVOLUTION_DATA_SERVER_LIBS)		\
	$(GNOME_PLATFORM_LIBS)

EXTRA_DIST = org-gnome-addressbook-file.eplug.xml

BUILT_SOURCES = $(plugin_DATA)
CLEANFILES = $(BUILT_SOURCES)

-include $(top_srcdir)/git.mk

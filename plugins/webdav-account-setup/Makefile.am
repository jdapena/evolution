@EVO_PLUGIN_RULE@

plugin_DATA = org-gnome-evolution-webdav.eplug

plugin_LTLIBRARIES = liborg-gnome-evolution-webdav.la

liborg_gnome_evolution_webdav_la_CPPFLAGS = 	\
	$(AM_CPPFLAGS)				\
	-I .					\
	-I$(top_srcdir)				\
	$(EVOLUTION_DATA_SERVER_CFLAGS)		\
	$(GNOME_PLATFORM_CFLAGS)

liborg_gnome_evolution_webdav_la_SOURCES = \
	webdav-contacts-source.c

liborg_gnome_evolution_webdav_la_LIBADD =	\
	$(top_builddir)/e-util/libeutil.la	\
	$(EVOLUTION_DATA_SERVER_LIBS)		\
	$(GNOME_PLATFORM_LIBS)

liborg_gnome_evolution_webdav_la_LDFLAGS = -module -avoid-version $(NO_UNDEFINED)

EXTRA_DIST = \
	org-gnome-evolution-webdav.eplug.xml

CLEANFILES = org-gnome-evolution-webdav.eplug

-include $(top_srcdir)/git.mk

privsolib_LTLIBRARIES = libevolution-calendar-importers.la

libevolution_calendar_importers_la_CPPFLAGS = 		\
	$(AM_CPPFLAGS)					\
	-DEVOLUTION_SOUNDDIR=\""$(soundsdir)"\"		\
	-DG_LOG_DOMAIN=\"Evolution-Importer\"		\
	-I$(top_srcdir)					\
	-I$(top_srcdir)/calendar			\
	-I$(top_srcdir)/widgets				\
	-I$(top_builddir)/calendar			\
	$(EVOLUTION_DATA_SERVER_CFLAGS)			\
	$(GNOME_PLATFORM_CFLAGS)			\
	$(GTKHTML_CFLAGS)

libevolution_calendar_importers_la_SOURCES = \
	evolution-calendar-importer.h	     \
	icalendar-importer.c

libevolution_calendar_importers_la_LDFLAGS = $(NO_UNDEFINED)

libevolution_calendar_importers_la_LIBADD =			\
	$(top_builddir)/e-util/libeutil.la 			\
	$(top_builddir)/shell/libeshell.la			\
	$(top_builddir)/widgets/misc/libemiscwidgets.la		\
	$(EVOLUTION_DATA_SERVER_LIBS)				\
	$(GNOME_PLATFORM_LIBS)					\
	$(GTKHTML_LIBS)

-include $(top_srcdir)/git.mk

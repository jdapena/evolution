eds_datadir = `pkg-config --variable=privdatadir evolution-data-server-1.2`

@EVO_PLUGIN_RULE@

plugin_DATA = org-gnome-calendar-weather.eplug
plugin_LTLIBRARIES = liborg-gnome-calendar-weather.la

liborg_gnome_calendar_weather_la_CPPFLAGS =			\
	$(AM_CPPFLAGS)						\
	-I$(top_srcdir)						\
	-I$(top_srcdir)/widgets					\
	-DWEATHER_DATADIR=\""$(weatherdatadir)"\"		\
	-DWEATHER_EDS_DATADIR=\""$(eds_datadir)/weather"\"	\
	$(EVOLUTION_DATA_SERVER_CFLAGS)				\
	$(GNOME_PLATFORM_CFLAGS)				\
	$(GWEATHER_CFLAGS)

liborg_gnome_calendar_weather_la_SOURCES = calendar-weather.c
liborg_gnome_calendar_weather_la_LDFLAGS = -module -avoid-version $(NO_UNDEFINED)
liborg_gnome_calendar_weather_la_LIBADD =			\
	$(top_builddir)/calendar/gui/libevolution-calendar.la	\
	$(top_builddir)/e-util/libeutil.la			\
	$(EVOLUTION_DATA_SERVER_LIBS)				\
	$(GNOME_PLATFORM_LIBS)					\
	$(GWEATHER_LIBS)

EXTRA_DIST =					\
	org-gnome-calendar-weather.eplug.xml	\
	$(weatherdata_DATA)

BUILT_SOURCES = $(plugin_DATA)
CLEANFILES = $(BUILT_SOURCES)

-include $(top_srcdir)/git.mk

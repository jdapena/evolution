if OS_WIN32
NO_UNDEFINED_REQUIRED_LIBS =				\
	$(top_builddir)/mail/libevolution-mail.la	\
	$(top_builddir)/e-util/libeutil.la		\
	$(GNOME_PLATFORM_LIBS)
endif

@EVO_PLUGIN_RULE@

plugin_DATA = org-gnome-audio-inline.eplug

plugin_LTLIBRARIES = liborg-gnome-audio-inline.la

liborg_gnome_audio_inline_la_CPPFLAGS =			\
	$(AM_CPPFLAGS)					\
	-I$(top_srcdir)					\
	-I$(top_srcdir)/widgets				\
	$(EVOLUTION_DATA_SERVER_CFLAGS)			\
	$(GNOME_PLATFORM_CFLAGS)			\
	$(GSTREAMER_CFLAGS)				\
	$(GTKHTML_CFLAGS)

liborg_gnome_audio_inline_la_SOURCES = audio-inline.c

liborg_gnome_audio_inline_la_LDFLAGS = -module -avoid-version $(NO_UNDEFINED)

liborg_gnome_audio_inline_la_LIBADD =			\
	$(top_builddir)/mail/libevolution-mail.la	\
	$(top_builddir)/e-util/libeutil.la		\
	$(top_builddir)/em-format/libemformat.la	\
	$(EVOLUTION_DATA_SERVER_LIBS)			\
	$(GNOME_PLATFORM_LIBS)				\
	$(GSTREAMER_LIBS)				\
	$(GTKHTML_LIBS)

EXTRA_DIST = org-gnome-audio-inline.eplug.xml

BUILT_SOURCES = $(plugin_DATA)
CLEANFILES = $(BUILT_SOURCES)

-include $(top_srcdir)/git.mk

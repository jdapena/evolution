@EVO_PLUGIN_RULE@

plugin_DATA = org-gnome-mail-to-task.eplug

plugin_LTLIBRARIES = liborg-gnome-mail-to-task.la

liborg_gnome_mail_to_task_la_CPPFLAGS =			\
	$(AM_CPPFLAGS)					\
	-I$(top_srcdir)					\
	-I$(top_srcdir)/widgets				\
	$(EVOLUTION_DATA_SERVER_CFLAGS)			\
	$(GNOME_PLATFORM_CFLAGS)			\
	$(GTKHTML_CFLAGS)

liborg_gnome_mail_to_task_la_SOURCES = mail-to-task.c

liborg_gnome_mail_to_task_la_LDFLAGS = -module -avoid-version $(NO_UNDEFINED)

liborg_gnome_mail_to_task_la_LIBADD =	\
	$(top_builddir)/e-util/libeutil.la				\
	$(top_builddir)/shell/libeshell.la				\
	$(top_builddir)/em-format/libemformat.la			\
	$(top_builddir)/calendar/gui/libevolution-calendar.la		\
	$(top_builddir)/mail/libevolution-mail.la			\
	$(top_builddir)/widgets/misc/libemiscwidgets.la			\
	$(EVOLUTION_DATA_SERVER_LIBS)					\
	$(GNOME_PLATFORM_LIBS)						\
	$(GTKHTML_LIBS)

EXTRA_DIST = org-gnome-mail-to-task.eplug.xml

BUILT_SOURCES = org-gnome-mail-to-task.eplug

CLEANFILES = $(BUILT_SOURCES)

-include $(top_srcdir)/git.mk

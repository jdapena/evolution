@EVO_PLUGIN_RULE@

plugin_DATA = org-gnome-face.eplug

plugin_LTLIBRARIES = liborg-gnome-face.la

liborg_gnome_face_la_CPPFLAGS =				\
	$(AM_CPPFLAGS)					\
	-I. 						\
	-I$(top_srcdir)					\
	-I$(top_srcdir)/widgets				\
	-I$(top_srcdir)/widgets/misc			\
	-I$(top_builddir)/mail				\
	-I$(top_builddir)/composer			\
	$(EVOLUTION_DATA_SERVER_CFLAGS)			\
	$(GNOME_PLATFORM_CFLAGS)			\
	$(GTKHTML_CFLAGS)

liborg_gnome_face_la_SOURCES = face.c

liborg_gnome_face_la_LIBADD = 					\
	$(top_builddir)/e-util/libeutil.la 			\
	$(top_builddir)/composer/libcomposer.la 		\
	$(top_builddir)/widgets/misc/libemiscwidgets.la		\
	$(top_builddir)/mail/libevolution-mail.la 		\
	$(EVOLUTION_DATA_SERVER_LIBS)				\
	$(GNOME_PLATFORM_LIBS)					\
	$(GTKHTML_LIBS)

liborg_gnome_face_la_LDFLAGS = -module -avoid-version $(NO_UNDEFINED)

error_DATA = org-gnome-face.error

errordir = $(privdatadir)/errors

# GConf schemas

schemadir   = $(GCONF_SCHEMA_FILE_DIR)
schema_in_files = apps_evolution_eplugin_face.schemas.in
schema_DATA = $(schema_in_files:.schemas.in=.schemas)

@INTLTOOL_SCHEMAS_RULE@

EXTRA_DIST = 				\
	org-gnome-face.eplug.xml	\
	org-gnome-face.error.xml	\
	$(schema_in_files)

BUILT_SOURCES = 			\
	org-gnome-face.eplug 		\
	org-gnome-face.error

CLEANFILES = $(BUILT_SOURCES)

DISTCLEANFILES = $(schema_DATA)

if GCONF_SCHEMAS_INSTALL

if OS_WIN32
install-data-local:
	if test -z "$(DESTDIR)" ; then							\
		for p in $(schema_DATA) ; do						\
			(echo set GCONF_CONFIG_SOURCE=$(GCONF_SCHEMA_CONFIG_SOURCE);	\
			 echo $(GCONFTOOL) --makefile-install-rule $$p) >_temp.bat;	\
			cmd /c _temp.bat; 						\
			rm _temp.bat;							\
		done									\
	fi
else
install-data-local:
	if test -z "$(DESTDIR)" ; then							\
		for p in $(schema_DATA) ; do						\
			GCONF_CONFIG_SOURCE=$(GCONF_SCHEMA_CONFIG_SOURCE) 		\
				$(GCONFTOOL) --makefile-install-rule $$p;		\
		done									\
	fi
endif

endif

-include $(top_srcdir)/git.mk

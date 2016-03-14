bin_PROGRAMS = piclone

piclone_CFLAGS = \
	-I$(top_srcdir) \
	-DPACKAGE_LIB_DIR=\""$(libdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)/piclone"\" \
	-DPACKAGE_UI_DIR=\""$(datadir)/piclone/ui"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	$(PACKAGE_CFLAGS) \
	$(G_CAST_CHECKS)

piclone_SOURCES = piclone.c

piclone_includedir = $(includedir)/piclone

piclone_include_HEADERS =

piclone_LDFLAGS = $(DYNAMIC_FLAGS)
piclone_DEPENDENCIES_EXTRA = $(BUILTIN_PLUGINS)

piclone_LDADD = \
		$(BUILTIN_PLUGINS) \
		$(PACKAGE_LIBS) \
		$(X11_LIBS) \
		$(INTLLIBS)

EXTRA_DIST = \
		backup

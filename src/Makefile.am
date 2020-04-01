bin_PROGRAMS = rp-bookshelf

rp_bookshelf_CFLAGS = \
	-I$(top_srcdir) \
	-DPACKAGE_LIB_DIR=\""$(libdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)/rp-bookshelf"\" \
	-DPACKAGE_UI_DIR=\""$(datadir)/rp-bookshelf/ui"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	$(PACKAGE_CFLAGS) \
	$(G_CAST_CHECKS)

rp_bookshelf_SOURCES = rp_bookshelf.c

rp_bookshelf_includedir = $(includedir)/rp-bookshelf

rp_bookshelf_include_HEADERS =

rp_bookshelf_LDFLAGS = $(DYNAMIC_FLAGS)
rp_bookshelf_DEPENDENCIES_EXTRA = $(BUILTIN_PLUGINS)

rp_bookshelf_LDADD = \
		$(BUILTIN_PLUGINS) \
		$(PACKAGE_LIBS) \
		$(X11_LIBS) \
		$(INTLLIBS)

EXTRA_DIST =

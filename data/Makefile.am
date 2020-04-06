uidir = $(datadir)/rp-bookshelf

ui_in_files = rp_bookshelf.ui \
				nocover.png \
				cloud.png \
				grey.png \
				new.png \
				cat.xml
	
ui_DATA = $(ui_in_files)

desktopdir=$(datadir)/applications

desktop_in_files= \
	rp-bookshelf.desktop.in \
	$(NULL)
desktop_DATA = $(desktop_in_files:.desktop.in=.desktop)
@INTLTOOL_DESKTOP_RULE@

EXTRA_DIST = $(ui_in_files) \
			$(desktop_in_files) \
			$(desktop_DATA) \
			$(NULL)

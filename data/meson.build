data = files (
    'rp_bookshelf.ui',
    'cloud.png',
    'grey.png',
    'new.png',
    'nocover.png'
)

install_data(data, install_dir: resource_dir)
install_subdir('icons', install_dir: share_dir)
i18n.merge_file(input: 'rp-bookshelf.desktop.in',
      output: 'rp-bookshelf.desktop',
      type: 'desktop',
      po_dir: '../po',
      install: true,
      install_dir: desktop_dir
)

gnome = import ('gnome')
gnome.post_install (gtk_update_icon_cache : true)

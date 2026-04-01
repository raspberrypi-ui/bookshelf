sources = files (
    'rp_bookshelf.c',
    'activate.c'
)

gtk = dependency ('gtk+-3.0')
curl = dependency ('libcurl')
deps = [ deps, gtk, curl ]

executable (meson.project_name(), sources, dependencies: deps, install: true)

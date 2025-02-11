sources = files (
    'rp_bookshelf.c'
)

add_global_arguments('-Wno-unused-result', language : 'c')

gtk = dependency ('gtk+-3.0')
curl = dependency ('libcurl')
deps = [ gtk, curl ]

executable (meson.project_name(), sources, dependencies: deps, install: true)

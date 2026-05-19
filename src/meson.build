sources = files (
    'piclone.c',
    'activate.c'
)

gtk = dependency ('gtk+-3.0')

deps = [ deps, gtk ]

executable (meson.project_name(), sources, dependencies: deps, install: true,
  c_args : [ '-DPACKAGE_DATA_DIR="' + resource_dir + '"', '-DGETTEXT_PACKAGE="' + meson.project_name() + '"' ]
)

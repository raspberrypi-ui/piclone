project ('piclone','c')

share_dir = join_paths(get_option('prefix'), 'share')
bin_dir = join_paths(get_option('prefix'), 'bin')
resource_dir = join_paths(share_dir, meson.project_name())
desktop_dir = join_paths(share_dir, 'applications')

i18n = import('i18n')

add_project_arguments('-DPACKAGE_LOCALE_DIR="' + share_dir + '/locale"', language : 'c' )
add_global_arguments('-Wno-unused-result', language : 'c')

wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols')
subdir('proto')
deps = [ wf_protos ]

subdir('po')
subdir('src')
subdir('data')

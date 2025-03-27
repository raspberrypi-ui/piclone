project ('piclone','c')

share_dir = join_paths(get_option('prefix'), 'share')
bin_dir = join_paths(get_option('prefix'), 'bin')
resource_dir = join_paths(share_dir, meson.project_name())
desktop_dir = join_paths(share_dir, 'applications')

i18n = import('i18n')

add_project_arguments('-DPACKAGE_LOCALE_DIR="' + share_dir + '/locale"', language : 'c' )

subdir('po')
subdir('src')
subdir('data')

project(
  'xfce-openai-panel-plugin',
  'c',
  version: '0.1.0',
  meson_version: '>=0.59.0',
  default_options: ['warning_level=2', 'c_std=c11'],
)

prefix = get_option('prefix')
libdir = join_paths(prefix, get_option('libdir'))
datadir = join_paths(prefix, get_option('datadir'))

xfce_panel_plugindir = join_paths(libdir, 'xfce4', 'panel', 'plugins')
xfce_panel_desktopdir = join_paths(datadir, 'xfce4', 'panel', 'plugins')

subdir('src')
subdir('data')

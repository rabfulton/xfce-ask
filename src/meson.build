plugin_sources = files(
  'openai-ask-plugin.c',
  'openai-client.c',
  'markdown-pango.c',
  'keyring.c',
  'log.c',
)

deps = [
  dependency('gtk+-3.0'),
  dependency('libxfce4panel-2.0'),
  dependency('libsoup-3.0'),
  dependency('json-glib-1.0'),
  dependency('libsecret-1'),
]

shared_module(
  'openai-ask',
  plugin_sources,
  dependencies: deps,
  install: true,
  install_dir: xfce_panel_plugindir,
)

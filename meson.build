project('youtube-chat-client', 'c',
        default_options: ['c_std=c99', 'warning_level=2'],
        meson_version: '>=1.1.0')

if get_option('log-http')
  add_project_arguments('-DYOUTUBE_CHAT_CLIENT_LOGGING', language: 'c')
endif

gobject = dependency('gobject-2.0', required: true)
libsoup = dependency('libsoup-3.0', required: true)
json = dependency('json-glib-1.0', required: true)
librest = dependency('rest-1.0', required: true)
gio = dependency('gio-2.0', required: true)

executable('yt-chat', ['src/main.c', 'src/youtube_chat_client.c', 'src/youtube_chat_parser.c'],
    dependencies: [gobject, libsoup, json, librest, gio]
)

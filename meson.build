project('youtube-chat-client', 'c',
        default_options: ['c_std=c99', 'warning_level=2'])

gobject = dependency('gobject-2.0', required: true)
libsoup = dependency('libsoup-3.0', required: true)
json = dependency('json-glib-1.0', required: true)

executable('yt-chat', 'main.c',
    dependencies: [gobject, libsoup, json]
)

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define YOUTUBE_CHAT_ERROR youtube_chat_error_quark()
GQuark youtube_chat_error_quark(void);

#define YOUTUBE_TYPE_CHAT_CLIENT youtube_chat_client_get_type()
G_DECLARE_FINAL_TYPE(YoutubeChatClient, youtube_chat_client, YOUTUBE, CHAT_CLIENT, GObject)

typedef struct {
    char* display_name;
    GDateTime* timestamp;
    char* content;
} YoutubeChatMessage;

YoutubeChatClient* youtube_chat_client_new(const char* api_key);

void youtube_chat_client_connect_async(YoutubeChatClient* client, const char* stream_url,
                                       GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data);
void youtube_chat_client_connect_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error);

G_END_DECLS

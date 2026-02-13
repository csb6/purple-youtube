#pragma once

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define YOUTUBE_TYPE_CHAT_CLIENT youtube_chat_client_get_type()
G_DECLARE_FINAL_TYPE(YoutubeChatClient, youtube_chat_client, YOUTUBE, CHAT_CLIENT, GObject)

YoutubeChatClient* youtube_chat_client_new(const char* api_key);

void youtube_chat_client_get_channel_id_async(YoutubeChatClient* client, const char* handle,
                                              GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data);
char* youtube_chat_client_get_channel_id_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error);

void youtube_chat_client_get_live_streams_async(YoutubeChatClient* client, char* channel_id,
                                                GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data);
GPtrArray* youtube_chat_client_get_live_streams_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error);

G_END_DECLS

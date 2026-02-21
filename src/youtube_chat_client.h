/*
purple-youtube
Copyright (C) 2026 Cole Blakley

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once

#include <gio/gio.h>
#include <glib-object.h>
#include "youtube_types.h"

G_BEGIN_DECLS

typedef void (*YoutubeChatClientErrorCallback)(GError* error, gpointer data);

#define YOUTUBE_TYPE_CHAT_CLIENT youtube_chat_client_get_type()
G_DECLARE_FINAL_TYPE(YoutubeChatClient, youtube_chat_client, YOUTUBE, CHAT_CLIENT, GObject)

YoutubeChatClient* youtube_chat_client_new(const char* api_key);

void youtube_chat_client_set_error_callback(YoutubeChatClient* client,
                                            YoutubeChatClientErrorCallback callback, gpointer data);

void youtube_chat_client_connect_async(YoutubeChatClient* client, const char* stream_url,
                                       GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data);
void youtube_chat_client_connect_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error);

G_END_DECLS

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
#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>
#include "youtube_chat_client.h"

static
void on_connect(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    YoutubeChatClient* client = (YoutubeChatClient*)source_object;
    GMainLoop* main_loop = data;
    youtube_chat_client_connect_finish(client, result, &error);
    if(error) {
        g_printerr("Failed to connect to live stream: %s\n", error->message);
        g_clear_error(&error);
        g_main_loop_quit(main_loop);
    }
}

static
void on_new_messages(YoutubeChatClient* client, GPtrArray* messages, gpointer data)
{
    for(guint i = 0; i < messages->len; ++i) {
        YoutubeChatMessage* msg = messages->pdata[i];
        GDateTime* local_timestamp = g_date_time_to_local(msg->timestamp);
        char* timestamp_str = g_date_time_format(local_timestamp, "%I:%M:%S %p");
        g_print("%s (%s): %s\n\n", msg->display_name, timestamp_str, msg->content);
        g_free(timestamp_str);
        g_date_time_unref(local_timestamp);
    }
}

int main(int argc, char** argv)
{
    if(argc != 2) {
        g_printerr("Usage: %s stream_url\n", argv[0]);
        return 1;
    }
    gchar* api_key = (gchar*)g_getenv("YT_API_KEY");
    if(!api_key) {
        g_printerr("Environment variable YT_API_KEY must be set to a YouTube Data API key\n");
        return 1;
    }
    api_key = g_strdup(api_key);
    const char* stream_url = argv[1];

    GMainLoop* main_loop = g_main_loop_new(g_main_context_default(), /*is_running=*/FALSE);

    YoutubeChatClient* client = youtube_chat_client_new(api_key);
    g_signal_connect(client, "new-messages", G_CALLBACK(on_new_messages), NULL);
    youtube_chat_client_connect_async(client, stream_url, NULL, on_connect, main_loop);

    g_main_loop_run(main_loop);
    g_main_loop_unref(main_loop);
    g_object_unref(client);
    g_free(api_key);

    return 0;
}

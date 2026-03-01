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
#include <stdlib.h>
#include "youtube_chat_client.h"

static
void on_auth_url_generated(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    YoutubeChatClient* client = YOUTUBE_CHAT_CLIENT(source_object);
    GMainLoop* main_loop = data;
    char* auth_url = youtube_chat_client_generate_auth_url_finish(client, result, &error);
    if(error) {
        g_printerr("Failed to get OAuth authorization URL: %s\n", error->message);
        g_clear_error(&error);
        g_main_loop_quit(main_loop);
    } else {
        char* cmd = g_strdup_printf("xdg-open \"%s\"", auth_url);
        system(cmd);
        g_free(cmd);
        g_free(auth_url);
    }
}

static
void on_connect(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    YoutubeChatClient* client = YOUTUBE_CHAT_CLIENT(source_object);
    GMainLoop* main_loop = data;
    youtube_chat_client_connect_to_chat_finish(client, result, &error);
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

typedef struct {
    GMainLoop* main_loop;
    const char* stream_url;
} ConnectData;

static
void on_authorized(YoutubeChatClient* client, gboolean is_authorized, gpointer data)
{
    ConnectData* connect_data = data;
    if(is_authorized) {
        youtube_chat_client_connect_to_chat_async(client, connect_data->stream_url, NULL,
                                                  on_connect, connect_data->main_loop);
    } else {
        // Unreachable currently (no notification sent on auth error)
        g_printerr("Failed to authorize\n");
        g_main_loop_quit(connect_data->main_loop);
    }
    g_free(connect_data);
}

int main(int argc, char** argv)
{
    int status = 0;
    GMainLoop* main_loop = NULL;
    YoutubeChatClient* client = NULL;
    if(argc != 2) {
        g_printerr("Usage: %s stream_url\n", argv[0]);
        return 1;
    }
    const char* stream_url = argv[1];
    char** env = g_get_environ();
    const char* client_id = g_environ_getenv(env, "YT_CLIENT_ID");
    if(!client_id) {
        g_printerr("Missing environment variable YT_CLIENT_ID");
        status = 1;
        goto cleanup;
    }
    const char* client_secret = g_environ_getenv(env, "YT_CLIENT_SECRET");
    if(!client_secret) {
        g_printerr("Missing environment variable YT_CLIENT_SECRET");
        status = 1;
        goto cleanup;
    }
    main_loop = g_main_loop_new(g_main_context_default(), /*is_running=*/FALSE);
    client = youtube_chat_client_new(client_id, client_secret);
    g_signal_connect(client, "new-messages", G_CALLBACK(on_new_messages), NULL);
    ConnectData* connect_data = g_new(ConnectData, 1);
    connect_data->main_loop = main_loop;
    connect_data->stream_url = stream_url;
    g_signal_connect(client, "notify::is-authorized", G_CALLBACK(on_authorized), connect_data);
    youtube_chat_client_generate_auth_url_async(client, NULL, on_auth_url_generated, NULL);
    g_main_loop_run(main_loop);
cleanup:
    if(main_loop) g_main_loop_unref(main_loop);
    g_object_unref(client);
    g_strfreev(env);
    return status;
}

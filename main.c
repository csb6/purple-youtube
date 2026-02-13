#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>
#include "youtube_chat_client.h"

static
void on_live_streams(GObject* source_object, GAsyncResult* result, gpointer data);

static
void on_channel_id(GObject* source_object, GAsyncResult* result, gpointer data)
{
    YoutubeChatClient* client = (YoutubeChatClient*)source_object;
    GError* error = NULL;
    char* channel_id = youtube_chat_client_get_channel_id_finish(client, result, &error);
    if(error) {
        g_printerr("Failed to get channel ID\n");
        return;
    }
    g_printf("Channel ID: %s\n", channel_id);

    youtube_chat_client_get_live_streams_async(client, g_strdup(channel_id), NULL, on_live_streams, data);
    g_free(channel_id);
}

static
void on_live_streams(GObject* source_object, GAsyncResult* result, gpointer data)
{
    YoutubeChatClient* client = (YoutubeChatClient*)source_object;
    GError* error = NULL;
    g_printf("Active live streams:\n");
    GPtrArray* live_streams = youtube_chat_client_get_live_streams_finish(client, result, &error);
    if(error) {
        g_printerr("Failed to get live streams\n");
        return;
    }
    guint live_stream_count = live_streams->len;
    for(guint i = 0; i < live_stream_count; ++i) {
        g_printf("%s\n", (const char*)live_streams->pdata[i]);
    }

    for(guint i = 0; i < live_stream_count; ++i) {
        g_free(live_streams->pdata[i]);
    }
    g_ptr_array_free(live_streams, TRUE);
    g_main_loop_quit((GMainLoop*)data);
}

int main(int argc, char** argv)
{
    if(argc != 2) {
        g_printerr("Usage: %s channel_handle\n", argv[0]);
        return 1;
    }
    gchar* api_key = (gchar*)g_getenv("YT_API_KEY");
    if(!api_key) {
        g_printerr("Environment variable YT_API_KEY must be set to a YouTube Data API key\n");
        return 1;
    }
    api_key = g_strdup(api_key);
    const char* channel_handle = argv[1];

    GMainLoop* main_loop = g_main_loop_new(g_main_context_default(), /*is_running=*/FALSE);

    YoutubeChatClient* client = youtube_chat_client_new(api_key);
    youtube_chat_client_get_channel_id_async(client, channel_handle, NULL, on_channel_id, main_loop);

    g_main_loop_run(main_loop);
    g_main_loop_unref(main_loop);
    g_object_unref(client);
    g_free(api_key);

    return 0;
}

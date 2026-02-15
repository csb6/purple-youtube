#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>
#include "youtube_chat_client.h"

static
void on_error(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    YoutubeChatClient* client = (YoutubeChatClient*)source_object;
    GMainLoop* main_loop = data;
    youtube_chat_client_connect_finish(client, result, &error);
    if(error) {
        g_printerr("Request failed: %s\n", error->message);
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
    youtube_chat_client_connect_async(client, stream_url, NULL, on_error, main_loop);

    g_main_loop_run(main_loop);
    g_main_loop_unref(main_loop);
    g_object_unref(client);
    g_free(api_key);

    return 0;
}

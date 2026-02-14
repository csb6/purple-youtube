#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>
#include "youtube_chat_client.h"

static
void on_finish(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    YoutubeChatClient* client = (YoutubeChatClient*)source_object;
    GMainLoop* main_loop = data;
    youtube_chat_client_connect_finish(client, result, &error);
    if(error) {
        g_printerr("Request failed: %s\n", error->message);
    }
    g_main_loop_quit(main_loop);
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
    youtube_chat_client_connect_async(client, stream_url, NULL, on_finish, main_loop);

    g_main_loop_run(main_loop);
    g_main_loop_unref(main_loop);
    g_object_unref(client);
    g_free(api_key);

    return 0;
}

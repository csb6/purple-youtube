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
#include "youtube_chat_client.h"
#include <string.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <rest/rest.h>
#include <libsoup/soup.h>
#include "youtube_chat_parser.h"

// References:
// - https://gist.github.com/w3cj/4f1fa02b26303ae1e0b1660f2349e705
// - https://developers.google.com/youtube/v3/live/docs/liveChatMessages

// TODO: switch to using streamList. This avoids polling (unless server disconnects us)
//  by keeping socket open and listening, but means data is returned in unpredictable chunks
//  that need to be read in as they are available. Use rest_proxy_call_continuous()

#define YOUTUBE_API_BASE_URL "https://www.googleapis.com/youtube/v3/"

typedef struct {
    YoutubeChatClient* client;
    char* next_page_token;
    guint poll_interval;
} FetchData;

struct _YoutubeChatClient {
    GObject parent_instance;
    RestProxy* proxy;
    char* api_key;
    YoutubeStreamInfo* stream_info;
};
G_DEFINE_TYPE(YoutubeChatClient, youtube_chat_client, G_TYPE_OBJECT)
//G_DEFINE_DYNAMIC_TYPE_EXTENDED(YoutubeChatClient, youtube_chat_client, G_TYPE_OBJECT,
//                               G_TYPE_FLAG_FINAL, {})
G_DEFINE_QUARK(youtube-chat-error-quark, youtube_chat_error)

enum {
    SIG_NEW_MESSAGES,
    N_SIGNALS
};
static guint signals[N_SIGNALS] = {0};

static
char* extract_video_id(const char* stream_url, GError** error);

static
void free_stream_info(gpointer data);


static
void youtube_chat_client_init(YoutubeChatClient* client)
{
    client->proxy = rest_proxy_new(YOUTUBE_API_BASE_URL, /*binding_required=*/FALSE);
    #ifdef YOUTUBE_CHAT_CLIENT_LOGGING
    SoupLogger* logger = soup_logger_new(SOUP_LOGGER_LOG_HEADERS);
    rest_proxy_add_soup_feature(client->proxy, SOUP_SESSION_FEATURE(logger));
    g_object_unref(logger);
    #endif
}

static
void youtube_chat_client_finalize(GObject* obj)
{
    YoutubeChatClient* client = YOUTUBE_CHAT_CLIENT(obj);
    g_clear_object(&client->proxy);
    g_free(client->api_key);
    free_stream_info(client->stream_info);
    G_OBJECT_CLASS(youtube_chat_client_parent_class)->finalize(obj);
}

//static
//void youtube_chat_client_class_finalize(YoutubeChatClientClass* klass)
//{}

static
void youtube_chat_client_class_init(YoutubeChatClientClass* klass)
{
    GObjectClass* obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = youtube_chat_client_finalize;

    GType param_types[] = { G_TYPE_PTR_ARRAY };
    signals[SIG_NEW_MESSAGES] = g_signal_newv(
        "new-messages",
        G_OBJECT_CLASS_TYPE(klass),
        G_SIGNAL_RUN_LAST,
        NULL, NULL, NULL, NULL, G_TYPE_NONE,
        /*n_params=*/1,
        param_types
    );
}

YoutubeChatClient* youtube_chat_client_new(const char* api_key)
{
    YoutubeChatClient* client = g_object_new(YOUTUBE_TYPE_CHAT_CLIENT, NULL);
    client->api_key = g_strdup(api_key);
    return client;
}

static
void free_stream_info(gpointer data)
{
    if(data) {
        YoutubeStreamInfo* stream_info = data;
        g_free(stream_info->title);
        g_free(stream_info->live_chat_id);
        g_free(stream_info);
    }
}

static
void get_live_stream_info_1(GObject* source_object, GAsyncResult* result, gpointer data);

static
void get_live_stream_info_async(YoutubeChatClient* client, const char* video_id,
                                GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    GTask* task = g_task_new(client, cancellable, callback, data);
    RestProxyCall* call = rest_proxy_new_call(client->proxy);
    rest_proxy_call_add_header(call, "x-goog-api-key", client->api_key);
    rest_proxy_call_add_params(call,
        "part", "snippet,liveStreamingDetails",
        "fields", "items(snippet(title),liveStreamingDetails(activeLiveChatId))",
        "id", video_id,
        NULL);
    rest_proxy_call_set_function(call, "videos");
    rest_proxy_call_invoke_async(call, cancellable, get_live_stream_info_1, task);
}

static
void get_live_stream_info_1(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    RestProxyCall* call = REST_PROXY_CALL(source_object);
    GTask* task = data;

    rest_proxy_call_invoke_finish(call, result, &error);
    if(error) {
        g_task_return_error(task, error);
        goto cleanup;
    }

    const char* response = rest_proxy_call_get_payload(call);
    gssize response_len = rest_proxy_call_get_payload_length(call);
    YoutubeStreamInfo* stream_info = youtube_parse_stream_info(response, response_len, &error);
    if(error) {
        g_task_return_error(task, error);
    } else {
        g_task_return_pointer(task, stream_info, free_stream_info);
    }
cleanup:
    g_object_unref(task);
    g_object_unref(call);
}

static
YoutubeStreamInfo* get_live_stream_info_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
{
    g_return_val_if_fail(g_task_is_valid(result, client), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

static
void connect_1(GObject* source_object, GAsyncResult* result, gpointer data);

void youtube_chat_client_connect_async(YoutubeChatClient* client, const char* stream_url,
                                       GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    GError* error = NULL;
    GTask* task = g_task_new(client, cancellable, callback, data);
    char* video_id = extract_video_id(stream_url, &error);
    if(error) {
        g_task_return_error(task, error);
    } else {
        get_live_stream_info_async(client, video_id, cancellable, connect_1, task);
        g_free(video_id);
    }
}

static
void fetch_messages_async(YoutubeChatClient* client, FetchData* fetch_data);

static
void connect_1(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    GTask* task = data;
    YoutubeChatClient* client = YOUTUBE_CHAT_CLIENT(source_object);
    YoutubeStreamInfo* stream_info = get_live_stream_info_finish(client, result, &error);
    if(error) {
        g_task_return_error(task, error);
    } else {
        // Cache stream info
        client->stream_info = stream_info;
        FetchData* fetch_data = g_new(FetchData, 1);
        fetch_data->client = client;
        fetch_data->poll_interval = 5000; // Default poll interval
        fetch_messages_async(client, fetch_data);
        g_task_return_pointer(task, NULL, NULL);
    }
    g_object_unref(task);
}

static
void fetch_messages_1(GObject* source_object, GAsyncResult* result, gpointer data);

static
void fetch_messages_thunk(gpointer data);

static
void fetch_messages_async(YoutubeChatClient* client, FetchData* fetch_data)
{
    RestProxyCall* call = rest_proxy_new_call(client->proxy);
    rest_proxy_call_add_header(call, "x-goog-api-key", client->api_key);
    rest_proxy_call_add_params(call,
                               "liveChatId", client->stream_info->live_chat_id,
                               "part", "snippet,authorDetails",
                               "fields", "nextPageToken,pollingIntervalMillis,"
                               "items(id,authorDetails(displayName),snippet(type,publishedAt,displayMessage))",
                               NULL);
    if(fetch_data->next_page_token) {
        // Only request messages we haven't seen before
        rest_proxy_call_add_param(call, "pageToken", fetch_data->next_page_token);
    }
    rest_proxy_call_set_function(call, "liveChat/messages");
    rest_proxy_call_invoke_async(call, NULL, fetch_messages_1, fetch_data);
}

static
void fetch_messages_1(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    RestProxyCall* call = REST_PROXY_CALL(source_object);
    FetchData* fetch_data = data;
    GPtrArray* messages = NULL;
    guint poll_interval = 0;
    char* next_page_token = NULL;

    rest_proxy_call_invoke_finish(call, result, &error);
    if(error) {
        // TODO: implement some kind of retry mechanism then give up
        // Note: will try again using the last known polling interval
        goto cleanup;
    }
    const char* response = rest_proxy_call_get_payload(call);
    gssize response_len = rest_proxy_call_get_payload_length(call);
    messages = youtube_parse_chat_messages(response, response_len, &poll_interval, &next_page_token, &error);
    if(error) {
        goto cleanup;
    }
    fetch_data->poll_interval = poll_interval;
    g_free(fetch_data->next_page_token);
    fetch_data->next_page_token = next_page_token;
    if(messages->len > 0) {
        // Notify all listeners that a new batch of messages has been received
        g_signal_emit(fetch_data->client, signals[SIG_NEW_MESSAGES], 0, messages);
    }
cleanup:
    if(messages) {
        for(guint i = 0; i < messages->len; ++i) {
            g_free(messages->pdata[i]);
        }
        g_ptr_array_unref(messages);
    }
    g_object_unref(call);
    g_timeout_add_once(fetch_data->poll_interval, fetch_messages_thunk, fetch_data);
}

static
void fetch_messages_thunk(gpointer data)
{
    FetchData* fetch_data = data;
    fetch_messages_async(fetch_data->client, fetch_data);
}

void youtube_chat_client_connect_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
{
    g_return_if_fail(g_task_is_valid(result, client));
    g_task_propagate_pointer(G_TASK(result), error);
}

static
char* extract_video_id(const char* stream_url, GError** error)
{
    GHashTable* params = NULL;
    char* video_id = NULL;
    GUri* stream_uri = g_uri_parse(stream_url, 0, error);
    if(*error) {
        goto cleanup;
    }
    const char* query = g_uri_get_query(stream_uri);
    params = g_uri_parse_params(query, strlen(query), "&", 0, error);
    if(*error) {
        goto cleanup;
    }
    const char* video_id_str = g_hash_table_lookup(params, "v");
    if(!video_id_str) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing parameter in video URL");
        goto cleanup;
    }
    video_id = g_strdup(video_id_str);
cleanup:
    g_uri_unref(stream_uri);
    if(params) g_hash_table_unref(params);
    return video_id;
}

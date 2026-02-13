#include "youtube_chat_client.h"
#include <glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

// TODO: https://developers.google.com/youtube/v3/live/docs/liveChatMessages
//  - Seems like will have to poll using pollingIntervalMillis and nextPageToken

#define YOUTUBE_API_BASE_URL "https://www.googleapis.com/youtube/v3/"

#define cnt_of_array(arr) (sizeof(arr) / sizeof(arr[0]))

typedef struct {
    const char* name;
    const char* value;
} Param;

struct _YoutubeChatClient {
    GObject parent_instance;
    SoupSession* session;
    char* api_key;
};
G_DEFINE_TYPE(YoutubeChatClient, youtube_chat_client, G_TYPE_OBJECT)
//G_DEFINE_DYNAMIC_TYPE_EXTENDED(YoutubeChatClient, youtube_chat_client, G_TYPE_OBJECT,
//                               G_TYPE_FLAG_FINAL, {})
G_DEFINE_QUARK(youtube-chat-error-quark, youtube_chat_error)

static
SoupMessage* create_youtube_message(const char* api_key, const char* resource, const Param* params, int param_count);

static
JsonNode* parse_json(GBytes* data, GError** error);

static
JsonArray* match_json_path(JsonNode* root, const char* path);


static
void youtube_chat_client_init(YoutubeChatClient* client)
{
    client->session = soup_session_new();
}

static
void youtube_chat_client_finalize(GObject* obj)
{
    YoutubeChatClient* client = YOUTUBE_CHAT_CLIENT(obj);
    g_clear_object(&client->session);
    g_free(client->api_key);
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

    // TODO: signals/properties
}

YoutubeChatClient* youtube_chat_client_new(const char* api_key)
{
    YoutubeChatClient* client = g_object_new(YOUTUBE_TYPE_CHAT_CLIENT, NULL);
    client->api_key = g_strdup(api_key);
    return client;
}

static
void on_channel_id_response(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    SoupSession* session = SOUP_SESSION(source_object);
    GTask* task = (GTask*)data;
    GBytes* response = soup_session_send_and_read_finish(session, result, &error);
    JsonNode* response_root = NULL;
    JsonArray* results = NULL;

    if(error) {
        g_task_return_error(task, error);
        goto cleanup;
    }

    response_root = parse_json(response, &error);
    if(!response_root) {
        g_task_return_error(task, error);
        goto cleanup;
    }

    results = match_json_path(response_root, "$['items'][0]['id']");
    if(json_array_get_length(results) != 1) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Unexpected channel ID results");
        g_task_return_error(task, error);
        goto cleanup;
    }

    const char* id = json_array_get_string_element(results, 0);
    if(!id) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Unexpected value for channel ID");
        g_task_return_error(task, error);
        goto cleanup;
    }
    char* id_str = g_strdup(id);
    g_task_return_pointer(task, id_str, g_object_unref);
cleanup:
    json_array_unref(results);
    json_node_unref(response_root);
    g_bytes_unref(response);
    g_object_unref(task);
}

// TODO: make cancellable if needed
void youtube_chat_client_get_channel_id_async(YoutubeChatClient* client, const char* handle,
                                              GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    Param params[] = {
        {"part", "id"},
        {"fields", "items/id"},
        {"forHandle", handle}
    };
    SoupMessage* msg = create_youtube_message(client->api_key, "channels", params, cnt_of_array(params));
    GTask* task = g_task_new(client, cancellable, callback, data);
    soup_session_send_and_read_async(client->session, msg, G_PRIORITY_DEFAULT, cancellable, on_channel_id_response, task);
}

char* youtube_chat_client_get_channel_id_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
{
    g_return_val_if_fail(g_task_is_valid(result, client), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

static
void on_live_streams_response(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    SoupSession* session = SOUP_SESSION(source_object);
    GTask* task = (GTask*)data;
    GBytes* response = soup_session_send_and_read_finish(session, result, &error);
    JsonNode* response_root = NULL;
    JsonArray* results = NULL;

    if(error) {
        g_task_return_error(task, error);
        goto cleanup;
    }

    response_root = parse_json(response, &error);
    if(error) {
        g_task_return_error(task, error);
        goto cleanup;
    }
    results = match_json_path(response_root, "$['items'][*]['id']['videoId']");
    GPtrArray* live_streams = g_ptr_array_new();
    guint result_count = json_array_get_length(results);
    for(guint i = 0; i < result_count; ++i) {
        const char* video_id = json_array_get_string_element(results, i);
        if(!video_id) {
            g_warning("Unexpected format for video ID");
            continue;
        }
        g_ptr_array_add(live_streams, g_strdup(video_id));
    }
    g_task_return_pointer(task, live_streams, g_object_unref);
cleanup:
    g_bytes_unref(response);
    json_array_unref(results);
    json_node_unref(response_root);
    g_object_unref(task);
}

void youtube_chat_client_get_live_streams_async(YoutubeChatClient* client, char* channel_id,
                                                GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    Param params[] = {
        {"part", "snippet,id"},
        {"fields","items/id/videoId"},
        {"eventType", "live"},
        {"channelId", channel_id},
        {"order", "date"},
        {"type", "video"}
    };
    SoupMessage* msg = create_youtube_message(client->api_key, "search", params, cnt_of_array(params));
    GTask* task = g_task_new(client, cancellable, callback, data);
    soup_session_send_and_read_async(client->session, msg, G_PRIORITY_DEFAULT, cancellable, on_live_streams_response, task);
    g_free(channel_id);
}

GPtrArray* youtube_chat_client_get_live_streams_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
{
    g_return_val_if_fail(g_task_is_valid(result, client), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

static
SoupMessage* create_youtube_message(const char* api_key, const char* resource, const Param* params, int param_count)
{
    GString* url = g_string_new(YOUTUBE_API_BASE_URL);
    g_string_append(url, resource);
    if(param_count > 0) {
        g_string_append(url, "?");
        for(int i = 0; i < param_count; ++i) {
            g_string_append_uri_escaped(url, params[i].name, NULL, TRUE);
            g_string_append(url, "=");
            g_string_append_uri_escaped(url, params[i].value, NULL, TRUE);
            if(i != param_count - 1) {
                g_string_append(url, "&");
            }
        }
    }
    SoupMessage* request = soup_message_new(SOUP_METHOD_GET, url->str);
    g_string_free(url, TRUE);
    SoupMessageHeaders* headers = soup_message_get_request_headers(request);
    soup_message_headers_append(headers, "x-goog-api-key", api_key);
    return request;
}

static
JsonNode* parse_json(GBytes* data, GError** error)
{
    gsize data_size = 0;
    gconstpointer data_ptr = g_bytes_get_data(data, &data_size);
    JsonParser* parser = json_parser_new_immutable();
    JsonNode* response_root = NULL;

    json_parser_load_from_data(parser, data_ptr, data_size, error);
    if(*error) {
        goto cleanup;
    }

    response_root = json_parser_get_root(parser);
    if(!response_root) {
        goto cleanup;
    }
cleanup:
    json_node_ref(response_root);
    g_object_unref(parser);
    return response_root;
}

static
JsonArray* match_json_path(JsonNode* root, const char* path)
{
    JsonPath* path_obj = json_path_new();
    json_path_compile(path_obj, path, NULL);
    JsonNode* results = json_path_match(path_obj, root);
    JsonArray* results_array = json_node_get_array(results);
    json_array_ref(results_array);
    json_node_unref(results);
    g_clear_object(&path_obj);
    return results_array;
}

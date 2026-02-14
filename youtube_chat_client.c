#include "youtube_chat_client.h"
#include <glib.h>
#include <json-glib/json-glib.h>
#include <rest/rest.h>

// TODO: https://developers.google.com/youtube/v3/live/docs/liveChatMessages
//  - Seems like will have to poll using pollingIntervalMillis and nextPageToken

#define YOUTUBE_API_BASE_URL "https://www.googleapis.com/youtube/v3/"

struct _YoutubeChatClient {
    GObject parent_instance;
    RestProxy* proxy;
    char* api_key;
};
G_DEFINE_TYPE(YoutubeChatClient, youtube_chat_client, G_TYPE_OBJECT)
//G_DEFINE_DYNAMIC_TYPE_EXTENDED(YoutubeChatClient, youtube_chat_client, G_TYPE_OBJECT,
//                               G_TYPE_FLAG_FINAL, {})
G_DEFINE_QUARK(youtube-chat-error-quark, youtube_chat_error)

static
JsonNode* parse_json(const char* data, gssize data_len, GError** error);

static
JsonArray* match_json_path(JsonNode* root, const char* path);


static
void youtube_chat_client_init(YoutubeChatClient* client)
{
    client->proxy = rest_proxy_new(YOUTUBE_API_BASE_URL, /*binding_required=*/FALSE);
}

static
void youtube_chat_client_finalize(GObject* obj)
{
    YoutubeChatClient* client = YOUTUBE_CHAT_CLIENT(obj);
    g_clear_object(&client->proxy);
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
    RestProxyCall* call = REST_PROXY_CALL(source_object);
    GTask* task = data;
    JsonNode* response_root = NULL;
    JsonArray* results = NULL;

    rest_proxy_call_invoke_finish(call, result, &error);
    if(error) {
        g_task_return_error(task, error);
        goto cleanup;
    }

    const char* response = rest_proxy_call_get_payload(call);
    gssize response_len = rest_proxy_call_get_payload_length(call);
    response_root = parse_json(response, response_len, &error);
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
    g_object_unref(task);
    g_object_unref(call);
}

// TODO: make cancellable if needed
void youtube_chat_client_get_channel_id_async(YoutubeChatClient* client, const char* handle,
                                              GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    RestProxyCall* call = rest_proxy_new_call(client->proxy);
    rest_proxy_call_add_header(call, "x-goog-api-key", client->api_key);
    rest_proxy_call_add_params(call,
        "part", "id",
        "fields", "items/id",
        "forHandle", handle,
        NULL);
    rest_proxy_call_set_function(call, "channels");

    GTask* task = g_task_new(client, cancellable, callback, data);
    rest_proxy_call_invoke_async(call, cancellable, on_channel_id_response, task);
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
    RestProxyCall* call = REST_PROXY_CALL(source_object);
    GTask* task = data;
    JsonNode* response_root = NULL;
    JsonArray* results = NULL;

    rest_proxy_call_invoke_finish(call, result, &error);
    if(error) {
        g_task_return_error(task, error);
        goto cleanup;
    }

    const char* response = rest_proxy_call_get_payload(call);
    gssize response_len = rest_proxy_call_get_payload_length(call);
    response_root = parse_json(response, response_len, &error);
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
    json_array_unref(results);
    json_node_unref(response_root);
    g_object_unref(task);
    g_object_unref(call);
}

void youtube_chat_client_get_live_streams_async(YoutubeChatClient* client, char* channel_id,
                                                GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    RestProxyCall* call = rest_proxy_new_call(client->proxy);
    rest_proxy_call_add_header(call, "x-goog-api-key", client->api_key);
    rest_proxy_call_add_params(call,
        "part", "snippet,id",
        "fields","items/id/videoId",
        "eventType", "live",
        "channelId", channel_id,
        "order", "date",
        "type", "video",
        NULL);
    rest_proxy_call_set_function(call, "search");

    GTask* task = g_task_new(client, cancellable, callback, data);
    rest_proxy_call_invoke_async(call, cancellable, on_live_streams_response, task);
    g_free(channel_id);
}

GPtrArray* youtube_chat_client_get_live_streams_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
{
    g_return_val_if_fail(g_task_is_valid(result, client), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

static
JsonNode* parse_json(const char* data, gssize data_len, GError** error)
{
    JsonParser* parser = json_parser_new_immutable();
    JsonNode* response_root = NULL;

    json_parser_load_from_data(parser, data, data_len, error);
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

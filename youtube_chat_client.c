#include "youtube_chat_client.h"
#include <glib.h>
#include <json-glib/json-glib.h>
#include <rest/rest.h>
#include <libsoup/soup.h>

// References:
// - https://gist.github.com/w3cj/4f1fa02b26303ae1e0b1660f2349e705
// - https://developers.google.com/youtube/v3/live/docs/liveChatMessages

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

typedef struct {
    char* title;
    char* live_chat_id;
} LiveStreamInfo;

static
JsonNode* parse_json(const char* data, gssize data_len, GError** error);

static
JsonArray* match_json_path(JsonNode* root, const char* path);

static
char* extract_video_id(const char* stream_url, GError** error);


static
void youtube_chat_client_init(YoutubeChatClient* client)
{
    client->proxy = rest_proxy_new(YOUTUBE_API_BASE_URL, /*binding_required=*/FALSE);
    #ifndef NDEBUG
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
void free_live_stream_info(gpointer data)
{
    if(data) {
        LiveStreamInfo* stream_info = data;
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
    JsonNode* response_root = NULL;
    JsonArray* titles = NULL;
    JsonArray* live_chat_ids = NULL;

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
    // Get stream title
    titles = match_json_path(response_root, "$['items'][*]['snippet']['title']");
    if(json_array_get_length(titles) != 1) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Missing live stream title");
        g_task_return_error(task, error);
        goto cleanup;
    }
    const char* title = json_array_get_string_element(titles, 0);
    if(!title) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Missing live stream title");
        g_task_return_error(task, error);
        goto cleanup;
    }
    // Get stream live chat ID
    live_chat_ids = match_json_path(response_root, "$['items'][*]['liveStreamingDetails']['activeLiveChatId']");
    if(json_array_get_length(live_chat_ids) != 1) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Missing live chat ID");
        g_task_return_error(task, error);
        goto cleanup;
    }
    const char* live_chat_id = json_array_get_string_element(live_chat_ids, 0);
    if(!live_chat_id) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Missing live chat ID");
        g_task_return_error(task, error);
        goto cleanup;
    }
    LiveStreamInfo* stream_info = g_new(LiveStreamInfo, 1);
    stream_info->title = g_strdup(title);
    stream_info->live_chat_id = g_strdup(live_chat_id);
    g_task_return_pointer(task, stream_info, free_live_stream_info);
cleanup:
    if(titles) json_array_unref(titles);
    if(live_chat_ids) json_array_unref(live_chat_ids);
    if(response_root) json_node_unref(response_root);
    g_object_unref(task);
    g_object_unref(call);
}

static
LiveStreamInfo* get_live_stream_info_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
{
    g_return_val_if_fail(g_task_is_valid(result, client), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

static
void connect_1(GObject* source_object, GAsyncResult* result, gpointer data);

static
void connect_2(GObject* source_object, GAsyncResult* result, gpointer data);

void youtube_chat_client_connect_async(YoutubeChatClient* client, const char* stream_url,
                                       GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    GError* error = NULL;
    GTask* task = g_task_new(client, cancellable, callback, data);
    char* video_id = extract_video_id(stream_url, &error);
    if(error) {
        g_task_return_error(task, error);
        g_object_unref(task);
        goto cleanup;
    }
    get_live_stream_info_async(client, video_id, cancellable, connect_1, task);
    g_free(video_id);
cleanup:
    return;
}

static
void connect_1(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    GTask* task = data;
    YoutubeChatClient* client = YOUTUBE_CHAT_CLIENT(source_object);
    LiveStreamInfo* stream_info = get_live_stream_info_finish(client, result, &error);
    if(error) {
        g_task_return_error(task, error);
        g_object_unref(task);
        goto cleanup;
    }
    RestProxyCall* call = rest_proxy_new_call(client->proxy);
    rest_proxy_call_add_header(call, "x-goog-api-key", client->api_key);
    rest_proxy_call_add_params(call,
                               "liveChatId", stream_info->live_chat_id,
                               "part", "snippet",
                               "fields", "nextPageToken,pollingIntervalMillis,"
                               "items(id,snippet(type,liveChatId,publishedAt,hasDisplayContent,displayMessage))",
                               NULL);
    rest_proxy_call_set_function(call, "liveChat/messages");
    rest_proxy_call_invoke_async(call, g_task_get_cancellable(task), connect_2, task);
cleanup:
    free_live_stream_info(stream_info);
}

static
void connect_2(GObject* source_object, GAsyncResult* result, gpointer data)
{
    GError* error = NULL;
    RestProxyCall* call = REST_PROXY_CALL(source_object);
    GTask* task = data;

    rest_proxy_call_invoke_finish(call, result, &error);
    if(error) {
        g_task_return_error(task, error);
        g_object_unref(task);
        goto cleanup;
    }

    const char* response = rest_proxy_call_get_payload(call);
    g_print("%s\n", response);
    g_task_return_pointer(task, NULL, g_object_unref);
cleanup:
    g_object_unref(task);
    g_object_unref(call);
}

void youtube_chat_client_connect_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
{
    g_return_if_fail(g_task_is_valid(result, client));
    g_task_propagate_pointer(G_TASK(result), error);
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

#include "youtube_chat_client.h"
#include <string.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <rest/rest.h>
#include <libsoup/soup.h>

// References:
// - https://gist.github.com/w3cj/4f1fa02b26303ae1e0b1660f2349e705
// - https://developers.google.com/youtube/v3/live/docs/liveChatMessages

#define YOUTUBE_API_BASE_URL "https://www.googleapis.com/youtube/v3/"

typedef struct {
    char* title;
    char* live_chat_id;
} StreamInfo;

typedef struct {
    YoutubeChatClient* client;
    char* next_page_token;
    guint poll_interval;
} FetchData;

struct _YoutubeChatClient {
    GObject parent_instance;
    RestProxy* proxy;
    char* api_key;
    StreamInfo* stream_info;
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
StreamInfo* parse_stream_info(JsonNode* response, GError** error);

static
YoutubeChatMessage* parse_message(JsonNode* response, GError** error);

static
void free_stream_info(gpointer data);

static
JsonNode* parse_json(const char* data, gssize data_len, GError** error);

static
char* match_json_string(JsonNode* root, const char* path);

static
guint match_json_uint(JsonNode* root, const char* path);

static
guint match_json_uint(JsonNode* root, const char* path);

static
GDateTime* match_json_date(JsonNode* root, const char* path);

static
JsonArray* match_json_path(JsonNode* root, const char* path);


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
        StreamInfo* stream_info = data;
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
    StreamInfo* stream_info = parse_stream_info(response_root, &error);
    if(error) {
        g_task_return_error(task, error);
    } else {
        g_task_return_pointer(task, stream_info, free_stream_info);
    }
cleanup:
    if(response_root) json_node_unref(response_root);
    g_object_unref(task);
    g_object_unref(call);
}

static
StreamInfo* get_live_stream_info_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
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
    StreamInfo* stream_info = get_live_stream_info_finish(client, result, &error);
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
    JsonNode* response_root = NULL;
    JsonArray* items = NULL;
    GPtrArray* results = NULL;

    rest_proxy_call_invoke_finish(call, result, &error);
    if(error) {
        // TODO: implement some kind of retry mechanism then give up
        // Note: will try again using the last known polling interval
        goto cleanup;
    }
    const char* response = rest_proxy_call_get_payload(call);
    gssize response_len = rest_proxy_call_get_payload_length(call);
    response_root = parse_json(response, response_len, &error);
    if(error) {
        goto cleanup;
    }
    // Get interval to wait before sending next request
    guint poll_interval = match_json_uint(response_root, "$['pollingIntervalMillis']");
    if(poll_interval == G_MAXUINT) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Invalid polling interval");
        goto cleanup;
    }
    fetch_data->poll_interval = poll_interval;
    // Get the page token to sent in the next request
    char* next_page_token = match_json_string(response_root, "$['nextPageToken']");
    if(!next_page_token) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Missing nextPageToken");
        goto cleanup;
    }
    g_free(fetch_data->next_page_token);
    fetch_data->next_page_token = next_page_token;
    // Process the batch of chat messages we have received
    items = match_json_path(response_root, "$['items'][*]");
    if(!items) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Missing response items");
        goto cleanup;
    }
    results = g_ptr_array_new();
    guint item_count = json_array_get_length(items);
    for(guint i = 0; i < item_count; ++i) {
        YoutubeChatMessage* msg = parse_message(json_array_get_element(items, i), &error);
        if(error || !msg) {
            // Skip unrecognized/malformed messages
            g_clear_error(&error);
            continue;
        }
        g_ptr_array_add(results, msg);
    }
    if(results->len > 0) {
        // Notify all listeners that a new batch of messages has been received
        g_signal_emit(fetch_data->client, signals[SIG_NEW_MESSAGES], 0, results);
    }
cleanup:
    if(results) {
        for(guint i = 0; i < results->len; ++i) {
            g_free(results->pdata[i]);
        }
        g_ptr_array_unref(results);
    }
    if(response_root) json_node_unref(response_root);
    if(items) json_array_unref(items);
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

static
StreamInfo* parse_stream_info(JsonNode* response, GError** error)
{
    char* title = NULL;
    char* live_chat_id = NULL;
    StreamInfo* stream_info = NULL;

    // Get stream title
    title = match_json_string(response, "$['items'][*]['snippet']['title']");
    if(!title) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing live stream title");
        goto cleanup;
    }
    // Get stream live chat ID
    live_chat_id = match_json_string(response, "$['items'][*]['liveStreamingDetails']['activeLiveChatId']");
    if(!live_chat_id) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing live chat ID");
        g_free(title);
        goto cleanup;
    }
    stream_info = g_new(StreamInfo, 1);
    stream_info->title = title;
    stream_info->live_chat_id = live_chat_id;
cleanup:
    return stream_info;
}

static
YoutubeChatMessage* parse_message(JsonNode* response, GError** error)
{
    char* display_name = NULL;
    GDateTime* timestamp = NULL;
    char* msg_content = NULL;
    YoutubeChatMessage* msg = NULL;

    char* message_type = match_json_string(response, "$['snippet']['type']");
    if(!message_type) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing message type");
        goto cleanup;
    }

    if(strcmp(message_type, "textMessageEvent") == 0) {
        // Get commenter's display name
        display_name = match_json_string(response, "$['authorDetails']['displayName']");
        if(!display_name) {
            g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing commenter display name");
            goto cleanup;
        }
        // Get timestamp
        timestamp = match_json_date(response, "$['snippet']['publishedAt']");
        if(!timestamp) {
            g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing comment timestamp");
            g_free(display_name);
            goto cleanup;
        }
        msg_content = match_json_string(response, "$['snippet']['displayMessage']");
        if(!msg_content) {
            g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing message content");
            g_free(display_name);
            g_free(timestamp);
            goto cleanup;
        }
        msg = g_new(YoutubeChatMessage, 1);
        msg->display_name = display_name;
        msg->timestamp = timestamp;
        msg->content = msg_content;
    } else {
        g_message("Unsupported message type: %s", message_type);
    }
cleanup:
    g_free(message_type);
    return msg;
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
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Unexpected empty JSON");
        goto cleanup;
    }

cleanup:
    json_node_ref(response_root);
    g_object_unref(parser);
    return response_root;
}

static
char* match_json_string(JsonNode* root, const char* path)
{
    JsonArray* matches = match_json_path(root, path);
    if(json_array_get_length(matches) != 1) {
        return NULL;
    }
    const char* str = json_array_get_string_element(matches, 0);
    if(!str) {
        json_array_unref(matches);
        return NULL;
    }
    char* match = g_strdup(str);
    json_array_unref(matches);
    return match;
}

static
guint match_json_uint(JsonNode* root, const char* path)
{
    JsonArray* matches = match_json_path(root, path);
    if(json_array_get_length(matches) != 1) {
        return G_MAXUINT;
    }
    gint64 value = json_array_get_int_element(matches, 0);
    // Note: cannot represent G_MAXUINT even though it is a valid
    //  value for guint to have. Not going to be a problem since
    //  unlikely to actually be returned (it's a huge value)
    if(value < 0 && value >= G_MAXUINT) {
        value = G_MAXUINT;
    }
    json_array_unref(matches);
    return (guint)value;
}

static
GDateTime* match_json_date(JsonNode* root, const char* path)
{
    GDateTime* timestamp = NULL;
    JsonArray* matches = match_json_path(root, path);
    if(json_array_get_length(matches) != 1) {
        goto cleanup;
    }
    const char* str = json_array_get_string_element(matches, 0);
    if(!str) {
        goto cleanup;
    }
    timestamp = g_date_time_new_from_iso8601(str, NULL);
cleanup:
    json_array_unref(matches);
    return timestamp;
}

static
JsonArray* match_json_path(JsonNode* root, const char* path)
{
    JsonNode* results = json_path_query(path, root, NULL);
    JsonArray* results_array = json_node_get_array(results);
    json_array_ref(results_array);
    json_node_unref(results);
    return results_array;
}

#include <libsoup/soup.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>
#include <json-glib/json-glib.h>

#define YOUTUBE_API_BASE_URL "https://www.googleapis.com/youtube/v3"

static
SoupMessage* create_youtube_message(const char* endpoint, const char* api_key)
{
    char* url = g_strconcat(YOUTUBE_API_BASE_URL, endpoint, NULL);
    SoupMessage* request = soup_message_new(SOUP_METHOD_GET, url);
    g_free(url);
    SoupMessageHeaders* headers = soup_message_get_request_headers(request);
    soup_message_headers_append(headers, "x-goog-api-key", api_key);
    return request;
}

static
JsonNode* parse_json(GBytes* data)
{
    GError* err = NULL;
    gsize data_size = 0;
    gconstpointer data_ptr = g_bytes_get_data(data, &data_size);
    JsonParser* parser = json_parser_new_immutable();
    json_parser_load_from_data(parser, data_ptr, data_size, &err);
    if(err) {
        g_printerr("Failed to parse JSON response");
        g_object_unref(parser);
        return NULL;
    }

    JsonNode* response_root = json_parser_get_root(parser);
    if(!response_root) {
        g_printerr("Failed to get JSON root");
        g_object_unref(parser);
        return NULL;
    }
    json_node_ref(response_root);
    g_object_unref(parser);
    return response_root;
}

static
char* fetch_channel_id(SoupSession* session, const char* handle, const char* api_key)
{
    char* escaped_handle = g_uri_escape_string(handle, NULL, FALSE);
    char* endpoint = g_strdup_printf("/channels?part=id&forHandle=%s", escaped_handle);
    SoupMessage* msg = create_youtube_message(endpoint, api_key);
    g_free(escaped_handle);
    g_free(endpoint);

    GError* err = NULL;
    GBytes* response = soup_session_send_and_read(session, msg, NULL, &err);
    if(err) {
        g_printerr("Failed to fetch data");
        g_object_unref(msg);
        return NULL;
    }
    g_object_unref(msg);

    JsonNode* response_root = parse_json(response);
    g_bytes_unref(response);
    if(!response_root) {
        g_printerr("Failed to get JSON root");
        return NULL;
    }

    JsonPath* path = json_path_new();
    json_path_compile(path, "$['items'][0]['id']", NULL);
    JsonNode* results = json_path_match(path, response_root);
    g_object_unref(path);
    JsonArray* results_array = json_node_get_array(results);
    if(json_array_get_length(results_array) != 1) {
        g_printerr("Unexpected number of channel ID results");
        g_object_unref(results);
        json_node_unref(response_root);
        return NULL;
    }

    JsonNode* id = json_array_get_element(results_array, 0);
    json_node_ref(id);
    json_node_unref(results);
    if(!JSON_NODE_HOLDS_VALUE(id)) {
        g_printerr("Unexpected value for channel ID");
        json_node_free(id);
        json_node_unref(response_root);
        return NULL;
    }

    GValue value = G_VALUE_INIT;
    json_node_get_value(id, &value);
    if(!G_VALUE_HOLDS_STRING(&value)) {
        g_printerr("Unexpected value for channel ID");
        g_value_unset(&value);
        json_node_free(id);
        json_node_unref(response_root);
        return NULL;
    }
    char* result = g_strdup(g_value_get_string(&value));
    g_value_unset(&value);
    json_node_free(id);
    json_node_unref(response_root);
    return result;
}

static
void fetch_live_streams(SoupSession* session, const char* channel_id, const char* api_key)
{
    char* escaped_channel_id = g_uri_escape_string(channel_id, NULL, FALSE);
    char* endpoint = g_strdup_printf("/search?part=snippet&channelId=%s&order=date&type=video",
                                     escaped_channel_id);
    g_free(escaped_channel_id);
    SoupMessage* msg = create_youtube_message(endpoint, api_key);
    g_free(endpoint);

    GError* err = NULL;
    GBytes* response = soup_session_send_and_read(session, msg, NULL, &err);
    if(err) {
        g_printerr("Failed to fetch data");
        g_object_unref(msg);
        return;
    }

    g_print("Message content: %s", (const char*)g_bytes_get_data(response, NULL));
    g_bytes_unref(response);
    g_object_unref(msg);
}

int main(int argc, char** argv)
{
    if(argc != 2) {
        g_printerr("Usage: %s channel_handle\n", argv[0]);
        return 1;
    }
    SoupSession* session = soup_session_new();
    gchar* api_key = (gchar*)g_getenv("YT_API_KEY");
    if(!api_key) {
        g_printerr("Environment variable YT_API_KEY must be set to a YouTube Data API key\n");
        return 1;
    }
    api_key = g_strdup(api_key);
    const char* channel_handle = argv[1];
    char* channel_id = fetch_channel_id(session, channel_handle, api_key);
    if(!channel_id) {
        g_free(api_key);
        return 1;
    }
    g_printf("Channel ID: %s\n", channel_id);
    fetch_live_streams(session, channel_id, api_key);

    g_free(api_key);
    return 0;
}

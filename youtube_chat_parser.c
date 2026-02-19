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
#include "youtube_chat_parser.h"
#include <json-glib/json-glib.h>

static
YoutubeChatMessage* parse_chat_message(JsonNode* message, GError** error);

static
JsonNode* parse_json(const char* data, gssize data_len, GError** error);

static
char* match_json_string(JsonNode* root, const char* path);

static
guint match_json_uint(JsonNode* root, const char* path);

static
GDateTime* match_json_date(JsonNode* root, const char* path);

static
JsonArray* match_json_path(JsonNode* root, const char* path);

GPtrArray* youtube_parse_chat_messages(const char* response, guint response_len,
                                       guint* poll_interval, char** next_page_token, GError** error)
{
    JsonNode* response_root = NULL;
    JsonArray* items = NULL;
    GPtrArray* messages = NULL;

    response_root = parse_json(response, response_len, error);
    if(*error) {
        goto cleanup;
    }
    // Get interval to wait before sending next request
    *poll_interval = match_json_uint(response_root, "$['pollingIntervalMillis']");
    if(*poll_interval == G_MAXUINT) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Invalid polling interval");
        goto cleanup;
    }
    // Get the page token to sent in the next request
    *next_page_token = match_json_string(response_root, "$['nextPageToken']");
    if(!*next_page_token) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing nextPageToken");
        goto cleanup;
    }
    // Process the batch of chat messages we have received
    items = match_json_path(response_root, "$['items'][*]");
    if(!items) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing chat messages");
        goto cleanup;
    }
    messages = g_ptr_array_new();
    guint item_count = json_array_get_length(items);
    for(guint i = 0; i < item_count; ++i) {
        YoutubeChatMessage* msg = parse_chat_message(json_array_get_element(items, i), error);
        if(*error || !msg) {
            // Skip unrecognized/malformed messages
            g_clear_error(error);
            continue;
        }
        g_ptr_array_add(messages, msg);
    }
cleanup:
    if(response_root) json_node_unref(response_root);
    if(items) json_array_unref(items);
    return messages;
}

static
YoutubeChatMessage* parse_chat_message(JsonNode* message, GError** error)
{
    char* display_name = NULL;
    GDateTime* timestamp = NULL;
    char* msg_content = NULL;
    YoutubeChatMessage* msg = NULL;
    char* message_type = NULL;

    message_type = match_json_string(message, "$['snippet']['type']");
    if(!message_type) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing message type");
        goto cleanup;
    }

    if(strcmp(message_type, "textMessageEvent") == 0) {
        // Get commenter's display name
        display_name = match_json_string(message, "$['authorDetails']['displayName']");
        if(!display_name) {
            g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing commenter display name");
            goto cleanup;
        }
        // Get timestamp
        timestamp = match_json_date(message, "$['snippet']['publishedAt']");
        if(!timestamp) {
            g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing comment timestamp");
            g_free(display_name);
            goto cleanup;
        }
        msg_content = match_json_string(message, "$['snippet']['displayMessage']");
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

YoutubeStreamInfo* youtube_parse_stream_info(const char* response, guint response_len, GError** error)
{
    char* title = NULL;
    char* live_chat_id = NULL;
    YoutubeStreamInfo* stream_info = NULL;
    JsonNode* response_root = NULL;

    response_root = parse_json(response, response_len, error);
    if(*error) {
        goto cleanup;
    }

    // Get stream title
    title = match_json_string(response_root, "$['items'][*]['snippet']['title']");
    if(!title) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing live stream title");
        goto cleanup;
    }
    // Get stream live chat ID
    live_chat_id = match_json_string(response_root, "$['items'][*]['liveStreamingDetails']['activeLiveChatId']");
    if(!live_chat_id) {
        g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Missing live chat ID");
        g_free(title);
        goto cleanup;
    }
    stream_info = g_new(YoutubeStreamInfo, 1);
    stream_info->title = title;
    stream_info->live_chat_id = live_chat_id;
cleanup:
    if(response_root) json_node_unref(response_root);
    return stream_info;
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


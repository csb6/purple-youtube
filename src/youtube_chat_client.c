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
#include <stdbool.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <rest/rest.h>
#include <gio/gio.h>
#ifdef G_OS_WIN32
    #include <bcrypt.h>
#endif /* G_OS_WIN32 */
#include <libsoup/soup.h>
#include "youtube_chat_parser.h"

// References:
// - https://gist.github.com/w3cj/4f1fa02b26303ae1e0b1660f2349e705
// - https://developers.google.com/youtube/v3/live/docs/liveChatMessages

// TODO: switch to using streamList. This avoids polling (unless server disconnects us)
//  by keeping socket open and listening, but means data is returned in unpredictable chunks
//  that need to be read in as they are available. Use rest_proxy_call_continuous()

#define YOUTUBE_API_BASE_URL "https://www.googleapis.com/youtube/v3/"
#define YOUTUBE_API_AUTH_URL "https://accounts.google.com/o/oauth2/v2/auth"
#define YOUTUBE_API_TOKEN_URL "https://oauth2.googleapis.com/token"
#define YOUTUBE_API_SCOPE "https://www.googleapis.com/auth/youtube.force-ssl"
#define LOOPBACK_REDIRECT_URL "http://127.0.0.1:43215"
#define REDIRECT_PORT 43215
#define STATE_STR_LEN 16

struct _YoutubeChatClient {
    GObject parent_instance;
    RestOAuth2Proxy* proxy;

    RestPkceCodeChallenge* pkce;
    char* state_str;
    SoupServer* auth_response_listener;
    bool is_authorized;

    YoutubeStreamInfo* stream_info;
    YoutubeChatClientErrorCallback error_cb;
    gpointer error_cb_data;
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

enum {
    PROP_IS_AUTHORIZED = 1,
    N_PROPERTIES
};
static GParamSpec* obj_properties[N_PROPERTIES] = {0};

typedef struct {
    YoutubeChatClient* client;
    char* next_page_token;
    guint poll_interval;
} FetchData;

typedef struct {
    YoutubeChatClient* client;
    SoupServerMessage* msg;
} OAuthData;

/* OAuth functions */

static void handle_oauth_auth_response(SoupServer* server, SoupServerMessage* msg, const char* path,
                                       GHashTable* query, gpointer user_data);

static void handle_oauth_access_token_response(GObject* source_object, GAsyncResult* result, gpointer data);

/* YouTube API functions */

static void get_live_stream_info_async(YoutubeChatClient* client, const char* video_id,
                                       GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data);
static void get_live_stream_info_1(GObject* source_object, GAsyncResult* result, gpointer data);
static YoutubeStreamInfo* get_live_stream_info_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error);

static void connect_1(GObject* source_object, GAsyncResult* result, gpointer data);

static void fetch_messages_async(YoutubeChatClient* client, FetchData* fetch_data);
static void fetch_messages_1(GObject* source_object, GAsyncResult* result, gpointer data);
static void fetch_messages_thunk(gpointer data);

/* Helper functions */

static char* extract_video_id(const char* stream_url, GError** error);

static void get_random_string(char* buffer, guint buffer_len, GError** error);

static void free_stream_info(gpointer data);

static void call_error_callback(YoutubeChatClient* client, GError* error);

static void set_server_error_response(SoupServerMessage* msg, SoupStatus status, const char* error_str);


static
void youtube_chat_client_init(YoutubeChatClient* client)
{
}

static
void youtube_chat_client_finalize(GObject* obj)
{
    YoutubeChatClient* client = YOUTUBE_CHAT_CLIENT(obj);
    g_clear_object(&client->proxy);
    g_free(client->state_str);
    if(client->pkce) rest_pkce_code_challenge_free(client->pkce);
    g_clear_object(&client->auth_response_listener);
    free_stream_info(client->stream_info);
    G_OBJECT_CLASS(youtube_chat_client_parent_class)->finalize(obj);
}

static
void youtube_chat_client_get_property(GObject* object, guint property_id, GValue* value, GParamSpec* pspec)
{
    YoutubeChatClient* client = YOUTUBE_CHAT_CLIENT(object);
    switch(property_id) {
        case PROP_IS_AUTHORIZED:
            g_value_set_boolean(value, client->is_authorized);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

//static
//void youtube_chat_client_class_finalize(YoutubeChatClientClass* klass)
//{}

static
void youtube_chat_client_class_init(YoutubeChatClientClass* klass)
{
    GObjectClass* obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = youtube_chat_client_finalize;
    obj_class->get_property = youtube_chat_client_get_property;

    GType param_types[] = { G_TYPE_PTR_ARRAY };
    signals[SIG_NEW_MESSAGES] = g_signal_newv(
        "new-messages",
        G_OBJECT_CLASS_TYPE(klass),
        G_SIGNAL_RUN_LAST,
        NULL, NULL, NULL, NULL, G_TYPE_NONE,
        /*n_params=*/1,
        param_types
    );

    obj_properties[PROP_IS_AUTHORIZED] = g_param_spec_boolean(
        "is-authorized",
        "Is Authorized",
        "Is the client authorized to use the YouTube API on behalf of the user?",
        /*default_value=*/FALSE,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS
    );
    g_object_class_install_properties(obj_class, N_PROPERTIES, obj_properties);
}

/**
 * youtube_chat_client_new: (constructor)
 * @client_id: (not nullable) (transfer none): The OAuth client ID.
 * @client_secret: (not nullable) (transfer none): The OAuth client secret.
 *
 * Creates a new chat client instance. The client will not initially be authorized to use the YouTube API
 * and must request permissions using OAuth authorization before making any API requests; see
 * Youtube.ChatClient.generate_auth_url_async.
 *
 * Returns: (not nullable) (transfer full): The chat client instance.
 */
YoutubeChatClient* youtube_chat_client_new(const char* client_id, const char* client_secret)
{
    YoutubeChatClient* client = g_object_new(YOUTUBE_TYPE_CHAT_CLIENT, NULL);
    // TODO: see if we can avoid using the client secret entirely
    client->proxy = rest_oauth2_proxy_new(
        YOUTUBE_API_AUTH_URL,
        YOUTUBE_API_TOKEN_URL,
        LOOPBACK_REDIRECT_URL,
        client_id, client_secret,
        YOUTUBE_API_BASE_URL);
    #ifdef YOUTUBE_CHAT_CLIENT_LOGGING
    SoupLogger* logger = soup_logger_new(SOUP_LOGGER_LOG_HEADERS);
    rest_proxy_add_soup_feature(REST_PROXY(client->proxy), SOUP_SESSION_FEATURE(logger));
    g_object_unref(logger);
    #endif

    return client;
}

/**
 * youtube_chat_client_new_authorized: (constructor)
 * @client_id: (not nullable) (transfer none): The OAuth client ID.
 * @client_secret: (not nullable) (transfer none): The OAuth client secret.
 * @access_token: (not nullable) (transfer none): The OAuth access token.
 * @refresh_token: (not nullable) (transfer none): The OAuth refresh token.
 * @access_token_expiration: (not nullable) (transfer full): The expiration time of the access token.
 *
 * Creates a new chat client instance. The client will use the given tokens when accessing the YouTube API
 * and will not require further authorization steps. If the access token is expired, it will use the refresh
 * token to request a new access token.
 *
 * Returns: (not nullable) (transfer full): The chat client instance.
 */
YoutubeChatClient* youtube_chat_client_new_authorized(const char* client_id, const char* client_secret,
                                                      const char* access_token, const char* refresh_token,
                                                      GDateTime* access_token_expiration)
{
    YoutubeChatClient* client = youtube_chat_client_new(client_id, client_secret);
    rest_oauth2_proxy_set_access_token(client->proxy, access_token);
    rest_oauth2_proxy_set_refresh_token(client->proxy, refresh_token);
    GDateTime* curr_time = g_date_time_new_now_utc();
    if(g_date_time_compare(access_token_expiration, curr_time) >= 0) {
        // TODO: schedule refresh
        client->is_authorized = FALSE;
    } else {
        client->is_authorized = TRUE;
    }

    return client;
}

/**
 * youtube_chat_client_set_error_callback:
 * @callback: Handler invoke when the error occurs
 * @data: User data passed to callback
 *
 * Registers an error handler for all errors that occur during operations not directly tied to
 * an asynchronous public method of the chat client (e.g. periodic polling of server,
 * handling of OAuth redirections, refreshing the OAuth access token)
 */
void youtube_chat_client_set_error_callback(YoutubeChatClient* client,
                                            YoutubeChatClientErrorCallback callback, gpointer data)
{
    client->error_cb = callback;
    client->error_cb_data = data;
}

/* OAuth functions */

/**
 * youtube_chat_client_generate_auth_url_async:
 * @cancellable: (nullable) (transfer none): A #GCancellable.
 * @callback: (nullable): The callback to invoke.
 * @data: (transfer none): Data for callback.
 *
 * Generates a YouTube OAuth authorization URL to grant the application the ability to send and receive
 * chat messages. The URL will be passed to @callback, and the application must then open it in a web browser.
 * The user will be able to login to their Google account and grant the application the required permissions.
 *
 * The client will listen on a local socket, which the Google authorization server will redirect to after
 * the user approves the permissions. This will automatically trigger the next step of the OAuth flow.
 * (retrieving the access and refresh tokens)
 *
 * Once the OAuth flow is complete (or if an error occurs), the user's web browser will be served a status
 * page that indicates the outcome, and the is-authorized property will be set accordingly.
 *
 * If OAuth authorization succeeds, it will then be safe to call the client's other public methods.
 *
 * The access token will be attached to each YouTube API request, and the access token will be refreshed
 * asynchronously whenever it expires.
 */
void youtube_chat_client_generate_auth_url_async(YoutubeChatClient* client,
                                                 GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    GError* error = NULL;
    GTask* task = g_task_new(client, cancellable, callback, data);
    if(client->pkce || client->state_str || client->auth_response_listener) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "Already have an in-progress OAuth flow");
        g_task_return_error(task, error);
        goto cleanup;
    }
    // Generate a PKCE challenge (i.e. a hashed random string). Server will use this value to
    // validate that the same client is sending all OAuth requests.
    RestPkceCodeChallenge* pkce = rest_pkce_code_challenge_new_random();
    // State string serves as a way to tag this request so that later we can be reasonably sure the server
    // is sending a reply to this request specifically
    char* state_str = g_malloc0(STATE_STR_LEN);
    get_random_string(state_str, STATE_STR_LEN, &error);
    if(error) {
        g_free(state_str);
        if(pkce) rest_pkce_code_challenge_free(pkce);
        g_task_return_error(task, error);
        goto cleanup;
    }

    // User must open this URL in a browser and grant the application permissions.
    // Once they have done so, they will get redirected to LOOPBACK_REDIRECT_URL. We
    // will be listening on REDIRECT_PORT and will continue the authorization flow from
    // there.
    char* auth_url = rest_oauth2_proxy_build_authorization_url(client->proxy,
                                                               rest_pkce_code_challenge_get_challenge(pkce),
                                                               YOUTUBE_API_SCOPE, &state_str);
    SoupServer* auth_response_listener = soup_server_new("server-header", "PurpleYoutube", NULL);
    // TODO: timeout for server?
    soup_server_add_handler(auth_response_listener, NULL, handle_oauth_auth_response, client, NULL);
    soup_server_listen_local(auth_response_listener, REDIRECT_PORT, SOUP_SERVER_LISTEN_IPV4_ONLY, &error);
    if(error) {
        g_object_unref(auth_response_listener);
        g_free(auth_url);
        if(pkce) rest_pkce_code_challenge_free(pkce);
        g_free(state_str);
        g_task_return_error(task, error);
        goto cleanup;
    }
    client->pkce = pkce;
    client->state_str = state_str;
    client->auth_response_listener = auth_response_listener;
    g_task_return_pointer(task, auth_url, g_free);
cleanup:
    g_object_unref(task);
}

/**
 * youtube_chat_client_generate_auth_url_finish:
 * @result: The #GAsyncResult passed to your callback.
 * @error: The return location for a recoverable error.
 *
 * Completes the Youtube.ChatClient.generate_auth_url_async operation.
 *
 * If successful, it returns the authorization URL, which the user should then open in a web browser
 * in order to authorize the application to use the YouTube API.
 *
 * Returns: (transfer full) (nullable): The OAuth authorization URL, or NULL on error.
 */
char* youtube_chat_client_generate_auth_url_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
{
    g_return_val_if_fail(g_task_is_valid(result, client), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

static
void handle_oauth_auth_response(SoupServer* server, SoupServerMessage* msg, const char* path,
                                GHashTable* query, gpointer user_data)
{
    GError* error = NULL;
    YoutubeChatClient* client = user_data;
    char* error_str = g_hash_table_lookup(query, "error");
    if(error_str) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "OAuth redirect error: %s", error_str);
        call_error_callback(client, error);
        set_server_error_response(msg, SOUP_STATUS_FORBIDDEN, error->message);
        goto cleanup;
    }
    char* auth_code = g_hash_table_lookup(query, "code");
    if(!auth_code) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "OAuth redirect error: Missing auth code");
        set_server_error_response(msg, SOUP_STATUS_BAD_REQUEST, error->message);
        call_error_callback(client, error);
        goto cleanup;
    }
    char* state_str = g_hash_table_lookup(query, "state");
    if(!state_str || strcmp(state_str, client->state_str) != 0) {
        g_set_error(&error, YOUTUBE_CHAT_ERROR, 1, "OAuth redirect error: Missing state string");
        set_server_error_response(msg, SOUP_STATUS_BAD_REQUEST, error->message);
        call_error_callback(client, error);
        goto cleanup;
    }
    // TODO: use a different email for the purple-youtube Google app
    soup_server_message_pause(msg);
    g_object_ref(msg);
    OAuthData* oauth_data = g_new(OAuthData, 1);
    oauth_data->client = client;
    oauth_data->msg = msg;
    rest_oauth2_proxy_fetch_access_token_async(client->proxy, auth_code,
                                               rest_pkce_code_challenge_get_verifier(client->pkce),
                                               NULL, handle_oauth_access_token_response, oauth_data);
cleanup:
    if(client->pkce) rest_pkce_code_challenge_free(client->pkce);
    client->pkce = NULL;
    g_free(client->state_str);
    client->state_str = NULL;
    // TODO: when to unref the server instance and the msg?
}

static
void handle_oauth_access_token_response(GObject* source_object, GAsyncResult* result, gpointer data)
{
    static const char success_response[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
          "<head>"
            "<title>Purple-Youtube - Authorization Successful</title>"
          "</head>"
          "<body>"
            "<p>Successfully authorized Purple-Youtube! You now can close this tab.</p>"
          "</body>"
        "</html>";

    GError* error = NULL;
    RestOAuth2Proxy* proxy = REST_OAUTH2_PROXY(source_object);
    OAuthData* oauth_data = data;
    YoutubeChatClient* client = oauth_data->client;
    SoupServerMessage* server_msg = oauth_data->msg;

    rest_oauth2_proxy_fetch_access_token_finish(proxy, result, &error);
    if(error) {
        call_error_callback(client, error);
        // TODO: map GError to HTTP error code
        set_server_error_response(server_msg, SOUP_STATUS_FORBIDDEN, error->message);
    } else {
        // From this point forwards, OAuth2Proxy will add the access token as an
        // 'Authorization: Bearer <access_token>' header to each request
        client->is_authorized = TRUE;
        // TODO: set to false if token refresh operation fails
        g_object_notify_by_pspec(G_OBJECT(client), obj_properties[PROP_IS_AUTHORIZED]);
        // Send the user's web browser a message letting them know authorization was successful
        soup_server_message_set_status(server_msg, SOUP_STATUS_OK, NULL);
        soup_server_message_set_response(server_msg, "text/html", SOUP_MEMORY_STATIC,
                                         success_response, sizeof(success_response));
        // TODO: schedule next refresh token update
        // TODO: how to ensure requests do not use expired access tokens
        // TODO: seems like librest is treating some error responses as success. If we send an empty client
        //  secret, everything appears to succeed but the Bearer token is '(null)', causing API calls to fail
    }
    soup_server_message_unpause(server_msg);
    g_free(oauth_data);
}

/* YouTube API functions */

static
void get_live_stream_info_async(YoutubeChatClient* client, const char* video_id,
                                GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    GTask* task = g_task_new(client, cancellable, callback, data);
    RestProxyCall* call = rest_proxy_new_call(REST_PROXY(client->proxy));
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

void youtube_chat_client_connect_finish(YoutubeChatClient* client, GAsyncResult* result, GError** error)
{
    g_return_if_fail(g_task_is_valid(result, client));
    g_task_propagate_pointer(G_TASK(result), error);
}

static
void fetch_messages_async(YoutubeChatClient* client, FetchData* fetch_data)
{
    RestProxyCall* call = rest_proxy_new_call(REST_PROXY(client->proxy));
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
        call_error_callback(fetch_data->client, error);
        goto cleanup;
    }
    const char* response = rest_proxy_call_get_payload(call);
    gssize response_len = rest_proxy_call_get_payload_length(call);
    messages = youtube_parse_chat_messages(response, response_len, &poll_interval, &next_page_token, &error);
    if(error) {
        call_error_callback(fetch_data->client, error);
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
    g_clear_error(&error);
    g_object_unref(call);
    g_timeout_add_once(fetch_data->poll_interval, fetch_messages_thunk, fetch_data);
}

static
void fetch_messages_thunk(gpointer data)
{
    FetchData* fetch_data = data;
    fetch_messages_async(fetch_data->client, fetch_data);
}

/* Helper functions */

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

/**
 * get_random_string:
 * @buffer (out): (transfer none): Buffer in which to place the string.
 * @buffer_len: Size of the buffer. Must be at least 1.
 * @error (inout): Error.
 *
 * Generates a random null-terminated string of the specified length using a high-entropy source
 * of randomness.
 */
static
void get_random_string(char* buffer, guint buffer_len, GError** error)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~";

    g_assert(buffer_len > 0);
    // Leave space for a null terminator
    --buffer_len;
    #if defined(G_OS_UNIX)
        GFile* urandom = g_file_new_for_path("/dev/urandom");
        GFileInputStream* rand_stream = g_file_read(urandom, NULL, error);
        if(*error) {
            goto cleanup;
        }
        gsize bytes_read = 0;
        g_input_stream_read_all(G_INPUT_STREAM(rand_stream), buffer, buffer_len, &bytes_read, NULL, error);
        if(*error) {
            goto cleanup;
        }
    #elif defined(G_OS_WIN32)
        // TODO: test on Windows somehow
        NTSTATUS status = BCryptGenRandom(NULL, buffer, buffer_len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if(status != STATUS_SUCCESS) {
            g_set_error(error, YOUTUBE_CHAT_ERROR, 1, "Failed to read random data during OAuth authorization");
            goto cleanup;
        }
    #else
        #error "The cryptographic random number generator API for this platform is not supported"
    #endif
    for(guint i = 0; i < buffer_len; ++i) {
        buffer[i] = alphabet[buffer[i] % (sizeof(alphabet) - 1)];
    }
    buffer[buffer_len] = '\0';
cleanup:
    #ifdef G_OS_UNIX
        g_object_unref(rand_stream);
        g_object_unref(urandom);
    #endif /* G_OS_UNIX */
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

/**
 * call_error_callback:
 *
 * Invokes the client's error callback (if present) with the given error
 */
static
void call_error_callback(YoutubeChatClient* client, GError* error)
{
    if(client->error_cb) {
        client->error_cb(error, client->error_cb_data);
    }
}

/**
 * set_server_error_response:
 * @status: The HTTP error status to send.
 * @error_str: (transfer none) (not nullable): The error message to display on the error page.
 *
 * Prepares an error page to the user's web browser indicating an error occurred during OAuth
 * authorization.
 */
static
void set_server_error_response(SoupServerMessage* msg, SoupStatus status, const char* error_str)
{
    static const char error_response[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
          "<head>"
            "<title>Purple-Youtube - Error</title>"
          "</head>"
          "<body>"
            "<p>Failed to grant permissions to Purple-Youtube:</p>"
            "<p>%s</p>"
          "</body>"
        "</html>";

    soup_server_message_set_status(msg, status, NULL);
    char* content = g_strdup_printf(error_response, error_str);
    soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_TAKE, content, strlen(content));
}

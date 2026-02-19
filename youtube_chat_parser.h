#pragma once

#include "youtube_types.h"

GPtrArray* youtube_parse_chat_messages(const char* response, guint response_len,
                                       guint* poll_interval, char** next_page_token, GError** error);
YoutubeStreamInfo* youtube_parse_stream_info(const char* response, guint response_len, GError** error);

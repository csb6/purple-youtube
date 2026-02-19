#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define YOUTUBE_CHAT_ERROR youtube_chat_error_quark()
GQuark youtube_chat_error_quark(void);

typedef struct {
    char* title;
    char* live_chat_id;
} YoutubeStreamInfo;

typedef struct {
    char* display_name;
    GDateTime* timestamp;
    char* content;
} YoutubeChatMessage;

G_END_DECLS

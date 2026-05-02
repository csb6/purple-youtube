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
#include <peel/GLib/functions.h>
#include <peel/GLib/MainLoop.h>
#include <peel/GLib/MainContext.h>
#include <peel/GLib/DateTime.h>
#include <peel/ArrayRef.h>
#include "youtube_types.hpp"
#include "youtube_chat_client.hpp"
#include <memory>

namespace glib = peel::GLib;

int main(int argc, char** argv)
{
    if(argc != 2) {
        g_printerr("Usage: %s stream_url\n", argv[0]);
        return 1;
    }

    const char* stream_url = argv[1];
    std::unique_ptr<char*, decltype(&g_strfreev)> env{g_get_environ(), &g_strfreev};
    const char* client_id = g_environ_getenv(env.get(), "YT_CLIENT_ID");
    if(!client_id) {
        g_printerr("Missing environment variable YT_CLIENT_ID");
        return 1;
    }
    const char* client_secret = g_environ_getenv(env.get(), "YT_CLIENT_SECRET");
    if(!client_secret) {
        g_printerr("Missing environment variable YT_CLIENT_SECRET");
        return 1;
    }

    auto main_loop = glib::MainLoop::create(glib::MainContext::default_(), /*is_running=*/false);
    auto client = youtube::ChatClient::create(client_id, client_secret);
    client->connect_new_messages([](youtube::ChatClient*, void* data) {
        auto& messages = *static_cast<peel::ArrayRef<const youtube::ChatMessage>*>(data);
        for(const auto& msg : messages) {
            auto local_timestamp = msg.timestamp->to_local();
            auto timestamp_str = local_timestamp->format("%I:%M:%S %p");
            g_print("%s (%s): %s\n\n", msg.display_name.c_str(), timestamp_str.c_str(), msg.content.c_str());
        }
    });
    client->connect_notify(client->prop_is_authorized(),
                           [stream_url, main_loop](gobject::Object* obj, bool is_authorized) {
        auto* client = static_cast<youtube::ChatClient*>(obj);
        if(is_authorized) {
            // TODO: handle error returned by the coroutine on completion
            client->connect_to_chat_async(stream_url, nullptr).start();
        } else {
            // Unreachable currently (no notification sent on auth error)
            g_printerr("Failed to authorize\n");
            main_loop->quit();
        }
    });

    peel::UniquePtr<glib::Error> error;
    auto auth_url = client->generate_auth_url_async(&error);
    if(error) {
        g_printerr("Failed to get OAuth authorization URL: %s\n", error->message);
        return 1;
    }
    char* cmd = g_strdup_printf("xdg-open \"%s\"", auth_url.c_str());
    system(cmd);
    g_free(cmd);
    main_loop->run();

    return 0;
}

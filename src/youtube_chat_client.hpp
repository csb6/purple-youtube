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
#pragma once

#include <peel/GLib/DateTime.h>
#include <peel/GLib/Error.h>
#include <peel/GObject/GObject.h>
#include <peel/Gio/Cancellable.h>
#include <peel/class.h>
#include <peel/RefPtr.h>
#include <peel/UniquePtr.h>
#include <peel/String.h>
#include <peel/signal.h>
#include <peel/property.h>
#include <memory>
#include <functional>
#include "youtube_types.hpp"
#include "task.hpp"

namespace youtube {

class ChatClient final : public gobject::Object {
    PEEL_SIMPLE_CLASS(ChatClient, Object)
public:
    using ErrorCallback = std::function<void(glib::Error*)>;

    void init(Class*);
    static peel::RefPtr<ChatClient> create(const char* client_id, const char* client_secret);
    static peel::RefPtr<ChatClient> create_authorized(const char* client_id, const char* client_secret,
                                                      const char* access_token, const char* refresh_token,
                                                      glib::DateTime* access_token_expiration);

    void set_error_callback(ErrorCallback&&);
    peel::String generate_auth_url_async(peel::UniquePtr<glib::Error>*);
    Task<void> connect_to_chat_async(const char* stream_url, gio::Cancellable* cancellable);
    bool get_is_authorized() const;

    PEEL_PROPERTY(bool, is_authorized, "is-authorized")
    PEEL_SIGNAL_CONNECT_METHOD(new_messages, sig_new_messages)
private:
    template<typename F>
    static void define_properties(F& f)
    {
        f.prop(prop_is_authorized(), false)
         .get(&ChatClient::get_is_authorized);
    }
    // TODO: find way to pass std::ArrayRef<const ChatMessage> as a parameter
    static peel::Signal<ChatClient, void(void*)> sig_new_messages;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace youtube

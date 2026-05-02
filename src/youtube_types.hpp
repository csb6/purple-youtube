#pragma once

#include "youtube_error.h"
#include <peel/String.h>
#include <peel/RefPtr.h>
#include <peel/GLib/DateTime.h>
#include <peel/GObject/Value.h>
#include <peel/ArrayRef.h>

namespace peel {
    namespace GObject {}
    namespace Gio {}
    namespace Soup {}
    namespace Rest {}
    namespace Json {}
}

namespace glib = peel::GLib;
namespace gio = peel::Gio;
namespace gobject = peel::GObject;
namespace soup = peel::Soup;
namespace rest = peel::Rest;
namespace json = peel::Json;

namespace youtube {

struct StreamInfo {
    peel::String title;
    peel::String live_chat_id;
};

struct ChatMessage {
    peel::String display_name;
    peel::RefPtr<glib::DateTime> timestamp;
    peel::String content;
};

} // namespace youtube

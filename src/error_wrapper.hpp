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

#include <peel/GLib/Error.h>
#include <peel/UniquePtr.h>

namespace glib = peel::GLib;

/* Simple wrapper to make copyable version of peel::UniquePtr<glib::Error> */
class ErrorPtr {
public:
    ErrorPtr() {}
    ErrorPtr(peel::UniquePtr<glib::Error>&& error)
        : error(std::move(error))
    {}
    template<typename ...Args>
    ErrorPtr(glib::Quark domain, int code, const char *format, Args ...args)
        : error(glib::Error::create(domain, code, format, std::forward<Args>(args)...))
    {}
    ErrorPtr(const ErrorPtr& other)
        : error(other.error->copy())
    {}
    ErrorPtr(ErrorPtr&&) noexcept = default;

    ErrorPtr& operator=(const ErrorPtr& other) noexcept
    {
        error = other.error->copy();
        return *this;
    }
    ErrorPtr& operator=(ErrorPtr&&) noexcept = default;

    operator bool() { return error; }
    glib::Error* operator->() { return error; }
    glib::Error* get() { return error; }
private:
    peel::UniquePtr<glib::Error> error;
};

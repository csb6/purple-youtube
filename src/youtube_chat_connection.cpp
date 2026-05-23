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
#include "youtube_chat_connection.hpp"

namespace youtube {

PEEL_CLASS_IMPL(Connection, "YoutubeConnection", purple::Connection)

struct Connection::Impl {
    peel::RefPtr<purple::Account> account;
};

void Connection::Class::init()
{}

void Connection::init(Class*)
{
    m_impl = std::make_unique<Impl>();
}

peel::RefPtr<Connection> Connection::create(peel::RefPtr<purple::Account> account)
{
    auto connection = Object::create<Connection>();
    connection->m_impl->account = std::move(account);
    return connection;
}


} // namespace youtube

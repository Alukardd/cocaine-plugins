/*
    Copyright (c) 2011-2012 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2012 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>. 
*/

#ifndef COCAINE_BLASTBEAT_DRIVER_HPP
#define COCAINE_BLASTBEAT_DRIVER_HPP

#include <cocaine/common.hpp>
#include <cocaine/asio.hpp>
#include <cocaine/io.hpp>

#include <cocaine/api/driver.hpp>

namespace cocaine { namespace driver {

class blastbeat_t:
    public api::driver_t
{
    public:
        typedef api::driver_t category_type;

    public:
        blastbeat_t(context_t& context,
                    const std::string& name,
                    const Json::Value& args,
                    engine::engine_t& engine);

        virtual
        ~blastbeat_t();

        virtual
        Json::Value
        info() const;

        template<class T>
        void
        send(const std::string& sid,
             const std::string& type,
             const T& message)
        {
            m_socket.send_multipart(
                boost::make_tuple(
                    io::protect(sid),
                    io::protect(type),
                    message
                )
            );

            on_check(m_checker, ev::PREPARE);
        }

    private:
        void
        on_event(ev::io&, int);
        
        void
        on_check(ev::prepare&, int);

        void
        process_events();
        
        void
        on_ping();
        
        void
        on_spawn();
        
        void
        on_uwsgi(const std::string& sid,
                 zmq::message_t& message);
        
        void
        on_body(const std::string& sid, 
                zmq::message_t& message);

        void
        on_end(const std::string& sid); 
    
    protected:
        context_t& m_context;
        boost::shared_ptr<logging::logger_t> m_log;

        // Configuration

        std::string m_event;
        std::string m_identity;
        std::string m_endpoint;

        // I/O

        io::socket<
           io::policies::unique
        > m_socket;

        // Event loop

        ev::io m_watcher; 
        ev::prepare m_checker;
        
        // Session tracking

        typedef boost::unordered_map<
            std::string,
            boost::shared_ptr<api::stream_t>
        > stream_map_t;

        stream_map_t m_streams;
};

}}

#endif

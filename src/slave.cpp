/*
    Copyright (c) 2011-2013 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2013 Other contributors as noted in the AUTHORS file.

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

#include "cocaine/detail/slave.hpp"

#include "cocaine/context.hpp"

#include "cocaine/api/event.hpp"
#include "cocaine/api/stream.hpp"

#include "cocaine/detail/engine.hpp"
#include "cocaine/detail/manifest.hpp"
#include "cocaine/detail/profile.hpp"
#include "cocaine/detail/session.hpp"

#include "cocaine/logging.hpp"
#include "cocaine/messages.hpp"

#include "cocaine/rpc/channel.hpp"

#include <array>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

using namespace cocaine;
using namespace cocaine::engine;
using namespace cocaine::io;

using namespace std::placeholders;

struct engine::pipe_t {
    typedef int endpoint_type;

    pipe_t(endpoint_type endpoint):
        m_pipe(endpoint)
    {
        ::fcntl(m_pipe, F_SETFL, O_NONBLOCK);
    }

   ~pipe_t() {
        ::close(m_pipe);
    }

    int
    fd() const {
        return m_pipe;
    }

    ssize_t
    read(char* buffer, size_t size, std::error_code& ec) {
        ssize_t length = ::read(m_pipe, buffer, size);

        if(length == -1) {
            switch(errno) {
                case EAGAIN:
    #if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
                case EWOULDBLOCK:
    #endif
                case EINTR:
                    break;

                default:
                    ec = std::error_code(errno, std::system_category());
            }
        }

        return length;
    }

private:
    int m_pipe;
};

namespace {
    struct ignore {
        void
        operator()(const std::error_code& /* ec */) {
            // Do nothing.
        }
    };
}

slave_t::slave_t(context_t& context,
                 reactor_t& reactor,
                 const manifest_t& manifest,
                 const profile_t& profile,
                 const std::string& id,
                 engine_t& engine):
    m_context(context),
    m_log(new logging::log_t(context, cocaine::format("app/%s", manifest.name))),
    m_reactor(reactor),
    m_manifest(manifest),
    m_profile(profile),
    m_id(id),
    m_engine(engine),
    m_state(states::unknown),
#if defined(__clang__) || defined(HAVE_GCC47)
    m_birthstamp(std::chrono::steady_clock::now()),
#else
    m_birthstamp(std::chrono::monotonic_clock::now()),
#endif
    m_heartbeat_timer(reactor.native()),
    m_idle_timer(reactor.native()),
    m_output_ring(profile.crashlog_limit)
{
    // NOTE: Initialization heartbeat can be different.
    m_heartbeat_timer.set<slave_t, &slave_t::on_timeout>(this);
    m_heartbeat_timer.start(m_profile.startup_timeout);

    COCAINE_LOG_DEBUG(m_log, "slave %s is activating", m_id);

    auto isolate = m_context.get<api::isolate_t>(
        m_profile.isolate.type,
        m_context,
        m_manifest.name,
        m_profile.isolate.args
    );

    COCAINE_LOG_DEBUG(m_log, "slave %s is spawning '%s'", m_id, m_manifest.slave);

    typedef std::map<std::string, std::string> string_map_t;

    string_map_t args;
    string_map_t environment;

    args["--app"] = m_manifest.name;
    args["--endpoint"] = m_manifest.endpoint;
    args["--uuid"] = m_id;

    // Standard output capture

    std::array<int, 2> pipes;

    if(::pipe(pipes.data()) != 0) {
        throw std::system_error(errno, std::system_category(), "unable to create an output pipe");
    }

    // Mark both ends of the pipe as close-on-exec.
    for(auto it = pipes.begin(); it != pipes.end(); ++it) {
        ::fcntl(*it, F_SETFD, FD_CLOEXEC);
    }

    try {
        m_handle = isolate->spawn(m_manifest.slave, args, environment, pipes[1]);
    } catch(...) {
        std::for_each(pipes.begin(), pipes.end(), ::close);
        throw;
    }

    // This end of the pipe is already cloned by the isolate, so we can safely close it.
    ::close(pipes[1]);

    m_output_pipe.reset(new readable_stream<pipe_t>(reactor, pipes[0]));

    m_output_pipe->bind(
        std::bind(&slave_t::on_output, this, _1, _2),
        ignore()
    );
}

slave_t::~slave_t() {
    BOOST_ASSERT(m_state == states::inactive);
    BOOST_ASSERT(m_sessions.empty() && m_queue.empty());

    m_heartbeat_timer.stop();
    m_idle_timer.stop();

    // Closes our end of the socket.
    m_channel.reset();

    m_handle->terminate();
    m_handle.reset();

    COCAINE_LOG_DEBUG(m_log, "slave %s has been terminated", m_id);
}

void
slave_t::bind(const std::shared_ptr<channel<io::socket<local>>>& channel_) {
    BOOST_ASSERT(m_state == states::unknown);
    BOOST_ASSERT(!m_channel);

    m_channel = channel_;

    m_channel->rd->bind(
        std::bind(&slave_t::on_message,    this, _1),
        std::bind(&slave_t::on_disconnect, this, _1)
    );

    m_channel->wr->bind(
        std::bind(&slave_t::on_disconnect, this, _1)
    );

    on_ping();
}

void
slave_t::assign(const std::shared_ptr<session_t>& session) {
    BOOST_ASSERT(m_state != states::inactive);

    std::unique_lock<std::mutex> lock(m_mutex);

    if(m_sessions.size() >= m_profile.concurrency || m_state == states::unknown) {
        m_queue.push_back(session);
        return;
    }

    BOOST_ASSERT(m_state == states::active);

    if(session->event.policy.deadline &&
       session->event.policy.deadline <= m_reactor.native().now())
    {
        lock.unlock();

        COCAINE_LOG_DEBUG(m_log, "session %s has expired, dropping", session->id);

        session->upstream->error(
            deadline_error,
            "the session has expired in the queue"
        );

        return;
    }

    m_idle_timer.stop();

    m_sessions.insert(std::make_pair(session->id, session));

    // NOTE: Allows other sessions to be processed while this one is being attached.
    lock.unlock();

    COCAINE_LOG_DEBUG(m_log, "slave %s has started processing session %s", m_id, session->id);

    session->attach(m_channel->wr->stream());
}

void
slave_t::stop() {
    BOOST_ASSERT(m_state == states::active);

    m_state = states::inactive;

    m_channel->wr->write<rpc::terminate>(0UL, rpc::terminate::normal, "engine shutdown");
}

void
slave_t::on_message(const message_t& message) {
    COCAINE_LOG_DEBUG(
        m_log,
        "slave %s received type %d message in session %d",
        m_id,
        message.id(),
        message.band()
    );

    switch(message.id()) {
        case event_traits<rpc::heartbeat>::id:
            on_ping();
            break;

        case event_traits<rpc::terminate>::id: {
            rpc::terminate::code code;
            std::string reason;

            message.as<rpc::terminate>(code, reason);
            on_death(code, reason);

            break;
        }

        case event_traits<rpc::chunk>::id: {
            std::string chunk;

            message.as<rpc::chunk>(chunk);
            on_chunk(message.band(), chunk);

            break;
        }

        case event_traits<rpc::error>::id: {
            error_code code;
            std::string reason;

            message.as<rpc::error>(code, reason);
            on_error(message.band(), code, reason);

            break;
        }

        case event_traits<rpc::choke>::id:
            on_choke(message.band());
            break;

        default:
            COCAINE_LOG_WARNING(
                m_log,
                "slave %s dropped unknown type %d message in session %d",
                m_id,
                message.id(),
                message.band()
            );
    }
}

namespace {
    struct detach_with {
        template<class T>
        void
        operator()(const T& session) const {
            session.second->upstream->error(code, message);
            session.second->detach();
        }

        const error_code code;
        const std::string message;
    };
}

void
slave_t::on_disconnect(const std::error_code& ec) {
    COCAINE_LOG_ERROR(
        m_log,
        "slave %s has unexpectedly disconnected - [%d] %s",
        m_id,
        ec.value(),
        ec.message()
    );

    m_state = states::inactive;

    {
        std::unique_lock<std::mutex> lock(m_mutex);

        COCAINE_LOG_WARNING(m_log, "slave %s dropping %llu sessions", m_id, m_sessions.size());

        std::for_each(m_sessions.begin(), m_sessions.end(), detach_with {
            resource_error,
            "the session has been aborted"
        });

        m_sessions.clear();
    }

    dump();
    terminate(rpc::terminate::code::abnormal, "unexpectedly disconnected");
}

void
slave_t::on_ping() {
    if(m_state == states::unknown) {
        using namespace std::chrono;

        auto uptime = duration_cast<duration<float>>(
#if defined(__clang__) || defined(HAVE_GCC47)
            steady_clock::now() - m_birthstamp
#else
            monotonic_clock::now() - m_birthstamp
#endif
        );

        COCAINE_LOG_DEBUG(
            m_log,
            "slave %s became active in %.03f seconds",
            m_id,
            uptime.count()
        );

        m_state = states::active;

        // Start the idle timer, which will kill the slave when it's not used.
        m_idle_timer.set<slave_t, &slave_t::on_idle>(this);
        m_idle_timer.start(m_profile.idle_timeout);

        pump();
    }

    COCAINE_LOG_DEBUG(
        m_log,
        "slave %s is resetting heartbeat timeout to %.02f seconds",
        m_id,
        m_profile.heartbeat_timeout
    );

    m_heartbeat_timer.stop();
    m_heartbeat_timer.start(m_profile.heartbeat_timeout);

    m_channel->wr->write<rpc::heartbeat>(0UL);
}

void
slave_t::on_death(int code, const std::string& reason) {
    COCAINE_LOG_DEBUG(
        m_log,
        "slave %s is committing suicide: %s",
        m_id,
        reason
    );

    m_state = states::inactive;

    {
        std::unique_lock<std::mutex> lock(m_mutex);

        COCAINE_LOG_WARNING(m_log, "slave %s dropping %llu sessions", m_id, m_sessions.size());

        std::for_each(m_sessions.begin(), m_sessions.end(), detach_with {
            resource_error,
            "the session has been aborted"
        });

        m_sessions.clear();
    }

    terminate(code, reason);
}

void
slave_t::on_chunk(uint64_t session_id, const std::string& chunk) {
    BOOST_ASSERT(m_state == states::active);

    COCAINE_LOG_DEBUG(
        m_log,
        "slave %s received session %s chunk, size: %llu bytes",
        m_id,
        session_id,
        chunk.size()
    );

    session_map_t::iterator it;

    {
        std::unique_lock<std::mutex> lock(m_mutex);

        it = m_sessions.find(session_id);

        // TEST: Ensure that this slave is responsible for the session.
        BOOST_ASSERT(it != m_sessions.end());
    }

    it->second->upstream->write(chunk.data(), chunk.size());
}

void
slave_t::on_error(uint64_t session_id, error_code code, const std::string& reason) {
    BOOST_ASSERT(m_state == states::active);

    COCAINE_LOG_DEBUG(
        m_log,
        "slave %s received session %s error, code: %d, message: %s",
        m_id,
        session_id,
        code,
        reason
    );

    session_map_t::iterator it;

    {
        std::unique_lock<std::mutex> lock(m_mutex);

        it = m_sessions.find(session_id);

        // TEST: Ensure that this slave is responsible for the session.
        BOOST_ASSERT(it != m_sessions.end());
    }

    it->second->upstream->error(code, reason);
}

void
slave_t::on_choke(uint64_t session_id) {
    BOOST_ASSERT(m_state == states::active);

    COCAINE_LOG_DEBUG(
        m_log,
        "slave %s has completed session %s",
        m_id,
        session_id
    );

    session_map_t::iterator it;

    {
        std::unique_lock<std::mutex> lock(m_mutex);

        it = m_sessions.find(session_id);

        // TEST: Ensure that this slave is responsible for the session.
        BOOST_ASSERT(it != m_sessions.end());
    }

    it->second->upstream->close();

    // NOTE: As we're destroying the session here, we have to close the
    // downstream, otherwise the client wouldn't be able to close it later.
    // it->second->send<rpc::choke>();

    it->second->detach();

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_sessions.erase(it);
    }

    pump();
}

void
slave_t::on_timeout(ev::timer&, int) {
    switch(m_state) {
        case states::unknown:
            COCAINE_LOG_ERROR(m_log, "slave %s has failed to activate", m_id);

            m_state = states::inactive;

            break;

        case states::active:
            COCAINE_LOG_ERROR(m_log, "slave %s has timed out", m_id);

            m_state = states::inactive;

            {
                std::unique_lock<std::mutex> lock(m_mutex);

                COCAINE_LOG_WARNING(m_log, "slave %s dropping %llu sessions", m_id, m_sessions.size());

                std::for_each(m_sessions.begin(), m_sessions.end(), detach_with {
                    timeout_error,
                    "the session had timed out"
                });

                m_sessions.clear();
            }

            break;

        case states::inactive:
            COCAINE_LOG_ERROR(m_log, "slave %s has failed to deactivate", m_id);
            break;
    }

    dump();
    terminate(rpc::terminate::code::normal, "timed out");
}

void
slave_t::on_idle(ev::timer&, int) {
    BOOST_ASSERT(m_state == states::active);
    BOOST_ASSERT(m_sessions.empty() && m_queue.empty());

    COCAINE_LOG_DEBUG(m_log, "slave %s is idle, deactivating", m_id);

    m_state = states::inactive;

    m_channel->wr->write<rpc::terminate>(0UL, rpc::terminate::normal, "idle");
}

size_t
slave_t::on_output(const char* data, size_t size) {
    std::string input(data, size),
                line;

    std::istringstream stream(input);

    size_t leftovers = 0;

    while(stream) {
        if(std::getline(stream, line)) {
            m_output_ring.push_back(line);
        } else {
            leftovers = line.size();
        }
    }

    return size - leftovers;
}

void
slave_t::pump() {
    while(!m_queue.empty()) {
        std::unique_lock<std::mutex> lock(m_mutex);

        if(m_sessions.size() >= m_profile.concurrency) {
            return;
        }

        if(m_queue.empty()) {
            if(m_sessions.empty() && m_profile.idle_timeout > 0.0f) {
                m_idle_timer.start(m_profile.idle_timeout);
            }

            break;
        }

        session_queue_t::value_type session = m_queue.front();
        m_queue.pop_front();

        // This lock is reacquired inside the assign() method.
        lock.unlock();

        assign(session);
    }

    m_engine.pump();
}

void
slave_t::dump() {
    if(m_output_ring.empty()) {
        COCAINE_LOG_WARNING(m_log, "slave %s has died in silence", m_id);
        return;
    }

#if defined(__clang__) || defined(HAVE_GCC47)
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#else
    auto now = std::chrono::monotonic_clock::now().time_since_epoch().count();
#endif

    std::string key = cocaine::format("%lld:%s", now, m_id);

    COCAINE_LOG_INFO(m_log, "slave %s is dumping output to 'crashlogs/%s'", m_id, key);

    std::vector<std::string> dump;
    std::copy(m_output_ring.begin(), m_output_ring.end(), std::back_inserter(dump));

    try {
        api::storage(m_context, "core")->put("crashlogs", key, dump, std::vector<std::string> {
            m_manifest.name
        });
    } catch(const std::exception& e) {
        COCAINE_LOG_ERROR(m_log, "slave %s is unable to save the crashlog - %s", m_id, e.what());
    }
}

void
slave_t::terminate(int code, const std::string& reason) {
    m_reactor.post(std::bind(&engine_t::erase, std::ref(m_engine), m_id, code, reason));
}

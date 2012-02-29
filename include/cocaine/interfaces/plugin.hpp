//
// Copyright (C) 2011 Andrey Sibiryov <me@kobology.ru>
//
// Licensed under the BSD 2-Clause License (the "License");
// you may not use this file except in compliance with the License.
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef COCAINE_GENERIC_SLAVE_BACKEND_PLUGIN_INTERFACE_HPP
#define COCAINE_GENERIC_SLAVE_BACKEND_PLUGIN_INTERFACE_HPP

#include "cocaine/common.hpp"
#include "cocaine/forwards.hpp"
#include "cocaine/object.hpp"

#include "cocaine/app.hpp"
#include "cocaine/interfaces/module.hpp"

namespace cocaine { namespace engine {

// Plugin invocation site
// ----------------------

class invocation_site_t {
    public:
        invocation_site_t(overseer_t& overseer,
                          const void* request,
                          size_t request_size);

        void pull();
        void push(const void* data, size_t size);
        void emit(const std::string& key, const void* data, size_t size);

    public:
        const void* request;
        size_t request_size;

    private:
        overseer_t& m_overseer;
};

// Generic slave backend plugin interface
// --------------------------------------

class plugin_t:
    public object_t
{
    public:
        plugin_t(context_t& ctx, const std::string& identity):
            object_t(ctx, identity)
        { }

        virtual void initialize(const app_t& app) = 0;
        virtual void invoke(invocation_site_t& site, const std::string& method) = 0;
};

// Allowed exceptions
// ------------------

class unrecoverable_error_t:
    public std::runtime_error
{
    public:
        unrecoverable_error_t(const std::string& what):
            std::runtime_error(what)
        { }
};

class recoverable_error_t:
    public std::runtime_error
{
    public:
        recoverable_error_t(const std::string& what):
            std::runtime_error(what)
        { }
};

}}

#endif
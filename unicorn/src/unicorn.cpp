/*
* 2015+ Copyright (c) Anton Matveenko <antmat@yandex-team.ru>
* All rights reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*/

#include "cocaine/unicorn.hpp"
#include "cocaine/unicorn/value.hpp"
#include "cocaine/unicorn/handlers.hpp"
#include "cocaine/zookeeper/handler.hpp"
#include "cocaine/zookeeper/exception.hpp"

#include <cocaine/context.hpp>
#include <asio/io_service.hpp>
#include <cocaine/zookeeper/exception.hpp>

#include <memory>

using namespace cocaine::unicorn;

namespace cocaine {
namespace service {

namespace {
/**
* Converts dynamic_t to zookepers config.
*/
zookeeper::cfg_t make_zk_config(const dynamic_t& args) {
    const auto& cfg = args.as_object();
    const auto& endpoints_cfg = cfg.at("endpoints", dynamic_t::empty_array).as_array();
    std::vector<zookeeper::cfg_t::endpoint_t> endpoints;
    for (size_t i = 0; i < endpoints_cfg.size(); i++) {
        endpoints.emplace_back(endpoints_cfg[i].as_object().at("host").as_string(), endpoints_cfg[i].as_object().at("port").as_uint());
    }
    if (endpoints.empty()) {
        endpoints.emplace_back("localhost", 2181);
    }
    return zookeeper::cfg_t(endpoints, cfg.at("recv_timeout", 1000u).as_uint());
}
}

void release_lock(unicorn_service_t* service, const path_t& path) {
    try {
        service->zk.del(path, NOT_EXISTING_VERSION, nullptr);
    }
    catch(const zookeeper::exception& e) {
        COCAINE_LOG_WARNING(service->log, "ZK Exception during delete of lock: %s. Reconnecting to discard lock for sure.", e.what());
        try {
            service->zk.reconnect();
        }
        catch(const zookeeper::exception& e) {
            COCAINE_LOG_WARNING(service->log, "Give up on deleting lock. Exception: %s", e.what());
        }
    }
}

const io::basic_dispatch_t&
unicorn_service_t::prototype() const {
    return *this;
}

unicorn_service_t::unicorn_service_t(context_t& context, asio::io_service& _asio, const std::string& _name, const dynamic_t& args) :
    service_t(context, _asio, _name, args),
    dispatch<io::unicorn_tag>(_name),
    name(_name),
    zk_session(),
    zk(make_zk_config(args), zk_session),
    log(context.log("unicorn"))
{
    using namespace std::placeholders;

    on<io::unicorn::subscribe> (std::make_shared<subscribe_slot_t> (this, &unicorn_dispatch_t::subscribe));
    on<io::unicorn::lsubscribe>(std::make_shared<lsubscribe_slot_t>(this, &unicorn_dispatch_t::lsubscribe));
    on<io::unicorn::put>       (std::make_shared<put_slot_t>       (this, &unicorn_dispatch_t::put));
    on<io::unicorn::create>    (std::make_shared<create_slot_t>    (this, &unicorn_dispatch_t::create));
    on<io::unicorn::del>       (std::make_shared<del_slot_t>       (this, &unicorn_dispatch_t::del));
    on<io::unicorn::increment> (std::make_shared<increment_slot_t> (this, &unicorn_dispatch_t::increment));
    on<io::unicorn::lock>      (std::make_shared<lock_slot_t>      (this));

}

unicorn_dispatch_t::unicorn_dispatch_t(const std::string& name, unicorn_service_t* _service) :
    dispatch<io::unicorn_final_tag>(name),
    service(_service),
    handler_scope(std::make_shared<zookeeper::handler_scope_t>())
{
}


unicorn_dispatch_t::response::put
unicorn_dispatch_t::put(path_t path, value_t value, version_t version) {
    response::put result;
    auto& handler = handler_scope->get_handler<put_action_t>(
        service,
        result,
        std::move(path),
        std::move(value),
        std::move(version)
    );
    service->zk.put(handler.path, handler.encoded_value, handler.version, handler);
    return result;
}

unicorn_dispatch_t::response::create
unicorn_dispatch_t::create(path_t path, value_t value) {
    response::create result;
    auto& handler = handler_scope->get_handler<create_action_t>(
        service,
        result,
        std::move(path),
        std::move(value)
    );
    service->zk.create(handler.path, handler.encoded_value, handler.ephemeral, handler);
    return result;
}

unicorn_dispatch_t::response::del
unicorn_dispatch_t::del(path_t path, version_t version) {
    response::del result;
    auto handler = std::make_unique<del_action_t>(result);
    service->zk.del(path, version, std::move(handler));
    return result;
}

unicorn_dispatch_t::response::subscribe
unicorn_dispatch_t::subscribe(path_t path) {
    unicorn_dispatch_t::response::subscribe result;
    auto& handler = handler_scope->get_handler<subscribe_action_t>(result, service, std::move(path));
    service->zk.get(handler.path, handler, handler);
    return result;
}

unicorn_dispatch_t::response::lsubscribe
unicorn_dispatch_t::lsubscribe(path_t path) {
    unicorn_dispatch_t::response::lsubscribe result;
    auto& handler = handler_scope->get_handler<lsubscribe_action_t>(result, service, std::move(path));
    service->zk.childs(handler.path, handler, handler);
    return result;
}

unicorn_dispatch_t::response::increment
unicorn_dispatch_t::increment(path_t path, value_t value) {
    unicorn_dispatch_t::response::increment result;
    if (!value.is_double() && !value.is_int() && !value.is_uint()) {
        int rc = zookeeper::ZOO_EXTRA_ERROR::INVALID_TYPE;
        result.abort(rc, zookeeper::get_error_message(rc));
        return result;
    }
    auto& handler = handler_scope->get_handler<increment_action_t>(
        service,
        std::move(result),
        std::move(path),
        std::move(value),
        handler_scope
    );
    service->zk.get(handler.path, handler);
    return result;
}














/**************************************************
****************    PUT    ************************
**************************************************/


unicorn_dispatch_t::put_action_t::put_action_t(
    const zookeeper::handler_tag& tag,
    unicorn_service_t* _service,
    unicorn_dispatch_t::response::put _result,
    path_t _path,
    value_t _value,
    version_t _version
):
    managed_handler_base_t(tag),
    managed_stat_handler_base_t(tag),
    managed_data_handler_base_t(tag),
    service(_service),
    result(std::move(_result)),
    path(std::move(_path)),
    initial_value(std::move(_value)),
    encoded_value(serialize(initial_value)),
    version(std::move(_version))
{}

void
unicorn_dispatch_t::put_action_t::operator()(int rc, zookeeper::node_stat const& stat) {
    try {
        if (rc == ZBADVERSION) {
            service->zk.get(path, *this);
        }
        else if (rc != 0) {
            result.abort(rc, zookeeper::get_error_message(rc));
        }
        else {
            result.write(versioned_value_t(initial_value, stat.version));
        }
    }
    catch(const zookeeper::exception& e) {
        COCAINE_LOG_WARNING(service->log, "Failure during put action: %s", e.what());
    }
}

void
unicorn_dispatch_t::put_action_t::operator()(int rc, std::string value, zookeeper::node_stat const& stat) {
    if (rc) {
        result.abort(rc, "Failed get after version mismatch:" + zookeeper::get_error_message(rc));
    }
    else {
        try {
            result.write(versioned_value_t(unserialize(value), stat.version));
        }
        catch (const zookeeper::exception& e) {
            result.abort(e.code(), std::string("Error during new value get:") + e.what());
        }
    }
}



/**************************************************
*******************  CREATE  **********************
**************************************************/





unicorn_dispatch_t::create_action_base_t::create_action_base_t(
    const zookeeper::handler_tag& tag,
    unicorn_service_t* _service,
    path_t _path,
    value_t _value,
    bool _ephemeral
):
    zookeeper::managed_string_handler_base_t(tag),
    service(_service),
    path(std::move(_path)),
    initial_value(std::move(_value)),
    encoded_value(serialize(initial_value)),
    depth(0)
{}

void
unicorn_dispatch_t::create_action_base_t::operator()(int rc, zookeeper::value_t value) {
    try {
        if (rc == ZOK) {
            if (depth == 0) {
                finalize();
            }
            else if (depth == 1) {
                depth--;
                service->zk.create(path, encoded_value, ephemeral, *this);
            }
            else {
                depth--;
                service->zk.create(zookeeper::path_parent(path, depth), "", false, *this);
            }
        }
        else if (rc == ZNONODE) {
            depth++;
            service->zk.create(zookeeper::path_parent(path, depth), "", false, *this);
        }
        else {
            abort(rc);
        }
    }
    catch(const zookeeper::exception& e) {
        COCAINE_LOG_WARNING(service->log, "Could not create node hierarchy. Exception: %s", e.what());
        abort(e.code());
    }
}


unicorn_dispatch_t::create_action_t::create_action_t(
    const zookeeper::handler_tag& tag,
    unicorn_service_t* _service,
    unicorn_dispatch_t::response::create _result,
    path_t _path,
    value_t _value
):
    create_action_base_t(tag, _service, std::move(_path), std::move(_value), false),
    result(_result)
{}

void
unicorn_dispatch_t::create_action_t::finalize() {
    result.write(true);
}

void
unicorn_dispatch_t::create_action_t::abort(int rc) {
    result.abort(rc, zookeeper::get_error_message(rc));
}



/**************************************************
****************    DEL    ************************
**************************************************/

unicorn_dispatch_t::del_action_t::del_action_t(unicorn_dispatch_t::response::del _result) :
    result(std::move(_result)) {
}

void
unicorn_dispatch_t::del_action_t::operator()(int rc) {
    if (rc) {
        result.abort(rc, zookeeper::get_error_message(rc));
    }
    else {
        result.write(true);
    }
}




/**************************************************
*****************  SUBSCRIBE   ********************
**************************************************/

unicorn_dispatch_t::subscribe_action_t::subscribe_action_t(
    const zookeeper::handler_tag& tag,
    unicorn_dispatch_t::response::subscribe _result,
    unicorn_service_t* _service,
    path_t _path
) :
    managed_handler_base_t(tag),
    managed_data_handler_base_t(tag),
    managed_watch_handler_base_t(tag),
    managed_stat_handler_base_t(tag),
    result(std::move(_result)),
    service(_service),
    write_lock(),
    last_version(unicorn::MIN_VERSION),
    path(std::move(_path))
{}

void
unicorn_dispatch_t::subscribe_action_t::operator()(int rc, std::string value, const zookeeper::node_stat& stat) {
    if(rc == ZNONODE) {
        if(last_version != MIN_VERSION && last_version != NOT_EXISTING_VERSION) {
            result.abort(rc, zookeeper::get_error_message(rc));
        }
        else {
            // Write that node is not exist to client only first time.
            // After that set a watch to see when it will appear
            if(last_version == MIN_VERSION) {
                std::lock_guard<std::mutex> guard(write_lock);
                if (NOT_EXISTING_VERSION > last_version) {
                    result.write(versioned_value_t(value_t(), NOT_EXISTING_VERSION));
                }
            }
            try {
                service->zk.exists(path,*this, *this);
            }
            catch(const zookeeper::exception& e) {
                COCAINE_LOG_WARNING(service->log, "Failure during subscription: %s", e.what());
                result.abort(e.code(), e.what());
            }
        }
    }
    else if (rc != 0) {
        result.abort(rc, zookeeper::get_error_message(rc));
    }
    else if (stat.numChildren != 0) {
        rc = zookeeper::CHILD_NOT_ALLOWED;
        result.abort(rc, zookeeper::get_error_message(rc));
    }
    else {
        version_t new_version(stat.version);
        std::lock_guard<std::mutex> guard(write_lock);
        if (new_version > last_version) {
            last_version = new_version;
            value_t val;
            try {
                result.write(versioned_value_t(unserialize(value), new_version));
            }
            catch(const zookeeper::exception& e) {
                result.abort(e.code(), e.what());
            }
        }
    }
}

void
unicorn_dispatch_t::subscribe_action_t::operator()(int rc, zookeeper::node_stat const& stat) {
    // Someone created a node in a gap between
    // we received nonode and issued exists
    if(rc == ZOK) {
        try {
            service->zk.get(path, *this, *this);
        }
        catch(const zookeeper::exception& e)  {
            COCAINE_LOG_WARNING(service->log, "Failure during subscription: %s", e.what());
            result.abort(e.code(), e.what());
        }
    }
}

void
unicorn_dispatch_t::subscribe_action_t::operator()(int type, int state, zookeeper::path_t path) {
    try {
        service->zk.get(path, *this, *this);
    }
    catch(const zookeeper::exception& e)  {
        result.abort(e.code(), e.what());
        COCAINE_LOG_WARNING(service->log, "Failure during subscription: %s", e.what());
    }
}



/**************************************************
*****************   LSUBSCRIBE   ******************
**************************************************/


unicorn_dispatch_t::lsubscribe_action_t::lsubscribe_action_t(
    const zookeeper::handler_tag& tag,
    unicorn_dispatch_t::response::lsubscribe _result,
    unicorn_service_t* _service,
    path_t _path
) :
    managed_handler_base_t(tag),
    managed_strings_stat_handler_base_t(tag),
    managed_watch_handler_base_t(tag),
    result(std::move(_result)),
    service(_service),
    write_lock(),
    last_version(unicorn::MIN_VERSION),
    path(std::move(_path))
{}


void
unicorn_dispatch_t::lsubscribe_action_t::operator()(int rc, std::vector<std::string> childs, const zookeeper::node_stat& stat) {
    if (rc != 0) {
        result.abort(rc, zookeeper::get_error_message(rc));
    }
    else {
        version_t new_version(stat.cversion);
        std::lock_guard<std::mutex> guard(write_lock);
        if (new_version > last_version) {
            last_version = new_version;
            value_t val;
            result.write(std::make_tuple(new_version, childs));
        }
    }
}

void
unicorn_dispatch_t::lsubscribe_action_t::operator()(int type, int state, zookeeper::path_t path) {
    try {
        service->zk.childs(path, *this, *this);
    }
    catch(const zookeeper::exception& e) {
        result.abort(e.code(), e.what());
        COCAINE_LOG_WARNING(service->log, "Failure during subscription for childs: %s", e.what());
    }
}


/**************************************************
*****************   INCREMENT   *******************
**************************************************/

unicorn_dispatch_t::increment_action_t::increment_action_t(
    const zookeeper::handler_tag& tag,
    unicorn_service_t* _service,
    unicorn_dispatch_t::response::increment _result,
    path_t _path,
    value_t _increment,
    const std::shared_ptr<zookeeper::handler_scope_t>& _scope
):
    managed_handler_base_t(tag),
    managed_stat_handler_base_t(tag),
    managed_data_handler_base_t(tag),
    service(_service),
    result(std::move(_result)),
    path(std::move(_path)),
    increment(std::move(_increment)),
    total(),
    scope(_scope)
{}


void unicorn_dispatch_t::increment_action_t::operator()(int rc, zookeeper::node_stat const& stat) {
    if (rc == ZOK) {
        result.write(versioned_value_t(total, stat.version));
    }
    else if (rc == ZBADVERSION) {
        service->zk.get(path, *this);
    }
}

void
unicorn_dispatch_t::increment_action_t::operator()(int rc, zookeeper::value_t value, const zookeeper::node_stat& stat) {
    try {
        if(rc == ZNONODE) {
            auto scope_ptr = scope.lock();
            if (!scope_ptr) {
                return;
            }
            auto& create_handler = scope_ptr->get_handler<increment_create_action_t>(service, std::move(result), path, increment);
            service->zk.create(path, serialize(increment), false, create_handler);
        }
        else if (rc != ZOK) {
            result.abort(rc, "Error during value get:" + zookeeper::get_error_message(rc));
        }
        else {
            value_t parsed;
            if (!value.empty()) {
                parsed = unserialize(value);
            }
            if (stat.numChildren != 0) {
                rc = zookeeper::ZOO_EXTRA_ERROR::CHILD_NOT_ALLOWED;
                result.abort(rc, "Error during value get:" + zookeeper::get_error_message(rc));
                return;
            }
            if (!parsed.is_double() && !parsed.is_int() && !parsed.is_null() && !parsed.is_uint()) {
                rc = zookeeper::ZOO_EXTRA_ERROR::INVALID_TYPE;
                result.abort(rc, "Error during value get:" + zookeeper::get_error_message(rc));
                return;
            }
            if (parsed.is_double() || increment.is_double()) {
                total = parsed.to<double>() + increment.to<double>();
                service->zk.put(path, serialize(total), stat.version, *this);
            }
            else {
                total = parsed.to<int64_t>() + increment.to<int64_t>();
                service->zk.put(path, serialize(total), stat.version, *this);
            }
        }
    }
    catch(const zookeeper::exception& e) {
        COCAINE_LOG_WARNING(service->log, "Failure during get action of increment: %s", e.what());
        result.abort(e.code(), zookeeper::get_error_message(e.code()));
    }
}

unicorn_dispatch_t::increment_create_action_t::increment_create_action_t(
    const zookeeper::handler_tag& tag,
    unicorn_service_t* _service,
    unicorn_dispatch_t::response::increment _result,
    path_t _path,
    value_t _value
):
    create_action_base_t(tag, _service, std::move(_path), std::move(_value), false),
    result(_result)
{}

void
unicorn_dispatch_t::increment_create_action_t::finalize() {
    result.write(versioned_value_t(initial_value, version_t()));
}

void
unicorn_dispatch_t::increment_create_action_t::abort(int rc) {
    result.abort(rc, zookeeper::get_error_message(rc));
}

distributed_lock_t::put_ephemeral_context_t::put_ephemeral_context_t(
    const zookeeper::handler_tag& tag,
    unicorn_service_t* service,
    const std::shared_ptr<distributed_lock_t>& _parent,
    path_t _path,
    value_t _value,
    unicorn_dispatch_t::response::acquire _result
) :
    unicorn_dispatch_t::create_action_base_t(tag, service, std::move(_path), std::move(_value), true),
    parent(_parent),
    result(std::move(_result))
{}

void
distributed_lock_t::put_ephemeral_context_t::finalize() {
    auto parent_ptr = parent.lock();
    if(!parent_ptr) {
        //Client has gone and did not delete lock.
        release_lock(service, path);
    }
    auto ptr = parent_ptr->state.synchronize();
    if(ptr->discarded) {
        //Client has gone and did not delete lock.
        release_lock(service, path);
    }
    ptr->lock_acquired = true;
    result.write(true);
}

void
distributed_lock_t::put_ephemeral_context_t::abort(int rc) {
    result.abort(rc, zookeeper::get_error_message(rc));
}

lock_slot_t::lock_slot_t(unicorn_service_t* _service):
    service(_service)
{}

boost::optional<std::shared_ptr<const lock_slot_t::dispatch_type>>
lock_slot_t::operator()(tuple_type&& args, upstream_type&& upstream)
{
    //At least clang(Apple LLVM version 6.0) do not accept implicit return of shared_ptr
    return boost::optional<std::shared_ptr<const lock_slot_t::dispatch_type>> (
        //We use non const here because clang doesn't accept const
        std::make_shared<distributed_lock_t>(service->name, std::move(std::get<0>(args)), service)
    );
}

distributed_lock_t::distributed_lock_t(const std::string& name, path_t _path, unicorn_service_t* _service) :
    dispatch<io::unicorn_locked_tag>(name),
    path(_path),
    service(_service)
{
    on<io::unicorn::unlock>(std::bind(&distributed_lock_t::discard, this, std::error_code()));
    on<io::unicorn::acquire>(std::bind(&distributed_lock_t::acquire, this));
}


void
distributed_lock_t::discard(const std::error_code& ec) const {
    auto ptr = state.synchronize();
    if(ptr->lock_acquired) {
        release_lock(service, path);
    }
    ptr->discarded = true;
}

unicorn_dispatch_t::response::acquire
distributed_lock_t::acquire() {
    unicorn_dispatch_t::response::acquire result;
    auto& handler = handler_scope.get_handler<put_ephemeral_context_t>(service, shared_from_this(), path, value_t(time(nullptr)), result);
    service->zk.create(path, handler.encoded_value, handler.ephemeral, handler);
    return result;
}

}}

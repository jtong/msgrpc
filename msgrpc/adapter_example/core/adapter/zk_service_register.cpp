#include <adapter_example/core/adapter/zk_service_register.h>

namespace demo {
    using msgrpc::InstanceInfo;
    using msgrpc::instance_vector_t;
    using msgrpc::SRListener;

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    const string k_services_root = "/services";

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    struct instance_info_compare {
        bool operator() (const InstanceInfo &a, const InstanceInfo &b) const { return a.service_id_ < b.service_id_; }
    };

    typedef std::set<InstanceInfo, instance_info_compare> instance_set_t;

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    namespace {
        boost::optional<msgrpc::service_id_t> str_to_service_id(const string& endpoint) {
            size_t sep = endpoint.find(":");
            if (sep == string::npos) {
                return boost::none;
            }

            string ip = string(endpoint, 0, sep);
            unsigned short port = (unsigned short)strtoul(endpoint.c_str() + sep + 1, NULL, 0);

            return msgrpc::service_id_t(boost::asio::ip::address::from_string(ip), port);
        }

        string service_version_of(const string& endpoint) {
            size_t sep = endpoint.find("#");
            if (sep == string::npos) {
                return string();
            }

            return string(endpoint, sep + 1);
        }


        void strings_to_instances(const vector<string>& instance_strings, instance_vector_t& instances) {
            instance_set_t instance_set;

            for (const auto& si : instance_strings) {
                boost::optional<msgrpc::service_id_t> service_id = str_to_service_id(si);

                if (service_id) {
                    InstanceInfo ii;
                    ii.service_id_ = service_id.value();
                    ii.version_ = service_version_of(si);

                    instance_set.insert(ii);
                }
            }

            instances.assign(instance_set.begin(), instance_set.end());
        }


        bool is_zk_connected(const unique_ptr<ZKHandle> &zk) {
            return zk && (zk->getState() == ZOO_CONNECTED_STATE);
        }
    }


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void close_zk_connection_at_exit() {
        if (ZkServiceRegister::instance().zk_)
            ZkServiceRegister::instance().zk_->close();
    }


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool ZkServiceRegister::init() {
        auto listeners = msgrpc::InstancesCollector<SRListener>::instance().instances();
        for (auto& listener : listeners) {
            this->register_listener(*listener);
        }

        wait_util_zk_is_connected();
        return try_fetch_services_from_zk(services_cache_, old_services_);
    }

    bool ZkServiceRegister::register_service(const char* service_name, const char* version, const char *end_point) {
        if (service_name == nullptr || end_point == nullptr) {
            return false;
        }

        wait_util_zk_is_connected();

        return create_ephemeral_node_for_service_instance(service_name, version, end_point);
    }

    msgrpc::optional_service_id_t ZkServiceRegister::service_name_to_id(const char* service_name, const char* req, size_t req_len) {
        const auto& iter = services_cache_.find(service_name);
        if (iter == services_cache_.end()) {
            return boost::none;
        }

        instance_vector_t& instances = iter->second;
        if (instances.empty()) {
            return boost::none;
        }

        size_t size = instances.size();
        if (size == 1) {
            return instances[0].service_id_;
        }

        //using round-robin as default load-balance strategy
        size_t next_rr_index = round_robin_map_[service_name];
        round_robin_map_[service_name] = next_rr_index + 1;

        return instances[ next_rr_index % size ].service_id_;
    }

    msgrpc::instance_vector_t ZkServiceRegister::instances_of(const char* service_name) {
        msgrpc::instance_vector_t ___iv;
        const auto& iter = services_cache_.find(service_name);
        if (iter == services_cache_.end()) {
            return ___iv;
        }

        return iter->second;
    }

    void ZkServiceRegister::register_listener(SRListener& listener) {
        std::set<SRListener*>& listeners = listeners_map_[listener.service_to_listener()];
        listeners.insert(&listener);

        string service_name = listener.service_to_listener();

        auto iter = services_cache_.find(service_name);
        if (iter != services_cache_.end()) {
            this->do_notify_listeners(service_name, iter->second);
        }
    }

    void ZkServiceRegister::unregister_listener(SRListener& ___l) {
        auto iter = listeners_map_.find(___l.service_to_listener());
        if (iter == listeners_map_.end()) {
            return;
        }

        std::set<SRListener*>& listeners = iter->second;
        listeners.erase(&___l);
    }


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool ZkServiceRegister::try_connect_zk() {
        if (is_zk_connected(zk_)) {
            return true;
        }

        if ( ! zk_) {
            std::atexit(close_zk_connection_at_exit);
        }

        //TODO: read zk server address from configuration
        auto zk = ConservatorFrameworkFactory().newClient("localhost:2181");
        try {
            zk->start();
        } catch (const char* msg) {
            zk->close();
            ___log_error("catched exception during zk_start: %s", msg);
            return false;
        }

        if ( ! is_zk_connected(zk)) {
            zk->close();
            return false;
        }

        zk->checkExists()->withWatcher(session_watcher_fn, this)->forPath("/");
        zk_ = std::move(zk);
        return true;
    }

    void ZkServiceRegister::wait_util_zk_is_connected() {
        bool connected;

        do {
            if ( ! (connected = try_connect_zk())) {
                ___log_warning("connect to zookeeper failed, will continue retry.");
            }
        } while( ! connected);
    }

    //TODO: peroidically test existence of ephemeral node of service, and create the ephemeral node if need.
    //      should re-register into zk, if zk's data was accidentally deleted.

    //TODO: should periodically fetch services/instances info from zk,
    //      incase miss notifications between handling watch notification and set watch again.

    bool ZkServiceRegister::create_ephemeral_node_for_service_instance(const char* service_name, const char* version, const char *end_point) {
        int ret;
        ret = zk_->create()->forPath(k_services_root);
        zk_->getChildren()->withWatcher(service_child_watcher_fn, this)->forPath(k_services_root);

        ret = zk_->create()->forPath(k_services_root + "/" + service_name);

        const clientid_t* session_id = zoo_client_id(zk_->handle());
        if (session_id == nullptr) {
            ___log_error("can not get session id of zookeeper client.");
            return false;
        }

        string path = k_services_root + "/" + service_name + "/" + end_point + "@" + std::to_string(session_id->client_id) + "#" + version;
        ret = zk_->create()->withFlags(ZOO_EPHEMERAL)->forPath(path, end_point);

        bool result = (ZOK == ret || ZNODEEXISTS == ret);
        if (!result) {
            ___log_error("register service on zk failed, path: %s, zk_reuslt: %d", path.c_str(), ret);
        }

        return result;
    }


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void ZkServiceRegister::session_watcher_fn(zhandle_t *zh, int type, int state, const char *path, void *watcher_ctxt) {
        if (type == ZOO_SESSION_EVENT) {

            if (ZOO_CONNECTING_STATE == state) {
                ___log_warning("zookeeper connection status changed to: ZOO_CONNECTING_STATE");
            } else if (ZOO_CONNECTED_STATE == state) {
                ___log_warning("zookeeper connection status changed to: ZOO_CONNECTED_STATE");
            } else {
                ___log_warning("zookeeper connection status changed to: %d", state);
            }
        }
    }

    void ZkServiceRegister::service_child_watcher_fn(zhandle_t *zh, int type, int state, const char *path, void *watcher_ctxt) {
        if (type != ZOO_CHILD_EVENT) {
            return;
        }

        auto* srv_register = (ZkServiceRegister*)watcher_ctxt;

        vector<string> services = srv_register->try_fetch_services();
        {
            vector<string> changed_services;
            {
                std::sort(services.begin(), services.end());
                set_difference(services.begin(), services.end(), old_services_.begin(), old_services_.end(), back_inserter(changed_services) );
                set_difference(old_services_.begin(), old_services_.end(), services.begin(), services.end(), back_inserter(changed_services) );
            }

            for (const auto& ___s : changed_services) {
                fetch_and_update_instances(srv_register, ___s);
            }
        }

        old_services_ = services;
    }


    void ZkServiceRegister::instance_child_watcher_fn(zhandle_t *zh, int type, int state, const char *path, void *watcher_ctxt) {
        if (type != ZOO_CHILD_EVENT) {
            return;
        }

        assert(strlen(path) > k_services_root.length() + 1 /* / */  && "expects path starts with /services/");
        string service_name = path + k_services_root.length() + 1 /* / */;

        fetch_and_update_instances((ZkServiceRegister*)watcher_ctxt, service_name);
    }


    void ZkServiceRegister::fetch_and_update_instances(ZkServiceRegister* srv_register, const string &service_name) {
        instance_vector_t ___iv;

        bool fetch_ok = srv_register->fetch_service_instances_from_zk(service_name, ___iv);

        if (fetch_ok) {
            msgrpc::Task::dispatch_async_to_main_queue(
                    [srv_register, service_name, ___iv] {
                        srv_register->services_cache_[service_name] = ___iv;

                        srv_register->do_notify_listeners(service_name, ___iv);
                    }
            );
        }
    }


    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
    vector<string> ZkServiceRegister::try_fetch_services() {
        return zk_->getChildren()->withWatcher(service_child_watcher_fn, this)->forPath(k_services_root);
    }

    void ZkServiceRegister::do_notify_listeners(const string& service_name, const instance_vector_t& ___iv) {
        auto iter = listeners_map_.find(service_name);
        if (iter != listeners_map_.end()) {
            for (auto &___l : iter->second) {
                ___l->on_changes(___iv);
            }
        }
    }

    bool ZkServiceRegister::try_fetch_services_from_zk(services_cache_t& cache, vector<string>& services) {
        bool connected = try_connect_zk();
        if (!connected) {
            ___log_error("try_fetch_services_from_zk failed, can not connect to zk.");
            return false;
        }

        vector<string> latest_services = zk_->getChildren()->withWatcher(service_child_watcher_fn, this)->forPath(k_services_root);

        services = latest_services;
        std::sort(services.begin(), services.end());

        for (const auto& service : latest_services) {
            instance_vector_t ___iv;

            bool fetch_ok = fetch_service_instances_from_zk(service, ___iv);
            if (fetch_ok) {

                auto filters = msgrpc::InstancesCollector<msgrpc::ServiceFilter>::instance().instances();
                for (auto& filter : filters) {
                    ___iv = filter->filter_service(service, ___iv);
                }

                cache[service] = ___iv;
                do_notify_listeners(service, ___iv);
            }
        }

        return true;
    }

    bool ZkServiceRegister::fetch_service_instances_from_zk(const string& service, instance_vector_t& instances) {
        bool connected = try_connect_zk();
        if (!connected) {
            ___log_error("fetch_service_instances_from_zk failed, can not connect to zk.");
            return false;
        }

        string service_path = k_services_root + "/" + service;
        vector<string> instance_strings = zk_->getChildren()->withWatcher(instance_child_watcher_fn, this)->forPath(service_path);

        for (string service_instance : instance_strings) {
            ___log_debug("    %s instance : %s", service.c_str(), service_instance.c_str());
        }

        strings_to_instances(instance_strings, instances);
        return true;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    vector<string> ZkServiceRegister::old_services_;   //owned by zk client thread
}

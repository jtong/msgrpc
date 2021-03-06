#ifndef PROJECT_COMBINED_RESOLVER_H
#define PROJECT_COMBINED_RESOLVER_H

#include <msgrpc/util/singleton.h>
#include <msgrpc/core/service_discovery/service_resolver.h>

namespace msgrpc {

    template<typename DEFAULT_RESOLVER, typename... RESOLVER>
    struct ServiceResolvers
            : ServiceResolver , Singleton<ServiceResolvers<DEFAULT_RESOLVER, RESOLVER...>> {

        ServiceResolvers();
        virtual optional_service_id_t service_name_to_id(const char* service_name, const char* req, size_t req_len) override;

      private:
        std::map<std::string, ServiceResolver*> resolvers_;
    };

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    template<typename DEFAULT_RESOLVER, typename... RESOLVER>
    ServiceResolvers<DEFAULT_RESOLVER, RESOLVER...>::ServiceResolvers()
            : resolvers_({
                                 {RESOLVER::service_name_to_resolve_, &RESOLVER::instance()}...
                         })
    {/**/}


    template<typename DEFAULT_RESOLVER, typename... RESOLVER>
    optional_service_id_t
    ServiceResolvers<DEFAULT_RESOLVER, RESOLVER...>::service_name_to_id(const char* service_name, const char* req, size_t req_len) {
        //1. resolve service instance by service specific resolver
        auto iter = resolvers_.find(service_name);
        if (iter != resolvers_.end()) {
            return iter->second->service_name_to_id(service_name, req, req_len);
        }

        //2. using default resolver, if can not find service specific resolver
        return DEFAULT_RESOLVER::instance().service_name_to_id(service_name, req, req_len);
    }

}

#endif //PROJECT_COMBINED_RESOLVER_H

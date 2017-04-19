#ifndef MSGRPC_MSG_CHANNEL_H
#define MSGRPC_MSG_CHANNEL_H

#include <msgrpc/core/typedefs.h>
#include <cstddef>

namespace msgrpc {

    struct MsgChannel {
        virtual bool send_msg(const service_id_t& remote_service_id, msg_id_t msg_id, const char* buf, size_t len) const = 0;
    };

}

#endif //MSGRPC_MSG_CHANNEL_H
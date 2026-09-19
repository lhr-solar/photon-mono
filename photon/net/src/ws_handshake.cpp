#include "photon/ws_handshake.hpp"

#include "photon/base64.hpp"
#include "photon/sha1.hpp"

#include <string>

namespace photon::ws {

std::string acceptKey(std::string_view clientKey) {
    std::string joined;
    joined.reserve(clientKey.size() + kGuid.size());
    joined.append(clientKey);
    joined.append(kGuid);
    const auto digest = crypto::sha1(joined);
    return crypto::base64Encode(digest);
}

}  // namespace photon::ws

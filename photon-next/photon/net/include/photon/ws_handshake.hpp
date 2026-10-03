#pragma once

#include <string>
#include <string_view>

namespace photon::ws {

inline constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// RFC 6455 Sec-WebSocket-Accept = Base64( SHA1( key + GUID ) )
std::string acceptKey(std::string_view clientKey);

}  // namespace photon::ws

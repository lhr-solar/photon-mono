#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace photon::crypto {

std::string base64Encode(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> base64Decode(std::string_view text);

}  // namespace photon::crypto

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace photon::crypto {

std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data);
std::array<std::uint8_t, 20> sha1(std::string_view data);

}  // namespace photon::crypto

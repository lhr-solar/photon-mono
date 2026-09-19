#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace photon::auth {

inline constexpr char kMagic[4] = {'P', 'H', 'A', 'U'};
inline constexpr std::size_t kPasswordMax = 64;

inline bool encode(std::string_view password, std::vector<std::uint8_t>& out) {
    if (password.empty() || password.size() > kPasswordMax) {
        return false;
    }
    out.resize(5 + password.size());
    out[0] = static_cast<std::uint8_t>(kMagic[0]);
    out[1] = static_cast<std::uint8_t>(kMagic[1]);
    out[2] = static_cast<std::uint8_t>(kMagic[2]);
    out[3] = static_cast<std::uint8_t>(kMagic[3]);
    out[4] = static_cast<std::uint8_t>(password.size());
    for (std::size_t i = 0; i < password.size(); ++i) {
        out[5 + i] = static_cast<std::uint8_t>(password[i]);
    }
    return true;
}

}  // namespace photon::auth

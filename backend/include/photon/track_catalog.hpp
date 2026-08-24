#pragma once

#include "photon/tracksim.hpp"

#include <optional>
#include <string>
#include <vector>

namespace photon {

class TrackCatalog {
public:
    explicit TrackCatalog(std::string tracksDir);

    static std::string discoverAssetsDir();
    static std::string tracksDirFromAssets(const std::string& assetsDir);

    const std::string& tracksDir() const { return tracksDir_; }
    std::vector<tracksim::TrackInfo> list() const;
    std::optional<tracksim::Track> load(const std::string& id, std::string* error = nullptr) const;

private:
    std::string tracksDir_;
};

}  // namespace photon

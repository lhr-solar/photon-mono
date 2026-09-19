#include "photon/track_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace photon {
namespace {

namespace fs = std::filesystem;

bool isSafeTrackId(std::string_view id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (char c : id) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

bool looksLikeTracksDir(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec);
}

std::optional<std::string> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

std::string TrackCatalog::tracksDirFromAssets(const std::string& assetsDir) {
    return (fs::path(assetsDir) / "tracks").string();
}

std::string TrackCatalog::discoverAssetsDir() {
#ifdef PHOTON_ASSETS_DIR
    if (const char* compiled = PHOTON_ASSETS_DIR; compiled != nullptr && compiled[0] != '\0') {
        if (looksLikeTracksDir(fs::path(compiled) / "tracks")) {
            return compiled;
        }
    }
#endif
#if defined(_WIN32)
    char envBuf[1024];
    std::size_t envLen = 0;
    if (getenv_s(&envLen, envBuf, sizeof(envBuf), "PHOTON_ASSETS_DIR") == 0 && envLen > 1) {
        if (looksLikeTracksDir(fs::path(envBuf) / "tracks")) {
            return std::string(envBuf);
        }
    }
#else
    if (const char* env = std::getenv("PHOTON_ASSETS_DIR")) {
        if (env[0] != '\0' && looksLikeTracksDir(fs::path(env) / "tracks")) {
            return env;
        }
    }
#endif

    const fs::path cwd = fs::current_path();
    const fs::path candidates[] = {
        cwd / "assets",
        cwd / ".." / "assets",
        cwd.parent_path() / "assets",
        cwd.parent_path().parent_path() / "assets",
    };
    for (const auto& dir : candidates) {
        if (looksLikeTracksDir(dir / "tracks")) {
            return fs::weakly_canonical(dir).string();
        }
    }
    return (cwd / "assets").string();
}

TrackCatalog::TrackCatalog(std::string tracksDir) : tracksDir_(std::move(tracksDir)) {}

std::vector<tracksim::TrackInfo> TrackCatalog::list() const {
    std::vector<tracksim::TrackInfo> out;
    std::error_code ec;
    if (!fs::is_directory(tracksDir_, ec)) {
        return out;
    }
    for (const auto& entry : fs::directory_iterator(tracksDir_, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() != ".json") {
            continue;
        }
        const std::string id = entry.path().stem().string();
        if (!isSafeTrackId(id)) {
            continue;
        }
        std::string error;
        if (auto track = load(id, &error)) {
            out.push_back(tracksim::infoOf(*track));
        }
    }
    std::sort(out.begin(), out.end(), [](const tracksim::TrackInfo& a, const tracksim::TrackInfo& b) {
        return a.id < b.id;
    });
    return out;
}

std::optional<tracksim::Track> TrackCatalog::load(const std::string& id, std::string* error) const {
    if (!isSafeTrackId(id)) {
        if (error) {
            *error = "invalid track_id";
        }
        return std::nullopt;
    }
    const fs::path path = fs::path(tracksDir_) / (id + ".json");
    const auto text = readFile(path);
    if (!text) {
        if (error) {
            *error = "unknown track_id";
        }
        return std::nullopt;
    }
    auto track = tracksim::parseTrack(*text, error);
    if (track && track->id.empty()) {
        track->id = id;
    }
    return track;
}

}  // namespace photon

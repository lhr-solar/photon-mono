#pragma once

#include "photon/canp.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace photon::vsd1 {

// On-disk VSD1 segment framing from photon-server/server/store.go.
// magic 0x56534431 ("VSD1"), 24-byte big-endian header, payload is raw CANP.
// Zip archives are not supported (no stdlib unzip); pass a .log file or a
// directory of .log segments (also accepts nested logs/ or segments/).
inline constexpr std::uint32_t kMagic = 0x56534431u;
inline constexpr std::size_t kHeaderSize = 24;

struct FrameHeader {
    std::uint32_t payloadBytes = 0;
    std::uint64_t id = 0;
    std::uint32_t ingestUnixS = 0;
    std::uint32_t crc = 0;
};

struct IndexedBatch {
    std::uint64_t timestampMs = 0;
    std::size_t fileIndex = 0;
    std::uint64_t frameOffset = 0;
    std::uint32_t payloadBytes = 0;
    // Heap pointer so file-backed indexes stay ~40B/batch (optional<Batch> is ~1.8KB).
    std::unique_ptr<canp::Batch> batch;
};

struct Archive {
    std::vector<std::filesystem::path> files;
    std::vector<IndexedBatch> batches;
    std::uint64_t minTimeMs = 0;
    std::uint64_t maxTimeMs = 0;
    bool complete = true;

    bool empty() const { return batches.empty(); }
};

// Parse one aligned VSD1 header at the start of `bytes`. Does not require a
// valid CRC (legacy Photon log replay also skips CRC).
bool parseHeader(std::span<const std::uint8_t> bytes, FrameHeader& out);

// Walk concatenated VSD1 frames and extract CANP v3 batches (in-memory).
std::optional<Archive> indexBytes(std::span<const std::uint8_t> bytes, std::string* error = nullptr);

// Single file or directory of .log segments. `.zip` is rejected with an error.
// `cancel` is polled between frames; when set, returns nullopt with an error.
std::optional<Archive> indexPath(const std::filesystem::path& path, std::string* error = nullptr,
                                 std::atomic<bool>* cancel = nullptr);

// Sidecar next to a single .log (`file.log.photon-idx`) or `.photon-idx` in a
// directory. Validated against each file's size + mtime.
std::optional<Archive> tryLoadIndexCache(const std::vector<std::filesystem::path>& files);
bool saveIndexCache(const Archive& archive);
void finishArchive(Archive& archive);

// Stream each CANP batch without keeping payloads. Used for early playback.
bool walkLogFiles(const std::vector<std::filesystem::path>& files, std::atomic<bool>* cancel,
                  std::string* error, const std::function<void(IndexedBatch&&)>& visit);

// Cheap directory listing (no payload read). Resolves nested logs/ or segments/.
std::vector<std::filesystem::path> listLogFiles(const std::filesystem::path& path);

std::vector<canp::Batch> extractCanp(std::span<const std::uint8_t> payload);

// First batch with timestampMs >= cursor, or batches.size() if none.
std::size_t seekIndex(const Archive& archive, std::uint64_t cursorMs);

// Load the CANP batch for an indexed frame (file-backed archives).
std::optional<canp::Batch> readBatch(const Archive& archive, const IndexedBatch& item,
                                     std::string* error = nullptr);

// Playhead over an archive: play/pause/seek. Live is a documented no-op.
class ReplayCursor {
public:
    enum class Mode { Pause, Play, Live };

    void bind(const Archive& archive);
    void setMode(Mode mode);
    void seek(std::uint64_t cursorMs);
    void setSpeed(double speed);

    Mode mode() const { return mode_; }
    double speed() const { return speed_; }
    std::uint64_t cursorMs() const { return cursorMs_; }
    std::size_t playIndex() const { return playIndex_; }

    // Advance wall-clock dt (already multiplied by the caller if needed).
    // Returns [begin, end) indices into archive.batches that should emit now.
    struct EmitRange {
        std::size_t begin = 0;
        std::size_t end = 0;
    };
    EmitRange advance(std::uint64_t wallDtMs);

private:
    const Archive* archive_ = nullptr;
    Mode mode_ = Mode::Pause;
    double speed_ = 1.0;
    std::uint64_t cursorMs_ = 0;
    std::size_t playIndex_ = 0;
};

}  // namespace photon::vsd1

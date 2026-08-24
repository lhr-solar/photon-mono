#include "photon/vsd1.hpp"

#include "photon/endian.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <utility>

namespace photon::vsd1 {
namespace {

namespace fs = std::filesystem;

std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool isZip(const fs::path& path) {
    const auto ext = toLower(path.extension().string());
    return ext == ".zip";
}

bool isLog(const fs::path& path) {
    const auto ext = toLower(path.extension().string());
    return ext == ".log";
}

std::vector<fs::path> collectLogFiles(const fs::path& root) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        files.push_back(root);
        return files;
    }
    if (!fs::is_directory(root, ec)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (isLog(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

fs::path resolveRoot(const fs::path& requested) {
    std::error_code ec;
    if (fs::is_directory(requested, ec)) {
        const auto nestedLogs = requested / "logs";
        if (fs::is_directory(nestedLogs, ec) && !collectLogFiles(nestedLogs).empty()) {
            return nestedLogs;
        }
        const auto nestedSeg = requested / "segments";
        if (fs::is_directory(nestedSeg, ec) && !collectLogFiles(nestedSeg).empty()) {
            return nestedSeg;
        }
    }
    return requested;
}

void indexPayload(Archive& archive, std::span<const std::uint8_t> payload, std::size_t fileIndex,
                  std::uint64_t frameOffset, std::uint32_t payloadBytes, bool keepBatch) {
    auto batches = extractCanp(payload);
    for (auto& batch : batches) {
        IndexedBatch item;
        item.timestampMs = batch.timestamp;
        item.fileIndex = fileIndex;
        item.frameOffset = frameOffset;
        item.payloadBytes = payloadBytes;
        if (keepBatch) {
            item.batch = std::make_unique<canp::Batch>(std::move(batch));
        }
        archive.batches.push_back(std::move(item));
    }
}

void finishArchive(Archive& archive) {
    std::sort(archive.batches.begin(), archive.batches.end(),
              [](const IndexedBatch& a, const IndexedBatch& b) {
                  if (a.timestampMs != b.timestampMs) {
                      return a.timestampMs < b.timestampMs;
                  }
                  if (a.fileIndex != b.fileIndex) {
                      return a.fileIndex < b.fileIndex;
                  }
                  return a.frameOffset < b.frameOffset;
              });
    if (archive.batches.empty()) {
        archive.minTimeMs = 0;
        archive.maxTimeMs = 0;
        return;
    }
    archive.minTimeMs = archive.batches.front().timestampMs;
    archive.maxTimeMs = archive.batches.back().timestampMs;
}

bool indexFile(Archive& archive, const fs::path& path, std::size_t fileIndex, std::string* error,
               std::atomic<bool>* cancel) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot open VSD1 segment: " + path.string();
        }
        return false;
    }
    std::vector<std::uint8_t> header(kHeaderSize);
    std::vector<std::uint8_t> payload;
    std::size_t frames = 0;
    std::uint64_t offset = 0;
    while (in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(kHeaderSize))) {
        if (cancel != nullptr && (frames & 0x3Fu) == 0 && cancel->load(std::memory_order_relaxed)) {
            if (error) {
                *error = "index cancelled";
            }
            return false;
        }
        ++frames;
        FrameHeader fh{};
        if (!parseHeader(header, fh)) {
            in.seekg(static_cast<std::streamoff>(offset + 1), std::ios::beg);
            offset += 1;
            continue;
        }
        payload.resize(fh.payloadBytes);
        if (fh.payloadBytes > 0 &&
            !in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(fh.payloadBytes))) {
            break;
        }
        indexPayload(archive, payload, fileIndex, offset, fh.payloadBytes, false);
        offset += kHeaderSize + static_cast<std::uint64_t>(fh.payloadBytes);
    }
    return true;
}

}  // namespace

bool parseHeader(std::span<const std::uint8_t> bytes, FrameHeader& out) {
    if (bytes.size() < kHeaderSize) {
        return false;
    }
    std::size_t off = 0;
    std::uint32_t magic = 0;
    if (!readU32Be(bytes, off, magic) || magic != kMagic) {
        return false;
    }
    if (!readU32Be(bytes, off, out.payloadBytes) || !readU64Be(bytes, off, out.id) ||
        !readU32Be(bytes, off, out.ingestUnixS) || !readU32Be(bytes, off, out.crc)) {
        return false;
    }
    return true;
}

std::vector<canp::Batch> extractCanp(std::span<const std::uint8_t> payload) {
    std::vector<canp::Batch> out;
    canp::StreamParser parser;
    parser.feed(payload);
    while (auto batch = parser.take()) {
        out.push_back(std::move(*batch));
    }
    return out;
}

std::optional<Archive> indexBytes(std::span<const std::uint8_t> bytes, std::string* error) {
    Archive archive;
    std::size_t off = 0;
    while (off + kHeaderSize <= bytes.size()) {
        FrameHeader fh{};
        if (!parseHeader(bytes.subspan(off), fh)) {
            ++off;
            continue;
        }
        if (off + kHeaderSize + fh.payloadBytes > bytes.size()) {
            break;
        }
        const auto payload = bytes.subspan(off + kHeaderSize, fh.payloadBytes);
        indexPayload(archive, payload, 0, off, fh.payloadBytes, true);
        off += kHeaderSize + fh.payloadBytes;
    }
    if (archive.batches.empty()) {
        if (error) {
            *error = "no VSD1/CANP batches in buffer";
        }
        return std::nullopt;
    }
    finishArchive(archive);
    return archive;
}

std::vector<fs::path> listLogFiles(const fs::path& path) {
    return collectLogFiles(resolveRoot(path));
}

std::optional<Archive> indexPath(const fs::path& path, std::string* error, std::atomic<bool>* cancel) {
    if (isZip(path)) {
        if (error) {
            *error = "log_replay does not unzip archives; extract segments first";
        }
        return std::nullopt;
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (error) {
            *error = "log path does not exist";
        }
        return std::nullopt;
    }
    Archive archive;
    archive.files = collectLogFiles(resolveRoot(path));
    if (archive.files.empty()) {
        if (error) {
            *error = "no VSD1 .log files at path";
        }
        return std::nullopt;
    }
    for (std::size_t i = 0; i < archive.files.size(); ++i) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            if (error) {
                *error = "index cancelled";
            }
            return std::nullopt;
        }
        std::string fileError;
        if (!indexFile(archive, archive.files[i], i, &fileError, cancel)) {
            if (error) {
                *error = fileError;
            }
            return std::nullopt;
        }
    }
    if (archive.batches.empty()) {
        if (error) {
            *error = "segments have no readable VSD1/CANP frames";
        }
        return std::nullopt;
    }
    finishArchive(archive);
    return archive;
}

std::size_t seekIndex(const Archive& archive, std::uint64_t cursorMs) {
    const auto it =
        std::lower_bound(archive.batches.begin(), archive.batches.end(), cursorMs,
                         [](const IndexedBatch& b, std::uint64_t t) { return b.timestampMs < t; });
    return static_cast<std::size_t>(it - archive.batches.begin());
}

std::optional<canp::Batch> readBatch(const Archive& archive, const IndexedBatch& item, std::string* error) {
    if (item.batch) {
        return *item.batch;
    }
    if (item.fileIndex >= archive.files.size()) {
        if (error) {
            *error = "invalid VSD1 file index";
        }
        return std::nullopt;
    }
    std::ifstream in(archive.files[item.fileIndex], std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot reopen VSD1 segment";
        }
        return std::nullopt;
    }
    in.seekg(static_cast<std::streamoff>(item.frameOffset), std::ios::beg);
    std::vector<std::uint8_t> header(kHeaderSize);
    if (!in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(kHeaderSize))) {
        if (error) {
            *error = "short VSD1 header";
        }
        return std::nullopt;
    }
    FrameHeader fh{};
    if (!parseHeader(header, fh)) {
        if (error) {
            *error = "bad VSD1 magic at stored offset";
        }
        return std::nullopt;
    }
    std::vector<std::uint8_t> payload(fh.payloadBytes);
    if (fh.payloadBytes > 0 &&
        !in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(fh.payloadBytes))) {
        if (error) {
            *error = "short VSD1 payload";
        }
        return std::nullopt;
    }
    auto batches = extractCanp(payload);
    if (batches.empty()) {
        if (error) {
            *error = "VSD1 payload is not CANP v3";
        }
        return std::nullopt;
    }
    for (auto& batch : batches) {
        if (batch.timestamp == item.timestampMs) {
            return std::move(batch);
        }
    }
    return std::move(batches.front());
}

void ReplayCursor::bind(const Archive& archive) {
    archive_ = &archive;
    mode_ = Mode::Play;
    speed_ = 1.0;
    playIndex_ = 0;
    cursorMs_ = archive.minTimeMs;
}

void ReplayCursor::setMode(Mode mode) {
    if (mode == Mode::Live) {
        return;
    }
    mode_ = mode;
}

void ReplayCursor::seek(std::uint64_t cursorMs) {
    if (archive_ == nullptr || archive_->empty()) {
        cursorMs_ = cursorMs;
        playIndex_ = 0;
        return;
    }
    if (cursorMs < archive_->minTimeMs) {
        cursorMs = archive_->minTimeMs;
    }
    if (cursorMs > archive_->maxTimeMs) {
        cursorMs = archive_->maxTimeMs;
    }
    cursorMs_ = cursorMs;
    playIndex_ = seekIndex(*archive_, cursorMs_);
}

void ReplayCursor::setSpeed(double speed) {
    if (speed > 0.0) {
        speed_ = speed;
    }
}

ReplayCursor::EmitRange ReplayCursor::advance(std::uint64_t wallDtMs) {
    EmitRange range;
    if (archive_ == nullptr || archive_->empty() || mode_ != Mode::Play) {
        return range;
    }
    const double scaled = static_cast<double>(wallDtMs) * speed_;
    const auto dt = static_cast<std::uint64_t>(scaled < 0.0 ? 0.0 : scaled);
    const std::uint64_t nextCursor = cursorMs_ + dt;
    const std::uint64_t cap = archive_->maxTimeMs;
    cursorMs_ = nextCursor > cap ? cap : nextCursor;
    if (playIndex_ > archive_->batches.size()) {
        playIndex_ = archive_->batches.size();
    }
    if (playIndex_ == archive_->batches.size() && cursorMs_ < archive_->maxTimeMs) {
        playIndex_ = seekIndex(*archive_, cursorMs_);
    }
    range.begin = playIndex_;
    while (playIndex_ < archive_->batches.size() && archive_->batches[playIndex_].timestampMs <= cursorMs_) {
        ++playIndex_;
        if (playIndex_ - range.begin >= 64) {
            cursorMs_ = archive_->batches[playIndex_ - 1].timestampMs;
            break;
        }
    }
    range.end = playIndex_;
    if (cursorMs_ >= archive_->maxTimeMs && playIndex_ >= archive_->batches.size()) {
        mode_ = Mode::Pause;
        cursorMs_ = archive_->maxTimeMs;
    }
    return range;
}

}  // namespace photon::vsd1

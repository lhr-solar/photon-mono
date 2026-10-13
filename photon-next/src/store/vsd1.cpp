#include "store/vsd1.hpp"

#include "base/endian.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
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
    // Day extracts keep run.log plus per-stint copies. Index the merged day
    // once so replay is not 2x the bytes and "no data" while buffering.
    for (const auto& file : files) {
        if (toLower(file.filename().string()) == "run.log") {
            return {file};
        }
    }
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

bool peekCanpBatch(std::span<const std::uint8_t> bytes, std::uint64_t& timestamp, std::size_t& consumed) {
    if (bytes.size() < canp::kHeaderSize) {
        return false;
    }
    std::size_t off = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t count = 0;
    std::uint32_t seq = 0;
    if (!readU32Be(bytes, off, magic) || !readU16Be(bytes, off, version) || !readU16Be(bytes, off, count) ||
        !readU32Be(bytes, off, seq) || !readU64Be(bytes, off, timestamp)) {
        return false;
    }
    if (magic != canp::kMagic || version != canp::kVersion || count == 0 || count > canp::kMaxBatch) {
        return false;
    }
    const std::size_t need = canp::kHeaderSize + static_cast<std::size_t>(count) * canp::kPacketSize;
    if (bytes.size() < need) {
        return false;
    }
    consumed = need;
    return true;
}

void visitPayload(std::span<const std::uint8_t> payload, std::size_t fileIndex, std::uint64_t frameOffset,
                  std::uint32_t payloadBytes, bool keepBatch, const std::function<void(IndexedBatch&&)>& visit) {
    if (keepBatch) {
        auto batches = extractCanp(payload);
        for (auto& batch : batches) {
            IndexedBatch item;
            item.timestampMs = batch.timestamp;
            item.fileIndex = fileIndex;
            item.frameOffset = frameOffset;
            item.payloadBytes = payloadBytes;
            item.batch = std::make_unique<canp::Batch>(std::move(batch));
            visit(std::move(item));
        }
        return;
    }
    std::size_t off = 0;
    while (off < payload.size()) {
        std::uint64_t timestamp = 0;
        std::size_t consumed = 0;
        if (peekCanpBatch(payload.subspan(off), timestamp, consumed)) {
            IndexedBatch item;
            item.timestampMs = timestamp;
            item.fileIndex = fileIndex;
            item.frameOffset = frameOffset;
            item.payloadBytes = payloadBytes;
            visit(std::move(item));
            off += consumed;
            continue;
        }
        auto batches = extractCanp(payload.subspan(off));
        for (auto& batch : batches) {
            IndexedBatch item;
            item.timestampMs = batch.timestamp;
            item.fileIndex = fileIndex;
            item.frameOffset = frameOffset;
            item.payloadBytes = payloadBytes;
            visit(std::move(item));
        }
        break;
    }
}

void indexPayload(Archive& archive, std::span<const std::uint8_t> payload, std::size_t fileIndex,
                  std::uint64_t frameOffset, std::uint32_t payloadBytes, bool keepBatch) {
    visitPayload(payload, fileIndex, frameOffset, payloadBytes, keepBatch,
                 [&](IndexedBatch&& item) { archive.batches.push_back(std::move(item)); });
}

constexpr std::uint32_t kIndexMagic = 0x58444950u;  // "PIDX" LE
constexpr std::uint32_t kIndexVersion = 1;

fs::path cachePathFor(const std::vector<fs::path>& files) {
    if (files.size() == 1) {
        return fs::path(files[0].string() + ".photon-idx");
    }
    return files.front().parent_path() / ".photon-idx";
}

std::uint64_t fileMtimeCount(const fs::path& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    return static_cast<std::uint64_t>(t.time_since_epoch().count());
}

bool walkFile(const fs::path& path, std::size_t fileIndex, std::atomic<bool>* cancel, std::string* error,
              const std::function<void(IndexedBatch&&)>& visit) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot open VSD1 segment: " + path.string();
        }
        return false;
    }
    std::vector<char> ioBuf(1 << 20);
    in.rdbuf()->pubsetbuf(ioBuf.data(), static_cast<std::streamsize>(ioBuf.size()));
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
        visitPayload(payload, fileIndex, offset, fh.payloadBytes, false, visit);
        offset += kHeaderSize + static_cast<std::uint64_t>(fh.payloadBytes);
    }
    return true;
}

}  // namespace

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

bool walkLogFiles(const std::vector<fs::path>& files, std::atomic<bool>* cancel, std::string* error,
                  const std::function<void(IndexedBatch&&)>& visit) {
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            if (error) {
                *error = "index cancelled";
            }
            return false;
        }
        if (!walkFile(files[i], i, cancel, error, visit)) {
            return false;
        }
    }
    return true;
}

std::optional<Archive> tryLoadIndexCache(const std::vector<fs::path>& files) {
    if (files.empty()) {
        return std::nullopt;
    }
    const auto cache = cachePathFor(files);
    std::ifstream in(cache, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t off = 0;
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t fileCount = 0;
    if (!readU32Le(bytes, off, magic) || magic != kIndexMagic || !readU32Le(bytes, off, version) ||
        version != kIndexVersion || !readU32Le(bytes, off, fileCount) ||
        fileCount != static_cast<std::uint32_t>(files.size())) {
        return std::nullopt;
    }
    for (const auto& file : files) {
        std::uint64_t size = 0;
        std::uint64_t mtime = 0;
        std::uint32_t nameLen = 0;
        if (!readU64Le(bytes, off, size) || !readU64Le(bytes, off, mtime) || !readU32Le(bytes, off, nameLen)) {
            return std::nullopt;
        }
        if (off + nameLen > bytes.size()) {
            return std::nullopt;
        }
        const std::string name(reinterpret_cast<const char*>(bytes.data() + off), nameLen);
        off += nameLen;
        std::error_code ec;
        if (name != file.filename().string() || fs::file_size(file, ec) != size || fileMtimeCount(file) != mtime) {
            return std::nullopt;
        }
    }
    std::uint64_t batchCount = 0;
    if (!readU64Le(bytes, off, batchCount)) {
        return std::nullopt;
    }
    Archive archive;
    archive.files = files;
    archive.complete = true;
    archive.batches.reserve(static_cast<std::size_t>(batchCount));
    for (std::uint64_t i = 0; i < batchCount; ++i) {
        IndexedBatch item;
        std::uint32_t fileIndex = 0;
        if (!readU64Le(bytes, off, item.timestampMs) || !readU64Le(bytes, off, item.frameOffset) ||
            !readU32Le(bytes, off, item.payloadBytes) || !readU32Le(bytes, off, fileIndex)) {
            return std::nullopt;
        }
        item.fileIndex = fileIndex;
        archive.batches.push_back(std::move(item));
    }
    finishArchive(archive);
    return archive;
}

bool saveIndexCache(const Archive& archive) {
    if (archive.files.empty() || archive.batches.empty()) {
        return false;
    }
    std::vector<std::uint8_t> out;
    out.reserve(16 + archive.files.size() * 48 + archive.batches.size() * 24);
    writeU32Le(out, kIndexMagic);
    writeU32Le(out, kIndexVersion);
    writeU32Le(out, static_cast<std::uint32_t>(archive.files.size()));
    for (const auto& file : archive.files) {
        std::error_code ec;
        writeU64Le(out, fs::file_size(file, ec));
        writeU64Le(out, fileMtimeCount(file));
        const auto name = file.filename().string();
        writeU32Le(out, static_cast<std::uint32_t>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
    }
    writeU64Le(out, static_cast<std::uint64_t>(archive.batches.size()));
    for (const auto& item : archive.batches) {
        writeU64Le(out, item.timestampMs);
        writeU64Le(out, item.frameOffset);
        writeU32Le(out, item.payloadBytes);
        writeU32Le(out, static_cast<std::uint32_t>(item.fileIndex));
    }
    const auto cache = cachePathFor(archive.files);
    std::ofstream file(cache, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(file);
}

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
    if (auto cached = tryLoadIndexCache(archive.files)) {
        return cached;
    }
    if (!walkLogFiles(archive.files, cancel, error,
                      [&](IndexedBatch&& item) { archive.batches.push_back(std::move(item)); })) {
        return std::nullopt;
    }
    if (archive.batches.empty()) {
        if (error) {
            *error = "segments have no readable VSD1/CANP frames";
        }
        return std::nullopt;
    }
    finishArchive(archive);
    saveIndexCache(archive);
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
    if (cursorMs_ >= archive_->maxTimeMs && playIndex_ >= archive_->batches.size() && archive_->complete) {
        mode_ = Mode::Pause;
        cursorMs_ = archive_->maxTimeMs;
    }
    return range;
}

}  // namespace photon::vsd1

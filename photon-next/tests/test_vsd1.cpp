#include "photon/canp.hpp"
#include "photon/endian.hpp"
#include "photon/vsd1.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

void runVsd1Tests();

namespace {

namespace fs = std::filesystem;

std::vector<std::uint8_t> encodeVsd1(std::uint64_t id, const photon::canp::Batch& batch) {
    const auto payload = photon::canp::encodeBatch(batch);
    std::vector<std::uint8_t> out;
    photon::writeU32Be(out, photon::vsd1::kMagic);
    photon::writeU32Be(out, static_cast<std::uint32_t>(payload.size()));
    photon::writeU64Be(out, id);
    photon::writeU32Be(out, 0);
    photon::writeU32Be(out, 0);
    photon::writeBytes(out, payload);
    return out;
}

photon::canp::Batch makeBatch(std::uint32_t seq, std::uint64_t ts, std::uint32_t canId, std::uint8_t first) {
    photon::canp::Batch batch{};
    batch.seq = seq;
    batch.timestamp = ts;
    batch.count = 1;
    batch.packets[0].canId = canId;
    batch.packets[0].dlc = 8;
    batch.packets[0].data[0] = first;
    return batch;
}

std::vector<std::uint8_t> threeFrameBlob() {
    std::vector<std::uint8_t> bytes;
    auto a = encodeVsd1(1, makeBatch(1, 1000, 0x100, 0x11));
    auto b = encodeVsd1(2, makeBatch(2, 2500, 0x101, 0x22));
    auto c = encodeVsd1(3, makeBatch(3, 4000, 0x102, 0x33));
    bytes.insert(bytes.end(), a.begin(), a.end());
    bytes.insert(bytes.end(), b.begin(), b.end());
    bytes.insert(bytes.end(), c.begin(), c.end());
    return bytes;
}

}  // namespace

void runVsd1Tests() {
    const auto blob = threeFrameBlob();
    std::string error;
    auto mem = photon::vsd1::indexBytes(blob, &error);
    CHECK(mem.has_value());
    CHECK(mem->batches.size() == 3);
    CHECK(mem->minTimeMs == 1000);
    CHECK(mem->maxTimeMs == 4000);
    CHECK(mem->batches[0].timestampMs == 1000);
    CHECK(mem->batches[1].timestampMs == 2500);
    CHECK(mem->batches[2].timestampMs == 4000);
    CHECK(photon::vsd1::seekIndex(*mem, 0) == 0);
    CHECK(photon::vsd1::seekIndex(*mem, 1000) == 0);
    CHECK(photon::vsd1::seekIndex(*mem, 2500) == 1);
    CHECK(photon::vsd1::seekIndex(*mem, 2501) == 2);
    CHECK(photon::vsd1::seekIndex(*mem, 4000) == 2);
    CHECK(photon::vsd1::seekIndex(*mem, 5000) == 3);

    CHECK(sizeof(photon::vsd1::IndexedBatch) < 64);
    CHECK(mem->batches[0].batch != nullptr);
    CHECK(mem->batches[0].batch->packets[0].canId == 0x100);
    CHECK(mem->batches[1].batch->packets[0].data[0] == 0x22);

    photon::vsd1::ReplayCursor cursor;
    cursor.bind(*mem);
    CHECK(cursor.mode() == photon::vsd1::ReplayCursor::Mode::Play);
    CHECK(cursor.cursorMs() == 1000);
    CHECK(cursor.playIndex() == 0);
    auto first = cursor.advance(0);
    CHECK(first.begin == 0);
    CHECK(first.end == 1);

    cursor.seek(2500);
    CHECK(cursor.cursorMs() == 2500);
    CHECK(cursor.playIndex() == 1);
    auto mid = cursor.advance(0);
    CHECK(mid.begin == 1);
    CHECK(mid.end == 2);

    cursor.seek(1000);
    cursor.setMode(photon::vsd1::ReplayCursor::Mode::Pause);
    auto paused = cursor.advance(5000);
    CHECK(paused.begin == paused.end);

    cursor.setMode(photon::vsd1::ReplayCursor::Mode::Play);
    cursor.setSpeed(2.0);
    auto sped = cursor.advance(750);
    CHECK(sped.end > sped.begin);
    CHECK(cursor.speed() == 2.0);

    cursor.setMode(photon::vsd1::ReplayCursor::Mode::Live);
    CHECK(cursor.mode() == photon::vsd1::ReplayCursor::Mode::Play);

    const auto tmpDir = fs::temp_directory_path() / "photon-next-vsd1";
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    const auto fileA = tmpDir / "000000000001.log";
    const auto fileB = tmpDir / "000000000002.log";
    {
        std::ofstream out(fileA, std::ios::binary);
        const auto a = encodeVsd1(1, makeBatch(1, 1000, 0x100, 0x11));
        const auto b = encodeVsd1(2, makeBatch(2, 2500, 0x101, 0x22));
        out.write(reinterpret_cast<const char*>(a.data()), static_cast<std::streamsize>(a.size()));
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    {
        std::ofstream out(fileB, std::ios::binary);
        const auto c = encodeVsd1(3, makeBatch(3, 4000, 0x102, 0x33));
        out.write(reinterpret_cast<const char*>(c.data()), static_cast<std::streamsize>(c.size()));
    }

    auto onDisk = photon::vsd1::indexPath(fileA, &error);
    CHECK(onDisk.has_value());
    CHECK(onDisk->batches.size() == 2);
    auto loaded = photon::vsd1::readBatch(*onDisk, onDisk->batches[1], &error);
    CHECK(loaded.has_value());
    CHECK(loaded->timestamp == 2500);
    CHECK(loaded->packets[0].canId == 0x101);

    auto dir = photon::vsd1::indexPath(tmpDir, &error);
    CHECK(dir.has_value());
    CHECK(dir->files.size() == 2);
    CHECK(dir->batches.size() == 3);
    CHECK(dir->minTimeMs == 1000);
    CHECK(dir->maxTimeMs == 4000);
    CHECK(photon::vsd1::seekIndex(*dir, 2500) == 1);

    photon::vsd1::ReplayCursor diskCursor;
    diskCursor.bind(*dir);
    diskCursor.seek(2500);
    CHECK(diskCursor.playIndex() == 1);
    auto emit = diskCursor.advance(0);
    CHECK(emit.begin == 1);
    CHECK(emit.end == 2);

    auto zipErr = photon::vsd1::indexPath(tmpDir / "nope.zip", &error);
    CHECK(!zipErr.has_value());
    CHECK(error.find("unzip") != std::string::npos);

    const auto dayDir = tmpDir / "day";
    fs::create_directories(dayDir, ec);
    fs::copy_file(fileA, dayDir / "run.log", fs::copy_options::overwrite_existing, ec);
    fs::copy_file(fileB, dayDir / "session-01.log", fs::copy_options::overwrite_existing, ec);
    const auto dayFiles = photon::vsd1::listLogFiles(dayDir);
    CHECK(dayFiles.size() == 1);
    CHECK(dayFiles[0].filename() == "run.log");

    auto once = photon::vsd1::indexPath(dayDir, &error);
    CHECK(once.has_value());
    CHECK(once->batches.size() == 2);
    CHECK(photon::vsd1::tryLoadIndexCache(once->files).has_value());
    auto cached = photon::vsd1::indexPath(dayDir, &error);
    CHECK(cached.has_value());
    CHECK(cached->batches.size() == once->batches.size());
    CHECK(cached->minTimeMs == once->minTimeMs);
    CHECK(cached->maxTimeMs == once->maxTimeMs);

    fs::remove_all(tmpDir, ec);
}

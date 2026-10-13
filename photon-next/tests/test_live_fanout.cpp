#include "live_fanout.hpp"
#include "wire/canp.hpp"
#include "net/net.hpp"

#ifdef near
#undef near
#endif

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

void runLiveFanoutTests();

namespace {

std::vector<std::uint8_t> oneBpsBatch(std::uint64_t timestampMs, std::uint32_t seq) {
    photon::canp::Batch batch;
    batch.seq = seq;
    batch.timestamp = timestampMs;
    batch.count = 1;
    batch.packets[0].canId = 1;  // BPS_Status
    batch.packets[0].dlc = 8;
    batch.packets[0].data[0] = 0x10;
    batch.packets[0].data[1] = 0x27;  // some voltage-ish raw
    return photon::canp::encodeBatch(batch);
}

}  // namespace

void runLiveFanoutTests() {
    photon::net::Guard sockets;
    auto listener = photon::net::listenLoopback(0);
    CHECK(listener.valid());
    const auto port = listener.localPort();
    CHECK(port != 0);

    const auto tmp = fs::temp_directory_path() / ("photon-live-fanout-" + std::to_string(port));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    const auto dbcPath = (fs::path(PHOTON_ASSETS_DIR) / "dbc" / "carcan_telemcan.dbc").string();
    CHECK(fs::exists(dbcPath));

    std::atomic<bool> stop{false};
    LiveFanoutOptions opt;
    opt.host = "127.0.0.1";
    opt.port = port;
    opt.outDir = tmp.string();
    opt.dbcPath = dbcPath;
    opt.maxFrames = 2;
    opt.stop = &stop;

    std::thread writer([&]() { CHECK(runLiveFanout(opt) == 0); });

    auto client = photon::net::acceptClient(listener);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!client.valid() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        client = photon::net::acceptClient(listener);
    }
    if (!client.valid()) {
        stop.store(true);
        writer.join();
        CHECK(client.valid());
        fs::remove_all(tmp, ec);
        return;
    }

    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto sendAll = [&](const std::vector<std::uint8_t>& bytes) {
        std::size_t off = 0;
        const auto sendDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (off < bytes.size() && std::chrono::steady_clock::now() < sendDeadline) {
            const int n = client.sendSome(bytes.data() + off, bytes.size() - off);
            if (n < 0) {
                return false;
            }
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            off += static_cast<std::size_t>(n);
        }
        return off == bytes.size();
    };
    CHECK(sendAll(oneBpsBatch(now, 1)));
    CHECK(sendAll(oneBpsBatch(now + 10, 2)));
    client.close();

    writer.join();

    CHECK(fs::exists(tmp / "catalog.json"));
    bool foundSession = false;
    for (const auto& entry : fs::directory_iterator(tmp)) {
        if (entry.is_directory() && fs::exists(entry.path() / "session.json")) {
            foundSession = true;
        }
    }
    CHECK(foundSession);

    std::ifstream live(tmp / "live.json");
    CHECK(live.good());
    std::string liveBody((std::istreambuf_iterator<char>(live)), std::istreambuf_iterator<char>());
    CHECK(liveBody.find("\"live\":false") != std::string::npos);

    fs::remove_all(tmp, ec);
}

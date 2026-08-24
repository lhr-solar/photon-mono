#include "photon/video_framing.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <span>
#include <vector>

void runVideoTests() {
    using photon::video::ServerFrame;
    using photon::video::StreamParser;
    using photon::video::encodeRequest;
    using photon::video::encodeServerFrame;
    using photon::video::kFlagConfig;
    using photon::video::kFlagKeyframe;
    using photon::video::kModeLive;
    using photon::video::kModeReplay;

    const auto liveReq = encodeRequest(kModeLive, 0);
    CHECK(liveReq.size() == 9);
    CHECK(liveReq[0] == 0);
    for (int i = 1; i < 9; ++i) {
        CHECK(liveReq[static_cast<std::size_t>(i)] == 0);
    }

    const auto replayReq = encodeRequest(kModeReplay, 0x0102030405060708ull);
    CHECK(replayReq[0] == 1);
    CHECK(replayReq[1] == 0x01);
    CHECK(replayReq[8] == 0x08);

    ServerFrame config;
    config.timestampMs = 1000;
    config.flags = kFlagConfig;
    config.payload = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E};

    ServerFrame idr;
    idr.timestampMs = 1033;
    idr.flags = kFlagKeyframe;
    idr.payload = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x20};

    ServerFrame delta;
    delta.timestampMs = 1066;
    delta.flags = 0;
    delta.payload = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A};

    const auto w0 = encodeServerFrame(config);
    const auto w1 = encodeServerFrame(idr);
    const auto w2 = encodeServerFrame(delta);
    CHECK(w0.size() == 13 + config.payload.size());
    CHECK(w0[12] == kFlagConfig);
    // payloadLen big-endian
    CHECK(w0[0] == 0);
    CHECK(w0[3] == static_cast<std::uint8_t>(config.payload.size()));

    StreamParser parser;
    parser.feed(w0);
    parser.feed(w1);
    parser.feed(w2);
    auto a = parser.take();
    auto b = parser.take();
    auto c = parser.take();
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK(c.has_value());
    CHECK(a->flags == kFlagConfig);
    CHECK(a->timestampMs == 1000);
    CHECK(a->payload == config.payload);
    CHECK(b->flags == kFlagKeyframe);
    CHECK(b->timestampMs == 1033);
    CHECK(c->flags == 0);
    CHECK(c->timestampMs == 1066);
    CHECK(!parser.take().has_value());

    // Split header, then split payload.
    StreamParser split;
    const auto all = [&] {
        std::vector<std::uint8_t> out;
        out.insert(out.end(), w0.begin(), w0.end());
        out.insert(out.end(), w1.begin(), w1.end());
        out.insert(out.end(), w2.begin(), w2.end());
        return out;
    }();

    split.feed(std::span<const std::uint8_t>(all.data(), 5));
    CHECK(!split.take().has_value());
    split.feed(std::span<const std::uint8_t>(all.data() + 5, 8));
    CHECK(!split.take().has_value());  // header only, payload still missing
    split.feed(std::span<const std::uint8_t>(all.data() + 13, 3));
    CHECK(!split.take().has_value());
    const std::size_t firstNeed = w0.size();
    split.feed(std::span<const std::uint8_t>(all.data() + 16, firstNeed - 16));
    auto first = split.take();
    CHECK(first.has_value());
    CHECK(first->flags == kFlagConfig);
    CHECK(first->payload == config.payload);

    // Remainder of IDR + delta in two chunks that do not land on a frame boundary.
    const std::size_t restOff = firstNeed;
    const std::size_t mid = restOff + 10;
    split.feed(std::span<const std::uint8_t>(all.data() + restOff, mid - restOff));
    CHECK(!split.take().has_value());
    split.feed(std::span<const std::uint8_t>(all.data() + mid, all.size() - mid));
    auto second = split.take();
    auto third = split.take();
    CHECK(second.has_value());
    CHECK(second->flags == kFlagKeyframe);
    CHECK(second->payload == idr.payload);
    CHECK(third.has_value());
    CHECK(third->flags == 0);
    CHECK(third->payload == delta.payload);
    CHECK(!split.failed());

    StreamParser overflow;
    std::vector<std::uint8_t> huge(13, 0);
    huge[0] = 0x01;  // payloadLen = 16 MiB + ... wait 0x01000000 = 16 MiB exactly is allowed
    // 16 MiB + 1
    huge[0] = 0x01;
    huge[3] = 0x01;
    overflow.feed(huge);
    CHECK(overflow.failed());
    CHECK(!overflow.take().has_value());
}

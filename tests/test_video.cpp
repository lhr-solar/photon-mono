#include "photon/video_framing.hpp"
#include "photon_ui/video_release.hpp"
#include "photon/video_framing.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <span>
#include <vector>

void runVideoTests() {
    {
        photon::vidx::GopRing ring;
        const std::vector<std::uint8_t> nal{0, 0, 0, 1, 0x65, 0x80};
        for (std::uint64_t t = 1000; t <= 5000; t += 1000) {
            ring.push(t, 2, nal);
            ring.push(t, 1, nal);
            ring.push(t + 33, 0, nal);
        }
        std::uint64_t seq = 0;
        std::vector<std::uint64_t> released;
        auto submit = [&](const photon::vidx::BufferedNal& n) {
            CHECK(n.seq > seq);
            seq = n.seq;
            released.push_back(n.videoMs);
        };
        photon::ui::releaseVideoThrough(ring, seq, 1000, submit);
        CHECK(released.size() == 2);
        photon::ui::releaseVideoThrough(ring, seq, 1033, submit);
        CHECK(released.size() == 3);
        photon::ui::releaseVideoThrough(ring, seq, 1033, submit);
        CHECK(released.size() == 3); // same cursor never replays a picture
        photon::ui::releaseVideoThrough(ring, seq, 5033, submit);
        CHECK(released.size() == 15); // no prefetched key/header skipped deltas
        for (std::size_t i = 1; i < released.size(); ++i) CHECK(released[i] >= released[i-1]);
    }
    {
        // Live pacing. A locate reply is a whole GOP, so the watermark has to
        // walk it on a clock; releasing it in one tick is what made Live show
        // one picture per locate.
        using photon::ui::paceLiveRelease;
        // Nothing released yet: enter on the newest keyframe.
        CHECK(paceLiveRelease(0, 10'000, 16.0).reenter);
        // Steady state: advance by the frame interval, no reset.
        const auto step = paceLiveRelease(10'000, 10'400, 33.0);
        CHECK(!step.reenter);
        CHECK(step.releaseMs == 10'033);
        // Behind by more than kLiveCatchUpMs: drain faster than real time.
        const auto fast = paceLiveRelease(10'000, 10'900, 100.0);
        CHECK(!fast.reenter);
        CHECK(fast.releaseMs == 10'150);
        // Starved: hold at what is resident instead of running ahead, so the
        // next batch is paced rather than dumped.
        const auto starved = paceLiveRelease(10'000, 10'050, 200.0);
        CHECK(starved.releaseMs == 10'050);
        // Playing through a 3 s GOP is not a stall (dt=16). Hidden tab: far
        // behind AND a long gap between paints.
        CHECK(!paceLiveRelease(10'000, 20'000, 16.0).reenter);
        CHECK(paceLiveRelease(10'000, 30'000, 250.0).reenter);
        // One paint cannot release more than kLiveMaxStepMs of pictures, even
        // after a long gap between paints. 2500 ms behind is the most that is
        // still paced rather than resynced.
        const auto clamped = paceLiveRelease(10'000, 12'500, 100'000.0);
        CHECK(!clamped.reenter);
        CHECK(clamped.releaseMs == 10'000 + static_cast<std::uint64_t>(photon::ui::kLiveMaxStepMs *
                                                                      photon::ui::kLiveCatchUpRate));
        // Time going backwards must not rewind the watermark.
        CHECK(paceLiveRelease(10'000, 10'400, -50.0).releaseMs == 10'000);
        // A minute of smooth rendering must not accumulate playback delay
        // from rounding each paint's fractional milliseconds away.
        for (const int hz : {60, 144}) {
            double release = 10'000.0;
            for (int tick = 1; tick <= hz * 60; ++tick) {
                const double wall = 10'000.0 + tick * (1000.0 / hz);
                const auto paced = paceLiveRelease(release,
                    static_cast<std::uint64_t>(wall + 100.0), 1000.0 / hz);
                CHECK(!paced.reenter);
                release = paced.releaseMs;
            }
            CHECK(release > 69'999.99);
            CHECK(release < 70'000.01);
        }
    }
    {
        // The live poll re-locates the same GOP while it grows, and the backend
        // ships only the bytes appended since the last poll. That resume relies
        // on GOP row offsets landing on NAL boundaries, and on the tail parsing
        // to exactly the pictures the client has not seen.
        const std::vector<std::uint8_t> whole{
            0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x1E,        // SPS
            0, 0, 0, 1, 0x68, 0xCE, 0x38, 0x80,        // PPS
            0, 0, 0, 1, 0x65, 0x88, 0x80, 0x20, 0x00,  // IDR
            0, 0, 0, 1, 0x41, 0x9A, 0x20, 0x00, 0x00,  // P
            0, 0, 0, 1, 0x41, 0x9A, 0x21, 0x00, 0x00,  // P
        };
        std::vector<std::uint64_t> allMs;
        photon::video::forEachStampedGopNal(whole, 1000, 1100,
                                            [&](std::uint64_t ms, std::uint8_t, std::span<const std::uint8_t>) {
                                                allMs.push_back(ms);
                                            });
        CHECK(allMs.size() == 5);
        // Split where the second poll's pointer ended: after the IDR.
        const std::size_t already = 25;
        const std::vector<std::uint8_t> tail(whole.begin() + static_cast<std::ptrdiff_t>(already), whole.end());
        std::vector<std::uint64_t> tailMs;
        std::size_t tailPics = 0;
        photon::video::forEachStampedGopNal(tail, 1000 + (100 * already) / whole.size(), 1100,
                                            [&](std::uint64_t ms, std::uint8_t flags,
                                                std::span<const std::uint8_t>) {
                                                tailMs.push_back(ms);
                                                if ((flags & photon::video::kFlagConfig) == 0) {
                                                    ++tailPics;
                                                }
                                            });
        // Only the two appended pictures, not the whole GOP again.
        CHECK(tailMs.size() == 2);
        CHECK(tailPics == 2);
        for (std::size_t i = 1; i < tailMs.size(); ++i) CHECK(tailMs[i] >= tailMs[i - 1]);
        CHECK(tailMs.front() >= 1000);
    }
    {
        // Send watermark: a client is only sent NALs it has not seen, which is
        // what keeps a 400 ms poll from re-sending the growing GOP each time.
        photon::vidx::GopRing cache;
        const std::vector<std::uint8_t> nal{0, 0, 0, 1, 0x41, 0x9A};
        std::uint64_t sentSeq = 0;
        auto flush = [&]() {
            std::size_t sent = 0;
            for (const auto& n : cache.nals()) {
                if (n.seq <= sentSeq) continue;
                sentSeq = n.seq;
                ++sent;
            }
            return sent;
        };
        cache.push(1000, 2, nal);
        cache.push(1000, 1, nal);
        CHECK(flush() == 2);
        CHECK(flush() == 0); // a poll with no growth ships nothing
        cache.push(1033, 0, nal);
        cache.push(1066, 0, nal);
        CHECK(flush() == 2); // only the appended tail
        // A new GOP replaces the ring, whose sequence restarts, so the
        // watermark has to restart with it or nothing would ever be sent.
        photon::vidx::GopRing fresh;
        fresh.push(2000, 2, nal);
        fresh.push(2000, 1, nal);
        cache = std::move(fresh);
        sentSeq = 0;
        CHECK(flush() == 2);
    }
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

    using photon::video::annexBNalType;
    using photon::video::flagsForNalType;
    using photon::video::forEachAnnexB;
    using photon::video::kFlagConfig;
    using photon::video::kFlagKeyframe;
    std::vector<std::uint8_t> packed;
    packed.insert(packed.end(), {0x00, 0x00, 0x00, 0x01, 0x09, 0x10});
    packed.insert(packed.end(), config.payload.begin(), config.payload.end());
    packed.insert(packed.end(), idr.payload.begin(), idr.payload.end());
    int nals = 0;
    int types[4] = {};
    forEachAnnexB(packed, [&](std::span<const std::uint8_t> nal) {
        if (nals < 4) {
            types[nals] = annexBNalType(nal);
        }
        ++nals;
    });
    CHECK(nals == 3);
    CHECK(types[0] == 9);
    CHECK(types[1] == 7);
    CHECK(types[2] == 5);
    CHECK(flagsForNalType(7) == kFlagConfig);
    CHECK(flagsForNalType(5) == kFlagKeyframe);
    CHECK(photon::video::nalIsTinyPslice(delta.payload));
    std::vector<std::uint8_t> fatP(40, 0);
    fatP[3] = 1;
    fatP[4] = 0x41;
    CHECK(!photon::video::nalIsTinyPslice(fatP));
    CHECK(!photon::video::nalIsTinyPslice(idr.payload));

    using photon::video::firstMbInSlice;
    using photon::video::forEachStampedGopNal;
    using photon::video::gopPictureMs;
    using photon::video::nalStartsPicture;
    CHECK(firstMbInSlice(idr.payload) == 0);
    CHECK(nalStartsPicture(idr.payload));
    CHECK(firstMbInSlice(delta.payload) == 0);
    const std::vector<std::uint8_t> midSlice{0x00, 0x00, 0x00, 0x01, 0x41, 0x40};
    CHECK(firstMbInSlice(midSlice) == 1);
    CHECK(!nalStartsPicture(midSlice));
    CHECK(gopPictureMs(1000, 1066, 0, 3) == 1000);
    CHECK(gopPictureMs(1000, 1066, 1, 3) == 1033);
    CHECK(gopPictureMs(1000, 1066, 2, 3) == 1066);
    CHECK(gopPictureMs(1000, 1000, 1, 3) == 1033);

    std::vector<std::uint8_t> gop;
    gop.insert(gop.end(), config.payload.begin(), config.payload.end());
    gop.insert(gop.end(), idr.payload.begin(), idr.payload.end());
    gop.insert(gop.end(), midSlice.begin(), midSlice.end());
    gop.insert(gop.end(), delta.payload.begin(), delta.payload.end());
    std::vector<std::uint64_t> stamps;
    std::vector<std::uint8_t> kinds;
    forEachStampedGopNal(gop, 1000, 1066, [&](std::uint64_t ms, std::uint8_t flags, std::span<const std::uint8_t>) {
        stamps.push_back(ms);
        kinds.push_back(flags);
    });
    CHECK(stamps.size() == 4);
    CHECK(kinds[0] == kFlagConfig);
    CHECK(kinds[1] == kFlagKeyframe);
    CHECK(stamps[0] == 1000);
    CHECK(stamps[1] == 1000);
    CHECK(stamps[2] == 1000);
    CHECK(stamps[3] == 1066);

    // Packed AU: AUD + SPS + PPS + IDR + 9-byte P stub. The IDR must be
    // flagged as a keyframe or the ring reports "GOP in cache" with nothing
    // to enter the decoder at.
    std::vector<std::uint8_t> packedAu;
    packedAu.insert(packedAu.end(), {0x00, 0x00, 0x00, 0x01, 0x09, 0x10});
    packedAu.insert(packedAu.end(), config.payload.begin(), config.payload.end());
    packedAu.insert(packedAu.end(), {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80});
    packedAu.insert(packedAu.end(), idr.payload.begin(), idr.payload.end());
    packedAu.insert(packedAu.end(), {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A});
    std::vector<int> packedTypes;
    std::vector<std::uint8_t> packedFlags;
    photon::video::forEachAnnexB(packedAu, [&](std::span<const std::uint8_t> n) {
        const int t = photon::video::annexBNalType(n);
        if (t != 1 && t != 5 && t != 7 && t != 8) {
            return;
        }
        if (photon::video::nalIsTinyPslice(n)) {
            return;
        }
        packedTypes.push_back(t);
        packedFlags.push_back(photon::video::flagsForNalType(t));
    });
    CHECK(packedTypes.size() == 3);
    CHECK(packedTypes[0] == 7);
    CHECK(packedTypes[1] == 8);
    CHECK(packedTypes[2] == 5);
    CHECK(packedFlags[2] == kFlagKeyframe);

    photon::vidx::GopRing late;
    late.push(5000, kFlagConfig, config.payload);
    late.push(5000, kFlagKeyframe, idr.payload);
    CHECK(late.keyframeAtOrBefore(1000) == -1);
    CHECK(late.keyframeAtOrBefore(late.newestMs()) == 1);
}

#include "photon/control_v2.hpp"
#include "test_harness.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace c2 = photon::ctrl2;

namespace {

// serialize writes every field, so parsing and serializing again gives the same
// text only when parse read each field back.
void expectRoundTrip(const c2::Message& m) {
    const std::string text = c2::serialize(m);
    std::string error;
    const auto back = c2::parse(text, &error);
    CHECK(back.has_value());
    if (!back) {
        std::fprintf(stderr, "  parse of %s failed: %s\n", text.c_str(), error.c_str());
        return;
    }
    CHECK(back->index() == m.index());
    CHECK(c2::serialize(*back) == text);
}

c2::Catalog sampleCatalog() {
    c2::Catalog cat;
    cat.sha = "3f9a0c";
    cat.buses = {"CarCAN", "DAqCAN"};

    c2::CatalogSignal s0;
    s0.index = 17;
    s0.name = "CellSumVoltage";
    s0.unit = "V";
    s0.min = 0.0;
    s0.max = 655.35;
    s0.integer = false;
    s0.mux = -1;
    s0.periodMs = 100.0;

    c2::CatalogSignal s1;
    s1.index = 18;
    s1.name = "PageValue";
    s1.unit = "";
    s1.min = -40.0;
    s1.max = -0.125;
    s1.integer = true;
    s1.mux = 3;
    s1.periodMs = 0.0;

    c2::CatalogMessage a;
    a.id = 1234;
    a.name = "BPSPackData";
    a.bus = 0;
    a.signals = {s0, s1};

    c2::CatalogSignal s2;
    s2.index = 19;
    s2.name = "Temp";
    s2.unit = "degC";
    s2.min = -55.5;
    s2.max = 125.25;
    s2.integer = false;
    s2.mux = -1;
    s2.periodMs = 0.0;

    c2::CatalogMessage b;
    b.id = 0x7ff;
    b.name = "Status";
    b.bus = 1;
    b.signals = {s2};

    cat.messages = {a, b};
    return cat;
}

c2::Runs sampleRuns() {
    c2::Run first;
    first.id = "run-1";
    first.label = "Test drive";
    first.source = "pcan";
    first.device = "dev-a";
    first.dbcSha = "abc";
    first.startMs = 1'800'000'000'000;
    first.endMs = 1'800'000'600'000;
    first.rev = 0;
    first.live = false;

    c2::Run second;
    second.id = "run-2";
    second.label = "";
    second.source = "live";
    second.device = "";
    second.dbcSha = "";
    second.startMs = 1'800'001'000'000;
    second.endMs = 1'800'001'500'000;
    second.rev = 7;
    second.live = true;

    c2::Runs runs;
    runs.runs = {first, second};
    return runs;
}

c2::HelloAck sampleHelloAck() {
    c2::HelloAck ack;
    ack.protocol = 2;
    ack.server = "photon-server";
    ack.user = "test-user";
    ack.serverTimeNs = 1'800'000'000'123'456'789;
    ack.catalogSha = "3f9a0c";
    ack.liveRun = "run-2";
    ack.levels = {0, 10, 100, 1000, 10000, 60000, 600000};
    ack.features = {"catalog", "live", "runs", "tiles"};
    return ack;
}

void testRoundTripEveryAlternative() {
    c2::Hello withToken;
    withToken.client = "test-client";
    withToken.token = "secret";
    expectRoundTrip(withToken);

    c2::Hello noToken;
    noToken.client = "test-client";
    expectRoundTrip(noToken);

    expectRoundTrip(sampleHelloAck());
    expectRoundTrip(sampleCatalog());

    c2::Subscribe none;
    expectRoundTrip(none);
    c2::Subscribe several;
    several.signals = {3, 17, 18, 19};
    expectRoundTrip(several);

    expectRoundTrip(c2::RunsList{});
    expectRoundTrip(sampleRuns());

    c2::TileRequest request;
    request.qid = 5;
    request.run = "run-1";
    request.signal = 17;
    request.level = 2;
    request.index = -3;
    expectRoundTrip(request);

    c2::TileCancel cancel;
    cancel.qid = 5;
    expectRoundTrip(cancel);

    c2::TileError tileError;
    tileError.qid = 5;
    tileError.message = "history is off";
    expectRoundTrip(tileError);

    c2::VideoSubscribe videoSubscribe;
    videoSubscribe.mode = "replay";
    videoSubscribe.fromMs = 1'800'000'000'000;
    expectRoundTrip(videoSubscribe);

    c2::VideoSeek seek;
    seek.qid = 6;
    seek.tMs = 1'800'000'500'000;
    expectRoundTrip(seek);

    c2::VideoStatus status;
    status.mode = "replay";
    status.tMs = 1'800'000'500'123;
    status.clock = "frame";
    expectRoundTrip(status);

    c2::Ping ping;
    ping.tMs = 1'700'000'000'000;
    expectRoundTrip(ping);

    expectRoundTrip(c2::Pong{});

    c2::Error error;
    error.code = "protocol";
    error.message = "protocol 2 required";
    expectRoundTrip(error);

    c2::Error escaped;
    escaped.code = "bad_json";
    escaped.message = "quote \" backslash \\ tab\tnewline\n";
    expectRoundTrip(escaped);
}

void testExactJsonText() {
    c2::Hello hello;
    hello.client = "c";
    CHECK(c2::serialize(hello) == R"({"client":"c","protocol":2,"token":"","type":"hello"})");
    CHECK(c2::serialize(c2::Subscribe{}) == R"({"signals":[],"type":"subscribe"})");
    CHECK(c2::serialize(c2::RunsList{}) == R"({"type":"runs_list"})");
    CHECK(c2::serialize(c2::Runs{}) == R"({"runs":[],"type":"runs"})");

    c2::Ping ping;
    ping.tMs = 5;
    CHECK(c2::serialize(ping) == R"({"t_ms":5,"type":"ping"})");

    c2::TileCancel cancel;
    cancel.qid = 7;
    CHECK(c2::serialize(cancel) == R"({"qid":7,"type":"tile_cancel"})");

    CHECK(c2::serialize(c2::Catalog{}) == R"({"buses":[],"messages":[],"sha":"","type":"catalog"})");

    c2::HelloAck bare;
    const std::string bareText = c2::serialize(bare);
    CHECK(bareText.find(R"("features":[])") != std::string::npos);
    CHECK(bareText.find(R"("levels":[])") != std::string::npos);
}

void testCatalogFields() {
    const auto back = c2::parse(c2::serialize(sampleCatalog()));
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    const auto* cat = std::get_if<c2::Catalog>(&*back);
    CHECK(cat != nullptr);
    if (!cat) {
        return;
    }
    CHECK(cat->sha == "3f9a0c");
    CHECK(cat->buses.size() == 2);
    CHECK(cat->messages.size() == 2);
    CHECK(cat->messages[0].id == 1234);
    CHECK(cat->messages[0].bus == 0);
    CHECK(cat->messages[0].signals.size() == 2);
    CHECK(cat->messages[0].signals[0].index == 17);
    CHECK(cat->messages[0].signals[0].max == 655.35);
    CHECK(cat->messages[0].signals[0].periodMs == 100.0);
    CHECK(cat->messages[0].signals[0].mux == -1);
    CHECK(!cat->messages[0].signals[0].integer);
    CHECK(cat->messages[0].signals[1].mux == 3);
    CHECK(cat->messages[0].signals[1].integer);
    CHECK(cat->messages[0].signals[1].min == -40.0);
    CHECK(cat->messages[0].signals[1].max == -0.125);
    CHECK(cat->messages[1].bus == 1);
    CHECK(cat->messages[1].signals[0].min == -55.5);
    CHECK(cat->messages[1].signals[0].max == 125.25);
}

void testRunsFields() {
    const auto back = c2::parse(c2::serialize(sampleRuns()));
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    const auto* runs = std::get_if<c2::Runs>(&*back);
    CHECK(runs != nullptr);
    if (!runs) {
        return;
    }
    CHECK(runs->runs.size() == 2);
    CHECK(runs->runs[0].rev == 0);
    CHECK(!runs->runs[0].live);
    CHECK(runs->runs[0].startMs == 1'800'000'000'000);
    CHECK(runs->runs[1].rev == 7);
    CHECK(runs->runs[1].live);
    CHECK(runs->runs[1].endMs == 1'800'001'500'000);
    CHECK(runs->runs[1].dbcSha.empty());
}

void testHelloAckServerTime() {
    // server_time_ns is a JSON number read through a double. At 1.8e18 the spacing
    // of doubles is 256 ns, so the value comes back within 256 ns, not exactly.
    const c2::HelloAck sent = sampleHelloAck();
    const auto back = c2::parse(c2::serialize(sent));
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    const auto* ack = std::get_if<c2::HelloAck>(&*back);
    CHECK(ack != nullptr);
    if (!ack) {
        return;
    }
    const std::int64_t diff = ack->serverTimeNs - sent.serverTimeNs;
    CHECK(diff <= 256 && diff >= -256);
    CHECK(ack->levels == sent.levels);
    CHECK(ack->levels.size() == 7);
    CHECK(ack->features.size() == 4);
    CHECK(ack->protocol == 2);
}

void testNonFiniteDoublesWrittenAsZero() {
    c2::CatalogSignal sig;
    sig.min = std::numeric_limits<double>::quiet_NaN();
    sig.max = std::numeric_limits<double>::infinity();
    c2::CatalogMessage msg;
    msg.signals = {sig};
    c2::Catalog cat;
    cat.messages = {msg};
    const std::string text = c2::serialize(cat);
    CHECK(text.find(R"("max":0,)") != std::string::npos);
    CHECK(text.find(R"("min":0,)") != std::string::npos);
    const auto back = c2::parse(text);
    CHECK(back.has_value());
}

void testParseServerText() {
    // Copied from the server's encoders: v2_tiles.go, v2_server.go and v2_catalog.go.
    const auto runs = c2::parse(R"({"type":"runs","runs":[]})");
    CHECK(runs.has_value());
    if (runs) {
        const auto* r = std::get_if<c2::Runs>(&*runs);
        CHECK(r != nullptr && r->runs.empty());
    }

    // Features missing: an empty list, not an error.
    const auto ack = c2::parse(
        R"({"type":"hello_ack","protocol":2,"server":"photon-server","user":"test-user",)"
        R"("server_time_ns":1800000000123456768,"catalog_sha":"","live_run":"",)"
        R"("levels":[0,10,100,1000,10000,60000,600000]})");
    CHECK(ack.has_value());
    if (ack) {
        const auto* a = std::get_if<c2::HelloAck>(&*ack);
        CHECK(a != nullptr);
        if (a) {
            CHECK(a->features.empty());
            CHECK(a->levels.size() == 7);
            CHECK(a->levels[6] == 600000);
        }
    }

    const auto cat = c2::parse(R"({"type":"catalog","sha":"","buses":[],"messages":[]})");
    CHECK(cat.has_value());
    if (cat) {
        const auto* c = std::get_if<c2::Catalog>(&*cat);
        CHECK(c != nullptr && c->sha.empty() && c->messages.empty());
    }

    const auto tileError = c2::parse(R"({"type":"tile_error","qid":4,"message":"history is off"})");
    CHECK(tileError.has_value());
    if (tileError) {
        const auto* e = std::get_if<c2::TileError>(&*tileError);
        CHECK(e != nullptr && e->qid == 4 && e->message == "history is off");
    }

    const auto error = c2::parse(R"({"type":"error","code":"protocol","message":"protocol 2 required"})");
    CHECK(error.has_value());
    if (error) {
        const auto* e = std::get_if<c2::Error>(&*error);
        CHECK(e != nullptr && e->code == "protocol");
    }

    const auto pong = c2::parse(R"({"type":"pong","t_ms":1700000000000})");
    CHECK(pong.has_value());
    if (pong) {
        const auto* p = std::get_if<c2::Pong>(&*pong);
        CHECK(p != nullptr && p->tMs == 1'700'000'000'000);
    }
}

void testParseDefaults() {
    const auto ping = c2::parse(R"({"type":"ping"})");
    CHECK(ping.has_value());
    if (ping) {
        const auto* p = std::get_if<c2::Ping>(&*ping);
        CHECK(p != nullptr && p->tMs == 0);
    }

    // Unknown fields are ignored. A fraction in an integer field is cut toward zero.
    const auto extra = c2::parse(R"({"type":"pong","t_ms":2.9,"extra":[[[]]]})");
    CHECK(extra.has_value());
    if (extra) {
        const auto* p = std::get_if<c2::Pong>(&*extra);
        CHECK(p != nullptr && p->tMs == 2);
    }

    const auto noToken = c2::parse(R"({"type":"hello","client":"c"})");
    CHECK(noToken.has_value());
    if (noToken) {
        const auto* h = std::get_if<c2::Hello>(&*noToken);
        CHECK(h != nullptr && h->client == "c" && h->token.empty());
    }
}

void testParseRejectsBadInput() {
    std::string error;
    CHECK(!c2::parse("", &error).has_value());
    CHECK(!error.empty());
    CHECK(!c2::parse("[]").has_value());
    CHECK(!c2::parse("null").has_value());
    CHECK(!c2::parse("not json").has_value());
    CHECK(!c2::parse("{}").has_value());
    CHECK(!c2::parse(R"({"type":5})").has_value());
    CHECK(!c2::parse(R"({"type":"nope"})").has_value());
    CHECK(!c2::parse(R"({"type":"subscribe","signals":"x"})").has_value());
    CHECK(!c2::parse(R"({"type":"subscribe","signals":[1,"a"]})").has_value());
    CHECK(!c2::parse(R"({"type":"subscribe","signals":[-1]})").has_value());
    CHECK(!c2::parse(R"({"type":"tile_request","qid":4294967296})").has_value());
    CHECK(!c2::parse(R"({"type":"hello_ack","protocol":"2"})").has_value());
    CHECK(!c2::parse(R"({"type":"runs","runs":[1]})").has_value());
    CHECK(!c2::parse(R"({"type":"runs","runs":[{"rev":"x"}]})").has_value());
    CHECK(!c2::parse(R"({"type":"hello","token":null})").has_value());
    CHECK(!c2::parse(R"({"type":"catalog","messages":[1]})").has_value());
    CHECK(!c2::parse(R"({"type":"catalog","messages":[{"id":1,"signals":[{"integer":1}]}]})").has_value());
    CHECK(!c2::parse(R"({"type":"ping","t_ms":1e400})").has_value());
    CHECK(!c2::parse(R"({"type":"ping","t_ms":9223372036854775807})").has_value());
}

void testParseDeepNesting() {
    const std::string deepArrays = "{\"type\":\"ping\",\"x\":" + std::string(100000, '[') +
                                   std::string(100000, ']') + "}";
    CHECK(!c2::parse(deepArrays).has_value());
    CHECK(!c2::parse(std::string(100000, '[')).has_value());

    const auto shallow = c2::parse(R"({"type":"ping","t_ms":3,"x":[[[[[[[[[[1]]]]]]]]]]})");
    CHECK(shallow.has_value());

    // Brackets inside a string do not count toward the depth.
    const auto inString = c2::parse(R"({"type":"ping","t_ms":4,"s":")" + std::string(200, '[') + R"("})");
    CHECK(inString.has_value());
    const auto escaped = c2::parse(R"({"type":"ping","t_ms":5,"s":"\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\""})");
    CHECK(escaped.has_value());
}

// video_status carries an optional qid (after a video_seek) and an optional gap.
void testVideoStatusGap() {
    c2::VideoStatus withGap;
    withGap.mode = "replay";
    withGap.tMs = 3;
    withGap.clock = "frame";
    withGap.qid = 0x40000001u;
    withGap.gap = c2::VideoGap{1, 2, "none"};
    CHECK(c2::serialize(withGap) ==
          R"({"clock":"frame","gap":{"from_ms":1,"reason":"none","to_ms":2},"mode":"replay","qid":1073741825,"t_ms":3,"type":"video_status"})");
    expectRoundTrip(withGap);

    // A plain status has neither field, so the text is the same as before the gap existed.
    c2::VideoStatus plain;
    plain.mode = "live";
    plain.clock = "receive";
    CHECK(c2::serialize(plain) == R"({"clock":"receive","mode":"live","t_ms":0,"type":"video_status"})");

    const auto stale = c2::parse(
        R"({"type":"video_status","mode":"live","t_ms":5,"clock":"receive","gap":{"from_ms":100,"to_ms":0,"reason":"stale"}})");
    CHECK(stale.has_value());
    if (stale) {
        const auto& st = std::get<c2::VideoStatus>(*stale);
        CHECK(st.gap.has_value());
        CHECK(st.gap && st.gap->reason == "stale");
        CHECK(st.gap && st.gap->fromMs == 100 && st.gap->toMs == 0);
        CHECK(!st.qid.has_value());
    }

    const auto archived = c2::parse(
        R"({"type":"video_status","mode":"replay","t_ms":5,"clock":"ntp","qid":7,"gap":{"from_ms":1,"to_ms":9,"reason":"archived"}})");
    CHECK(archived.has_value());
    if (archived) {
        const auto& st = std::get<c2::VideoStatus>(*archived);
        CHECK(st.qid.has_value() && *st.qid == 7u);
        CHECK(st.gap && st.gap->reason == "archived");
    }

    CHECK(!c2::parse(R"({"type":"video_status","mode":"live","t_ms":5,"clock":"x","gap":5})").has_value());
    CHECK(!c2::parse(R"({"type":"video_status","mode":"live","t_ms":5,"clock":"x","gap":{"from_ms":"a"}})")
               .has_value());
    CHECK(!c2::parse(R"({"type":"video_status","mode":"live","t_ms":5,"clock":"x","qid":-1})").has_value());
}

}  // namespace

void runControlV2Tests() {
    testRoundTripEveryAlternative();
    testExactJsonText();
    testCatalogFields();
    testRunsFields();
    testHelloAckServerTime();
    testNonFiniteDoublesWrittenAsZero();
    testParseServerText();
    testParseDefaults();
    testParseRejectsBadInput();
    testParseDeepNesting();
    testVideoStatusGap();
}

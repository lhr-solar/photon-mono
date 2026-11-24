#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Protocol v2 JSON control messages (photon-next/docs/protocol-v2.md): one
// message per text frame, with a "type" field. serialize writes every field the
// spec lists; parse accepts what the server sends and gives defaults for missing
// fields. Both use the photon::json helpers the v1 control code uses.
namespace photon::ctrl2 {

inline constexpr int kProtocolVersion = 2;

// C -> S. token is empty when the client has none (Cloudflare or no password).
struct Hello {
    std::string client;
    std::string token;
};

// S -> C. serverTimeNs is read through a double, so it is good to about 256 ns.
struct HelloAck {
    int protocol = kProtocolVersion;
    std::string server;
    std::string user;
    std::int64_t serverTimeNs = 0;
    std::string catalogSha;
    std::string liveRun;
    std::vector<std::int64_t> levels;  // bucket ms per tile level; levels[0] = 0 is raw
    std::vector<std::string> features;
};

// One signal of the catalog. index is its position in the catalog for one sha.
struct CatalogSignal {
    std::uint32_t index = 0;
    std::string name;
    std::string unit;
    double min = 0.0;
    double max = 0.0;
    bool integer = false;
    int mux = -1;
    double periodMs = 0.0;  // 0 when unknown
};

struct CatalogMessage {
    std::uint32_t id = 0;
    std::string name;
    int bus = -1;  // index into Catalog::buses, or -1
    std::vector<CatalogSignal> signals;
};

// S -> C. An empty sha with no messages means decoding is off.
struct Catalog {
    std::string sha;
    std::vector<std::string> buses;
    std::vector<CatalogMessage> messages;
};

// C -> S. Replaces the previous set; an empty list stops live data.
struct Subscribe {
    std::vector<std::uint32_t> signals;
};

struct RunsList {};

struct Run {
    std::string id;
    std::string label;
    std::string source;
    std::string device;
    std::string dbcSha;
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::int64_t rev = 0;  // rises when the run's history is rewritten
    bool live = false;
};

// S -> C. The reply to runs_list.
struct Runs {
    std::vector<Run> runs;
};

// C -> S.
struct TileRequest {
    std::uint32_t qid = 0;
    std::string run;
    std::uint32_t signal = 0;
    int level = 0;
    std::int64_t index = 0;
};

struct TileCancel {
    std::uint32_t qid = 0;
};

// S -> C.
struct TileError {
    std::uint32_t qid = 0;
    std::string message;
};

// C -> S. mode is "live", "replay" or "off"; fromMs applies to replay.
struct VideoSubscribe {
    std::string mode;
    std::int64_t fromMs = 0;
};

struct VideoSeek {
    std::uint32_t qid = 0;
    std::int64_t tMs = 0;
};

// S -> C. clock says where the time came from.
struct VideoStatus {
    std::string mode;
    std::int64_t tMs = 0;
    std::string clock;
};

// Both directions. tMs is echoed back by the server's pong.
struct Ping {
    std::int64_t tMs = 0;
};

struct Pong {
    std::int64_t tMs = 0;
};

// S -> C.
struct Error {
    std::string code;
    std::string message;
};

using Message = std::variant<Hello, HelloAck, Catalog, Subscribe, RunsList, Runs, TileRequest, TileCancel,
                             TileError, VideoSubscribe, VideoSeek, VideoStatus, Ping, Pong, Error>;

std::string serialize(const Message& msg);
// parse returns nullopt for text that is not a JSON object, has no known "type",
// or has a field of the wrong kind. error, when given, says why.
std::optional<Message> parse(std::string_view text, std::string* error = nullptr);

}  // namespace photon::ctrl2

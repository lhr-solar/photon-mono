#pragma once

#include "decode/dbc.hpp"
#include "wire/protocol.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace photon {

struct SignalSample {
    double timestampMs = 0;
    double value = 0;
};

struct SignalRing {
    std::uint32_t id = 0;
    std::uint32_t canId = 0;
    std::string name;
    std::deque<SignalSample> samples;
    std::size_t lastEmitted = 0;
};

class DecodeBuffer {
public:
    void setDatabase(const dbc::Database* db);
    const dbc::Database* database() const { return db_; }
    void ingest(const proto::CanMonitorItem& item);
    std::vector<proto::TelemetryBlock> takeTelemetry(const std::unordered_set<std::uint32_t>& ids);
    std::vector<proto::TelemetryBlock> snapshot(const std::unordered_set<std::uint32_t>& ids) const;
    const SignalRing* ring(std::uint32_t id) const;
    bool latest(std::uint32_t id, double& value) const;
    void clear();
    void markCaughtUp();
    // When set, only these signal ids are decoded into rings. Empty means decode
    // nothing. Call before ingest so unused DBC signals never allocate 16 k rings.
    void setWanted(std::unordered_set<std::uint32_t> ids);

private:
    const dbc::Database* db_ = nullptr;
    std::unordered_map<std::uint32_t, SignalRing> rings_;
    std::unordered_set<std::uint32_t> wanted_;
    bool restrictWanted_ = false;
};

}  // namespace photon

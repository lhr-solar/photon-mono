#pragma once

#include "photon/canp.hpp"
#include "photon/control.hpp"
#include "photon/dbc.hpp"
#include "photon/net.hpp"
#include "photon/pcan.hpp"
#include "photon/protocol.hpp"
#include "photon/vsd1.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
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
    void ingest(const proto::CanMonitorItem& item);
    std::vector<proto::TelemetryBlock> takeTelemetry(const std::unordered_set<std::uint32_t>& ids);
    void clear();

private:
    const dbc::Database* db_ = nullptr;
    std::unordered_map<std::uint32_t, SignalRing> rings_;
};

class ISource {
public:
    virtual ~ISource() = default;
    virtual ctrl::SourceKind kind() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void tick(std::uint64_t nowMs) = 0;
    virtual void attachPoll(std::vector<net::PollFd>& fds) const = 0;
    virtual void onPoll(const std::vector<net::PollFd>& fds) = 0;
    virtual std::vector<proto::TelemetryBlock> takeTelemetry(const std::unordered_set<std::uint32_t>& ids) = 0;
    virtual std::vector<proto::CanMonitorItem> takeCanFrames() = 0;
    virtual proto::TimelineStatus timeline() const = 0;
    virtual void controlTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs, double speed) {
        (void)mode;
        (void)cursorMs;
        (void)speed;
    }
    virtual bool listenOnly() const { return true; }
    virtual bool writeCan(std::uint32_t canId, std::uint8_t dlc, const std::uint8_t data[8], bool extended,
                          std::string& error) {
        (void)canId;
        (void)dlc;
        (void)data;
        (void)extended;
        error = "source does not support CAN TX";
        return false;
    }
};

class SyntheticSource final : public ISource {
public:
    SyntheticSource();
    ctrl::SourceKind kind() const override { return ctrl::SourceKind::Synthetic; }
    void start() override;
    void stop() override;
    void tick(std::uint64_t nowMs) override;
    void attachPoll(std::vector<net::PollFd>&) const override {}
    void onPoll(const std::vector<net::PollFd>&) override {}
    std::vector<proto::TelemetryBlock> takeTelemetry(const std::unordered_set<std::uint32_t>& ids) override;
    std::vector<proto::CanMonitorItem> takeCanFrames() override;
    proto::TimelineStatus timeline() const override;

private:
    std::vector<SignalRing> signals_;
    std::vector<proto::CanMonitorItem> pendingCan_;
    std::uint64_t startMs_ = 0;
    std::uint64_t lastGenMs_ = 0;
    std::uint64_t minTimeMs_ = 0;
    std::uint64_t maxTimeMs_ = 0;
    std::uint64_t totalBatches_ = 0;
    std::uint32_t stepPhase_ = 0;
    int lastStepBucket_ = -1;
    bool running_ = false;

    void pushSample(SignalRing& ring, double tMs, double value);
};

class DaqTcpSource final : public ISource {
public:
    DaqTcpSource(std::string host, std::uint16_t port, DecodeBuffer* decode);
    ctrl::SourceKind kind() const override { return ctrl::SourceKind::DaqTcp; }
    void start() override;
    void stop() override;
    void tick(std::uint64_t nowMs) override;
    void attachPoll(std::vector<net::PollFd>& fds) const override;
    void onPoll(const std::vector<net::PollFd>& fds) override;
    std::vector<proto::TelemetryBlock> takeTelemetry(const std::unordered_set<std::uint32_t>&) override { return {}; }
    std::vector<proto::CanMonitorItem> takeCanFrames() override;
    proto::TimelineStatus timeline() const override;

private:
    std::string host_;
    std::uint16_t port_ = 6500;
    DecodeBuffer* decode_ = nullptr;
    net::Socket sock_;
    canp::StreamParser parser_;
    std::vector<proto::CanMonitorItem> pendingCan_;
    std::uint64_t lastConnectAttemptMs_ = 0;
    std::uint64_t minTimeMs_ = 0;
    std::uint64_t maxTimeMs_ = 0;
    std::uint64_t totalBatches_ = 0;
    bool running_ = false;

    void tryConnect(std::uint64_t nowMs);
    void readAvailable();
};

class LogReplaySource final : public ISource {
public:
    LogReplaySource(std::string path, DecodeBuffer* decode);
    ~LogReplaySource() override;
    LogReplaySource(const LogReplaySource&) = delete;
    LogReplaySource& operator=(const LogReplaySource&) = delete;

    bool open(std::string& error);
    ctrl::SourceKind kind() const override { return ctrl::SourceKind::LogReplay; }
    void start() override;
    void stop() override;
    void tick(std::uint64_t nowMs) override;
    void attachPoll(std::vector<net::PollFd>&) const override {}
    void onPoll(const std::vector<net::PollFd>&) override {}
    std::vector<proto::TelemetryBlock> takeTelemetry(const std::unordered_set<std::uint32_t>&) override { return {}; }
    std::vector<proto::CanMonitorItem> takeCanFrames() override;
    proto::TimelineStatus timeline() const override;
    void controlTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs, double speed) override;

private:
    std::string path_;
    DecodeBuffer* decode_ = nullptr;
    vsd1::Archive archive_;
    vsd1::ReplayCursor cursor_;
    std::vector<proto::CanMonitorItem> pendingCan_;
    std::uint64_t lastTickMs_ = 0;
    bool running_ = false;
    bool loggedFirstEmit_ = false;
    bool havePendingTimeline_ = false;
    ctrl::TimelineMode pendingMode_ = ctrl::TimelineMode::Play;
    std::uint64_t pendingCursorMs_ = 0;
    double pendingSpeed_ = 1.0;
    std::thread indexThread_;
    mutable std::mutex indexMu_;
    std::atomic<bool> cancelIndex_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::string indexError_;

    void emitRange(vsd1::ReplayCursor::EmitRange range);
    void applyPendingTimeline();
    void indexWorker();
};

class PcanSource final : public ISource {
public:
    PcanSource(pcan::Config cfg, DecodeBuffer* decode);
    ~PcanSource() override;
    PcanSource(const PcanSource&) = delete;
    PcanSource& operator=(const PcanSource&) = delete;

    bool open(std::string& error);
    ctrl::SourceKind kind() const override { return ctrl::SourceKind::Pcan; }
    void start() override;
    void stop() override;
    void tick(std::uint64_t nowMs) override;
    void attachPoll(std::vector<net::PollFd>&) const override {}
    void onPoll(const std::vector<net::PollFd>&) override {}
    std::vector<proto::TelemetryBlock> takeTelemetry(const std::unordered_set<std::uint32_t>&) override { return {}; }
    std::vector<proto::CanMonitorItem> takeCanFrames() override;
    proto::TimelineStatus timeline() const override;
    bool listenOnly() const override { return cfg_.listenOnly; }
    bool writeCan(std::uint32_t canId, std::uint8_t dlc, const std::uint8_t data[8], bool extended,
                  std::string& error) override;

private:
    pcan::Config cfg_{};
    DecodeBuffer* decode_ = nullptr;
    pcan::Device device_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex apiMu_;
    std::mutex inboxMu_;
    std::vector<proto::CanMonitorItem> inbox_;
    std::vector<proto::CanMonitorItem> pendingCan_;
    std::uint64_t minTimeMs_ = 0;
    std::uint64_t maxTimeMs_ = 0;
    std::uint64_t totalBatches_ = 0;
    std::uint64_t anchorUnixMs_ = 0;
    std::uint64_t anchorHwMs_ = 0;
    bool haveAnchor_ = false;

    void readLoop();
    void enqueue(const proto::CanMonitorItem& item);
};

class SourceHub {
public:
    explicit SourceHub(std::string daqHost, std::uint16_t daqPort);

    bool select(ctrl::SourceKind kind, const std::string& host, std::uint16_t port, const std::string& path,
                std::string& error, const pcan::Config& pcanCfg = {});
    bool writeCan(const dbc::EncodedFrame& frame, std::string& error);
    bool listenOnly() const;
    void tick(std::uint64_t nowMs);
    void attachPoll(std::vector<net::PollFd>& fds) const;
    void onPoll(const std::vector<net::PollFd>& fds);
    std::vector<proto::TelemetryBlock> takeTelemetry(const std::unordered_set<std::uint32_t>& ids);
    std::vector<proto::CanMonitorItem> takeCanFrames();
    proto::TimelineStatus timeline() const;
    ctrl::SourceKind kind() const;
    void stop();
    void setDatabase(const dbc::Database* db);
    void controlTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs, double speed);
    ctrl::Message::ActiveSource activeSource() const { return active_; }

private:
    std::string daqHost_;
    std::uint16_t daqPort_ = 6500;
    std::unique_ptr<ISource> source_;
    DecodeBuffer decode_;
    ctrl::Message::ActiveSource active_{};
};

}  // namespace photon

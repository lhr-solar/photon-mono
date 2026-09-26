#include "photon/session.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace photon {

bool SessionHub::liveSource() const {
    if (!writeLiveTail_) {
        return false;
    }
    const auto kind = sources_.kind();
    return kind == ctrl::SourceKind::DaqTcp || kind == ctrl::SourceKind::Pcan;
}

void SessionHub::recoverCrashedLive() {
    if (tsdbDir_.empty()) {
        return;
    }
    std::string error;
    const auto n = tsdb::recoverCrashedSessions(tsdbDir_, &error);
    if (n > 0) {
        std::fprintf(stdout, "[photon-backend] recovered %u crashed live session(s)\n", n);
    }
    if (!error.empty()) {
        std::fprintf(stderr, "[photon-backend] live recover: %s\n", error.c_str());
    }
}

void SessionHub::startLiveWriter(std::uint64_t firstMs) {
    if (!liveSource() || !dbcLoaded_ || tsdbDir_.empty() || liveWriter_) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(tsdbDir_, ec);
    int seq = 0;
    std::string id;
    for (;;) {
        id = tsdb::sessionIdFromMs(firstMs, seq);
        if (!std::filesystem::exists(std::filesystem::path(tsdbDir_) / id, ec)) {
            break;
        }
        ++seq;
        if (seq > 99) {
            return;
        }
    }
    auto writer = std::make_unique<tsdb::LiveTailWriter>();
    tsdb::LiveOptions opts;
    std::string error;
    if (!writer->begin(tsdbDir_, id, dbc_, opts, &error)) {
        std::fprintf(stderr, "[photon-backend] live writer begin failed: %s\n", error.c_str());
        return;
    }
    liveWriter_ = std::move(writer);
    liveSessionId_ = std::move(id);
    liveLastMs_ = firstMs;
    std::fprintf(stdout, "[photon-backend] live tail session %s\n", liveSessionId_.c_str());
}

void SessionHub::stopLiveWriter() {
    if (!liveWriter_) {
        return;
    }
    std::string error;
    const auto dir = liveWriter_->dir();
    if (!liveWriter_->end(&error)) {
        std::fprintf(stderr, "[photon-backend] live writer end: %s\n", error.empty() ? "failed" : error.c_str());
    }
    publishLiveCatalog();
    tsdb::CompactionOptions copts;
    tsdb::CompactionReport crep;
    std::string cerr;
    if (!dir.empty() && !tsdb::compactSession(dir, copts, crep, &cerr) && !cerr.empty()) {
        std::fprintf(stderr, "[photon-backend] live compact: %s\n", cerr.c_str());
    }
    if (!liveSessionId_.empty()) {
        historyStores_.erase(liveSessionId_);
    }
    liveWriter_.reset();
    liveSessionId_.clear();
    liveLastMs_ = 0;
}

void SessionHub::publishLiveCatalog() {
    if (!liveWriter_ || liveSessionId_.empty()) {
        return;
    }
    const auto& man = liveWriter_->store().manifest();
    tsdb::upsertCatalog(tsdbDir_, liveSessionId_, man.minMs, man.maxMs, man.dbcSha, nullptr);
}

void SessionHub::feedLive(std::span<const proto::CanMonitorItem> frames) {
    if (!liveSource() || !dbcLoaded_ || tsdbDir_.empty()) {
        return;
    }
    for (const auto& frame : frames) {
        if (liveWriter_ && tsdb::isSessionBoundary(frame.timestampMs, liveLastMs_)) {
            stopLiveWriter();
        }
        if (!liveWriter_) {
            startLiveWriter(frame.timestampMs);
        }
        if (!liveWriter_) {
            return;
        }
        liveWriter_->onFrame(frame.canId, frame.dlc, frame.data, frame.timestampMs);
        if (liveWriter_->failed()) {
            std::fprintf(stderr, "[photon-backend] live writer: %s\n", liveWriter_->lastError().c_str());
            liveWriter_.reset();
            liveSessionId_.clear();
            return;
        }
        if (frame.timestampMs > liveLastMs_) {
            liveLastMs_ = frame.timestampMs;
        }
    }
    if (liveWriter_ && liveLastMs_ >= lastLiveCatalogMs_ + 5000) {
        lastLiveCatalogMs_ = liveLastMs_;
        publishLiveCatalog();
    }
}

void SessionHub::tickRetention(std::uint64_t nowMs) {
    if (tsdbDir_.empty() || nowMs < lastRetentionMs_ + 600000) {
        return;
    }
    lastRetentionMs_ = nowMs;
    tsdb::RetentionPolicy policy;
    if (const char* env =
#if defined(_MSC_VER)
            []() -> const char* {
                static char buf[32];
                size_t n = 0;
                return getenv_s(&n, buf, sizeof(buf), "PHOTON_L0_MAX_AGE_DAYS") == 0 && n > 1 ? buf : nullptr;
            }()
#else
            std::getenv("PHOTON_L0_MAX_AGE_DAYS")
#endif
    ) {
        const auto days = std::strtoull(env, nullptr, 10);
        if (days > 0) {
            policy.l0MaxAgeMs = days * 86400000ull;
        }
    }
    if (policy.l0MaxAgeMs == 0 && policy.l0BudgetBytes == 0) {
        return;
    }
    std::string error;
    const auto report = tsdb::applyRetention(tsdbDir_, policy, nowMs, &error);
    if (!report.ok && !error.empty()) {
        std::fprintf(stderr, "[photon-backend] retention: %s\n", error.c_str());
    } else if (report.l0BytesFreed > 0) {
        std::fprintf(stdout, "[photon-backend] retention freed %llu L0 bytes\n",
                     static_cast<unsigned long long>(report.l0BytesFreed));
    }
}

}  // namespace photon

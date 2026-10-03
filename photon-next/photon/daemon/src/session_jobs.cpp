#include "photon/session.hpp"

#include "photon/analysis_compute.hpp"
#include "photon/csv_export.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace photon {
namespace {

std::string sidecarClientIdFor(const ClientSession& client) {
    std::string id = std::to_string(client.id) + ":" + client.label;
    if (id.size() > 96) {
        id.resize(96);
    }
    for (char& c : id) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '_' || c == '-' || c == '.' || c == ':') {
            continue;
        }
        c = '_';
    }
    return id;
}

}  // namespace

void SessionHub::startCsvExport(net::Socket sock, std::string target) {
    if (exportBusy_.load() >= 2) {
        sock.setNonBlocking(false);
        const char* resp =
            "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: 12\r\nConnection: close\r\n\r\nexport busy\n";
        sock.sendSome(resp, std::strlen(resp));
        return;
    }
    exportBusy_.fetch_add(1);
    std::thread([sock = std::move(sock), target = std::move(target), dir = tsdbDir_,
                 busy = &exportBusy_]() mutable {
        handleCsvExport(std::move(sock), target, dir);
        busy->fetch_sub(1);
    }).detach();
}

void SessionHub::enqueueTrackSim(ClientSession& client, const ctrl::Message& req) {
    if (!client.helloOk) {
        sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
        return;
    }
    if (!(req.timeBudgetS > 0.0)) {
        sendControl(client, ctrl::errorAlert("bad_message", "time_budget_s must be positive"));
        return;
    }

    tracksim::SolveInput input;
    input.timeBudgetS = req.timeBudgetS;
    input.initialSoc = req.initialSoc;
    input.car = tracksim::applyCarOverride(tracksim::defaultCar(), req.carOverride);

    if (req.track) {
        input.track = *req.track;
    } else if (!req.trackId.empty()) {
        std::string loadError;
        auto track = catalog_.load(req.trackId, &loadError);
        if (!track) {
            sendControl(client, ctrl::errorAlert("unknown_track", loadError.empty() ? "unknown track_id" : loadError));
            return;
        }
        input.track = std::move(*track);
    } else {
        sendControl(client, ctrl::errorAlert("bad_message", "track_id or inline track required"));
        return;
    }

    const std::string trackId = input.track.id.empty() ? req.trackId : input.track.id;
    const std::uint64_t clientId = client.id;
    trackSimJobs_.push_back(TrackSimJob{
        clientId,
        std::async(std::launch::async,
                   [input, trackId]() { return ctrl::trackSimResult(tracksim::solve(input), trackId); }),
    });
}

void SessionHub::pollTrackSimJobs() {
    for (auto it = trackSimJobs_.begin(); it != trackSimJobs_.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto msg = it->future.get();
        if (auto* client = findClient(it->clientId)) {
            sendControl(*client, msg);
        }
        it = trackSimJobs_.erase(it);
    }
}

void SessionHub::handleAnalysisCompute(ClientSession& client, const ctrl::Message& req) {
    if (!client.helloOk) {
        sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
        return;
    }
    if (req.jobId.empty()) {
        sendControl(client, ctrl::errorAlert("bad_message", "job_id required"));
        return;
    }

    analysis::Addr addr;
    if (!analysis::parseAddr(analysisComputeAddr_, addr)) {
        sendControl(client, analysis::offlineResult(req.jobId));
        return;
    }

    int inflightClient = 0;
    for (const auto& job : analysisJobs_) {
        if (job.clientId == client.id) {
            ++inflightClient;
        }
    }
    if (inflightClient >= analysis::kMaxInflightPerClient) {
        sendControl(client, analysis::busyResult(req.jobId));
        return;
    }
    if (static_cast<int>(analysisJobs_.size()) >= analysis::kMaxInflightGlobal) {
        sendControl(client, analysis::busyResult(req.jobId));
        return;
    }

    analysis::JobSpec spec;
    spec.jobId = req.jobId;
    spec.clientId = sidecarClientIdFor(client);
    spec.sessionId = req.sessionId;
    spec.dbcHash = req.dbcHash.empty() ? req.dbcId : req.dbcHash;
    spec.signalIds = req.signalIds;
    spec.t0Ms = req.t0Ms != 0 ? req.t0Ms : req.fromMs;
    spec.t1Ms = req.t1Ms != 0 ? req.t1Ms : req.toMs;
    spec.maxPoints = req.maxPoints;
    spec.prompt = req.prompt;
    spec.recipeId = req.recipeId;
    spec.kind = req.computeKind.empty() ? "grok" : req.computeKind;
    spec.timeoutS = analysis::kDefaultTimeoutS;

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    const std::uint64_t clientId = client.id;
    analysisJobs_.push_back(AnalysisJob{
        clientId,
        req.jobId,
        spec.clientId,
        cancel,
        std::async(std::launch::async, [addr, spec, cancel]() { return analysis::runJob(addr, spec, cancel.get()); }),
    });
}

void SessionHub::handleAnalysisCancel(ClientSession& client, const ctrl::Message& req) {
    if (!client.helloOk) {
        sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
        return;
    }
    if (req.jobId.empty()) {
        sendControl(client, ctrl::errorAlert("bad_message", "job_id required"));
        return;
    }
    bool found = false;
    for (auto& job : analysisJobs_) {
        if (job.clientId == client.id && job.jobId == req.jobId) {
            found = true;
            if (job.cancel) {
                job.cancel->store(true);
            }
        }
    }
    if (!found) {
        sendControl(client, ctrl::analysisComputeResult(req.jobId, "failed", {}, "unknown_job", "job not found"));
        return;
    }
}

void SessionHub::pollAnalysisJobs() {
    for (auto it = analysisJobs_.begin(); it != analysisJobs_.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto msg = it->future.get();
        if (auto* client = findClient(it->clientId)) {
            sendControl(*client, msg);
        }
        it = analysisJobs_.erase(it);
    }
}

void SessionHub::cancelAllAnalysisJobs() {
    analysis::Addr addr;
    const bool haveAddr = analysis::parseAddr(analysisComputeAddr_, addr);
    for (auto& job : analysisJobs_) {
        if (job.cancel) {
            job.cancel->store(true);
        }
        if (haveAddr && !job.jobId.empty() && !job.sidecarClientId.empty()) {
            analysis::cancelJob(addr, job.jobId, job.sidecarClientId);
        }
        analysis::removeBundle((std::filesystem::temp_directory_path() / "photon-analysis-compute" / job.jobId /
                                "bundle.json")
                                   .string());
    }
    for (auto& job : analysisJobs_) {
        if (job.future.valid()) {
            job.future.wait();
        }
    }
    analysisJobs_.clear();
}

}  // namespace photon

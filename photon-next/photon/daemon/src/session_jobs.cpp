#include "photon/session.hpp"

#include "photon/csv_export.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace photon {

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

}  // namespace photon

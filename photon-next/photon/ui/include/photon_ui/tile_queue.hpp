#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace photon::ui {

// The client's tile requests: at most kMaxInFlight tile_request messages are out
// at once (the server refuses past 32). Visible tiles go before priming ones.
// Pure data, no socket, so the cap is testable.
class TileRequestQueue {
public:
    static constexpr std::size_t kMaxInFlight = 24;
    static constexpr std::size_t kMaxQueued = 2048;

    struct Request {
        std::uint32_t qid = 0;
        std::string run;
        std::uint32_t signal = 0;  // client id; the wire index is signal - 1
        int level = 0;
        std::int64_t index = 0;
        bool urgent = false;
    };

    bool full() const { return urgent_.size() + prime_.size() >= kMaxQueued; }
    std::size_t queued() const { return urgent_.size() + prime_.size(); }
    std::size_t inflight() const { return inflight_.size(); }
    bool isInflight(std::uint32_t qid) const { return inflight_.contains(qid); }

    void push(Request r) {
        if (r.urgent) {
            urgent_.push_back(std::move(r));
        } else {
            prime_.push_back(std::move(r));
        }
    }

    // The requests that may be sent now, in order. Each one moves to in-flight.
    std::vector<Request> take() {
        std::vector<Request> out;
        while (inflight_.size() < kMaxInFlight && (!urgent_.empty() || !prime_.empty())) {
            std::deque<Request>& q = urgent_.empty() ? prime_ : urgent_;
            Request r = std::move(q.front());
            q.pop_front();
            inflight_.insert(r.qid);
            out.push_back(std::move(r));
        }
        return out;
    }

    // A request finished: its last piece arrived, it failed, or it was cancelled.
    void done(std::uint32_t qid) { inflight_.erase(qid); }

    // Removes a queued request. True when it was queued (nothing was sent for it).
    bool dropQueued(std::uint32_t qid) {
        for (auto* q : {&urgent_, &prime_}) {
            for (auto it = q->begin(); it != q->end(); ++it) {
                if (it->qid == qid) {
                    q->erase(it);
                    return true;
                }
            }
        }
        return false;
    }

    // Everything the queue holds, queued or in flight, as qids. The queue is emptied.
    std::vector<std::uint32_t> clear() {
        std::vector<std::uint32_t> out;
        out.reserve(queued() + inflight_.size());
        for (auto* q : {&urgent_, &prime_}) {
            for (const auto& r : *q) {
                out.push_back(r.qid);
            }
        }
        for (const std::uint32_t qid : inflight_) {
            out.push_back(qid);
        }
        urgent_.clear();
        prime_.clear();
        inflight_.clear();
        return out;
    }

private:
    std::deque<Request> urgent_;
    std::deque<Request> prime_;
    std::unordered_set<std::uint32_t> inflight_;
};

}  // namespace photon::ui

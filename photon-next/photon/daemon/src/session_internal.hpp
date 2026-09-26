#pragma once

#include "photon/session.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace photon {

class SessionHub::HistoryPool {
public:
    explicit HistoryPool(std::size_t workers = 4) {
        threads_.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            threads_.emplace_back([this] { loop(); });
        }
    }

    ~HistoryPool() { stop(); }

    std::future<HistoryWork> submit(std::function<HistoryWork()> fn) {
        std::packaged_task<HistoryWork()> task(std::move(fn));
        auto fut = task.get_future();
        {
            std::lock_guard<std::mutex> g(mu_);
            if (stop_) {
                return fut;
            }
            q_.push_back(std::move(task));
        }
        cv_.notify_one();
        return fut;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (stop_) {
                return;
            }
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }

private:
    void loop() {
        for (;;) {
            std::packaged_task<HistoryWork()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) {
                    return;
                }
                task = std::move(q_.front());
                q_.pop_front();
            }
            task();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::packaged_task<HistoryWork()>> q_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

}  // namespace photon

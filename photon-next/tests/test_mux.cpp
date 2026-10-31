#include "ui/mux.hpp"
#include "test_harness.hpp"

#include <cmath>
#include <vector>

void runMuxTests() {
    using photon::ui::applyMuxSeries;

    {
        // Reconstructing an earlier/empty cursor must not retain later taps.
        std::vector<float> out(32, 3.9f);
        std::vector<double> stamps(32, 9000.0);
        const double t[] = {1000.0}, idx[] = {2.0}, value[] = {3.1};
        applyMuxSeries(t, idx, 1, t, value, 1, out, &stamps, 32, true);
        CHECK(near(out[2], 3.1, 1e-5));
        CHECK(out[7] == 0.0f && stamps[7] == 0.0);
        applyMuxSeries(nullptr, nullptr, 0, nullptr, nullptr, 0, out, &stamps, 32, true);
        CHECK(out[2] == 0.0f && stamps[2] == 0.0);
    }

    {
        // One CANP batch: 32 taps, one millisecond.
        std::vector<double> t(32, 1000.0);
        std::vector<double> idx(32);
        std::vector<double> val(32);
        for (int i = 0; i < 32; ++i) {
            idx[static_cast<std::size_t>(i)] = static_cast<double>(i);
            val[static_cast<std::size_t>(i)] = 3.200 + 0.010 * static_cast<double>(i);
        }
        std::vector<float> out;
        applyMuxSeries(t.data(), idx.data(), 32, t.data(), val.data(), 32, out, nullptr, 32, true);
        CHECK(out.size() == 32);
        int filled = 0;
        for (int i = 0; i < 32; ++i) {
            if (out[static_cast<std::size_t>(i)] > 0.05f) {
                ++filled;
                CHECK(near(static_cast<double>(out[static_cast<std::size_t>(i)]),
                           3.200 + 0.010 * static_cast<double>(i), 1e-5));
            }
        }
        CHECK(filled == 32);
    }

    {
        // Unique timestamps still fill the pack.
        std::vector<double> t(32);
        std::vector<double> idx(32);
        std::vector<double> val(32);
        for (int i = 0; i < 32; ++i) {
            t[static_cast<std::size_t>(i)] = 2000.0 + static_cast<double>(i);
            idx[static_cast<std::size_t>(i)] = static_cast<double>(i);
            val[static_cast<std::size_t>(i)] = 3.5;
        }
        std::vector<float> out;
        applyMuxSeries(t.data(), idx.data(), 32, t.data(), val.data(), 32, out, nullptr, 32, true);
        CHECK(out.size() == 32);
        int filled = 0;
        for (float v : out) {
            if (v > 0.05f) {
                ++filled;
            }
        }
        CHECK(filled == 32);
    }

    {
        // Newest sample wins when a tap repeats.
        const double idxT[] = {10.0, 20.0, 20.0};
        const double idxV[] = {0.0, 1.0, 0.0};
        const double valT[] = {10.0, 20.0, 20.0};
        const double valV[] = {3.1, 3.2, 3.9};
        std::vector<float> out;
        applyMuxSeries(idxT, idxV, 3, valT, valV, 3, out, nullptr, 32, true);
        CHECK(out.size() == 32);
        CHECK(near(static_cast<double>(out[0]), 3.9, 1e-5));
        CHECK(near(static_cast<double>(out[1]), 3.2, 1e-5));
    }

    {
        // Keep last pack when reset is false and this window misses a tap.
        std::vector<float> out(32, 0.0f);
        out[7] = 3.77f;
        const double idxT[] = {50.0};
        const double idxV[] = {3.0};
        const double valT[] = {50.0};
        const double valV[] = {3.33};
        applyMuxSeries(idxT, idxV, 1, valT, valV, 1, out, nullptr, 32, false);
        CHECK(out.size() == 32);
        CHECK(near(static_cast<double>(out[3]), 3.33, 1e-5));
        CHECK(near(static_cast<double>(out[7]), 3.77, 1e-5));
    }

    {
        // Unequal lengths: consume per-timestamp queues.
        const double idxT[] = {9.0, 9.0, 9.0};
        const double idxV[] = {0.0, 1.0, 2.0};
        const double valT[] = {9.0, 9.0};
        const double valV[] = {3.01, 3.02};
        std::vector<float> out;
        applyMuxSeries(idxT, idxV, 3, valT, valV, 2, out, nullptr, 32, true);
        CHECK(out.size() == 32);
        int filled = 0;
        for (float v : out) {
            if (v > 0.05f) {
                ++filled;
            }
        }
        CHECK(filled == 2);
        CHECK(out[1] > 0.05f);
        CHECK(out[2] > 0.05f);
    }
}

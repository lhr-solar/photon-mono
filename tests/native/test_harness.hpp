#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>

inline int gFailed = 0;
inline int gPassed = 0;

inline void expect(bool cond, const char* expr, const char* file, int line) {
    if (cond) {
        ++gPassed;
        return;
    }
    ++gFailed;
    std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
}

#define CHECK(cond) expect(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

inline bool near(double a, double b, double absTol = 1e-6, double relTol = 1e-6) {
    const double diff = std::fabs(a - b);
    const double scale = std::max(std::fabs(a), std::fabs(b));
    return diff <= absTol || diff <= relTol * scale;
}

#include "photon/vsd1.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : R"(C:\Users\alper\Downloads\logs\logs\000000001906.log)";
    std::string err;
    auto archive = photon::vsd1::indexPath(path, &err);
    if (!archive) {
        std::fprintf(stderr, "index failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stdout, "files=%zu batches=%zu min=%llu max=%llu\n", archive->files.size(),
                 archive->batches.size(), static_cast<unsigned long long>(archive->minTimeMs),
                 static_cast<unsigned long long>(archive->maxTimeMs));
    const std::size_t n = archive->batches.size() < 8 ? archive->batches.size() : 8;
    int ok = 0;
    int fail = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& item = archive->batches[i];
        std::string readErr;
        auto batch = photon::vsd1::readBatch(*archive, item, &readErr);
        if (!batch) {
            ++fail;
            std::fprintf(stdout, "read[%zu] FAIL file=%zu off=%llu ts=%llu err=%s\n", i, item.fileIndex,
                         static_cast<unsigned long long>(item.frameOffset),
                         static_cast<unsigned long long>(item.timestampMs), readErr.c_str());
            continue;
        }
        ++ok;
        std::fprintf(stdout, "read[%zu] OK count=%u can0=0x%x ts=%llu file=%zu off=%llu\n", i,
                     static_cast<unsigned>(batch->count),
                     batch->count ? batch->packets[0].canId : 0,
                     static_cast<unsigned long long>(batch->timestamp), item.fileIndex,
                     static_cast<unsigned long long>(item.frameOffset));
    }
    const std::size_t mid = archive->batches.size() / 2;
    if (mid >= n) {
        std::string readErr;
        auto batch = photon::vsd1::readBatch(*archive, archive->batches[mid], &readErr);
        std::fprintf(stdout, "read[mid %zu] %s err=%s count=%u\n", mid, batch ? "OK" : "FAIL",
                     readErr.c_str(), batch ? static_cast<unsigned>(batch->count) : 0);
        if (batch) {
            ++ok;
        } else {
            ++fail;
        }
    }
    std::fprintf(stdout, "ok=%d fail=%d\n", ok, fail);
    return fail == 0 ? 0 : 2;
}

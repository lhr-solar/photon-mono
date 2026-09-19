#include "photon/csv_export.hpp"

#include "export.hpp"
#include "photon/tsdb.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace photon {
namespace {

bool sendAll(const net::Socket& sock, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const int n = sock.sendSome(data + off, len - off);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool sendAll(const net::Socket& sock, std::string_view text) {
    return sendAll(sock, text.data(), text.size());
}

void sendError(const net::Socket& sock, int code, std::string_view reason, std::string_view body) {
    char hdr[256];
    const int n = std::snprintf(hdr, sizeof(hdr),
                                "HTTP/1.0 %d %.*s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                                "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                code, static_cast<int>(reason.size()), reason.data(), body.size());
    if (n > 0) {
        sendAll(sock, hdr, static_cast<std::size_t>(n));
    }
    sendAll(sock, body);
}

}  // namespace

void handleCsvExport(net::Socket sock, std::string_view target, const std::string& tsdbDir) {
    sock.setNonBlocking(false);
    sock.setNoDelay(true);

    pexport::CsvQuery q;
    if (!pexport::parseCsvQuery(target, &q)) {
        sendError(sock, 400, "Bad Request", q.error.empty() ? "missing session" : q.error);
        return;
    }

    tsdb::Store store;
    std::string error;
    const auto dir = std::filesystem::path(tsdbDir) / q.session;
    if (!store.open(dir, &error)) {
        sendError(sock, 404, "Not Found", error.empty() ? "unknown session" : error);
        return;
    }

    pexport::Request req;
    const auto& man = store.manifest();
    req.fromMs = q.haveFrom ? q.fromMs : man.minMs;
    req.toMs = q.haveTo ? q.toMs : man.maxMs;
    req.level = q.level;
    req.maxPoints = 1u << 30;
    if (q.allSignals || q.selectors.empty()) {
        req.signalIds = pexport::signalsWithData(store);
    } else {
        req.signalIds = pexport::resolveSignalSelectors(store, q.selectors);
        if (req.signalIds.empty()) {
            sendError(sock, 404, "Not Found", "no matching signals");
            return;
        }
    }
    if (req.fromMs > req.toMs) {
        sendError(sock, 400, "Bad Request", "from is after to");
        return;
    }

    char hdr[384];
    const int hn = std::snprintf(hdr, sizeof(hdr),
                                 "HTTP/1.0 200 OK\r\n"
                                 "Content-Type: text/csv; charset=utf-8\r\n"
                                 "Content-Disposition: attachment; filename=\"photon-%s.csv\"\r\n"
                                 "Cache-Control: no-store\r\n"
                                 "Connection: close\r\n\r\n",
                                 q.session.c_str());
    if (hn <= 0 || !sendAll(sock, hdr, static_cast<std::size_t>(hn))) {
        return;
    }

    std::string buf;
    buf.reserve(64 * 1024);
    auto flush = [&]() -> bool {
        if (buf.empty()) {
            return true;
        }
        const bool ok = sendAll(sock, buf);
        buf.clear();
        return ok;
    };
    pexport::Report rep;
    const bool ok = pexport::writeCsv(store, req, [&](std::string_view chunk) {
        buf.append(chunk);
        if (buf.size() >= 64 * 1024) {
            return flush();
        }
        return true;
    }, &rep);
    if (!flush() || !ok) {
        return;
    }
    (void)rep;
}

}  // namespace photon

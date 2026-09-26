#include "photon/analysis_compute.hpp"

#include "photon/json.hpp"
#include "photon/net.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace photon::analysis {
namespace {

bool sendAll(const net::Socket& sock, const void* data, std::size_t n, int timeoutMs) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t off = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (off < n) {
        const int w = sock.sendSome(p + off, n - off);
        if (w < 0) {
            return false;
        }
        if (w == 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            net::PollFd pfd;
            pfd.fd = sock.native();
            pfd.events = net::kPollOut;
            if (net::poll(&pfd, 1, 50) < 0) {
                return false;
            }
            continue;
        }
        off += static_cast<std::size_t>(w);
    }
    return true;
}

bool recvUntil(const net::Socket& sock, std::string& out, int timeoutMs, const std::string& needle) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char tmp[1024];
    while (out.find(needle) == std::string::npos) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        const int n = sock.recvSome(tmp, sizeof(tmp));
        if (n < 0) {
            return !out.empty();
        }
        if (n == 0) {
            net::PollFd pfd;
            pfd.fd = sock.native();
            pfd.events = net::kPollIn;
            if (net::poll(&pfd, 1, 50) < 0) {
                return false;
            }
            continue;
        }
        out.append(tmp, static_cast<std::size_t>(n));
        if (out.size() > 256 * 1024) {
            return false;
        }
    }
    return true;
}

bool headerSafe(std::string_view s) {
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}

bool httpJson(const Addr& addr, const std::string& method, const std::string& path, const std::string& body,
              int& status, std::string& respBody, std::string* error, std::string_view clientId = {}) {
    status = 0;
    respBody.clear();
    auto sock = net::connectTcpTimed(addr.host, addr.port, 400);
    if (!sock.valid()) {
        if (error) {
            *error = "sidecar unreachable";
        }
        return false;
    }
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << addr.host << ":" << addr.port << "\r\n";
    req << "Connection: close\r\n";
    if (!clientId.empty() && headerSafe(clientId)) {
        req << "X-Client-Id: " << clientId << "\r\n";
    }
    if (!body.empty()) {
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n";
    req << body;
    const std::string wire = req.str();
    if (!sendAll(sock, wire.data(), wire.size(), 800)) {
        if (error) {
            *error = "sidecar write failed";
        }
        return false;
    }
    std::string raw;
    if (!recvUntil(sock, raw, 1500, "\r\n\r\n")) {
        if (error) {
            *error = "sidecar read failed";
        }
        return false;
    }
    const auto headerEnd = raw.find("\r\n\r\n");
    const std::string headers = raw.substr(0, headerEnd);
    respBody = raw.substr(headerEnd + 4);
    int contentLen = -1;
    auto findLen = [&](const char* key) {
        const auto pos = headers.find(key);
        if (pos != std::string::npos) {
            contentLen = std::atoi(headers.c_str() + pos + std::strlen(key));
        }
    };
    findLen("Content-Length:");
    if (contentLen < 0) {
        findLen("content-length:");
    }
    if (contentLen > 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (static_cast<int>(respBody.size()) < contentLen) {
            if (std::chrono::steady_clock::now() > deadline) {
                break;
            }
            char tmp[1024];
            const int n = sock.recvSome(tmp, sizeof(tmp));
            if (n < 0) {
                break;
            }
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            respBody.append(tmp, static_cast<std::size_t>(n));
        }
        if (static_cast<int>(respBody.size()) > contentLen) {
            respBody.resize(static_cast<std::size_t>(contentLen));
        }
    }
    if (headers.size() >= 12 && headers.compare(0, 5, "HTTP/") == 0) {
        const auto sp = headers.find(' ');
        if (sp != std::string::npos) {
            status = std::atoi(headers.c_str() + sp + 1);
        }
    }
    return status > 0;
}

ctrl::Message viewToResult(const json::Value& root, const std::string& fallbackJob) {
    ctrl::Message out;
    out.type = ctrl::Type::AnalysisComputeResult;
    out.jobId = root["jobId"].asString();
    if (out.jobId.empty()) {
        out.jobId = root["job_id"].asString();
    }
    if (out.jobId.empty()) {
        out.jobId = fallbackJob;
    }
    out.computeStatus = root["status"].asString();
    out.summary = root["summary"].asString();
    out.code = root["code"].asString();
    out.message = root["message"].asString();
    if (root["derived"].isArray()) {
        for (const auto& item : root["derived"].asArray()) {
            if (!item.isObject()) {
                continue;
            }
            ctrl::Message::DerivedChannel row;
            row.name = item["name"].asString();
            row.unit = item["unit"].asString();
            row.pointsRef = item["pointsRef"].asString();
            if (row.pointsRef.empty()) {
                row.pointsRef = item["points_ref"].asString();
            }
            out.derived.push_back(std::move(row));
        }
    }
    if (root["events"].isArray()) {
        for (const auto& item : root["events"].asArray()) {
            if (!item.isObject()) {
                continue;
            }
            ctrl::Message::ComputeEvent row;
            row.tMs = static_cast<std::uint64_t>(item["tMs"].asInt());
            if (row.tMs == 0) {
                row.tMs = static_cast<std::uint64_t>(item["t_ms"].asInt());
            }
            row.name = item["name"].asString();
            row.message = item["message"].asString();
            out.events.push_back(std::move(row));
        }
    }
    if (out.computeStatus.empty()) {
        out.computeStatus = "failed";
        out.code = "analysis_error";
        out.message = "invalid sidecar response";
    }
    if (out.summary.size() > 16 * 1024) {
        out.summary.resize(16 * 1024);
    }
    return out;
}

std::string clientIdJSON(std::string_view clientId) {
    json::Object obj;
    obj.emplace("clientId", json::Value(std::string(clientId)));
    return json::stringify(json::Value(std::move(obj)));
}

}  // namespace

bool parseAddr(std::string_view text, Addr& out) {
    if (text.empty() || text == "off" || text == "-" || text == "0") {
        return false;
    }
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    out.host = std::string(text.substr(0, colon));
    char* end = nullptr;
    const std::string portStr(text.substr(colon + 1));
    const unsigned long v = std::strtoul(portStr.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v == 0 || v > 65535ul) {
        return false;
    }
    out.port = static_cast<std::uint16_t>(v);
    return out.host == "127.0.0.1" || out.host == "localhost";
}

ctrl::Message offlineResult(std::string jobId, std::string message) {
    if (message.empty()) {
        message = "analysis compute sidecar is not running";
    }
    return ctrl::analysisComputeResult(std::move(jobId), "failed", {}, kOfflineCode, std::move(message));
}

ctrl::Message busyResult(std::string jobId) {
    return ctrl::analysisComputeResult(std::move(jobId), "failed", {}, kBusyCode,
                                       "analysis compute inflight limit reached");
}

bool writeStubBundle(const JobSpec& spec, std::string& pathOut, std::string* error) {
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path() / "photon-analysis-compute";
    std::filesystem::create_directories(root, ec);
    if (ec) {
        if (error) {
            *error = "failed to create analysis scratch";
        }
        return false;
    }
    std::string dirName = spec.jobId.empty() ? "job" : spec.jobId;
    for (char& c : dirName) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '-' || c == '_' || c == '.')) {
            c = '_';
        }
    }
    const auto dir = root / dirName;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (error) {
            *error = "failed to create job scratch";
        }
        return false;
    }
    json::Object obj;
    obj.emplace("sessionId", json::Value(spec.sessionId));
    obj.emplace("dbcHash", json::Value(spec.dbcHash));
    obj.emplace("t0Ms", json::Value(spec.t0Ms));
    obj.emplace("t1Ms", json::Value(spec.t1Ms));
    obj.emplace("maxPoints", json::Value(static_cast<int>(spec.maxPoints)));
    obj.emplace("prompt", json::Value(spec.prompt));
    obj.emplace("recipeId", json::Value(spec.recipeId));
    obj.emplace("kind", json::Value(spec.kind));
    json::Array ids;
    ids.reserve(spec.signalIds.size());
    for (const auto id : spec.signalIds) {
        ids.emplace_back(static_cast<std::uint64_t>(id));
    }
    obj.emplace("signalIds", json::Value(std::move(ids)));
    const auto path = dir / "bundle.json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "failed to write job bundle";
        }
        return false;
    }
    out << json::stringify(json::Value(std::move(obj)));
    pathOut = path.string();
    return static_cast<bool>(out);
}

ctrl::Message runJob(const Addr& addr, const JobSpec& spec, const std::atomic<bool>* cancel) {
    std::string bundlePath;
    std::string bundleError;
    if (!spec.jobId.empty()) {
        writeStubBundle(spec, bundlePath, &bundleError);
    }
    json::Object body;
    body.emplace("jobId", json::Value(spec.jobId));
    body.emplace("clientId", json::Value(spec.clientId));
    body.emplace("bundlePath", json::Value(bundlePath));
    body.emplace("recipe", json::Value(spec.recipeId.empty() ? (spec.prompt.empty() ? "summarize_window" : "freeform")
                                                            : spec.recipeId));
    body.emplace("timeoutS", json::Value(spec.timeoutS > 0 ? spec.timeoutS : kDefaultTimeoutS));
    body.emplace("sessionId", json::Value(spec.sessionId));
    body.emplace("t0Ms", json::Value(spec.t0Ms));
    body.emplace("t1Ms", json::Value(spec.t1Ms));
    body.emplace("signalCount", json::Value(static_cast<int>(spec.signalIds.size())));
    if (!spec.prompt.empty()) {
        body.emplace("prompt", json::Value(spec.prompt));
    }
    const std::string payload = json::stringify(json::Value(std::move(body)));

    int status = 0;
    std::string resp;
    std::string err;
    if (!httpJson(addr, "POST", "/jobs", payload, status, resp, &err)) {
        removeBundle(bundlePath);
        return offlineResult(spec.jobId, err.empty() ? "analysis compute sidecar is not running" : err);
    }
    if (status == 503) {
        removeBundle(bundlePath);
        auto msg = busyResult(spec.jobId);
        const auto parsed = json::parse(resp);
        if (parsed && parsed->isObject()) {
            msg = viewToResult(*parsed, spec.jobId);
        }
        return msg;
    }
    if (status < 200 || status >= 300) {
        removeBundle(bundlePath);
        const auto parsed = json::parse(resp);
        if (parsed && parsed->isObject()) {
            return viewToResult(*parsed, spec.jobId);
        }
        return ctrl::analysisComputeResult(spec.jobId, "failed", {}, "analysis_error",
                                           resp.empty() ? "sidecar rejected job" : resp);
    }

    const int timeoutS = spec.timeoutS > 0 ? spec.timeoutS : kDefaultTimeoutS;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutS);
    for (;;) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            int cst = 0;
            std::string cbody;
            std::string cerr;
            httpJson(addr, "POST", "/jobs/" + spec.jobId + "/cancel", clientIdJSON(spec.clientId), cst, cbody, &cerr,
                     spec.clientId);
            removeBundle(bundlePath);
            return ctrl::analysisComputeResult(spec.jobId, "cancelled", {}, "analysis_cancelled", "job cancelled");
        }
        if (std::chrono::steady_clock::now() > deadline) {
            int cst = 0;
            std::string cbody;
            std::string cerr;
            httpJson(addr, "POST", "/jobs/" + spec.jobId + "/cancel", clientIdJSON(spec.clientId), cst, cbody, &cerr,
                     spec.clientId);
            removeBundle(bundlePath);
            return ctrl::analysisComputeResult(spec.jobId, "failed", {}, "analysis_timeout", "job timed out");
        }
        int gst = 0;
        std::string gbody;
        std::string gerr;
        if (!httpJson(addr, "GET", "/jobs/" + spec.jobId, {}, gst, gbody, &gerr, spec.clientId)) {
            removeBundle(bundlePath);
            return offlineResult(spec.jobId, gerr.empty() ? "analysis compute sidecar is not running" : gerr);
        }
        const auto parsed = json::parse(gbody);
        if (!parsed || !parsed->isObject()) {
            removeBundle(bundlePath);
            return ctrl::analysisComputeResult(spec.jobId, "failed", {}, "analysis_error", "invalid sidecar json");
        }
        auto result = viewToResult(*parsed, spec.jobId);
        if (result.computeStatus == "succeeded" || result.computeStatus == "failed" ||
            result.computeStatus == "cancelled") {
            removeBundle(bundlePath);
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

ctrl::Message cancelJob(const Addr& addr, std::string_view jobId, std::string_view clientId) {
    int status = 0;
    std::string body;
    std::string err;
    if (clientId.empty()) {
        return ctrl::analysisComputeResult(std::string(jobId), "failed", {}, "unknown_job", "job not found");
    }
    if (!httpJson(addr, "POST", "/jobs/" + std::string(jobId) + "/cancel", clientIdJSON(clientId), status, body, &err,
                  clientId)) {
        return offlineResult(std::string(jobId), err.empty() ? "analysis compute sidecar offline" : err);
    }
    if (status == 404 || status == 400) {
        return ctrl::analysisComputeResult(std::string(jobId), "failed", {}, "unknown_job", "job not found");
    }
    const auto parsed = json::parse(body);
    if (parsed && parsed->isObject()) {
        return viewToResult(*parsed, std::string(jobId));
    }
    return ctrl::analysisComputeResult(std::string(jobId), "cancelled", {}, "cancelled", "client cancel");
}

bool cancelRemote(std::string_view addrText, std::string_view jobId, std::string_view clientId) {
    Addr addr;
    if (!parseAddr(addrText, addr) || clientId.empty()) {
        return false;
    }
    const auto msg = cancelJob(addr, jobId, clientId);
    return msg.computeStatus == "cancelled" || msg.code == "cancelled";
}

void removeBundle(const std::string& path) {
    if (path.empty()) {
        return;
    }
    const auto norm = std::filesystem::path(path).lexically_normal().generic_string();
    const auto low = [&] {
        std::string s = norm;
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    }();
    if (low.find("data/tsdb") != std::string::npos || low.find("data/video") != std::string::npos ||
        low.find("docker.sock") != std::string::npos) {
        return;
    }
    if (low.find("photon-analysis-compute") == std::string::npos) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

}  // namespace photon::analysis

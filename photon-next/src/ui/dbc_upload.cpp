#include "ui/client.hpp"
#include "ui/platform.hpp"
#include "base/json.hpp"
#include <chrono>
#include <algorithm>
#include <thread>
#include <fstream>
#include <iterator>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>
#else
#include "net/net.hpp"
#endif

namespace photon::ui {
bool Client::sendExtension(std::string text) {
    if (state_ != State::HelloOk && state_ != State::Connected) return false;
#ifdef __EMSCRIPTEN__
    return socket_ > 0 && emscripten_websocket_send_utf8_text(socket_, text.c_str()) >= 0;
#else
    if (!ws_ || !wsHandshakeOk_) return false;
    ws_->queueText(text);
    return ws_->flushOutbound();
#endif
}
void Client::uploadDbc(std::string path) {
    dbcUploadStatus_ = "Uploading DBC...";
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_EM_ASM({
        const input = document.createElement('input');
        input.type = 'file'; input.accept = '.dbc';
        const base = new URL(UTF8ToString($0)); base.protocol = base.protocol === 'wss:' ? 'https:' : 'http:';
        input.onchange = async () => {
            try {
                const file = input.files[0]; if (!file) return;
                if (file.size > 4 * 1024 * 1024) throw new Error('DBC exceeds 4 MiB');
                base.pathname = '/api/dbcs'; base.search = '?name=' + encodeURIComponent(file.name);
                const response = await fetch(base.href, { method: 'POST', credentials: 'same-origin', body: file });
                const reply = await response.json();
                if (!response.ok) throw new Error(reply.error || 'DBC rejected');
                window.photonDbcUploadResult = JSON.stringify({hash: reply.hash, label: reply.label});
            } catch (error) { window.photonDbcUploadResult = JSON.stringify({ error: String(error) }); }
        };
        input.click();
    }, url_.c_str());
    (void)path;
#else
    if (dbcUpload_.valid()) { dbcUploadStatus_ = "An upload is already running"; return; }
    const std::string endpoint = url_, auth = nativeWsBasicB64();
    dbcUpload_ = std::async(std::launch::async, [path, endpoint, auth]() {
        auto fail = [](std::string error) {
            json::Object reply; reply.emplace("error", std::move(error)); return json::stringify(json::Value(std::move(reply)));
        };
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail("Cannot open DBC file");
        in.seekg(0, std::ios::end);
        if (in.tellg() > 4 * 1024 * 1024) return fail("DBC exceeds 4 MiB");
        in.seekg(0);
        std::string data((std::istreambuf_iterator<char>(in)), {});
        if (data.size() > 4 * 1024 * 1024) return fail("DBC exceeds 4 MiB");
        if (!endpoint.starts_with("ws://")) return fail("Native upload requires a ws:// connection");
        auto hostPort = endpoint.substr(5); hostPort = hostPort.substr(0, hostPort.find('/'));
        auto colon = hostPort.rfind(':');
        std::string host = colon == std::string::npos ? hostPort : hostPort.substr(0, colon);
        auto port = static_cast<std::uint16_t>(colon == std::string::npos ? 80 : std::stoi(hostPort.substr(colon + 1)));
        auto sock = net::connectTcpTimed(host, port, 1000); if (!sock.valid()) return fail("Server unavailable");
        sock.setNonBlocking(true);
        std::string request = "POST /api/dbcs?name=uploaded.dbc HTTP/1.0\r\nHost: " + hostPort +
                              "\r\nAuthorization: Basic " + auth + "\r\nContent-Length: " + std::to_string(data.size()) +
                              "\r\nConnection: close\r\n\r\n" + data;
        std::string response; std::size_t sent = 0; char buffer[4096];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (sent < request.size()) {
                int n = sock.sendSome(request.data() + sent, request.size() - sent);
                if (n < 0) return fail("Upload connection closed"); sent += static_cast<std::size_t>(n);
            } else {
                int n = sock.recvSome(buffer, sizeof(buffer)); if (n < 0) break;
                response.append(buffer, static_cast<std::size_t>(n));
                if (response.size() > 1024 * 1024) return fail("Upload reply exceeds limit");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto split = response.find("\r\n\r\n");
        if (split == std::string::npos) return fail("No upload reply");
        return response.substr(split + 4);
    });
#endif
}
void Client::pollDbcUpload() {
    std::string reply;
#ifdef __EMSCRIPTEN__
    char buffer[4096] = {};
    MAIN_THREAD_EM_ASM({
        if (window.photonDbcUploadResult) {
            stringToUTF8(window.photonDbcUploadResult, $0, $1); window.photonDbcUploadResult = '';
        }
    }, buffer, sizeof(buffer));
    reply = buffer;
#else
    if (dbcUpload_.valid() && dbcUpload_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try { reply = dbcUpload_.get(); } catch (const std::exception& e) { dbcUploadStatus_ = e.what(); }
    }
#endif
    if (reply.empty()) return;
    auto parsed = json::parse(reply); if (!parsed) { dbcUploadStatus_ = "Invalid upload reply"; return; }
    auto hash = (*parsed)["hash"].asString();
    if (hash.empty()) { dbcUploadStatus_ = (*parsed)["error"].asString("Upload failed"); return; }
    uploadedDbc_ = hash; dbcUploadStatus_ = "Uploaded " + (*parsed)["label"].asString();
    if (std::find(arena_.dbcs.begin(), arena_.dbcs.end(), hash) == arena_.dbcs.end()) arena_.dbcs.push_back(hash);
    arena_.pushLog(dbcUploadStatus_);
}
void Client::applyUploadedDbc() {
    if (uploadedDbc_.empty()) return;
    json::Object obj; obj.emplace("type", "dbc_apply"); obj.emplace("dbc_hash", uploadedDbc_);
    sendExtension(json::stringify(json::Value(std::move(obj))));
}
void Client::redecodeWithUploadedDbc() {
    if (uploadedDbc_.empty() || arena_.tiles().sessionId().empty()) return;
    json::Object obj; obj.emplace("type", "recording_redecode"); obj.emplace("dbc_hash", uploadedDbc_);
    obj.emplace("session_id", arena_.tiles().sessionId());
    sendExtension(json::stringify(json::Value(std::move(obj))));
}
void Client::selectDecoding(std::string session, std::string decode) {
    arena_.tiles().cancelAll(*this);
    json::Object obj; obj.emplace("type", "recording_select"); obj.emplace("session_id", std::move(session));
    obj.emplace("decode_id", std::move(decode)); sendExtension(json::stringify(json::Value(std::move(obj))));
    requestCatalogList();
}
}

/*
 *    Copyright (c) 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "bridge-ipc-server.h"

#include <lib/support/logging/CHIPLogging.h>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace BridgeIpc {
namespace {

constexpr size_t kMaxRequest = 8192; // a credential record is well under this
constexpr int kRecvTimeoutSec = 5;   // matches the Lua client's timeout

std::atomic<bool> gRunning{ false };
int gListenFd = -1;
std::thread gThread;
Callbacks gCallbacks;

// ---- tiny JSON helpers (same minimal philosophy as cameras-config) -----------

// Extract the string value of "key" from a flat JSON request line. Returns true
// and sets `out` when the key is present (even if the value is ""), else false.
// Good enough for our controlled, single-level protocol (no nested key clashes).
bool ExtractStr(const std::string & json, const char * key, std::string & out)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t k           = json.find(needle);
    if (k == std::string::npos)
        return false;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos)
        return false;
    size_t open = json.find('"', colon + 1);
    if (open == std::string::npos)
        return false;
    size_t close = json.find('"', open + 1);
    if (close == std::string::npos)
        return false;
    out = json.substr(open + 1, close - open - 1);
    return true;
}

// Escape a string for embedding in a JSON response (echoed ids, error text).
std::string Escape(const std::string & s)
{
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n";  break;
        case '\r': r += "\\r";  break;
        case '\t': r += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                r += ' '; // drop other control chars
            else
                r += c;
        }
    }
    return r;
}

// `id` rendered as a JSON value: "the-id" when present, bare null when absent.
std::string IdValue(bool hasId, const std::string & id)
{
    return hasId ? ("\"" + Escape(id) + "\"") : std::string("null");
}

// ---- response builders -------------------------------------------------------

std::string FailResponse(bool hasId, const std::string & id, const std::string & status, const std::string & error)
{
    return "{\"v\":1,\"id\":" + IdValue(hasId, id) + ",\"ok\":false,\"status\":\"" + Escape(status) +
        "\",\"error\":\"" + Escape(error) + "\"}";
}

std::string PingResponse(const std::string & id, size_t cameras)
{
    return "{\"v\":1,\"id\":\"" + Escape(id) + "\",\"ok\":true,\"status\":\"ready\",\"result\":{\"cameras\":" +
        std::to_string(cameras) + "}}";
}

std::string UpsertResponse(const std::string & id, const OpResult & r)
{
    return "{\"v\":1,\"id\":\"" + Escape(id) + "\",\"ok\":true,\"status\":\"onboarded\",\"result\":{\"endpoint\":" +
        std::to_string(r.endpoint) + ",\"profiles\":" + std::to_string(r.profiles) + ",\"video_codec\":\"" +
        Escape(r.videoCodec) + "\",\"has_ptz\":" + (r.hasPtz ? "true" : "false") + ",\"has_audio_out\":" +
        (r.hasAudioOut ? "true" : "false") + "}}";
}

std::string RemoveResponse(const std::string & id)
{
    return "{\"v\":1,\"id\":\"" + Escape(id) + "\",\"ok\":true,\"status\":\"removed\"}";
}

// `discover` reports the scan counts: how many ONVIF responders answered (`found`),
// how many NEW cameras were onboarded (`added`), and the total now bridged
// (`cameras`, carried in OpResult::endpoint like set_default_creds does).
std::string DiscoverResponse(const std::string & id, const OpResult & r)
{
    return "{\"v\":1,\"id\":\"" + Escape(id) + "\",\"ok\":true,\"status\":\"ok\",\"result\":{\"found\":" +
        std::to_string(r.found) + ",\"added\":" + std::to_string(r.added) + ",\"cameras\":" +
        std::to_string(r.endpoint) + "}}";
}

// ---- request handling --------------------------------------------------------

// Read one '\n'-terminated line (byte-at-a-time; requests are tiny). Trailing
// CR/LF stripped. Returns false on EOF/timeout before any data.
bool ReadLine(int fd, std::string & line)
{
    line.clear();
    char c;
    while (line.size() < kMaxRequest)
    {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0)
            return !line.empty(); // peer closed / timed out
        if (c == '\n')
            return true;
        if (c != '\r')
            line.push_back(c);
    }
    return true; // hit the cap; treat what we have as the request
}

void SendAll(int fd, const std::string & data)
{
    size_t off = 0;
    while (off < data.size())
    {
        ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0)
            return;
        off += static_cast<size_t>(n);
    }
}

std::string Dispatch(const std::string & req)
{
    std::string id;
    bool hasId = ExtractStr(req, "id", id);

    std::string op;
    if (!ExtractStr(req, "op", op))
        return FailResponse(hasId, id, "bad_request", "missing or unparseable 'op'");

    if (op == "ping")
    {
        return PingResponse(id, gCallbacks.count ? gCallbacks.count() : 0);
    }

    if (op == "remove_camera")
    {
        std::string dni;
        if (!ExtractStr(req, "dni", dni) || dni.empty())
            return FailResponse(hasId, id, "bad_request", "remove_camera requires 'dni'");
        OpResult r = gCallbacks.remove ? gCallbacks.remove(dni) : OpResult{};
        if (r.ok)
            return RemoveResponse(id);
        return FailResponse(hasId, id, r.status.empty() ? "internal_error" : r.status, r.error);
    }

    if (op == "upsert_camera")
    {
        UpsertRequest u;
        ExtractStr(req, "dni", u.dni);
        ExtractStr(req, "name", u.name);
        ExtractStr(req, "ip", u.ip);
        ExtractStr(req, "control_url", u.controlUrl);
        ExtractStr(req, "userid", u.userid);
        ExtractStr(req, "password", u.password);
        ExtractStr(req, "stream", u.stream);

        if (u.dni.empty() || u.controlUrl.empty())
            return FailResponse(hasId, id, "bad_request", "upsert_camera requires 'dni' and 'control_url'");

        OpResult r = gCallbacks.upsert ? gCallbacks.upsert(u) : OpResult{};
        if (r.ok)
            return UpsertResponse(id, r);
        return FailResponse(hasId, id, r.status.empty() ? "internal_error" : r.status, r.error);
    }

    if (op == "set_default_creds")
    {
        // One ONVIF login applied to every camera (the "shared default credentials"
        // model). Empty userid+password means "all cameras are anonymous".
        std::string user, pass;
        ExtractStr(req, "userid", user);
        ExtractStr(req, "password", pass);
        OpResult r = gCallbacks.setDefaultCreds ? gCallbacks.setDefaultCreds(user, pass) : OpResult{};
        if (r.ok)
            return "{\"v\":1,\"id\":\"" + Escape(id) + "\",\"ok\":true,\"status\":\"" + Escape(r.status) +
                "\",\"result\":{\"cameras\":" + std::to_string(r.endpoint) + "}}";
        return FailResponse(hasId, id, r.status.empty() ? "internal_error" : r.status, r.error);
    }

    if (op == "discover")
    {
        // Runtime LAN rescan (pull-to-refresh): trigger native WS-Discovery, onboard
        // newly-found cameras and update moved ones — the same work done once at boot.
        // No params: it uses the stored default creds (with anonymous fallback). This
        // blocks for a few seconds on the UDP multicast wait + per-camera SOAP resolve.
        OpResult r = gCallbacks.discover ? gCallbacks.discover() : OpResult{};
        if (r.ok)
            return DiscoverResponse(id, r);
        return FailResponse(hasId, id, r.status.empty() ? "internal_error" : r.status, r.error);
    }

    return FailResponse(hasId, id, "bad_request", "unknown op '" + op + "'");
}

void HandleConnection(int fd)
{
    struct timeval tv;
    tv.tv_sec  = kRecvTimeoutSec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string req;
    if (!ReadLine(fd, req) || req.empty())
        return;

    std::string resp = Dispatch(req);
    resp += "\n";
    SendAll(fd, resp);
}

void AcceptLoop()
{
    ChipLogProgress(NotSpecified, "BridgeIpc: accept loop running");
    while (gRunning.load())
    {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int fd         = accept(gListenFd, reinterpret_cast<sockaddr *>(&peer), &plen);
        if (fd < 0)
        {
            if (!gRunning.load())
                break;
            if (errno == EINTR)
                continue;
            ChipLogError(NotSpecified, "BridgeIpc: accept failed: %s", strerror(errno));
            continue;
        }
        HandleConnection(fd);
        close(fd);
    }
    ChipLogProgress(NotSpecified, "BridgeIpc: accept loop exited");
}

} // namespace

bool Start(uint16_t port, Callbacks callbacks)
{
    if (gRunning.load())
    {
        ChipLogError(NotSpecified, "BridgeIpc: already running");
        return false;
    }

    gCallbacks = std::move(callbacks);

    gListenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (gListenFd < 0)
    {
        ChipLogError(NotSpecified, "BridgeIpc: socket() failed: %s", strerror(errno));
        return false;
    }

    int one = 1;
    setsockopt(gListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0 — see header note on nsjail/LAN IP
    addr.sin_port        = htons(port);

    if (bind(gListenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ChipLogError(NotSpecified, "BridgeIpc: bind(:%u) failed: %s", port, strerror(errno));
        close(gListenFd);
        gListenFd = -1;
        return false;
    }

    if (listen(gListenFd, 8) < 0)
    {
        ChipLogError(NotSpecified, "BridgeIpc: listen() failed: %s", strerror(errno));
        close(gListenFd);
        gListenFd = -1;
        return false;
    }

    gRunning.store(true);
    gThread = std::thread(AcceptLoop);
    ChipLogProgress(NotSpecified, "BridgeIpc: listening on 0.0.0.0:%u", port);
    return true;
}

void Stop()
{
    if (!gRunning.exchange(false))
        return;

    if (gListenFd >= 0)
    {
        shutdown(gListenFd, SHUT_RDWR); // unblock accept()
        close(gListenFd);
        gListenFd = -1;
    }
    if (gThread.joinable())
        gThread.join();
    ChipLogProgress(NotSpecified, "BridgeIpc: stopped");
}

} // namespace BridgeIpc

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

// bridge-ipc-server — the daemon-side of the Edge⇄bridge IPC contract
// (ipc/PROTOCOL.md): newline-delimited JSON over TCP, one request → one response
// → close. The Edge driver (bridge_ipc.lua) is the client.
//
// This module is pure transport + parse + dispatch. It owns no Matter or ONVIF
// state: it parses a request, calls one of the supplied callbacks, and formats
// the JSON response. The callbacks (in camera-bridge-main.cpp) do the ONVIF
// resolve and the thread-safe endpoint add/remove on the Matter event loop.
//
// Bind address is 0.0.0.0 (not 127.0.0.1): Edge drivers run in an nsjail netns
// with no host-loopback access, so they reach the bridge via the hub's LAN IP.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace BridgeIpc {

// Raw fields of an `upsert_camera` request — ONVIF is NOT yet resolved here.
struct UpsertRequest
{
    std::string dni;        // stable join key (required)
    std::string name;       // display label
    std::string ip;         // diagnostics only
    std::string controlUrl; // ONVIF device-service URL (required)
    std::string userid;     // credentials ("" = anonymous)
    std::string password;
    std::string stream;     // "mainstream" | "substream"
};

// Outcome of an op; drives the JSON response. `status` uses the PROTOCOL.md
// vocabulary: onboarded | removed | auth_failed | unreachable | no_streams |
// bad_request | internal_error.
struct OpResult
{
    bool ok = false;
    std::string status;
    std::string error; // optional human-readable detail (failures)
    // upsert success payload:
    int endpoint = -1;
    int profiles = 0;
    std::string videoCodec;
    bool hasPtz       = false;
    bool hasAudioOut  = false;
    // discover success payload (`endpoint` doubles as the total bridged-camera
    // count here, mirroring how set_default_creds reports `result.cameras`):
    int found = 0; // ONVIF responders returned by this WS-Discovery scan
    int added = 0; // NEW cameras onboarded (bridged endpoints created) this scan
};

struct Callbacks
{
    std::function<OpResult(const UpsertRequest &)> upsert;
    std::function<OpResult(const std::string & dni)> remove;
    std::function<size_t()> count; // current camera count, for `ping`
    // [single_bridge] set one default ONVIF login applied to every camera.
    std::function<OpResult(const std::string & userid, const std::string & password)> setDefaultCreds;
    // [single_bridge] runtime LAN rescan: native WS-Discovery, onboard new cameras,
    // update moved ones. No params — uses the stored default creds (anonymous fallback).
    std::function<OpResult()> discover;
};

// Start the accept loop on 0.0.0.0:<port> in a background thread.
// Returns false if the listening socket cannot be created or bound.
bool Start(uint16_t port, Callbacks callbacks);

// Stop the server thread and close the listening socket. Safe if never started.
void Stop();

} // namespace BridgeIpc

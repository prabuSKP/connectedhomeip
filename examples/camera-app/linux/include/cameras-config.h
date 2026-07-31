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

#pragma once

#include "camera-device.h"

#include <string>
#include <vector>

namespace CameraConfig {

// One entry from cameras.json.
//
// `onvif` holds the *resolved* stream facts the GStreamer pipeline needs (RTSP
// URL, PTZ URL, token) plus credentials. `dni`/`controlUrl`/`stream` are the
// *source* fields the Edge driver sends over IPC: they let the bridge re-resolve
// the camera and keep a stable per-camera identity (dni) across restarts.
struct CameraEntry
{
    std::string name;       // Display name (BridgedDeviceBasicInformation.NodeLabel)
    std::string dni;        // Stable join key from WS-Discovery; also the Bridged uniqueId
    std::string controlUrl; // ONVIF device-service URL (source; kept for re-resolve).
                            // Empty for direct-RTSP cameras (mode=="direct").
    std::string stream;     // "mainstream" | "substream" (default mainstream)
    std::string mode;       // "onvif" (default/absent) | "direct". Direct = Hikvision-style
                            // camera with ONVIF disabled: rtsp URL formed from the IP, no
                            // control_url, no re-resolve (see HIKVISION_DISCOVERY_PLAN.md).
    Camera::OnvifConfig onvif; // resolved rtsp/ptz/token + user/pass + useTestSrc
};

// Default on-device path for the multi-camera config file.
// The file must survive OTA (lives on /data, not /usr).
constexpr const char * kDefaultPath = "/data/onvif-bridge/cameras.json";

// Load the camera list from a JSON file.
//
// Format (minimal subset of JSON; no nested objects, no escaped characters in values):
//   [
//     { "name": "Front Door", "dni": "onvif-urn:uuid:...",
//       "rtsp": "rtsp://192.168.68.104/live/ch00_0",
//       "ptz":  "http://192.168.68.104:8899/onvif/ptz_service",
//       "token":"PROFILE_000", "user": "", "pass": "",
//       "control_url": "http://192.168.68.104/onvif/device_service",
//       "stream": "mainstream" },
//     { ... }
//   ]
//
// `dni`/`control_url`/`stream` are optional (older files omit them; they default
// to empty / "mainstream"). Returns an empty vector if the file does not exist or
// cannot be parsed. The caller should fall back to single-camera CLI options then.
std::vector<CameraEntry> LoadFromFile(const char * path = kDefaultPath);

// Persist the camera list back to `path` (atomic: write a .tmp then rename).
// Written in the same minimal JSON format LoadFromFile reads, so it round-trips.
// Returns true on success. Values must not contain '"' (the minimal parser, and
// this writer, do not escape) — fine for URLs/tokens/usernames.
bool SaveToFile(const std::vector<CameraEntry> & cameras, const char * path = kDefaultPath);

// Rewrite ONE camera's video_codec in place: load `path`, update the entry whose dni matches,
// save. Returns true if a matching entry was found and the file was written.
//
// Exists for the self-healing codec correction (see camera-device.h), which runs on a
// CameraDevice's watchdog thread and therefore must NOT touch the bridge's gBridgedCameras
// vector — that is single-mutator (IPC thread) by design. This does file I/O only, and is
// serialised against SaveToFile by an internal mutex so a concurrent full save cannot
// interleave with this read-modify-write. It takes no other lock and is never held across a
// thread join, so it cannot deadlock the camera-removal path.
bool PatchVideoCodec(const std::string & dni, const std::string & videoCodec, const char * path = kDefaultPath);

} // namespace CameraConfig

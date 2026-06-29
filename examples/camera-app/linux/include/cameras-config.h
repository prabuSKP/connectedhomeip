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
struct CameraEntry
{
    std::string name;   // Display name shown in SmartThings (BridgedDeviceBasicInformation.NodeLabel)
    Camera::OnvifConfig onvif; // RTSP URL, PTZ URL, token, user, pass
};

// Default on-device path for the multi-camera config file.
// The file must survive OTA (lives on /data, not /usr).
constexpr const char * kDefaultPath = "/data/onvif-bridge/cameras.json";

// Load the camera list from a JSON file.
//
// Format (minimal subset of JSON; no nested objects, no escaped characters in values):
//   [
//     { "name": "Front Door",
//       "rtsp": "rtsp://192.168.68.104/live/ch00_0",
//       "ptz":  "http://192.168.68.104:8899/onvif/ptz_service",
//       "token":"PROFILE_000", "user": "", "pass": "" },
//     { ... }
//   ]
//
// Returns an empty vector if the file does not exist or cannot be parsed.
// The caller should fall back to single-camera CLI options in that case.
std::vector<CameraEntry> LoadFromFile(const char * path = kDefaultPath);

} // namespace CameraConfig

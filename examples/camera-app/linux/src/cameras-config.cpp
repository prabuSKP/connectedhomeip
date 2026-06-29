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

#include "cameras-config.h"

#include <fstream>
#include <lib/support/logging/CHIPLogging.h>
#include <sstream>
#include <string>
#include <vector>

namespace CameraConfig {

namespace {

// Minimal JSON field extractor for the fixed cameras.json format.
// Finds the value of a key in a JSON object fragment, e.g.
//   ExtractField(obj, "name") on { "name": "Front Door", ... } → "Front Door"
// Returns empty string if the key is absent.
std::string ExtractField(const std::string & obj, const std::string & key)
{
    // Search for "key": "value" pattern
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos         = obj.find(searchKey);
    if (keyPos == std::string::npos)
        return {};

    size_t colonPos = obj.find(':', keyPos + searchKey.size());
    if (colonPos == std::string::npos)
        return {};

    size_t openQuote = obj.find('"', colonPos + 1);
    if (openQuote == std::string::npos)
        return {};

    size_t closeQuote = obj.find('"', openQuote + 1);
    if (closeQuote == std::string::npos)
        return {};

    return obj.substr(openQuote + 1, closeQuote - openQuote - 1);
}

// Split the top-level JSON array into individual object strings (one per camera).
// Handles the simple format: [ { ... }, { ... } ]  where objects are not nested.
std::vector<std::string> SplitObjects(const std::string & json)
{
    std::vector<std::string> objects;
    size_t pos = 0;
    while (pos < json.size())
    {
        size_t open = json.find('{', pos);
        if (open == std::string::npos)
            break;

        // Find matching closing brace (no nesting in our format)
        size_t close = json.find('}', open + 1);
        if (close == std::string::npos)
            break;

        objects.push_back(json.substr(open, close - open + 1));
        pos = close + 1;
    }
    return objects;
}

} // namespace

std::vector<CameraEntry> LoadFromFile(const char * path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        ChipLogDetail(Camera, "cameras-config: %s not found, using CLI options", path);
        return {};
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string json = ss.str();

    std::vector<CameraEntry> cameras;
    for (const auto & obj : SplitObjects(json))
    {
        CameraEntry entry;
        entry.name         = ExtractField(obj, "name");
        entry.onvif.rtspUrl = ExtractField(obj, "rtsp");
        entry.onvif.ptzUrl  = ExtractField(obj, "ptz");
        entry.onvif.token   = ExtractField(obj, "token");
        entry.onvif.user    = ExtractField(obj, "user");
        entry.onvif.pass    = ExtractField(obj, "pass");

        // "rtsp": "test" selects a hardware-free GStreamer test pattern for this
        // camera (used to validate multi-camera behaviour without a 2nd ONVIF cam).
        if (entry.onvif.rtspUrl == "test")
        {
            entry.onvif.useTestSrc = true;
            entry.onvif.rtspUrl.clear();
        }

        if (entry.onvif.rtspUrl.empty() && !entry.onvif.useTestSrc)
        {
            ChipLogError(Camera, "cameras-config: skipping entry with no 'rtsp' field");
            continue;
        }
        if (entry.name.empty())
        {
            entry.name = "Camera " + std::to_string(cameras.size() + 1);
        }

        ChipLogProgress(Camera, "cameras-config: loaded '%s' source=%s", entry.name.c_str(),
                        entry.onvif.useTestSrc ? "test-pattern" : entry.onvif.rtspUrl.c_str());
        cameras.push_back(std::move(entry));
    }

    ChipLogProgress(Camera, "cameras-config: %zu camera(s) loaded from %s", cameras.size(), path);
    return cameras;
}

} // namespace CameraConfig

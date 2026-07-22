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

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <lib/support/logging/CHIPLogging.h>
#include <sstream>
#include <string>
#include <vector>

namespace CameraConfig {

namespace {

// Escape a string for embedding as a JSON string value. ONVIF-supplied fields (camera name,
// URLs) are untrusted and may contain '"', '\\', '}', or control chars — without escaping they
// would corrupt the file and silently drop cameras on the next load.
std::string JsonEscape(const std::string & in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char ch : in)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            }
            else
            {
                out += static_cast<char>(ch);
            }
        }
    }
    return out;
}

// Reverse of JsonEscape for the subset we emit (\" \\ \/ \b \f \n \r \t \uXXXX).
std::string JsonUnescape(const std::string & in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] != '\\' || i + 1 >= in.size())
        {
            out += in[i];
            continue;
        }
        char esc = in[++i];
        switch (esc)
        {
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case '/':
            out += '/';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'u': {
            if (i + 4 < in.size())
            {
                unsigned int code = 0;
                if (sscanf(in.c_str() + i + 1, "%4x", &code) == 1)
                {
                    // We only ever emit control chars (< 0x80) via \u, so a single byte suffices.
                    if (code < 0x80)
                    {
                        out += static_cast<char>(code);
                    }
                    i += 4;
                }
            }
            break;
        }
        default:
            out += esc;
            break;
        }
    }
    return out;
}

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

    // Find the closing quote, skipping backslash-escaped quotes (\").
    size_t closeQuote = openQuote + 1;
    while (closeQuote < obj.size())
    {
        if (obj[closeQuote] == '\\')
        {
            closeQuote += 2; // skip the escaped char
            continue;
        }
        if (obj[closeQuote] == '"')
            break;
        ++closeQuote;
    }
    if (closeQuote >= obj.size())
        return {};

    return JsonUnescape(obj.substr(openQuote + 1, closeQuote - openQuote - 1));
}

// Split the top-level JSON array into individual object strings (one per camera).
// Objects are not nested in our format, but string VALUES may contain '{' or '}' (untrusted
// ONVIF names/URLs), so brace matching must ignore braces that appear inside a quoted string
// (respecting backslash escapes) — otherwise a '}' in a value truncates the object and drops
// the camera on reload.
std::vector<std::string> SplitObjects(const std::string & json)
{
    std::vector<std::string> objects;
    size_t pos = 0;
    while (pos < json.size())
    {
        size_t open = json.find('{', pos);
        if (open == std::string::npos)
            break;

        size_t i        = open + 1;
        bool inString   = false;
        size_t close    = std::string::npos;
        for (; i < json.size(); ++i)
        {
            char ch = json[i];
            if (inString)
            {
                if (ch == '\\')
                {
                    ++i; // skip escaped char
                    continue;
                }
                if (ch == '"')
                    inString = false;
                continue;
            }
            if (ch == '"')
            {
                inString = true;
            }
            else if (ch == '}')
            {
                close = i;
                break;
            }
        }
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
        entry.dni          = ExtractField(obj, "dni");
        entry.controlUrl   = ExtractField(obj, "control_url");
        entry.stream       = ExtractField(obj, "stream");
        entry.mode         = ExtractField(obj, "mode");
        if (entry.mode.empty())
            entry.mode = "onvif"; // absent in older files → ONVIF-resolved camera
        entry.onvif.rtspUrl     = ExtractField(obj, "rtsp");
        entry.onvif.ptzUrl      = ExtractField(obj, "ptz");
        entry.onvif.snapshotUrl = ExtractField(obj, "snapshot");
        entry.onvif.token       = ExtractField(obj, "token");
        entry.onvif.user    = ExtractField(obj, "user");
        entry.onvif.pass    = ExtractField(obj, "pass");
        entry.onvif.needsBasicAuth = (ExtractField(obj, "basic_auth") == "true"); // absent -> false (default: Digest)

        std::string verifiedStr = ExtractField(obj, "audio_verified");
        if (verifiedStr == "true")
        {
            entry.onvif.audioCapability.verifiedByRtspSdp = true;
            std::string aCodec = ExtractField(obj, "audio_codec");
            if (aCodec == "PCMU")
                entry.onvif.audioCapability.codec = Camera::InboundAudioCodec::kPcmu;
            else if (aCodec == "PCMA")
                entry.onvif.audioCapability.codec = Camera::InboundAudioCodec::kPcma;
            else if (aCodec == "OPUS")
                entry.onvif.audioCapability.codec = Camera::InboundAudioCodec::kOpus;
            else
                entry.onvif.audioCapability.codec = Camera::InboundAudioCodec::kUnsupported;
            
            std::string rateStr = ExtractField(obj, "audio_rate");
            entry.onvif.audioCapability.clockRateHz = rateStr.empty() ? 0 : std::atoi(rateStr.c_str());
            std::string chanStr = ExtractField(obj, "audio_channels");
            entry.onvif.audioCapability.channels = chanStr.empty() ? 0 : static_cast<uint8_t>(std::atoi(chanStr.c_str()));
        }

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

bool SaveToFile(const std::vector<CameraEntry> & cameras, const char * path)
{
    // Write to a sibling .tmp then rename, so a crash mid-write can't truncate
    // the live config (rename is atomic on the same filesystem).
    std::string tmpPath = std::string(path) + ".tmp";

    std::ofstream file(tmpPath, std::ios::trunc);
    if (!file.is_open())
    {
        ChipLogError(Camera, "cameras-config: cannot open %s for write", tmpPath.c_str());
        return false;
    }

    file << "[\n";
    for (size_t i = 0; i < cameras.size(); ++i)
    {
        const auto & c        = cameras[i];
        std::string src       = c.onvif.useTestSrc ? "test" : c.onvif.rtspUrl;
        std::string streamVal = c.stream.empty() ? "mainstream" : c.stream;
        std::string modeVal   = c.mode.empty() ? "onvif" : c.mode;
        file << "  { \"name\": \"" << JsonEscape(c.name) << "\""
             << ", \"dni\": \"" << JsonEscape(c.dni) << "\""
             << ", \"rtsp\": \"" << JsonEscape(src) << "\""
             << ", \"ptz\": \"" << JsonEscape(c.onvif.ptzUrl) << "\""
             << ", \"snapshot\": \"" << JsonEscape(c.onvif.snapshotUrl) << "\""
             << ", \"token\": \"" << JsonEscape(c.onvif.token) << "\""
             << ", \"user\": \"" << JsonEscape(c.onvif.user) << "\""
             << ", \"pass\": \"" << JsonEscape(c.onvif.pass) << "\""
             << ", \"control_url\": \"" << JsonEscape(c.controlUrl) << "\""
             << ", \"stream\": \"" << JsonEscape(streamVal) << "\""
             << ", \"mode\": \"" << JsonEscape(modeVal) << "\"";

        if (c.onvif.needsBasicAuth)
            file << ", \"basic_auth\": \"true\"";

        if (c.onvif.audioCapability.verifiedByRtspSdp)
        {
            std::string aCodec = "Unsupported";
            if (c.onvif.audioCapability.codec == Camera::InboundAudioCodec::kPcmu)
                aCodec = "PCMU";
            else if (c.onvif.audioCapability.codec == Camera::InboundAudioCodec::kPcma)
                aCodec = "PCMA";
            else if (c.onvif.audioCapability.codec == Camera::InboundAudioCodec::kOpus)
                aCodec = "OPUS";

            file << ", \"audio_verified\": \"true\""
                 << ", \"audio_codec\": \"" << aCodec << "\""
                 << ", \"audio_rate\": \"" << c.onvif.audioCapability.clockRateHz << "\""
                 << ", \"audio_channels\": \"" << (int)c.onvif.audioCapability.channels << "\"";
        }
        file << " }" << (i + 1 < cameras.size() ? "," : "") << "\n";
    }
    file << "]\n";
    file.close();

    if (file.fail())
    {
        ChipLogError(Camera, "cameras-config: write to %s failed", tmpPath.c_str());
        std::remove(tmpPath.c_str());
        return false;
    }

    if (std::rename(tmpPath.c_str(), path) != 0)
    {
        ChipLogError(Camera, "cameras-config: rename %s -> %s failed", tmpPath.c_str(), path);
        std::remove(tmpPath.c_str());
        return false;
    }

    ChipLogProgress(Camera, "cameras-config: saved %zu camera(s) to %s", cameras.size(), path);
    return true;
}

} // namespace CameraConfig

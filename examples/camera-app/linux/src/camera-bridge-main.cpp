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

// Multi-camera Matter bridge main.
//
// Topology:
//   Endpoint 0  – Root node (static, from ZAP)
//   Endpoint 1  – Disabled at runtime (ZAP template / de-facto parent for bridged cameras)
//   Endpoint 2+ – Bridged cameras (dynamic), one per entry in cameras.json
//
// Each bridged camera endpoint carries:
//   • BridgedDeviceBasicInformation (0x0039)  – name, uniqueId, reachable
//   • CameraAVStreamManagement (0x0551)        ┐
//   • WebRTCTransportProvider  (0x0553)        │ registered via server-cluster
//   • CameraAVSettingsUserLevelMgmt (0x0552)   │ registry (not ember storage)
//   • ZoneManagement (0x0550)                  │
//   • Chime (0x0556)                           ┘
//   • PushAVStreamTransport (0x0555)
//
// Cameras are loaded from /data/onvif-bridge/cameras.json.
// Falls back to a single camera from CLI --camera-onvif-url etc. if the file
// is absent (single-camera bring-up / backward-compat).

#include "CameraAppCommandDelegate.h"
#include "bridge-ipc-server.h"
#include "camera-app.h"
#include "camera-device.h"
#include "cameras-config.h"
#include "tls-certificate-management-instance.h"
#include "tls-client-management-instance.h"

// Bridge-app Device base (provides BridgedDeviceBasicInformation cluster)
#include "Device.h"

// ONVIF resolve (control URL + creds -> RTSP/PTZ/token); from the daemon's libonvif.a.
#include "onvif/resolve_bridge.h"
// ONVIF WS-Discovery (find cameras on the LAN), so the bridge populates cameras.json itself.
#include "onvif/wsdiscovery.h"
// [hikvision] mDNS discovery + direct-RTSP probe: onboard cameras whose ONVIF is disabled
// (Hikvision default). See docs/HIKVISION_DISCOVERY_PLAN.md.
#include "onvif/mdnsdisc.h"
#include "onvif/rtspprobe.h"

#include <AppMain.h>
#include <Options.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/util/attribute-storage.h>
#include <app/util/endpoint-config-api.h>
#include <data-model-providers/codegen/CodegenDataModelProvider.h>
#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>

#include <atomic>
#include <cctype>
#include <chrono> // discover debounce: steady_clock timestamp of the last completed scan
#include <fstream>
#include <memory>
#include <sstream> // MacFromArp: parse /proc/net/arp
#include <string>
#include <unistd.h> // _exit
#include <vector>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::DeviceLayer;
using namespace Camera;

// ---------------------------------------------------------------------------
// Dynamic endpoint infrastructure
// ---------------------------------------------------------------------------
namespace {

void PopulateAudioCapability(const std::string & codec, int rate, int channels, int has_audio_in, Camera::OnvifConfig & onvif)
{
    onvif.audioCapability.verifiedByRtspSdp = has_audio_in != 0;
    if (codec == "PCMU")
        onvif.audioCapability.codec = Camera::InboundAudioCodec::kPcmu;
    else if (codec == "PCMA")
        onvif.audioCapability.codec = Camera::InboundAudioCodec::kPcma;
    else if (codec == "OPUS")
        onvif.audioCapability.codec = Camera::InboundAudioCodec::kOpus;
    else
        onvif.audioCapability.codec = Camera::InboundAudioCodec::kUnsupported;
    onvif.audioCapability.clockRateHz = rate;
    onvif.audioCapability.channels = static_cast<uint8_t>(channels);
}

constexpr int kDescriptorAttrSize = 254;

// Minimal ember endpoint definition for a bridged camera.
// Camera clusters are NOT stored in ember — they are registered via the
// server-cluster registry inside CameraApp::InitCameraDeviceClusters().
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(sCameraDescriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::DeviceTypeList::Id, ARRAY, kDescriptorAttrSize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ServerList::Id, ARRAY, kDescriptorAttrSize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ClientList::Id, ARRAY, kDescriptorAttrSize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::PartsList::Id, ARRAY, kDescriptorAttrSize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(sBridgedCameraClusters)
DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, sCameraDescriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(sBridgedCameraEndpoint, sBridgedCameraClusters);

// Canonical Matter bridge: each camera is a Bridged Node (0x0013) + Camera (0x0142)
// living under an Aggregator (0x000E) endpoint.  SmartThings, on commissioning a
// node advertised as an Aggregator, enumerates the bridged children and creates a
// card per camera.  (A single Matter node renders as only ONE camera otherwise.)
constexpr EmberAfDeviceType kBridgedCameraDeviceTypes[] = {
    { 0x0142 /* ma_camera */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

// Aggregator endpoint: Descriptor only; device type 0x000E.  Its PartsList groups
// all bridged camera endpoints so the controller treats the node as a bridge.
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(sAggregatorClusters)
DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, sCameraDescriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;
DECLARE_DYNAMIC_ENDPOINT(sAggregatorEndpoint, sAggregatorClusters);
constexpr EmberAfDeviceType kAggregatorDeviceTypes[] = { { 0x000E /* ma_aggregator */, 1 } };
DataVersion gAggregatorDataVersions[MATTER_ARRAY_SIZE(sAggregatorClusters)];
EndpointId gAggregatorEndpointId = kInvalidEndpointId;

EndpointId gCurrentEndpointId;
EndpointId gFirstDynamicEndpointId;

// Slot array mirrors bridge-app pattern; index → slot in the dynamic endpoint table.
Device * gDevices[CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT];

// ---------------------------------------------------------------------------
// BridgedCamera: one ONVIF/WebRTC backend bound to one dynamic endpoint.
// ---------------------------------------------------------------------------
class BridgedCamera : public Device
{
public:
    BridgedCamera(const char * name, const OnvifConfig & cfg) : Device(name, "")
    {
        mCameraDevice.SetOnvifConfig(cfg);
    }

    // Called after the endpoint ID is assigned to wire up all camera clusters.
    void InitClusters(EndpointId endpointId)
    {
        mCameraDevice.Init();
        mCameraApp = std::make_unique<CameraApp>(endpointId, &mCameraDevice);
        mCameraApp->InitCameraDeviceClusters();
    }

    void Shutdown()
    {
        // Close WebRTC connections before the Matter stack tears down.
        mCameraDevice.Shutdown();
        if (mCameraApp)
        {
            mCameraApp->ShutdownCameraDeviceClusters();
            mCameraApp.reset();
        }
    }

    // Purge this camera's persisted PushAV transports (CurrentConnections + the upload cert
    // reference they carry) so nothing is restored at next boot onto whichever camera then
    // occupies this endpoint id. RUNTIME REMOVE ONLY — ApplicationShutdown must never call
    // this: persisted transports are deliberately restored across reboots. Call before
    // Shutdown() (it needs the live cluster) and under StackLock (KVS access is serialised
    // with the Matter thread).
    void DeletePersistedPushAvTransports()
    {
        if (mCameraApp)
        {
            mCameraApp->DeletePersistedPushAvTransports();
        }
    }

    // The full source+resolved record, kept so we can persist cameras.json and
    // look the camera up by its stable DNI for remove/replace.
    void SetEntry(const CameraConfig::CameraEntry & e) { mEntry = e; }
    const CameraConfig::CameraEntry & GetEntry() const { return mEntry; }
    const std::string & GetDni() const { return mEntry.dni; }

    // DataVersion backing store for the ember cluster list above (Descriptor only = 1 entry).
    DataVersion mDataVersions[MATTER_ARRAY_SIZE(sBridgedCameraClusters)] = {};

private:
    void HandleDeviceChange(Device * /*dev*/, Device::Changed_t /*mask*/) override {}

    CameraDevice mCameraDevice;
    std::unique_ptr<CameraApp> mCameraApp;
    CameraConfig::CameraEntry mEntry;
};

// Owns all bridged cameras for the lifetime of the process. Mutated only on the
// single IPC accept thread at runtime (and in ApplicationInit/Shutdown, when no
// IPC thread is running), so it needs no lock of its own; the ember/registry
// mutations inside are serialised against the Matter event loop via StackLock.
std::vector<std::unique_ptr<BridgedCamera>> gBridgedCameras;

// Camera count published to the `ping` health response (read from the IPC thread).
std::atomic<size_t> gCameraCount{ 0 };

// [single_bridge] One default ONVIF login applied to EVERY camera, set from the Matter
// Bridge card. Persisted to /data so it survives restart and is used by WS-Discovery.
std::string gDefaultUser;
std::string gDefaultPass;
constexpr const char * kDefaultCredsPath = "/data/onvif-bridge/default_creds";

void LoadDefaultCreds()
{
    std::ifstream f(kDefaultCredsPath);
    if (!f.is_open())
        return;
    std::getline(f, gDefaultUser);
    std::getline(f, gDefaultPass);
    ChipLogProgress(Camera, "CameraBridge: loaded default ONVIF creds (user='%s')", gDefaultUser.c_str());
}

void SaveDefaultCreds()
{
    std::ofstream f(kDefaultCredsPath, std::ios::trunc);
    if (f.is_open())
        f << gDefaultUser << "\n" << gDefaultPass << "\n";
}

// Derive a STABLE camera identity that survives DHCP IP changes and, for cameras that
// regenerate their ONVIF UUID on every reboot, URN changes too. RFC-4122 UUIDs carry the
// NIC MAC in their last 12 hex "node" digits, and cheap ONVIF cameras keep that MAC stable
// even when the rest of the UUID (and the IP) churns. Keying identity on the MAC means one
// physical camera stays ONE bridged device (stable DNI + Matter UniqueID) instead of
// spawning a duplicate on every reconnect.
//   "urn:uuid:49fc6875-2d80-811c-e367-98eb03ed535f" -> "onvif-mac-98eb03ed535f"
// Ids we can't parse (already-normalized, or non-UUID DNIs) are returned unchanged.
std::string StableCameraId(const std::string & id)
{
    if (id.rfind("onvif-mac-", 0) == 0)
        return id; // already stable
    const std::string pfx = "urn:uuid:";
    if (id.size() < pfx.size() || id.compare(0, pfx.size(), pfx) != 0)
        return id;
    std::string hex;
    for (size_t i = pfx.size(); i < id.size(); ++i)
    {
        char c = id[i];
        if (c == '-')
            continue;
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return id; // not a clean UUID — leave as-is
        hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (hex.size() != 32)
        return id;
    return "onvif-mac-" + hex.substr(20); // last 12 hex = MAC / UUID node
}

// [hikvision] Derive the MAC-based stable id for an mDNS-discovered camera from the kernel
// ARP table. mDNS carries no ONVIF URN, but any camera we just probed over TCP is in
// /proc/net/arp, and its MAC yields the SAME "onvif-mac-<12hex>" that StableCameraId() derives
// from a WS-Discovery UUID — so a camera seen by both paths dedups to one bridged device.
// Returns "" if the IP has no complete ARP entry (caller falls back to the mDNS serial).
std::string MacFromArp(const std::string & ip)
{
    std::ifstream f("/proc/net/arp");
    if (!f.is_open())
        return {};
    std::string line;
    std::getline(f, line); // skip header row
    while (std::getline(f, line))
    {
        std::istringstream ss(line);
        std::string colIp, hwType, flags, hwAddr;
        if (!(ss >> colIp >> hwType >> flags >> hwAddr))
            continue;
        if (colIp != ip)
            continue;
        if (flags != "0x2") // 0x2 = ATF_COM (resolved/complete); skip incomplete entries
            return {};
        std::string hex;
        for (char c : hwAddr)
            if (c != ':')
                hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (hex.size() != 12 || hex == "000000000000")
            return {};
        return "onvif-mac-" + hex;
    }
    return {};
}

// Extract the host (IP or name) from an http(s)/rtsp URL: "http://1.2.3.4/onvif/..." -> "1.2.3.4".
std::string HostFromUrl(const std::string & url)
{
    size_t start = url.find("://");
    start        = (start == std::string::npos) ? 0 : start + 3;
    size_t at    = url.find('@', start);
    if (at != std::string::npos)
        start = at + 1;
    size_t end = start;
    while (end < url.size() && url[end] != ':' && url[end] != '/')
        ++end;
    return url.substr(start, end - start);
}

// [hikvision] Onboard a camera by a directly-formed Hikvision RTSP URL (used when ONVIF is
// disabled). Probes the modern scheme's main stream (H.264 required); an H.265 main stream
// falls back to the substream (commonly H.264); a 404 on the modern scheme falls back to the
// legacy pre-V5 path scheme (/h264/ch1/...). Fills `entry` (mode="direct", no
// ptz/snapshot/token) and `codecOut` on success. LOCKOUT-CRITICAL: at most ONE FAILED login
// per camera per boot — a rejected password (AUTH) aborts the whole chain immediately;
// substream/legacy probes only run after a success or a wrong-PATH 404, neither of which
// costs illegal-login-lock strikes.
// Returns RTSP_PROBE_OK on success, else the probe's failure code
// (AUTH / UNREACHABLE / NO_H264 / BAD_PATH).
int TryDirectRtsp(const std::string & ip, const std::string & name, const std::string & user,
                  const std::string & pass, const std::string & dni,
                  CameraConfig::CameraEntry & entry, std::string & codecOut)
{
    std::string chosenUrl;
    std::string streamKind;
    std::string authNote; // auth_note of the last main-stream probe — the one whose rc we return
                          // (non-empty only when that probe returned RTSP_PROBE_AUTH)

    // Probe a main/sub URL pair: main first; if main is H.265 (our pipeline is
    // H.264-only passthrough) try the substream, which is commonly H.264.
    rtsp_probe_result_t chosenRes;
    memset(&chosenRes, 0, sizeof(chosenRes));

    auto probePair = [&](const std::string & mainUrl, const std::string & subUrl) -> int {
        rtsp_probe_result_t res;
        int rc     = onvif_rtsp_probe(mainUrl.c_str(), user.c_str(), pass.c_str(), /*timeout=*/5, &res);
        chosenUrl  = mainUrl;
        streamKind = "mainstream";
        codecOut   = res.codec;
        authNote   = res.auth_note;
        chosenRes  = res;
        if (rc == RTSP_PROBE_NO_H264 && std::string(res.codec) == "H265")
        {
            ChipLogProgress(Camera, "CameraBridge: %s main stream is H.265 — trying substream", ip.c_str());
            rtsp_probe_result_t subRes;
            int subRc = onvif_rtsp_probe(subUrl.c_str(), user.c_str(), pass.c_str(), 5, &subRes);
            if (subRc == RTSP_PROBE_OK)
            {
                chosenUrl  = subUrl;
                streamKind = "substream";
                codecOut   = subRes.codec;
                chosenRes  = subRes;
                return subRc;
            }
        }
        return rc;
    };

    // Modern scheme first (firmware >= V5.0 — everything that also speaks mDNS).
    int rc = probePair("rtsp://" + ip + ":554/Streaming/Channels/101",
                       "rtsp://" + ip + ":554/Streaming/Channels/102");

    // 404 on the modern path: pre-V5 firmware serves the legacy scheme instead.
    // A wrong PATH costs no login-lock strikes (only a rejected password does),
    // so this second scheme stays within the one-failed-login budget (A5) —
    // an AUTH result above never reaches here.
    if (rc == RTSP_PROBE_BAD_PATH)
    {
        ChipLogProgress(Camera, "CameraBridge: %s modern RTSP path not found (404) — trying legacy /h264/ch1 scheme",
                        ip.c_str());
        rc = probePair("rtsp://" + ip + ":554/h264/ch1/main/av_stream",
                       "rtsp://" + ip + ":554/h264/ch1/sub/av_stream");
    }

    if (rc != RTSP_PROBE_OK)
    {
        // NO_H264 covers two distinct probe outcomes, told apart by the codec field:
        // "H265" = a stream exists but is H.265; "" = the camera returned no video stream at all.
        // AUTH with a non-empty auth_note means no credential was ever sent (the 401
        // challenge offered no scheme we implement, e.g. Digest SHA-256 only) — surface
        // the note instead of blaming the credentials.
        std::string authWhy = authNote.empty() ? std::string("auth_failed (check default credentials)")
                                               : "auth_failed: " + authNote;
        const char * why = rc == RTSP_PROBE_AUTH          ? authWhy.c_str()
            : rc == RTSP_PROBE_UNREACHABLE                ? "unreachable"
            : rc == RTSP_PROBE_BAD_PATH                   ? "bad_path (no known Hikvision RTSP scheme answered)"
            : rc == RTSP_PROBE_NO_H264 && codecOut == "H265"
                                                          ? "h265_only — set the camera's video encoding to H.264"
            : rc == RTSP_PROBE_NO_H264                    ? "no_video_stream"
                                                          : "probe_failed";
        ChipLogProgress(Camera, "CameraBridge: direct-RTSP onboarding of '%s' (%s) skipped: %s", name.c_str(),
                        ip.c_str(), why);
        return rc;
    }

    entry.name   = name.empty() ? std::string("Hikvision Camera") : name; // non-ONVIF direct camera
    entry.dni    = dni;
    entry.controlUrl.clear(); // no ONVIF control URL in direct mode
    entry.stream = streamKind;
    entry.mode   = "direct";
    entry.onvif.rtspUrl = chosenUrl;
    entry.onvif.ptzUrl.clear();
    entry.onvif.snapshotUrl.clear();
    entry.onvif.token.clear();
    entry.onvif.user = user;
    entry.onvif.pass = pass;
    entry.onvif.needsBasicAuth = chosenRes.used_basic_fallback ? true : false;
    PopulateAudioCapability(chosenRes.audio_codec, chosenRes.audio_rate, chosenRes.audio_channels, chosenRes.has_audio, entry.onvif);
    ChipLogProgress(Camera, "CameraBridge: direct-RTSP onboarded '%s' (%s) rtsp=%s codec=%s", entry.name.c_str(),
                    dni.c_str(), chosenUrl.c_str(), codecOut.c_str());
    return RTSP_PROBE_OK;
}

// ---------------------------------------------------------------------------
// Add one bridged camera endpoint.
// Returns the slot index (≥0) on success, -1 on failure.
// ---------------------------------------------------------------------------
int AddCameraEndpoint(BridgedCamera * cam, EndpointId parentId)
{
    // Slot 0 is reserved for the Aggregator endpoint; cameras occupy slots 1+.
    for (uint8_t index = 1; index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT; ++index)
    {
        if (gDevices[index] != nullptr)
            continue;

        gDevices[index] = cam;
        CHIP_ERROR err;

        while (true)
        {
            StackLock lock;
            cam->SetEndpointId(gCurrentEndpointId);
            cam->SetParentEndpointId(parentId);

            err = emberAfSetDynamicEndpoint(index, gCurrentEndpointId, &sBridgedCameraEndpoint,
                                            Span<DataVersion>(cam->mDataVersions),
                                            Span<const EmberAfDeviceType>(kBridgedCameraDeviceTypes), parentId);
            if (err == CHIP_NO_ERROR)
            {
                ChipLogProgress(DeviceLayer, "BridgedCamera '%s' → endpoint %u (slot %u)", cam->GetName(),
                                gCurrentEndpointId, index);

                if (cam->GetUniqueId()[0] == '\0')
                    cam->GenerateUniqueId();

                // BridgedDeviceBasicInformation (0x0039): gives this bridged camera
                // a unique node label + uniqueId so the controller shows a distinct
                // card per camera.  Required for the Bridged Node device type.
                // SoftwareVersion(+String) must be served: the SmartThings camera plugin
                // subscribes to attr 0x09 during setup and, if it errors, the whole
                // subscription dies and the plugin's setup (incl. its record of the
                // provisioned Push AV CA ids) is aborted -> "PAV server CA ID is absent".
                LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(
                    cam->CreateBridgedDeviceInfo(gCurrentEndpointId, { .reachable = true, .nodeLabel = cam->GetName() },
                                                 { .uniqueId              = cam->GetUniqueId(),
                                                   .softwareVersion      = static_cast<uint32_t>(1),
                                                   .softwareVersionString = std::string("1.0") })));

                // Register all camera clusters for this endpoint via the registry.
                cam->InitClusters(gCurrentEndpointId);

                return static_cast<int>(index);
            }

            if (err != CHIP_ERROR_ENDPOINT_EXISTS)
            {
                ChipLogError(DeviceLayer, "emberAfSetDynamicEndpoint failed: %" CHIP_ERROR_FORMAT, err.Format());
                gDevices[index] = nullptr;
                return -1;
            }

            // Endpoint ID collision — try the next one.
            if (++gCurrentEndpointId < gFirstDynamicEndpointId)
                gCurrentEndpointId = gFirstDynamicEndpointId;
        }
    }

    ChipLogError(DeviceLayer, "AddCameraEndpoint: all %d dynamic slots are full", CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT);
    return -1;
}

// ---------------------------------------------------------------------------
// Create a bridged camera from a fully-resolved config entry: make the backend,
// give it a stable DNI-derived uniqueId, register the dynamic endpoint, and keep
// it alive in gBridgedCameras. Returns the assigned endpoint id, or -1 on failure.
// Used by both ApplicationInit (cameras.json) and the IPC upsert path so the two
// behave identically.
// ---------------------------------------------------------------------------
int AddCamera(const CameraConfig::CameraEntry & entry)
{
    auto cam = std::make_unique<BridgedCamera>(entry.name.c_str(), entry.onvif);
    cam->SetEntry(entry);

    // Stable identity: derive the Bridged uniqueId from the DNI so a camera keeps
    // its identity across restarts. The id is capped (Device::kDeviceUniqueIdSize,
    // 32); keep the tail — the UUID part of "onvif-urn:uuid:<uuid>" — which is the
    // actually-unique portion.
    if (!entry.dni.empty())
    {
        constexpr size_t kUniqueIdCap = 32;
        std::string uid               = entry.dni;
        if (uid.size() > kUniqueIdCap)
            uid = uid.substr(uid.size() - kUniqueIdCap);
        cam->SetUniqueId(uid.c_str());
    }

    if (AddCameraEndpoint(cam.get(), gAggregatorEndpointId) < 0)
    {
        cam->Shutdown();
        return -1;
    }

    int endpoint = static_cast<int>(cam->GetEndpointId());
    gBridgedCameras.push_back(std::move(cam));
    gCameraCount.store(gBridgedCameras.size());
    // One greppable per-camera summary of the codec/auth verdict that will drive the live-view
    // pipeline and WebRTC packetizer. If live view later fails, this is the first line to check:
    // it must show the camera's REAL stream codec (grep "CAM_CODEC").
    ChipLogProgress(Camera, "CAM_CODEC: camera '%s' (dni=%s) endpoint=%d video_codec=%s needs_basic_auth=%d rtsp=%s",
                    entry.name.c_str(), entry.dni.c_str(), endpoint,
                    entry.onvif.videoCodec.empty() ? "H264(default)" : entry.onvif.videoCodec.c_str(),
                    entry.onvif.needsBasicAuth ? 1 : 0, entry.onvif.rtspUrl.c_str());
    return endpoint;
}

// Remove the bridged camera with this DNI: clear its dynamic endpoint and tear
// down its backend. Returns true if a matching camera was found.
//
// purgePersistedTransports: pass true ONLY when the camera is being permanently deleted
// (the `remove_camera` IPC op) — it wipes the endpoint's persisted PushAV transports so
// nothing stale is restored at next boot. Every replace flow (upsert re-onboard,
// set_default_creds re-resolve, discovery in-place rebuild) MUST pass false: the camera
// still exists afterwards and the controller does NOT re-send AllocatePushTransport after
// a reboot (transports are restore-designed) — purging there would silently break a
// still-present camera's provisioned recording.
//
// Called from the IPC accept thread, which does not hold the Matter stack lock,
// so the ember/registry mutations run under StackLock — the same foreign-thread
// pattern bridge-app uses in RemoveDeviceEndpoint. (Do NOT route this through
// PlatformMgr().ScheduleWork(): the POSIX event loop dispatches scheduled work
// with the non-recursive stack mutex already held, so StackLock would deadlock.)
bool RemoveCameraByDni(const std::string & dni, bool purgePersistedTransports)
{
    if (dni.empty())
        return false;

    for (auto it = gBridgedCameras.begin(); it != gBridgedCameras.end(); ++it)
    {
        if ((*it)->GetDni() != dni)
            continue;

        BridgedCamera * cam = it->get();
        {
            StackLock lock;
            // On true removal, delete the camera's persisted PushAV transports FIRST, before
            // any teardown step that could crash or be interrupted. A remove that dies later
            // can then never leave a stale transport — carrying this (deleted) camera's upload
            // client cert — to be restored at next boot on a reused endpoint id (seen on
            // hardware: every clip PUT through the resurrected transport got 403, cert CN !=
            // clip owner).
            if (purgePersistedTransports)
            {
                cam->DeletePersistedPushAvTransports();
            }
            for (uint8_t i = 1; i < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT; ++i)
            {
                if (gDevices[i] == cam)
                {
                    cam->Unregister();              // BridgedDeviceBasicInformation
                    emberAfClearDynamicEndpoint(i); // drop the endpoint from the data model
                    gDevices[i] = nullptr;
                    break;
                }
            }
            cam->Shutdown(); // camera clusters + WebRTC/GStreamer, while the stack is quiesced
        }

        ChipLogProgress(Camera, "CameraBridge: removed camera dni=%s", dni.c_str());
        gBridgedCameras.erase(it);
        gCameraCount.store(gBridgedCameras.size());
        return true;
    }
    return false;
}

// The RTSP Digest-quirk verdict (OnvifConfig::needsBasicAuth) is a CAMERA-firmware
// property, detected once at first onboard and STABLE across credential changes. Return
// the known verdict (0/1) for an already-bridged camera with this stable id, or -1 if
// the camera is not currently bridged (a fresh onboard -> the resolver should probe once
// to detect it). Passing the known value into onvif_resolve_bridge on a re-resolve means
// a later password/IP change never re-probes RTSP (no extra login attempts, no Hikvision
// lockout risk) and never flips a good verdict because a new/failed credential was tried.
// Runs on the IPC/discovery thread — the only mutator of gBridgedCameras — so no lock.
int KnownBasicAuth(const std::string & sid)
{
    for (const auto & cam : gBridgedCameras)
        if (StableCameraId(cam->GetEntry().dni) == sid)
            return cam->GetEntry().onvif.needsBasicAuth ? 1 : 0;
    return -1;
}

// The real stream codec is firmware-stable and detected once (from the SDP probe) at fresh
// onboard, exactly like needsBasicAuth. Return the already-known codec for a bridged camera,
// or "" if it is not currently bridged (fresh onboard -> use the just-probed value).
std::string KnownVideoCodec(const std::string & sid)
{
    for (const auto & cam : gBridgedCameras)
        if (StableCameraId(cam->GetEntry().dni) == sid)
            return cam->GetEntry().onvif.videoCodec;
    return {};
}

// Pick the sticky video codec when (re)building an entry.
//   prior < 0  => fresh onboard: the RTSP probe just ran and read the REAL SDP, so trust
//                 probedCodec (res.codec).
//   prior >= 0 => re-resolve / credential change / IP move: the probe did NOT run, so
//                 res.codec is only ONVIF metadata (not guaranteed to match the live stream)
//                 and must be ignored. Keep the camera's ALREADY-KNOWN codec: the in-hand
//                 entry's value first — this is the ONLY reliable source at BOOT, where no
//                 endpoints exist yet so the gBridgedCameras lookup would come back empty and
//                 wrongly default a persisted H.265 camera back to H.264 — then the live
//                 bridged camera, then H264 as a last resort.
// currentKnown: the existing entry's videoCodec when the caller has it in hand (re-resolve of a
//               known camera); pass "" for a fresh entry that has no prior value.
std::string PickVideoCodec(int prior, const char * probedCodec, const std::string & currentKnown, const std::string & sid)
{
    if (prior < 0 && probedCodec && probedCodec[0])
        return probedCodec;
    if (!currentKnown.empty())
        return currentKnown;
    std::string known = KnownVideoCodec(sid);
    return known.empty() ? std::string("H264") : known;
}

// Persist the current camera list to cameras.json so IPC-added cameras survive a
// restart. Called on the IPC thread after each successful add/remove.
void PersistCameras()
{
    std::vector<CameraConfig::CameraEntry> entries;
    entries.reserve(gBridgedCameras.size());
    for (const auto & cam : gBridgedCameras)
        entries.push_back(cam->GetEntry());
    CameraConfig::SaveToFile(entries);
}

// ---------------------------------------------------------------------------
// LAN discovery (native mDNS + WS-Discovery; boot + on-demand IPC `discover`)
// ---------------------------------------------------------------------------

// Resolve a freshly-discovered camera with the bridge default ONVIF login, falling
// back to anonymous if the default creds are rejected. On success fills a complete
// CameraEntry and returns true; returns false if the camera needs credentials we
// don't have (caller skips it). BLOCKING SOAP — never call under StackLock.
bool ResolveDiscoveredCamera(const onvif_discovered_t & d, const std::string & sid, CameraConfig::CameraEntry & e)
{
    onvif_resolved_bridge_t res;
    std::string cu = gDefaultUser, cp = gDefaultPass;
    // Fresh onboard (this path only runs for cameras not already known): -1 => probe
    // RTSP once to detect the broken-Digest quirk. KnownBasicAuth returns the persisted
    // verdict on the off chance this stable id is already bridged (then no re-probe).
    int prior = KnownBasicAuth(sid);
    int drc   = onvif_resolve_bridge(d.control_url, cu.c_str(), cp.c_str(), /*want_main=*/1, prior, &res);
    if (drc != ONVIF_BRIDGE_OK && !(cu.empty() && cp.empty()))
    {
        cu.clear(); // default creds rejected — fall back to anonymous
        cp.clear();
        drc = onvif_resolve_bridge(d.control_url, "", "", /*want_main=*/1, prior, &res);
    }
    if (drc != ONVIF_BRIDGE_OK)
        return false;

    e.name          = d.name[0] ? d.name : "ONVIF Camera";
    e.dni           = sid;
    e.controlUrl    = d.control_url;
    e.stream        = "mainstream";
    e.mode          = "onvif"; // WS-Discovery implies a working ONVIF service
    e.onvif.rtspUrl     = res.rtsp_url;
    e.onvif.ptzUrl      = res.ptz_url;
    e.onvif.snapshotUrl = res.snapshot_url;
    e.onvif.token       = res.token;
    e.onvif.user        = cu;
    e.onvif.pass        = cp;
    e.onvif.needsBasicAuth = res.needs_basic_auth ? true : false;
    // Fresh WS-D onboard entry: no in-hand prior codec (currentKnown=""). prior<0 uses the
    // probe; an already-bridged sid falls back to the live camera inside PickVideoCodec.
    e.onvif.videoCodec  = PickVideoCodec(prior, res.codec, /*currentKnown=*/"", sid);
    return true;
}

// A known camera's control URL (DHCP IP) changed. Re-resolve it — its own creds
// first, then the bridge default, then anonymous — and update `entry` IN PLACE so a
// reconnect doesn't spawn a duplicate. Returns true if re-resolved (entry updated),
// false to keep the previous config. BLOCKING SOAP — never call under StackLock.
bool ReresolveMovedCamera(const onvif_discovered_t & d, CameraConfig::CameraEntry & entry)
{
    onvif_resolved_bridge_t res;
    std::string cu = entry.onvif.user.empty() ? gDefaultUser : entry.onvif.user;
    std::string cp = entry.onvif.pass.empty() ? gDefaultPass : entry.onvif.pass;
    // Known camera (only its IP changed): pass the existing verdict so we don't re-probe
    // RTSP for the Digest quirk — it's a firmware property, unchanged by the IP move.
    int prior = entry.onvif.needsBasicAuth ? 1 : 0;
    int drc   = onvif_resolve_bridge(d.control_url, cu.c_str(), cp.c_str(), /*want_main=*/1, prior, &res);
    if (drc != ONVIF_BRIDGE_OK && !(cu.empty() && cp.empty()))
    {
        cu.clear();
        cp.clear();
        drc = onvif_resolve_bridge(d.control_url, "", "", /*want_main=*/1, prior, &res);
    }
    if (drc != ONVIF_BRIDGE_OK)
        return false;

    entry.controlUrl        = d.control_url;
    entry.onvif.rtspUrl     = res.rtsp_url;
    entry.onvif.ptzUrl      = res.ptz_url;
    entry.onvif.snapshotUrl = res.snapshot_url;
    entry.onvif.token       = res.token;
    entry.onvif.user        = cu;
    entry.onvif.pass        = cp;
    entry.onvif.needsBasicAuth = res.needs_basic_auth ? true : false;
    // Same reasoning as needsBasicAuth above: the real stream codec is a firmware property,
    // unchanged by an IP move alone — prior >= 0 here so this always keeps the known value
    // (no fresh probe ran), never trusting a stale ONVIF-metadata guess in res.codec. Pass the
    // in-hand persisted codec as currentKnown: this runs at BOOT (gBridgedCameras still empty),
    // so the in-hand value is the only reliable source — without it a moved H.265 camera would
    // wrongly reset to H.264. (Captured before the assignment overwrites the field.)
    const std::string priorCodec = entry.onvif.videoCodec;
    entry.onvif.videoCodec = PickVideoCodec(prior, res.codec, priorCodec, StableCameraId(entry.dni));
    return true;
}

struct DiscoverResult
{
    int found   = 0; // distinct LAN responders this scan (mDNS ∪ WS-Discovery; <0 = every scan failed)
    int added   = 0; // NEW cameras onboarded (appended / bridged endpoints created) this scan
    int updated = 0; // known cameras updated in place (DHCP IP change / identity merge)
};

// Shared two-pass LAN discovery used by BOTH boot (ApplicationInit) and the runtime
// `discover` IPC op, so the two behave identically.
//
// Pass 1 — mDNS FIRST ([hikvision], user priority): modern Hikvision cameras ship ONVIF
// (hence WS-Discovery) DISABLED by default but Bonjour ON, so the WS-Discovery pass below
// is blind to them. Per mDNS device: tier-1 ONVIF resolve anyway (in case the user enabled
// it — that yields PTZ/snapshot for free); otherwise tier-2 onboarding via a directly-formed
// RTSP URL (direct mode, TryDirectRtsp). Two-stage identity (A3, G1/G2): stage A dedups
// known cameras WITHOUT contacting them (opportunistic ARP + serial + host IP), stage B
// re-reads ARP AFTER a successful contact for the authoritative MAC id. LOCKOUT-CRITICAL
// (A5): at most ONE failed login per camera per scan — known cameras are never
// re-authenticated, an ONVIF AUTH rejection aborts before the RTSP probe, and TryDirectRtsp
// itself makes a single credentialed attempt. See docs/HIKVISION_DISCOVERY_PLAN.md.
//
// Pass 2 — WS-Discovery: ONVIF-enabled cameras. Matches responders to `list` by STABLE
// identity (MAC, StableCameraId) — not the volatile URN/IP — plus a by-IP crosscheck
// against entries the mDNS pass may have keyed hik-serial-… (G1); appends newly-resolved
// cameras and updates moved ones in place.
//
// COUNTING: `found` totals distinct responders — every mDNS responder plus each WS-D
// responder whose host IP the mDNS pass did not already report (a camera answering both
// passes is one physical device, counted once). `added` = new cameras onboarded; `updated`
// = known cameras rewritten in place (IP change / identity merge).
//
// When createEndpointsLive is true it ALSO makes each new/updated camera live via
// AddCamera / RemoveCameraByDni.  THREADING: this runs on the IPC accept thread (a
// foreign thread) and MUST NOT be wrapped in StackLock — the two scans, the ONVIF
// resolves and the RTSP probes block for seconds and would stall the Matter event loop,
// and AddCamera / RemoveCameraByDni each take StackLock internally for just the ember
// mutation, so an outer StackLock would self-deadlock (ScheduleWork is just as forbidden:
// the POSIX event loop dispatches scheduled work with the non-recursive stack mutex
// already held). This is exactly the lock discipline HandleIpcUpsert uses (resolve on the
// IPC thread BEFORE the locked section). At boot the caller is single-threaded (no event
// loop yet), createEndpointsLive is false, and ApplicationInit's post-scan loop builds
// every endpoint.
DiscoverResult RunDiscovery(std::vector<CameraConfig::CameraEntry> & list, bool createEndpointsLive)
{
    DiscoverResult result;

    // Rebuild the live bridged endpoint of an entry that changed: same Remove-then-Add
    // path upsert_camera uses at runtime. `oldDni` can differ from entry.dni when an
    // identity merge just migrated it (e.g. hik-serial-… → onvif-mac-…) — the live
    // endpoint is still keyed on the old one.
    auto rebuildLive = [](const std::string & oldDni, const CameraConfig::CameraEntry & entry) {
        RemoveCameraByDni(oldDni, /* purgePersistedTransports = */ false); // replace, camera stays
        if (AddCamera(entry) < 0)
            ChipLogError(Camera, "CameraBridge: discover failed to rebuild endpoint for %s", entry.dni.c_str());
    };

    // ------------------------------------------------------------------
    // Pass 1: mDNS ([hikvision]).
    // ------------------------------------------------------------------
    std::vector<std::string> mdnsIps; // responder IPs — pass 2 uses them to avoid double-counting `found`
    mdns_found_t mdnsFound[16];
    int nMdns = onvif_mdns_discover(mdnsFound, 16, /*wait_secs=*/4);
    ChipLogProgress(Camera, "CameraBridge: mDNS found %d Hikvision camera(s) on the LAN", nMdns);
    if (nMdns > 0)
        result.found += nMdns;
    for (int i = 0; i < nMdns; ++i)
    {
        std::string ip = mdnsFound[i].ip;
        mdnsIps.push_back(ip);

        // Two-stage identity (A3, G1/G2). ARP is only guaranteed complete AFTER a TCP
        // contact with the camera, so the authoritative MAC id is read post-contact in
        // stage B below. Stage A (here) decides "already known?" WITHOUT contacting the
        // camera — no login attempt is ever spent on a known camera (A5) — by matching
        // opportunistic ARP (often warm from the mDNS reply itself), the entry's host IP,
        // and the serial-fallback id, so entries persisted under EITHER id form
        // (onvif-mac-… or hik-serial-…) are recognized (G1).
        std::string arpSid    = MacFromArp(ip); // opportunistic — may be "" pre-contact
        std::string serialSid = mdnsFound[i].serial[0] ? std::string("hik-serial-") + mdnsFound[i].serial : std::string();

        CameraConfig::CameraEntry * match = nullptr;
        for (auto & e : list)
        {
            std::string esid = StableCameraId(e.dni);
            if ((!arpSid.empty() && esid == arpSid) || (!serialSid.empty() && esid == serialSid) ||
                HostFromUrl(e.controlUrl) == ip || HostFromUrl(e.onvif.rtspUrl) == ip)
            {
                match = &e;
                break;
            }
        }
        if (match != nullptr)
        {
            // Already known. For a direct camera, rebuild its RTSP URL in place on a DHCP
            // IP change (the mirror of the WS-D control-URL update below).
            if (match->mode == "direct")
            {
                std::string newUrl = "rtsp://" + ip + ":554/Streaming/Channels/" +
                    (match->stream == "substream" ? "102" : "101");
                if (match->onvif.rtspUrl != newUrl)
                {
                    ChipLogProgress(Camera, "CameraBridge: direct camera %s IP changed -> %s (rtsp updated in place)",
                                    match->dni.c_str(), ip.c_str());
                    match->onvif.rtspUrl = newUrl;
                    result.updated++;
                    if (createEndpointsLive)
                        rebuildLive(match->dni, *match);
                }
            }
            continue;
        }

        // Stage B helpers, used only after a SUCCESSFUL resolve/probe (i.e. a completed TCP
        // handshake, which guarantees a complete 0x2 ARP entry — A3/G2). The serial fallback
        // should be near-impossible past that point.
        auto authoritativeSid = [&ip, &mdnsFound, i]() {
            std::string s = MacFromArp(ip);
            return s.empty() ? std::string("hik-serial-") + mdnsFound[i].serial : s;
        };
        // Final dedup (G1): stage A can miss when ARP was cold and the camera is persisted
        // under onvif-mac-… with a since-changed IP (e.g. onboarded via WS-D on an earlier
        // boot). Now that the id is authoritative, fold the fresh resolve into the existing
        // entry in place instead of appending a duplicate bridged camera.
        auto mergeOrAppend = [&list, &result, &rebuildLive, createEndpointsLive](CameraConfig::CameraEntry && e) {
            for (auto & known : list)
                if (StableCameraId(known.dni) == StableCameraId(e.dni))
                {
                    ChipLogProgress(Camera,
                                    "CameraBridge: mDNS camera %s already known under its authoritative id (updated in place)",
                                    e.dni.c_str());
                    known.controlUrl = e.controlUrl;
                    known.stream     = e.stream;
                    known.mode       = e.mode;
                    known.onvif      = e.onvif;
                    result.updated++;
                    if (createEndpointsLive)
                        rebuildLive(known.dni, known);
                    return;
                }
            if (createEndpointsLive)
            {
                // Create the bridged endpoint LIVE (same path upsert_camera uses at runtime).
                if (AddCamera(e) < 0)
                {
                    ChipLogError(Camera, "CameraBridge: discover failed to add endpoint for '%s'", e.name.c_str());
                    return; // not appended — the caller's list must keep mirroring the live set
                }
            }
            list.push_back(std::move(e));
            result.added++;
        };

        // Tier 1 (A1): ONVIF resolve — works only if the user enabled ONVIF. For an
        // ONVIF-disabled camera the /onvif/device_service endpoint isn't served, so this
        // fails on the HTTP layer WITHOUT a login attempt (no lockout risk).
        std::string controlUrl = "http://" + ip + "/onvif/device_service";
        std::string cu = gDefaultUser, cp = gDefaultPass;
        onvif_resolved_bridge_t res;
        // Fresh onboard (stage A did not match this camera as known): -1 => probe RTSP
        // once to detect the broken-Digest quirk.
        int drc = onvif_resolve_bridge(controlUrl.c_str(), cu.c_str(), cp.c_str(), /*want_main=*/1, /*prior=*/-1, &res);
        if (drc != ONVIF_BRIDGE_OK && !(cu.empty() && cp.empty()))
        {
            drc = onvif_resolve_bridge(controlUrl.c_str(), "", "", 1, /*prior=*/-1, &res);
            if (drc == ONVIF_BRIDGE_OK)
            {
                cu.clear();
                cp.clear();
            }
        }
        if (drc == ONVIF_BRIDGE_OK)
        {
            // Stage B (A3/G2): the resolve's TCP contact just completed — read ARP NOW for
            // the authoritative MAC id, not the possibly-cold pre-contact one.
            CameraConfig::CameraEntry e;
            e.name       = mdnsFound[i].name[0] ? mdnsFound[i].name : "ONVIF Camera";
            e.dni        = authoritativeSid();
            e.controlUrl = controlUrl;
            e.stream     = "mainstream";
            e.mode       = "onvif";
            e.onvif.rtspUrl     = res.rtsp_url;
            e.onvif.ptzUrl      = res.ptz_url;
            e.onvif.snapshotUrl = res.snapshot_url;
            e.onvif.token       = res.token;
            e.onvif.user        = cu;
            e.onvif.pass        = cp;
            e.onvif.needsBasicAuth = res.needs_basic_auth ? true : false;
            // Fresh onboard: the probe ran (prior=-1), so trust res.codec; no in-hand prior.
            e.onvif.videoCodec     = PickVideoCodec(-1, res.codec, /*currentKnown=*/"", e.dni);
            PopulateAudioCapability(res.audio_codec, res.audio_rate, res.audio_channels, res.has_audio_in, e.onvif);
            ChipLogProgress(Camera, "CameraBridge: mDNS+ONVIF resolved '%s' (%s) rtsp=%s", e.name.c_str(),
                            e.dni.c_str(), e.onvif.rtspUrl.c_str());
            mergeOrAppend(std::move(e));
            continue;
        }

        // If ONVIF explicitly rejected the creds, the same creds will fail RTSP too — skip
        // direct mode so we don't burn the camera's login-lock budget (A5).
        if (drc == ONVIF_BRIDGE_AUTH)
        {
            ChipLogProgress(Camera, "CameraBridge: mDNS camera %s ONVIF auth failed — skipping (fix default creds)",
                            ip.c_str());
            continue;
        }

        // Tier 2 (A1/A5): directly-formed RTSP with the default login (single auth attempt).
        // The pre-contact id passed here is provisional (for the probe's own log line);
        // stage B overwrites it with the post-contact authoritative id on success.
        CameraConfig::CameraEntry e;
        std::string codec;
        if (TryDirectRtsp(ip, mdnsFound[i].name, gDefaultUser, gDefaultPass,
                          arpSid.empty() ? std::string("hik-serial-") + mdnsFound[i].serial : arpSid, e,
                          codec) == RTSP_PROBE_OK)
        {
            // Stage B (A3/G2): the probe's TCP contact just completed — re-read ARP for the
            // authoritative MAC id.
            e.dni = authoritativeSid();
            mergeOrAppend(std::move(e));
        }
    }

    // ------------------------------------------------------------------
    // Pass 2: WS-Discovery (ONVIF-enabled cameras).
    // ------------------------------------------------------------------
    onvif_discovered_t wsdFound[16];
    int nWsd = onvif_ws_discover(wsdFound, 16, /*wait_secs=*/4);
    ChipLogProgress(Camera, "CameraBridge: WS-Discovery found %d ONVIF camera(s) on the LAN", nWsd);
    for (int i = 0; i < nWsd; ++i)
    {
        std::string sid = StableCameraId(wsdFound[i].urn);

        // Match an already-known camera by STABLE identity (MAC), not the volatile URN/IP.
        // [hikvision] Also crosscheck by host IP (G1): the mDNS pass above may have keyed
        // this same camera hik-serial-… (ARP fallback) while its WS-D URN yields
        // onvif-mac-… — the shared IP is what ties the two views to one physical device.
        std::string wsdHost = HostFromUrl(wsdFound[i].control_url);

        // `found` dedup: a responder whose IP already answered the mDNS pass is the same
        // physical camera — count it once across both passes.
        bool seenByMdns = false;
        for (const auto & mip : mdnsIps)
            if (!wsdHost.empty() && mip == wsdHost)
            {
                seenByMdns = true;
                break;
            }
        if (!seenByMdns)
            result.found++;

        CameraConfig::CameraEntry * match = nullptr;
        for (auto & e : list)
            if (StableCameraId(e.dni) == sid ||
                (!wsdHost.empty() && (HostFromUrl(e.controlUrl) == wsdHost || HostFromUrl(e.onvif.rtspUrl) == wsdHost)))
            {
                match = &e;
                break;
            }

        if (match != nullptr)
        {
            const std::string oldDni = match->dni; // a live endpoint stays keyed on this until rebuilt
            match->dni               = sid;        // migrate any old full-URN / hik-serial dni to the stable form
            // Same physical camera. If its control URL (DHCP IP) changed, re-resolve and
            // update it IN PLACE so a reconnect doesn't spawn a duplicate bridged camera.
            if (match->controlUrl != wsdFound[i].control_url)
            {
                CameraConfig::CameraEntry updated = *match;
                if (ReresolveMovedCamera(wsdFound[i], updated))
                {
                    ChipLogProgress(Camera, "CameraBridge: camera %s IP changed %s -> %s (updated in place)", sid.c_str(),
                                    match->controlUrl.c_str(), wsdFound[i].control_url);
                    *match = updated;
                    result.updated++;
                    if (createEndpointsLive)
                        rebuildLive(oldDni, *match);
                }
                else
                {
                    ChipLogProgress(Camera, "CameraBridge: camera %s new IP %s failed to resolve — keeping previous",
                                    sid.c_str(), wsdFound[i].control_url);
                }
            }
            continue;
        }

        // New camera: resolve with the default ONVIF login (anonymous fallback).
        CameraConfig::CameraEntry e;
        if (!ResolveDiscoveredCamera(wsdFound[i], sid, e))
        {
            ChipLogProgress(Camera, "CameraBridge: discovered %s needs credentials (resolve failed) — skipping",
                            wsdFound[i].urn);
            continue;
        }
        ChipLogProgress(Camera, "CameraBridge: discovered + resolved '%s' (%s) rtsp=%s", e.name.c_str(), e.dni.c_str(),
                        e.onvif.rtspUrl.c_str());
        if (createEndpointsLive)
        {
            // Create the bridged endpoint LIVE (same path upsert_camera uses at runtime).
            if (AddCamera(e) < 0)
            {
                ChipLogError(Camera, "CameraBridge: discover failed to add endpoint for '%s'", e.name.c_str());
                continue;
            }
        }
        list.push_back(std::move(e));
        result.added++;
    }

    if (nMdns < 0 && nWsd < 0)
        result.found = -1; // BOTH scans failed outright (socket errors) — signal a scan error to the IPC caller

    return result;
}

// ---------------------------------------------------------------------------
// IPC callbacks — run on the BridgeIpc accept thread (a foreign thread).
// ---------------------------------------------------------------------------
BridgeIpc::OpResult HandleIpcUpsert(const BridgeIpc::UpsertRequest & req)
{
    BridgeIpc::OpResult out;

    // Resolve ONVIF off the Matter event loop (blocking SOAP network calls).
    // Basic-auth verdict: known (0/1) if this stable id is already bridged (a re-onboard —
    // e.g. the Edge driver re-pushing a camera, or a password change) so we preserve it
    // without re-probing RTSP; -1 for a genuinely new camera, which probes once to detect.
    onvif_resolved_bridge_t res;
    int wantMain = (req.stream != "substream") ? 1 : 0;
    int prior    = KnownBasicAuth(StableCameraId(req.dni));
    int rc       = onvif_resolve_bridge(req.controlUrl.c_str(), req.userid.c_str(), req.password.c_str(), wantMain, prior, &res);
    if (rc != ONVIF_BRIDGE_OK)
    {
        // [hikvision] ONVIF failed for a non-auth reason (unreachable / no ONVIF streams /
        // parse error — all consistent with ONVIF being DISABLED on a modern Hikvision that
        // still serves RTSP). Fall back to a directly-formed RTSP URL so the Edge driver's
        // manual add-by-IP works for such cameras with no driver/protocol change. We do NOT
        // fall back on ONVIF auth failure: that means the creds are wrong, so an RTSP probe with
        // the same creds would only burn the camera's login-lock budget (A5). The probe itself
        // makes a single authenticated attempt.
        if (rc != ONVIF_BRIDGE_AUTH)
        {
            std::string host = HostFromUrl(req.controlUrl);
            std::string sid  = !req.dni.empty() ? StableCameraId(req.dni) : MacFromArp(host);
            if (sid.empty())
                sid = std::string("hik-ip-") + host;
            CameraConfig::CameraEntry entry;
            std::string codec;
            int prc = TryDirectRtsp(host, req.name, req.userid, req.password, sid, entry, codec);
            if (prc == RTSP_PROBE_OK)
            {
                RemoveCameraByDni(sid, /* purgePersistedTransports = */ false); // idempotent re-onboard (replace, camera stays)
                int endpoint = AddCamera(entry);
                if (endpoint < 0)
                {
                    out.status = "internal_error";
                    out.error  = "failed to create camera endpoint";
                    return out;
                }
                PersistCameras();
                out.ok         = true;
                out.status     = "onboarded";
                out.endpoint   = endpoint;
                out.videoCodec = codec;
                ChipLogProgress(Camera, "CameraBridge: upsert dni=%s onboarded (direct-RTSP) on endpoint %d",
                                req.dni.c_str(), endpoint);
                return out;
            }
            if (prc == RTSP_PROBE_AUTH)
            {
                out.status = "auth_failed";
                out.error  = "RTSP auth failed (direct mode)";
                return out;
            }
            if (prc == RTSP_PROBE_NO_H264)
            {
                out.status = "no_streams";
                out.error  = "no H.264 stream (set the camera's video encoding to H.264)";
                return out;
            }
            if (prc == RTSP_PROBE_BAD_PATH)
            {
                // Both the modern and legacy Hikvision RTSP schemes got a 404. Keep the
                // protocol's status vocabulary ("unreachable"); the error text carries the
                // truth so nobody chases a credentials problem that does not exist.
                out.status = "unreachable";
                out.error  = "RTSP path not found (tried /Streaming/Channels/101 and legacy /h264/ch1)";
                return out;
            }
            // still unreachable — fall through to the standard error mapping
        }
        switch (rc)
        {
        case ONVIF_BRIDGE_AUTH:        out.status = "auth_failed"; break;
        case ONVIF_BRIDGE_UNREACHABLE: out.status = "unreachable"; break;
        case ONVIF_BRIDGE_NO_STREAMS:  out.status = "no_streams"; break;
        default:                       out.status = "internal_error"; break;
        }
        out.error = "ONVIF resolve failed (rc=" + std::to_string(rc) + ")";
        ChipLogError(Camera, "CameraBridge: upsert dni=%s resolve failed (%s)", req.dni.c_str(), out.status.c_str());
        return out;
    }

    // Key on the stable MAC-derived identity so a re-onboard after a reboot/IP change
    // updates the existing camera instead of creating a duplicate (see StableCameraId).
    std::string sid = StableCameraId(req.dni);

    CameraConfig::CameraEntry entry;
    entry.name          = req.name.empty() ? std::string("Camera") : req.name;
    entry.dni           = sid;
    entry.controlUrl    = req.controlUrl;
    entry.stream        = req.stream.empty() ? std::string("mainstream") : req.stream;
    entry.mode          = "onvif";
    entry.onvif.rtspUrl     = res.rtsp_url;
    entry.onvif.ptzUrl      = res.ptz_url;
    entry.onvif.snapshotUrl = res.snapshot_url;
    entry.onvif.token       = res.token;
    entry.onvif.user        = req.userid;
    entry.onvif.pass        = req.password;
    entry.onvif.needsBasicAuth = res.needs_basic_auth ? true : false;
    // Fresh IPC entry: no in-hand prior codec. A brand-new camera has prior<0 (probe ran, use
    // res.codec); a re-onboard of a still-bridged camera has prior>=0 and falls back to the live
    // bridged camera's codec inside PickVideoCodec (runtime path — gBridgedCameras is populated).
    entry.onvif.videoCodec     = PickVideoCodec(prior, res.codec, /*currentKnown=*/"", sid);
    PopulateAudioCapability(res.audio_codec, res.audio_rate, res.audio_channels, res.has_audio_in, entry.onvif);

    // Replace any existing camera with the same stable identity (idempotent re-onboard).
    RemoveCameraByDni(sid, /* purgePersistedTransports = */ false); // replace, camera stays

    int endpoint = AddCamera(entry);
    if (endpoint < 0)
    {
        out.status = "internal_error";
        out.error  = "failed to create camera endpoint";
        return out;
    }

    PersistCameras();

    out.ok          = true;
    out.status      = "onboarded";
    out.endpoint    = endpoint;
    out.profiles    = res.profiles;
    out.videoCodec  = res.codec;
    out.hasPtz      = res.has_ptz != 0;
    out.hasAudioOut = res.has_audio_out != 0;
    out.hasAudioIn  = res.has_audio_in != 0;
    out.audioCodec  = res.audio_codec;
    ChipLogProgress(Camera, "CameraBridge: upsert dni=%s onboarded on endpoint %d", req.dni.c_str(), endpoint);
    return out;
}

BridgeIpc::OpResult HandleIpcRemove(const std::string & dni, int endpoint)
{
    BridgeIpc::OpResult out;
    std::string target = dni;
    // Endpoint fallback: the Edge driver cannot read the child's UniqueID (hub-core
    // does not forward driver reads of BridgedDeviceBasicInformation), so at delete
    // time it may only know the endpoint from the child's device_network_id. Resolve
    // it to the stable dni here — we own the endpoint→camera mapping. Runs on the
    // IPC thread, the only mutator of gBridgedCameras (no lock needed).
    if (target.empty() && endpoint > 0)
    {
        for (const auto & cam : gBridgedCameras)
        {
            if (static_cast<int>(cam->GetEndpointId()) == endpoint)
            {
                target = cam->GetEntry().dni;
                ChipLogProgress(Camera, "CameraBridge: remove_camera endpoint=%d -> dni=%s", endpoint, target.c_str());
                break;
            }
        }
        if (target.empty())
            ChipLogProgress(Camera, "CameraBridge: remove_camera endpoint=%d matches no camera (already removed?)",
                            endpoint);
    }
    if (!target.empty())
        // TRUE removal (the only purging caller): also wipe this endpoint's persisted PushAV
        // transports so the deleted camera's upload cert can't be restored at next boot.
        // Accepted trade-off: a REPLACE that lands on a different endpoint id still orphans
        // the old endpoint's blob (pre-existing endpoint-id keying hazard) — single-camera
        // rigs reuse the same id so restore stays coherent, and the orphan is bounded by this
        // same-id purge on true removals.
        RemoveCameraByDni(target, /* purgePersistedTransports = */ true); // idempotent: a not-found camera is already "removed"
    PersistCameras();
    out.ok     = true;
    out.status = "removed";
    return out;
}

// [single_bridge] Apply ONE default ONVIF login to EVERY camera (from the Matter Bridge
// card): store + persist it, then apply it to every camera. CONSISTENCY RULE: the
// credentials the user entered are ALWAYS what each camera uses afterwards — there is NO
// silent fallback to the camera's previous login and NO anonymous fallback. So the answer
// to "which credentials is this camera using?" is always exactly "the ones last entered
// here" (mirrored in default_creds and in every cameras.json entry), which is what the
// operator can see. If the entered password is wrong, the stream then fails *with that
// password* — a visible "fix the credentials" signal — instead of appearing to succeed
// while a stale login quietly does the work.
//
// For ONVIF cameras we still re-resolve with the new creds to refresh the stream facts
// (rtsp/ptz/token/snapshot). If that resolve fails (wrong password, or the camera is
// momentarily offline) we keep the previously resolved stream URL but STILL swap in the
// entered credentials. Direct-RTSP cameras (no control URL) keep their IP-derived URL and
// are NOT re-probed here — each direct probe spends a Hikvision login-lock strike, and the
// URL is stable — but their credentials are updated the same way.
// Runs on the IPC thread (blocking SOAP off the Matter loop; endpoint mutations under StackLock).
BridgeIpc::OpResult HandleSetDefaultCreds(const std::string & user, const std::string & pass)
{
    BridgeIpc::OpResult out;
    gDefaultUser = user;
    gDefaultPass = pass;
    SaveDefaultCreds();
    ChipLogProgress(Camera, "CameraBridge: set_default_creds user='%s' pass=%s -> applying to all cameras",
                    user.c_str(), pass.empty() ? "(blank)" : "***");

    // Snapshot each camera's full entry so we don't iterate the live list while mutating it.
    std::vector<CameraConfig::CameraEntry> snap;
    for (const auto & c : gBridgedCameras)
        snap.push_back(c->GetEntry());

    int applied = 0;
    for (const auto & s : snap)
    {
        // Start from the camera's current config and overwrite ONLY the credentials: the
        // entered user/pass are unconditionally what this camera uses from now on.
        CameraConfig::CameraEntry ne = s;
        ne.onvif.user = user;
        ne.onvif.pass = pass;

        if (!s.controlUrl.empty())
        {
            // ONVIF camera: try to refresh the stream facts with the new creds.
            // Basic-auth verdict is a firmware property, unchanged by a credential change:
            // pass the existing verdict so this re-resolve does NOT re-probe RTSP (no extra
            // login attempts, no lockout) and can never flip a good verdict because the new
            // password happens to be wrong or an ONVIF-only account.
            int wantMain = (s.stream != "substream") ? 1 : 0;
            int prior    = s.onvif.needsBasicAuth ? 1 : 0;
            onvif_resolved_bridge_t res;
            int rc = onvif_resolve_bridge(s.controlUrl.c_str(), user.c_str(), pass.c_str(), wantMain, prior, &res);
            if (rc == ONVIF_BRIDGE_OK)
            {
                ne.onvif.rtspUrl     = res.rtsp_url;
                ne.onvif.ptzUrl      = res.ptz_url;
                ne.onvif.snapshotUrl = res.snapshot_url;
                ne.onvif.token       = res.token;
                ne.onvif.needsBasicAuth = res.needs_basic_auth ? true : false; // == prior (passed through)
                // Same reasoning: the real stream codec is a firmware property, unchanged by a
                // credential change alone. prior >= 0 here so this always keeps the known value —
                // ne started as a copy of the snapshot entry, so ne.onvif.videoCodec IS the
                // camera's existing codec; pass it as currentKnown (robust even if the camera
                // isn't in gBridgedCameras at this instant).
                ne.onvif.videoCodec = PickVideoCodec(prior, res.codec, ne.onvif.videoCodec, StableCameraId(s.dni));
                PopulateAudioCapability(res.audio_codec, res.audio_rate, res.audio_channels, res.has_audio_in,
                                        ne.onvif);
            }
            else
            {
                // Resolve rejected/unreachable: keep the last-known stream URL but still
                // apply the entered creds (already set above). A wrong password thus shows
                // up as a failing stream, not as a silently-unchanged camera.
                ChipLogError(Camera,
                             "CameraBridge: set_default_creds re-resolve failed for %s (rc=%d) — applied "
                             "entered creds, kept last-known stream URL",
                             s.dni.c_str(), rc);
            }
        }

        // Replace = remove-then-add. Every entry in `snap` was just read from the live set, so
        // the remove MUST match one; if it does not (e.g. an empty/duplicate dni that
        // RemoveCameraByDni can't key on), adding anyway would DUPLICATE the camera — and since
        // this loop runs on every set_default_creds, the duplicates compound until all endpoint
        // slots are exhausted. Guard: only add the rebuilt camera when the old one was actually
        // removed. (With the CLI camera now carrying a stable dni, this guard should never trip;
        // it's here so no future identity-less camera can trigger the same storm.)
        bool removed = RemoveCameraByDni(s.dni, /* purgePersistedTransports = */ false);
        if (!removed)
        {
            ChipLogError(Camera,
                         "CameraBridge: set_default_creds could not remove camera dni='%s' (rtsp=%s) — skipping re-add "
                         "to avoid a duplicate endpoint",
                         s.dni.c_str(), s.onvif.rtspUrl.c_str());
            continue;
        }
        if (AddCamera(ne) >= 0)
        {
            applied++;
            ChipLogProgress(Camera, "CameraBridge: applied creds to %s -> rtsp=%s user='%s'", s.dni.c_str(),
                            ne.onvif.rtspUrl.c_str(), user.empty() ? "(anonymous)" : user.c_str());
        }
    }
    PersistCameras();
    out.ok       = true;
    out.status   = "creds_applied";
    out.endpoint = applied; // carried back as result.cameras in the response
    return out;
}

// [single_bridge] Runtime LAN rescan (Edge pull-to-refresh): run the same two-pass native
// discovery as boot (mDNS first, then WS-Discovery — shared RunDiscovery()), onboard
// newly-found cameras and update moved ones live, then persist.
//
// DURATION: the two scans block ~4 s each, plus per-camera ONVIF resolves / RTSP probes
// — expect ~8-10 s end to end (more when several new cameras need resolving), during
// which no response byte is sent. The IPC client's request timeout must comfortably
// exceed that: a 12 s budget covers the common case with little headroom (note the stock
// bridge_ipc.lua M.TIMEOUT of 5 s is NOT enough for this op) — revisit before adding any
// further scan pass.
//
// Lock discipline (mirrors HandleIpcUpsert): the BLOCKING scans + per-camera ONVIF
// resolve / RTSP probe happen on this IPC thread FIRST, with NO StackLock held; the
// ember/registry endpoint mutations run under StackLock taken INTERNALLY by AddCamera /
// RemoveCameraByDni (createEndpointsLive=true) — never via ScheduleWork (the POSIX
// event loop dispatches scheduled work with the non-recursive stack mutex already
// held, so an inner StackLock would deadlock). gBridgedCameras is mutated only on this
// thread, so it needs no separate lock.
//
// DEBOUNCE: a scan costs ~8-10 s of blocking IPC-thread time, and each pull-to-refresh
// gesture fires one discover op — rapid pulls therefore queue back-to-back FULL scans
// (observed on hardware: ~10 pulls => ~90 s of continuous scanning). The LAN cannot
// change meaningfully within seconds, so a request arriving < kDiscoverDebounceSecs
// after the last completed scan replays that scan's result instead of re-scanning;
// pulls queued behind an in-flight scan then drain instantly.
constexpr std::chrono::seconds kDiscoverDebounceSecs{ 10 };
// Last completed scan's outcome + completion time (steady_clock: monotonic — never wall
// clock, so NTP jumps can't defeat or extend the window). SINGLE-WRITER: read and
// written only on the IPC accept thread — the same single-writer assumption
// gBridgedCameras relies on — so no lock is needed.
DiscoverResult gLastDiscoverResult;
std::chrono::steady_clock::time_point gLastDiscoverDone;
bool gHaveLastDiscover = false; // steady_clock's epoch is boot time, so a zero time_point is NOT "never"

BridgeIpc::OpResult HandleIpcDiscover()
{
    BridgeIpc::OpResult out;

    // Debounced path: replay the previous scan's `found`, report added=0 (anything that
    // scan added is already live and persisted), and report the CURRENT bridged-camera
    // count (not a cached one — an upsert/remove since the scan is still reflected).
    if (gHaveLastDiscover)
    {
        auto elapsed = std::chrono::steady_clock::now() - gLastDiscoverDone;
        if (elapsed < kDiscoverDebounceSecs)
        {
            out.ok       = true;
            out.status   = "ok";
            out.found    = gLastDiscoverResult.found;
            out.added    = 0;
            out.endpoint = static_cast<int>(gBridgedCameras.size()); // total bridged now (result.cameras)
            ChipLogProgress(Camera, "CameraBridge: discover debounced (last scan %us ago) -> found=%d cameras=%zu",
                            static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()),
                            gLastDiscoverResult.found, gBridgedCameras.size());
            return out;
        }
    }

    // Snapshot the currently-bridged cameras so RunDiscovery can match known cameras
    // by stable id; the live truth (gBridgedCameras) is mutated by AddCamera/Remove.
    std::vector<CameraConfig::CameraEntry> list;
    list.reserve(gBridgedCameras.size());
    for (const auto & c : gBridgedCameras)
        list.push_back(c->GetEntry());

    DiscoverResult dr = RunDiscovery(list, /*createEndpointsLive=*/true);
    if (dr.found < 0)
    {
        out.status = "unreachable";
        out.error  = "discovery scan failed (mDNS + WS-Discovery)";
        ChipLogError(Camera, "CameraBridge: discover scan failed (rc=%d)", dr.found);
        return out;
    }

    // Completed scan: cache the outcome for the debounce window above. Failed scans
    // (found < 0, early return) are deliberately NOT cached so a transient failure can
    // be retried immediately.
    gLastDiscoverResult = dr;
    gLastDiscoverDone   = std::chrono::steady_clock::now();
    gHaveLastDiscover   = true;

    if (dr.added > 0 || dr.updated > 0)
        PersistCameras();

    out.ok       = true;
    out.status   = "ok";
    out.found    = dr.found;
    out.added    = dr.added;
    out.endpoint = static_cast<int>(gBridgedCameras.size()); // total bridged now (result.cameras)
    ChipLogProgress(Camera, "CameraBridge: discover -> found=%d added=%d updated=%d cameras=%zu", dr.found, dr.added,
                    dr.updated, gBridgedCameras.size());
    return out;
}

// ---------------------------------------------------------------------------
// Named-pipe command glue (re-use CameraAppCommandDelegate for single-camera
// commands; extend later for per-camera index).
// ---------------------------------------------------------------------------
NamedPipeCommands sChipNamedPipeCommands;
CameraAppCommandDelegate sCameraAppCommandDelegate;

} // namespace

// ---------------------------------------------------------------------------
// ApplicationInit / ApplicationShutdown (called by ChipLinuxAppMainLoop)
// ---------------------------------------------------------------------------
void ApplicationInit()
{
    ChipLogProgress(Camera, "CameraBridge: ApplicationInit");

    memset(gDevices, 0, sizeof(gDevices));

    // Dynamic endpoint IDs start immediately after the last fixed endpoint.
    gFirstDynamicEndpointId = static_cast<EndpointId>(
        static_cast<int>(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1))) + 1);
    gCurrentEndpointId = gFirstDynamicEndpointId;

    // Disable the static ZAP camera endpoint (endpoint 1).  Its camera clusters
    // are declared in ember but have no backend wired (we only wire CameraApp to
    // the dynamic endpoints), so leaving it enabled makes a controller see a
    // broken second camera whose ZoneManagement/PushAV reads fail.  A disabled
    // endpoint is removed from the PartsList entirely, so controllers won't read it.
    EndpointId fixedCameraEndpointId =
        emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1));
    emberAfEndpointEnableDisable(fixedCameraEndpointId, false);

    // Create the Aggregator (0x000E) endpoint at dynamic slot 0.  Bridged cameras
    // are parented to it (slots 1+), so the controller sees a bridge with one
    // bridged camera device per endpoint.  Parent defaults to the root node.
    gAggregatorEndpointId = gCurrentEndpointId++;
    {
        StackLock lock;
        CHIP_ERROR err = emberAfSetDynamicEndpoint(0, gAggregatorEndpointId, &sAggregatorEndpoint,
                                                   Span<DataVersion>(gAggregatorDataVersions),
                                                   Span<const EmberAfDeviceType>(kAggregatorDeviceTypes));
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(Camera, "CameraBridge: failed to create Aggregator endpoint: %" CHIP_ERROR_FORMAT, err.Format());
        }
        else
        {
            ChipLogProgress(Camera, "CameraBridge: Aggregator endpoint %u created", gAggregatorEndpointId);
        }
    }

    // Optional named pipe for zone simulation / test commands.
    std::string pipePath = std::string(LinuxDeviceOptions::GetInstance().app_pipe);
    if (!pipePath.empty() && sChipNamedPipeCommands.Start(pipePath, &sCameraAppCommandDelegate) != CHIP_NO_ERROR)
    {
        ChipLogError(NotSpecified, "CameraBridge: failed to start NamedPipeCommands");
        TEMPORARY_RETURN_IGNORED sChipNamedPipeCommands.Stop();
    }

    // ------------------------------------------------------------------
    // Load camera list from /data/onvif-bridge/cameras.json.
    // Fall back to a single camera described via CLI options.
    // ------------------------------------------------------------------
    auto cameraList = CameraConfig::LoadFromFile();

    // [single_bridge] Load the one default ONVIF login (set from the Matter Bridge card)
    // so WS-Discovery below can resolve auth cameras with it.
    LoadDefaultCreds();

    // [single_bridge] Collapse pre-existing duplicates. Earlier builds keyed a camera on its
    // ONVIF URN + IP, both of which change when a camera reboots (fresh UUID) or takes a new
    // DHCP lease — so one physical camera could accumulate several entries (and several
    // SmartThings cards) over reconnects. Re-key every entry on its STABLE identity (MAC) and
    // drop duplicates, keeping the first occurrence.
    {
        std::vector<CameraConfig::CameraEntry> deduped;
        deduped.reserve(cameraList.size());
        for (auto & e : cameraList)
        {
            std::string sid = StableCameraId(e.dni);
            bool dup        = false;
            for (const auto & d : deduped)
                if (StableCameraId(d.dni) == sid)
                {
                    dup = true;
                    break;
                }
            if (dup)
            {
                ChipLogProgress(Camera, "CameraBridge: dropping duplicate camera entry %s (stable id %s)", e.dni.c_str(),
                                sid.c_str());
                continue;
            }
            e.dni = sid; // migrate the persisted DNI to the stable form
            deduped.push_back(e);
        }
        cameraList.swap(deduped);
    }

    // ------------------------------------------------------------------
    // [single_bridge][hikvision] Boot discovery: find cameras on the LAN ourselves —
    // mDNS first (Hikvision cameras with ONVIF disabled), then WS-Discovery
    // (ONVIF-enabled) — resolved with the default ONVIF login (falling back to
    // anonymous). Together with the PersistCameras() below, the bridge populates
    // cameras.json itself — no separate "ONVIF Camera Manager" Edge driver needed.
    // Runs at startup (single-threaded, before the event loop / IPC thread), so
    // cameras are present at commission time (which the commissioning rule requires).
    // The same RunDiscovery() backs the runtime `discover` IPC op (Edge
    // pull-to-refresh); here createEndpointsLive is false because the post-scan loop
    // below builds every endpoint.
    // ------------------------------------------------------------------
    RunDiscovery(cameraList, /*createEndpointsLive=*/false);

    // Only synthesize a single-camera CLI fallback when cameras.json is ABSENT.
    // A present-but-empty file ("[]") explicitly means "no cameras yet — the Edge
    // driver will add them over IPC", so we must not create a phantom CLI camera
    // (which would duplicate a camera once it is onboarded through the driver).
    bool haveConfigFile = std::ifstream(CameraConfig::kDefaultPath).good();

    if (cameraList.empty() && !haveConfigFile)
    {
        auto & opts = LinuxDeviceOptions::GetInstance();
        CameraConfig::CameraEntry entry;
        entry.name = "Camera";
        if (opts.cameraOnvifUrl.HasValue())    entry.onvif.rtspUrl = opts.cameraOnvifUrl.Value();
        if (opts.cameraOnvifPtzUrl.HasValue()) entry.onvif.ptzUrl  = opts.cameraOnvifPtzUrl.Value();
        if (opts.cameraOnvifToken.HasValue())  entry.onvif.token   = opts.cameraOnvifToken.Value();
        if (opts.cameraOnvifUser.HasValue())   entry.onvif.user    = opts.cameraOnvifUser.Value();
        if (opts.cameraOnvifPass.HasValue())   entry.onvif.pass    = opts.cameraOnvifPass.Value();
        // Assign a STABLE, non-empty dni derived from the RTSP host. A camera with an empty dni
        // cannot be deduped or removed (StableCameraId("")=="" and RemoveCameraByDni("") no-ops),
        // so any later replace path (notably set_default_creds, which removes-then-re-adds every
        // camera) would spawn a fresh duplicate on each call and eventually exhaust all 16
        // endpoint slots. Keying the CLI camera on its host makes remove/replace idempotent.
        std::string cliHost = HostFromUrl(entry.onvif.rtspUrl);
        entry.dni = cliHost.empty() ? std::string("cli-camera") : (std::string("cli-") + cliHost);
        ChipLogProgress(Camera, "CameraBridge: no cameras.json — single-camera CLI fallback (dni=%s rtsp=%s)",
                        entry.dni.c_str(), entry.onvif.rtspUrl.c_str());
        cameraList.push_back(std::move(entry));
    }
    else if (cameraList.empty())
    {
        ChipLogProgress(Camera, "CameraBridge: cameras.json present but empty — starting with 0 cameras (IPC-driven)");
    }

    // ------------------------------------------------------------------
    // Create one bridged endpoint per camera.
    // ------------------------------------------------------------------
    for (const auto & entry : cameraList)
    {
        if (AddCamera(entry) < 0)
            ChipLogError(Camera, "CameraBridge: failed to add endpoint for '%s'", entry.name.c_str());
    }

    ChipLogProgress(Camera, "CameraBridge: %zu camera(s) registered on dynamic endpoints", gBridgedCameras.size());

    // Persist the merged list (cameras.json entries + freshly discovered) so the
    // discovered cameras survive a restart — i.e. WS-Discovery populates cameras.json.
    PersistCameras();

    // ------------------------------------------------------------------
    // Start the Edge⇄bridge IPC server (ipc/PROTOCOL.md) so the Edge driver can
    // add/remove cameras at runtime without a restart. Binds 0.0.0.0:9444.
    // ------------------------------------------------------------------
    BridgeIpc::Callbacks ipcCallbacks;
    ipcCallbacks.upsert = [](const BridgeIpc::UpsertRequest & req) { return HandleIpcUpsert(req); };
    ipcCallbacks.remove = [](const std::string & dni, int endpoint) { return HandleIpcRemove(dni, endpoint); };
    ipcCallbacks.count  = []() -> size_t { return gCameraCount.load(); };
    ipcCallbacks.setDefaultCreds =
        [](const std::string & u, const std::string & p) { return HandleSetDefaultCreds(u, p); };
    ipcCallbacks.discover = []() { return HandleIpcDiscover(); };
    if (!BridgeIpc::Start(9444, std::move(ipcCallbacks)))
    {
        ChipLogError(Camera, "CameraBridge: IPC server failed to start on :9444 (runtime add/remove disabled)");
    }
}

void ApplicationShutdown()
{
    // Stop accepting IPC requests first and join the accept thread, so no upsert/
    // remove can mutate the camera list while we tear it down below.
    BridgeIpc::Stop();

    // Unregister each camera's BridgedDeviceBasicInformation from the global cluster registry
    // BEFORE its backend is freed (the runtime remove path already does this). Otherwise the
    // freed registration stays linked in the ServerClusterInterfaceRegistry and its static
    // destructor dereferences a dangling node at process exit. Also close WebRTC connections
    // here while the Matter SystemLayer is still running so WebRTC callbacks can use ScheduleLambda.
    for (auto & cam : gBridgedCameras)
    {
        cam->Unregister();
        cam->Shutdown();
    }

    gBridgedCameras.clear();

    TEMPORARY_RETURN_IGNORED sChipNamedPipeCommands.Stop();
}

int main(int argc, char * argv[])
{
    VerifyOrDie(ChipLinuxAppInit(argc, argv) == 0);

    // TLS delegates must be set before ChipLinuxAppMainLoop starts the server.
    InitializeTlsClientManagement();
    InitializeTlsCertificateManagement();

    ChipLinuxAppMainLoop(); // runs ApplicationShutdown + persists KVS/fabric/counters before returning

    // Skip C++ static/global destructors on the way out. CHIP's global
    // ServerClusterInterfaceRegistry destructor walks its registration list and calls
    // Shutdown() on each entry, but the static-destruction order of that singleton relative
    // to the (also static) camera/registry objects is undefined — some cluster objects are
    // already gone, so it dereferences a dangling node and SIGSEGVs at every process exit.
    // All meaningful persistence (KVS, fabric, counters) already happened in the main loop's
    // shutdown above; there is nothing left to flush, so exit immediately and let the OS
    // reclaim memory/fds. (Do not add a trailing return — it would be unreachable and trip
    // -Wunreachable-code/-Werror.)
    _exit(0);
}

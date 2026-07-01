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
#include <fstream>
#include <memory>
#include <string>
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
                LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(
                    cam->CreateBridgedDeviceInfo(gCurrentEndpointId, { .reachable = true, .nodeLabel = cam->GetName() },
                                                 { .uniqueId = cam->GetUniqueId() })));

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
    return endpoint;
}

// Remove the bridged camera with this DNI: clear its dynamic endpoint and tear
// down its backend. Returns true if a matching camera was found.
//
// Called from the IPC accept thread, which does not hold the Matter stack lock,
// so the ember/registry mutations run under StackLock — the same foreign-thread
// pattern bridge-app uses in RemoveDeviceEndpoint. (Do NOT route this through
// PlatformMgr().ScheduleWork(): the POSIX event loop dispatches scheduled work
// with the non-recursive stack mutex already held, so StackLock would deadlock.)
bool RemoveCameraByDni(const std::string & dni)
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
// IPC callbacks — run on the BridgeIpc accept thread (a foreign thread).
// ---------------------------------------------------------------------------
BridgeIpc::OpResult HandleIpcUpsert(const BridgeIpc::UpsertRequest & req)
{
    BridgeIpc::OpResult out;

    // Resolve ONVIF off the Matter event loop (blocking SOAP network calls).
    onvif_resolved_bridge_t res;
    int wantMain = (req.stream != "substream") ? 1 : 0;
    int rc       = onvif_resolve_bridge(req.controlUrl.c_str(), req.userid.c_str(), req.password.c_str(), wantMain, &res);
    if (rc != ONVIF_BRIDGE_OK)
    {
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
    entry.onvif.rtspUrl = res.rtsp_url;
    entry.onvif.ptzUrl  = res.ptz_url;
    entry.onvif.token   = res.token;
    entry.onvif.user    = req.userid;
    entry.onvif.pass    = req.password;

    // Replace any existing camera with the same stable identity (idempotent re-onboard).
    RemoveCameraByDni(sid);

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
    ChipLogProgress(Camera, "CameraBridge: upsert dni=%s onboarded on endpoint %d", req.dni.c_str(), endpoint);
    return out;
}

BridgeIpc::OpResult HandleIpcRemove(const std::string & dni)
{
    BridgeIpc::OpResult out;
    RemoveCameraByDni(dni); // idempotent: a not-found camera is already "removed"
    PersistCameras();
    out.ok     = true;
    out.status = "removed";
    return out;
}

// [single_bridge] Apply ONE default ONVIF login to EVERY camera (from the Matter Bridge
// card): store + persist it, then re-resolve each camera with it. Per camera we fall back
// to an anonymous resolve if the creds fail, so an anonymous camera is never broken.
// Runs on the IPC thread (blocking SOAP off the Matter loop; endpoint mutations under StackLock).
BridgeIpc::OpResult HandleSetDefaultCreds(const std::string & user, const std::string & pass)
{
    BridgeIpc::OpResult out;
    gDefaultUser = user;
    gDefaultPass = pass;
    SaveDefaultCreds();
    ChipLogProgress(Camera, "CameraBridge: set_default_creds user='%s' pass=%s -> re-resolving all cameras",
                    user.c_str(), pass.empty() ? "(blank)" : "***");

    // Snapshot identity/source of each camera so we don't iterate the live list while mutating it.
    struct Snap { std::string dni, controlUrl, name, stream; };
    std::vector<Snap> snap;
    for (const auto & c : gBridgedCameras)
    {
        const auto & e = c->GetEntry();
        snap.push_back({ e.dni, e.controlUrl, e.name, e.stream });
    }

    int applied = 0;
    for (const auto & s : snap)
    {
        if (s.controlUrl.empty())
            continue;
        int wantMain = (s.stream != "substream") ? 1 : 0;
        onvif_resolved_bridge_t res;
        std::string u = user, p = pass;
        int rc = onvif_resolve_bridge(s.controlUrl.c_str(), u.c_str(), p.c_str(), wantMain, &res);
        if (rc != ONVIF_BRIDGE_OK && !(user.empty() && pass.empty()))
        {
            u.clear(); // creds rejected — fall back to anonymous so we never break a working camera
            p.clear();
            rc = onvif_resolve_bridge(s.controlUrl.c_str(), "", "", wantMain, &res);
        }
        if (rc != ONVIF_BRIDGE_OK)
        {
            ChipLogError(Camera, "CameraBridge: set_default_creds re-resolve failed for %s (rc=%d) — left unchanged",
                         s.dni.c_str(), rc);
            continue;
        }
        CameraConfig::CameraEntry ne;
        ne.name          = s.name;
        ne.dni           = s.dni;
        ne.controlUrl    = s.controlUrl;
        ne.stream        = s.stream.empty() ? std::string("mainstream") : s.stream;
        ne.onvif.rtspUrl = res.rtsp_url;
        ne.onvif.ptzUrl  = res.ptz_url;
        ne.onvif.token   = res.token;
        ne.onvif.user    = u;
        ne.onvif.pass    = p;
        RemoveCameraByDni(s.dni);
        if (AddCamera(ne) >= 0)
        {
            applied++;
            ChipLogProgress(Camera, "CameraBridge: re-resolved %s with %s creds -> rtsp=%s user='%s'", s.dni.c_str(),
                            u.empty() ? "anonymous" : "default", ne.onvif.rtspUrl.c_str(), u.c_str());
        }
    }
    PersistCameras();
    out.ok       = true;
    out.status   = "creds_applied";
    out.endpoint = applied; // carried back as result.cameras in the response
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
    // [single_bridge] WS-Discovery: find ONVIF cameras on the LAN ourselves and add any
    // not already in the list, resolved with the default ONVIF login (falling back to
    // anonymous). Together with the PersistCameras() below, the bridge populates
    // cameras.json itself — no separate "ONVIF Camera Manager" Edge driver needed. Runs at
    // startup (single-threaded, before the event loop / IPC thread), so cameras are present
    // at commission time (which the commissioning rule requires).
    // ------------------------------------------------------------------
    {
        onvif_discovered_t found[16];
        int nf = onvif_ws_discover(found, 16, /*wait_secs=*/4);
        ChipLogProgress(Camera, "CameraBridge: WS-Discovery found %d ONVIF camera(s) on the LAN", nf);
        for (int i = 0; i < nf; ++i)
        {
            std::string sid = StableCameraId(found[i].urn);

            // Match an already-known camera by STABLE identity (MAC), not the volatile URN/IP.
            CameraConfig::CameraEntry * match = nullptr;
            for (auto & e : cameraList)
                if (StableCameraId(e.dni) == sid)
                {
                    match = &e;
                    break;
                }

            if (match != nullptr)
            {
                match->dni = sid; // migrate any old full-URN dni to the stable form
                // Same physical camera. If its control URL (DHCP IP) changed, re-resolve and
                // update it IN PLACE so a reconnect doesn't spawn a duplicate bridged camera.
                if (match->controlUrl != found[i].control_url)
                {
                    onvif_resolved_bridge_t res;
                    std::string cu = match->onvif.user.empty() ? gDefaultUser : match->onvif.user;
                    std::string cp = match->onvif.pass.empty() ? gDefaultPass : match->onvif.pass;
                    int drc = onvif_resolve_bridge(found[i].control_url, cu.c_str(), cp.c_str(), /*want_main=*/1, &res);
                    if (drc != ONVIF_BRIDGE_OK && !(cu.empty() && cp.empty()))
                    {
                        cu.clear();
                        cp.clear();
                        drc = onvif_resolve_bridge(found[i].control_url, "", "", /*want_main=*/1, &res);
                    }
                    if (drc == ONVIF_BRIDGE_OK)
                    {
                        ChipLogProgress(Camera, "CameraBridge: camera %s IP changed %s -> %s (updated in place)", sid.c_str(),
                                        match->controlUrl.c_str(), found[i].control_url);
                        match->controlUrl    = found[i].control_url;
                        match->onvif.rtspUrl = res.rtsp_url;
                        match->onvif.ptzUrl  = res.ptz_url;
                        match->onvif.token   = res.token;
                        match->onvif.user    = cu;
                        match->onvif.pass    = cp;
                    }
                    else
                    {
                        ChipLogProgress(Camera, "CameraBridge: camera %s new IP %s failed to resolve — keeping previous",
                                        sid.c_str(), found[i].control_url);
                    }
                }
                continue;
            }

            // Resolve with the default ONVIF login if one is set, else anonymously
            // (and fall back to anonymous if the default creds are rejected).
            onvif_resolved_bridge_t res;
            std::string cu = gDefaultUser, cp = gDefaultPass;
            int drc = onvif_resolve_bridge(found[i].control_url, cu.c_str(), cp.c_str(), /*want_main=*/1, &res);
            if (drc != ONVIF_BRIDGE_OK && !(cu.empty() && cp.empty()))
            {
                cu.clear();
                cp.clear();
                drc = onvif_resolve_bridge(found[i].control_url, "", "", /*want_main=*/1, &res);
            }
            if (drc != ONVIF_BRIDGE_OK)
            {
                ChipLogProgress(Camera, "CameraBridge: discovered %s needs credentials (resolve failed) — skipping",
                                found[i].urn);
                continue;
            }
            CameraConfig::CameraEntry e;
            e.name          = found[i].name[0] ? found[i].name : "ONVIF Camera";
            e.dni           = sid;
            e.controlUrl    = found[i].control_url;
            e.stream        = "mainstream";
            e.onvif.rtspUrl = res.rtsp_url;
            e.onvif.ptzUrl  = res.ptz_url;
            e.onvif.token   = res.token;
            e.onvif.user    = cu;
            e.onvif.pass    = cp;
            ChipLogProgress(Camera, "CameraBridge: discovered + resolved '%s' (%s) rtsp=%s", e.name.c_str(), e.dni.c_str(),
                            e.onvif.rtspUrl.c_str());
            cameraList.push_back(std::move(e));
        }
    }

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
        ChipLogProgress(Camera, "CameraBridge: no cameras.json — single-camera CLI fallback (rtsp=%s)",
                        entry.onvif.rtspUrl.c_str());
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
    ipcCallbacks.remove = [](const std::string & dni) { return HandleIpcRemove(dni); };
    ipcCallbacks.count  = []() -> size_t { return gCameraCount.load(); };
    ipcCallbacks.setDefaultCreds =
        [](const std::string & u, const std::string & p) { return HandleSetDefaultCreds(u, p); };
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

    // Close WebRTC connections while the Matter SystemLayer is still running so
    // that WebRTC callbacks (e.g. OnConnectionStateChanged) can use ScheduleLambda.
    for (auto & cam : gBridgedCameras)
        cam->Shutdown();

    gBridgedCameras.clear();

    TEMPORARY_RETURN_IGNORED sChipNamedPipeCommands.Stop();
}

int main(int argc, char * argv[])
{
    VerifyOrDie(ChipLinuxAppInit(argc, argv) == 0);

    // TLS delegates must be set before ChipLinuxAppMainLoop starts the server.
    InitializeTlsClientManagement();
    InitializeTlsCertificateManagement();

    ChipLinuxAppMainLoop();
    return 0;
}

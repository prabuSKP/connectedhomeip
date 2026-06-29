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
#include "camera-app.h"
#include "camera-device.h"
#include "cameras-config.h"
#include "tls-certificate-management-instance.h"
#include "tls-client-management-instance.h"

// Bridge-app Device base (provides BridgedDeviceBasicInformation cluster)
#include "Device.h"

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

#include <memory>
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

    // DataVersion backing store for the ember cluster list above (Descriptor only = 1 entry).
    DataVersion mDataVersions[MATTER_ARRAY_SIZE(sBridgedCameraClusters)] = {};

private:
    void HandleDeviceChange(Device * /*dev*/, Device::Changed_t /*mask*/) override {}

    CameraDevice mCameraDevice;
    std::unique_ptr<CameraApp> mCameraApp;
};

// Owns all bridged cameras for the lifetime of the process.
std::vector<std::unique_ptr<BridgedCamera>> gBridgedCameras;

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

    if (cameraList.empty())
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

    // ------------------------------------------------------------------
    // Create one bridged endpoint per camera.
    // ------------------------------------------------------------------
    for (const auto & entry : cameraList)
    {
        auto cam = std::make_unique<BridgedCamera>(entry.name.c_str(), entry.onvif);

        if (AddCameraEndpoint(cam.get(), gAggregatorEndpointId) < 0)
        {
            ChipLogError(Camera, "CameraBridge: failed to add endpoint for '%s'", entry.name.c_str());
            cam->Shutdown();
        }
        else
        {
            gBridgedCameras.push_back(std::move(cam));
        }
    }

    ChipLogProgress(Camera, "CameraBridge: %zu camera(s) registered on dynamic endpoints", gBridgedCameras.size());
}

void ApplicationShutdown()
{
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

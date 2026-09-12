/*
 *    Copyright (c) 2026 Project CHIP Authors
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

// Tuya (non-camera) bridged endpoints.
//
// This header is the seam between camera-bridge-main.cpp — which owns the dynamic
// endpoint slot table (gDevices), the endpoint-id allocator and the Aggregator id —
// and tuya-bridge-endpoints.cpp, which implements the `hubm_*` C API from
// matter_hub_glue.h on top of it.
//
// The whole file is only compiled into the bridge when the tuya_cloud_enabled GN arg
// is set (which also defines HAVE_TUYA); a TUYA=0 build never sees any of it.

#pragma once

#ifdef HAVE_TUYA

#include "Device.h" // bridge-app Device (BridgedDeviceBasicInformation delegate)

#include <app/util/attribute-storage.h>
#include <lib/core/DataModelTypes.h>
#include <lib/support/Span.h>

#include <functional>

namespace TuyaBridge {

// Number of ember clusters on the shared bridged-device endpoint template
// (Descriptor only — every functional cluster is registry-based). Kept in sync with
// camera-bridge-main.cpp by a static_assert there.
inline constexpr size_t kBridgedDeviceClusterCount = 1;

// ---------------------------------------------------------------------------
// Implemented in camera-bridge-main.cpp (the sole owner of the slot table).
// ---------------------------------------------------------------------------

// Claim a dynamic endpoint slot for `dev`, create the ember endpoint with `deviceTypes`
// parented to the Aggregator, register BridgedDeviceBasicInformation, then call
// `onEndpointReady(endpointId)` so the caller can register its functional clusters.
//
// Slot allocation is shared with the cameras (same gDevices array), so a Tuya device and
// a camera can never collide. Takes StackLock internally: call it from a foreign thread
// (Tuya worker/Pulsar/IPC) that does NOT already hold the stack lock, and never route it
// through PlatformMgr().ScheduleWork() (the POSIX event loop dispatches scheduled work
// with the non-recursive stack mutex held -> self-deadlock).
//
// Returns the assigned endpoint id, or chip::kInvalidEndpointId on failure (in which case
// `onEndpointReady` was not called and `dev` is not referenced by the slot table).
chip::EndpointId AddBridgedDeviceEndpoint(Device * dev, chip::Span<chip::DataVersion> dataVersions,
                                          chip::Span<const EmberAfDeviceType> deviceTypes,
                                          const std::function<void(chip::EndpointId)> & onEndpointReady);

// Reverse of the above: unregister BridgedDeviceBasicInformation, clear the dynamic
// endpoint and free the slot. `onEndpointCleared` runs under the same StackLock, after the
// endpoint is gone from the data model — that is where the functional clusters must be
// unregistered. Returns false if `dev` does not own a slot.
bool RemoveBridgedDeviceEndpoint(Device * dev, const std::function<void()> & onEndpointCleared);

// ---------------------------------------------------------------------------
// Implemented in tuya-bridge-endpoints.cpp, called by camera-bridge-main.cpp.
// ---------------------------------------------------------------------------

// Tear down every Tuya endpoint and stop the command-dispatch thread. Call from
// ApplicationShutdown AFTER tuya_runtime_stop() so no hubm_* call can race it.
void Shutdown();

} // namespace TuyaBridge

// ---------------------------------------------------------------------------
// Extended device callbacks (thermostat / window covering / fan).
//
// `hubm_device_ops` in matter_hub_glue.h only carries on_onoff/on_level/on_color_hs/
// on_colortemp, which cannot express a thermostat setpoint (int16, 0.01 °C), a system
// mode, or a covering stop. Rather than silently squeezing those through on_level and
// losing information, the extra callbacks live here as a strictly ADDITIVE, optional
// second ops struct: the C layer registers it per endpoint after hubm_add_thermostat /
// hubm_add_window_covering / hubm_add_fan.
//
// What still goes through the base hubm_device_ops (so a client that never registers the
// extension is not dead in the water):
//   • thermostat SystemMode  -> on_onoff(mode != Off)
//   • window covering lift   -> on_level(percent 0..100)
//   • fan mode               -> on_onoff(mode != Off), fan percent -> on_level(percent)
// The extension adds the information those lose; both fire for the same command.
// ---------------------------------------------------------------------------
extern "C" {

typedef struct
{
    // setpoint in 0.01 °C; heating != 0 means the heating setpoint, else the cooling one.
    void (*on_setpoint)(void * ctx, int endpoint, int16_t centideg, int heating);
    // Matter Thermostat SystemModeEnum value (0 = Off, 3 = Cool, 4 = Heat, 1 = Auto).
    void (*on_system_mode)(void * ctx, int endpoint, uint8_t system_mode);
    // 0 % = fully open, 100 % = fully closed.
    void (*on_lift_percent)(void * ctx, int endpoint, uint8_t percent);
    void (*on_stop)(void * ctx, int endpoint);
    // Matter FanControl FanModeEnum value (0 = Off, 1 = Low, 2 = Medium, 3 = High).
    void (*on_fan_mode)(void * ctx, int endpoint, uint8_t fan_mode);
    void (*on_fan_percent)(void * ctx, int endpoint, uint8_t percent);
} hubm_device_ops_ext;

// Attach the extended callbacks to an existing endpoint. Returns 0 on success, -1 if no
// such endpoint. Safe to call from any Tuya thread.
int hubm_set_device_ops_ext(int endpoint, const hubm_device_ops_ext * ops, void * ctx);

// Cloud -> Matter for the same three device types. matter_hub_glue.h's report set stops at
// onoff/level/colour/contact/temperature/humidity/motion, which cannot carry a setpoint or
// a mode; these complete it. (Window-covering position and fan speed have no gap: they are
// reported with hubm_report_level as a 0..100 percentage.)
void hubm_report_setpoint(int endpoint, int16_t centideg, int heating);
void hubm_report_system_mode(int endpoint, uint8_t system_mode);
void hubm_report_fan_mode(int endpoint, uint8_t fan_mode);

} // extern "C"

#endif // HAVE_TUYA

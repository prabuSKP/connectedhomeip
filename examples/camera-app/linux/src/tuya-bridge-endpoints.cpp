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

// Tuya non-camera bridged endpoints: the Matter-side implementation of the `hubm_*`
// C API declared in daemon/include/matter_hub_glue.h.
//
// One Tuya cloud device = one dynamic bridged endpoint under the same Aggregator the
// cameras hang off, carrying Bridged Node (0x0013) + BridgedDeviceBasicInformation
// (0x0039) + the functional clusters for its device type. Slots come from
// camera-bridge-main.cpp's shared table (see tuya-bridge-endpoints.h), so a Tuya device
// and a camera can never collide.
//
// Threading model (three rules, in order of how easy they are to break):
//
//  1. hubm_* is called from FOREIGN threads (the Tuya runtime's worker/Pulsar/IPC
//     threads). They take gDevicesMutex and then StackLock — directly, never through
//     PlatformMgr().ScheduleWork(), because the POSIX event loop dispatches scheduled
//     work with the non-recursive stack mutex already held (self-deadlock).
//
//  2. The reverse direction (a Matter command arriving for a Tuya device) runs ON the
//     Matter thread with StackLock held. It must not call into the Tuya C layer there:
//     that would (a) block the whole Matter node on a cloud HTTP round-trip and (b)
//     deadlock the moment the C layer answered with a hubm_report_* (StackLock is not
//     recursive). So command handlers only push a small record onto gQueue, and a
//     dedicated dispatch thread invokes the hubm_device_ops callbacks.
//
//  3. Lock order is therefore always gDevicesMutex -> StackLock, and the queue mutex is
//     never held while taking either of the other two. The Matter thread never takes
//     gDevicesMutex at all.

#ifdef HAVE_TUYA

#include "tuya-bridge-endpoints.h"

// The C glue header has no extern "C" guard of its own.
extern "C" {
#include "matter_hub_glue.h"
}

#include "tuya-clusters.h"

#include <app/clusters/boolean-state-server/BooleanStateCluster.h>
#include <app/clusters/level-control/LevelControlCluster.h>
#include <app/clusters/level-control/LevelControlDelegate.h>
#include <app/clusters/occupancy-sensor-server/OccupancySensingCluster.h>
#include <app/clusters/on-off-server/OnOffCluster.h>
#include <app/clusters/on-off-server/OnOffDelegate.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/server-cluster/ServerClusterInterfaceRegistry.h>
#include <data-model-providers/codegen/CodegenDataModelProvider.h>
#include <lib/core/CHIPError.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DefaultTimerDelegate.h>
#include <platform/PlatformManager.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using chip::DeviceLayer::StackLock;

namespace {

// Defined at the bottom of this file; see the comment there. Keeps the hubm_* API in the
// binary even while the Tuya C layer has no call sites for it yet.
void AnchorHubmApi();

// Timers for the cluster implementations that need them (OnOff scene transitions,
// LevelControl ramps, OccupancySensing hold time). One instance is enough: it is a thin
// wrapper over the Matter system layer, which serialises everything on the stack thread.
chip::app::DefaultTimerDelegate gTimerDelegate;

// ---------------------------------------------------------------------------
// Matter -> Tuya command queue (see rule 2 above)
// ---------------------------------------------------------------------------
enum class CmdKind : uint8_t
{
    kOnOff,
    kLevel,
    kColorHs,
    kColorTemp,
    kSetpoint,   // thermostat, extended ops
    kSystemMode, // thermostat, extended ops
    kLiftPercent,
    kStop, // window covering, extended ops
    kFanMode,
    kFanPercent,
};

struct PendingCommand
{
    EndpointId endpoint = chip::kInvalidEndpointId;
    CmdKind kind        = CmdKind::kOnOff;
    bool flag           = false; // on/off state, or "this setpoint is the heating one"
    uint8_t a           = 0;     // level / hue / percent / mode enum value
    uint8_t b           = 0;     // saturation
    uint16_t u16        = 0;     // colour temperature in mireds
    int16_t i16         = 0;     // thermostat setpoint in 0.01 °C
};

std::mutex gQueueMutex;
std::condition_variable gQueueCv;
std::deque<PendingCommand> gQueue;
std::thread gDispatchThread;
bool gDispatchRunning = false;

// ---------------------------------------------------------------------------
// Device list. Mutated and read only under gDevicesMutex; the entries themselves are
// only ever touched while that mutex is held, which is what keeps a report from racing
// a removal (the classic "detached thread derefs a freed owner" failure).
// ---------------------------------------------------------------------------
class TuyaEndpoint;
std::mutex gDevicesMutex;
std::vector<std::unique_ptr<TuyaEndpoint>> gTuyaDevices;

// hubm_start()'s opaque handle. The Matter stack is already running inside this process
// (ChipLinuxAppMainLoop owns it), so there is no node to bring up or tear down: hubm_start
// hands back this sentinel purely so callers written against the standalone simulation
// keep working, and every hubm_add_*/hubm_report_* ignores the handle entirely.
int gNodeSentinel = 0;

void EnqueueCommand(const PendingCommand & cmd)
{
    {
        std::lock_guard<std::mutex> guard(gQueueMutex);
        if (!gDispatchRunning)
            return;
        // Bound the queue: a wedged cloud connection must not grow it without limit.
        constexpr size_t kMaxQueued = 256;
        if (gQueue.size() >= kMaxQueued)
        {
            ChipLogError(DeviceLayer, "TuyaBridge: command queue full, dropping command for endpoint %u", cmd.endpoint);
            return;
        }
        gQueue.push_back(cmd);
    }
    gQueueCv.notify_one();
}

// Typed shorthands, so the cluster callbacks read as intent rather than as struct filling.
void PostOnOff(EndpointId ep, bool on)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kOnOff;
    cmd.flag     = on;
    EnqueueCommand(cmd);
}

void PostLevel(EndpointId ep, uint8_t level)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kLevel;
    cmd.a        = level;
    EnqueueCommand(cmd);
}

void PostColorHs(EndpointId ep, uint8_t hue, uint8_t saturation)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kColorHs;
    cmd.a        = hue;
    cmd.b        = saturation;
    EnqueueCommand(cmd);
}

void PostColorTemp(EndpointId ep, uint16_t mireds)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kColorTemp;
    cmd.u16      = mireds;
    EnqueueCommand(cmd);
}

void PostSetpoint(EndpointId ep, bool heating, int16_t centiDegrees)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kSetpoint;
    cmd.flag     = heating;
    cmd.i16      = centiDegrees;
    EnqueueCommand(cmd);
}

void PostSystemMode(EndpointId ep, uint8_t systemMode)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kSystemMode;
    cmd.a        = systemMode;
    EnqueueCommand(cmd);
}

void PostLiftPercent(EndpointId ep, uint8_t percent)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kLiftPercent;
    cmd.a        = percent;
    EnqueueCommand(cmd);
}

void PostStop(EndpointId ep)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kStop;
    EnqueueCommand(cmd);
}

void PostFanMode(EndpointId ep, uint8_t fanMode)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kFanMode;
    cmd.a        = fanMode;
    EnqueueCommand(cmd);
}

void PostFanPercent(EndpointId ep, uint8_t percent)
{
    PendingCommand cmd;
    cmd.endpoint = ep;
    cmd.kind     = CmdKind::kFanPercent;
    cmd.a        = percent;
    EnqueueCommand(cmd);
}

// ---------------------------------------------------------------------------
// TuyaEndpoint: one bridged Tuya device.
// ---------------------------------------------------------------------------
class TuyaEndpoint : public Device
{
public:
    TuyaEndpoint(const char * dni, const char * label) :
        Device((label != nullptr && label[0] != '\0') ? label : dni, ""), mDni(dni != nullptr ? dni : "")
    {
        // Stable Matter identity: derive the bridged uniqueId from the Tuya device id so
        // the endpoint keeps its identity across restarts (the numeric endpoint id may
        // shift; controllers key on UniqueID). Capped at Device::kDeviceUniqueIdSize —
        // keep the tail, which is the unique part of a Tuya device id.
        if (!mDni.empty())
        {
            constexpr size_t kUniqueIdCap = Device::kDeviceUniqueIdSize;
            std::string uid               = mDni;
            if (uid.size() > kUniqueIdCap)
                uid = uid.substr(uid.size() - kUniqueIdCap);
            SetUniqueId(uid.c_str());
        }
    }
    ~TuyaEndpoint() override = default;

    const std::string & GetDni() const { return mDni; }

    void SetOps(const hubm_device_ops * ops, void * ctx)
    {
        if (ops != nullptr)
            mOps = *ops;
        mCtx = ctx;
    }
    const hubm_device_ops & Ops() const { return mOps; }
    void * Ctx() const { return mCtx; }

    void SetOpsExt(const hubm_device_ops_ext * ops, void * ctx)
    {
        if (ops != nullptr)
            mOpsExt = *ops;
        else
            mOpsExt = {};
        mCtxExt = ctx;
    }
    const hubm_device_ops_ext & OpsExt() const { return mOpsExt; }
    void * CtxExt() const { return mCtxExt; }

    // Register the functional clusters. Runs under StackLock, from the callback
    // TuyaBridge::AddBridgedDeviceEndpoint invokes.
    //
    // The report guard is held across registration on purpose: registering a cluster runs
    // its Startup(), and several of them (LevelControlCluster, OnOffCluster) call their
    // delegates with the restored value. Without the guard, simply creating an endpoint
    // would push a spurious command to the Tuya Cloud.
    void InitClusters(EndpointId endpoint)
    {
        ApplyingReportScope scope(*this);
        DoInitClusters(endpoint);
    }

    // Unregister them. Also runs under StackLock.
    virtual void ShutdownClusters() = 0;

    // Cloud -> Matter. Default no-ops: a device only overrides what its clusters support,
    // so a mis-addressed report is ignored instead of crashing (no RTTI in this build).
    virtual void ApplyOnOff(bool) {}
    virtual void ApplyLevel(uint8_t) {}
    virtual void ApplyColorHs(uint8_t, uint8_t) {}
    virtual void ApplyColorTemp(uint16_t) {}
    virtual void ApplyContact(bool) {}
    virtual void ApplyOccupancy(bool) {}
    virtual void ApplyTemperature(int16_t) {}
    virtual void ApplyHumidity(uint16_t) {}
    virtual void ApplySetpoint(bool /* heating */, int16_t /* centiDegrees */) {}
    virtual void ApplySystemMode(uint8_t) {}
    virtual void ApplyFanMode(uint8_t) {}

    DataVersion mDataVersions[TuyaBridge::kBridgedDeviceClusterCount] = {};

protected:
    virtual void DoInitClusters(EndpointId endpoint) = 0;

    // While a cloud report is being written into a cluster, the cluster notifies our
    // delegates exactly as it would for a controller-driven change. Without this guard
    // every cloud state update would bounce straight back to the cloud as a command.
    bool IsApplyingReport() const { return mApplyingReport; }

    class ApplyingReportScope
    {
    public:
        explicit ApplyingReportScope(TuyaEndpoint & dev) : mDev(dev) { mDev.mApplyingReport = true; }
        ~ApplyingReportScope() { mDev.mApplyingReport = false; }

    private:
        TuyaEndpoint & mDev;
    };

private:
    void HandleDeviceChange(Device *, Device::Changed_t) override {}

    std::string mDni;
    hubm_device_ops mOps        = {};
    void * mCtx                 = nullptr;
    hubm_device_ops_ext mOpsExt = {};
    void * mCtxExt              = nullptr;
    bool mApplyingReport        = false;
};

// ---------------------------------------------------------------------------
// 5.1 — OnOff Plug-in Unit (0x010A): OnOff + BridgedDeviceBasicInformation
// ---------------------------------------------------------------------------
class TuyaOnOffEndpoint : public TuyaEndpoint, public OnOffDelegate
{
public:
    using TuyaEndpoint::TuyaEndpoint;

    void ShutdownClusters() override
    {
        if (mOnOff.IsConstructed())
        {
            mOnOff.Cluster().RemoveDelegate(this);
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mOnOff.Cluster()));
            mOnOff.Destroy();
        }
    }

    // OnOffDelegate — runs on the Matter thread under StackLock.
    void OnOffStartup(bool on) override {}
    void OnOnOffChanged(bool on) override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostOnOff(GetEndpointId(), on);
    }

    void ApplyOnOff(bool on) override
    {
        VerifyOrReturn(mOnOff.IsConstructed());
        ApplyingReportScope scope(*this);
        LogErrorOnFailure(mOnOff.Cluster().SetOnOff(on));
    }

protected:
    void DoInitClusters(EndpointId endpoint) override { InitOnOff(endpoint); }

    void InitOnOff(EndpointId endpoint)
    {
        mOnOff.Create(endpoint,
                      OnOffCluster::Context{ .timerDelegate = gTimerDelegate, .featureMap = {}, .defaults = { .onOff = false } });
        mOnOff.Cluster().AddDelegate(this);
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mOnOff.Registration()));
    }

    OnOffCluster & OnOff() { return mOnOff.Cluster(); }
    bool HasOnOff() const { return mOnOff.IsConstructed(); }

private:
    LazyRegisteredServerCluster<OnOffCluster> mOnOff;
};

// ---------------------------------------------------------------------------
// 5.2 — Dimmable Light (0x0100): OnOff + LevelControl
// ---------------------------------------------------------------------------
class TuyaDimmableEndpoint : public TuyaOnOffEndpoint, public LevelControlDelegate
{
public:
    using TuyaOnOffEndpoint::TuyaOnOffEndpoint;

    void ShutdownClusters() override
    {
        if (mLevel.IsConstructed())
        {
            // The level cluster is an OnOff delegate too (that is how On/Off restores the
            // previous brightness); unhook it before either object goes away.
            if (HasOnOff())
                OnOff().RemoveDelegate(&mLevel.Cluster());
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mLevel.Cluster()));
            mLevel.Destroy();
        }
        TuyaOnOffEndpoint::ShutdownClusters();
    }

    // LevelControlDelegate — Matter thread, under StackLock.
    void OnLevelChanged(uint8_t level) override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostLevel(GetEndpointId(), level);
    }

    void ApplyLevel(uint8_t level) override
    {
        VerifyOrReturn(mLevel.IsConstructed());
        ApplyingReportScope scope(*this);
        // ExecuteIfOff in both mask and override: a cloud brightness report must land even
        // when the light is currently off, otherwise the state the app shows drifts.
        const BitMask<LevelControl::OptionsBitmap> forceExecute(LevelControl::OptionsBitmap::kExecuteIfOff);
        mLevel.Cluster().MoveToLevel(std::clamp<uint8_t>(level, kMinLightingLevel, kMaxLevel),
                                     DataModel::NullNullable, forceExecute, forceExecute);
    }

protected:
    static constexpr uint8_t kMinLightingLevel = 1;
    static constexpr uint8_t kMaxLevel         = 254;

    void DoInitClusters(EndpointId endpoint) override
    {
        InitOnOff(endpoint);
        InitLevel(endpoint);
    }

    void InitLevel(EndpointId endpoint)
    {
        mLevel.Create(LevelControlCluster::Config(endpoint, gTimerDelegate, *this)
                          .WithOnOff(OnOff())
                          .WithLighting(DataModel::NullNullable)
                          .WithInitialCurrentLevel(kMaxLevel));
        // Couples On/Off to level per the spec (turning on restores the last level).
        OnOff().AddDelegate(&mLevel.Cluster());
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mLevel.Registration()));
    }

private:
    LazyRegisteredServerCluster<LevelControlCluster> mLevel;
};

// ---------------------------------------------------------------------------
// 5.3 — Extended Color Light (0x010D): OnOff + LevelControl + ColorControl
// ---------------------------------------------------------------------------
class TuyaColorLightEndpoint : public TuyaDimmableEndpoint, public TuyaClusters::ColorControlDelegate
{
public:
    using TuyaDimmableEndpoint::TuyaDimmableEndpoint;

    void ShutdownClusters() override
    {
        if (mColor.IsConstructed())
        {
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mColor.Cluster()));
            mColor.Destroy();
        }
        TuyaDimmableEndpoint::ShutdownClusters();
    }

    // ColorControlDelegate — Matter thread, under StackLock.
    void OnHueSaturationChanged(uint8_t hue, uint8_t saturation) override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostColorHs(GetEndpointId(), hue, saturation);
    }

    void OnColorTemperatureChanged(uint16_t mireds) override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostColorTemp(GetEndpointId(), mireds);
    }

    void ApplyColorHs(uint8_t hue, uint8_t saturation) override
    {
        VerifyOrReturn(mColor.IsConstructed());
        mColor.Cluster().ReportHueSaturation(hue, saturation);
    }

    void ApplyColorTemp(uint16_t mireds) override
    {
        VerifyOrReturn(mColor.IsConstructed());
        mColor.Cluster().ReportColorTemperature(mireds);
    }

protected:
    void DoInitClusters(EndpointId endpoint) override
    {
        TuyaDimmableEndpoint::DoInitClusters(endpoint);
        mColor.Create(endpoint, *this);
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mColor.Registration()));
    }

private:
    LazyRegisteredServerCluster<TuyaClusters::ColorControlCluster> mColor;
};

// ---------------------------------------------------------------------------
// 5.4 — Contact Sensor (0x0015): BooleanState
//
// StateValue follows the Matter device library: true = contact closed.
// ---------------------------------------------------------------------------
class TuyaContactSensorEndpoint : public TuyaEndpoint
{
public:
    using TuyaEndpoint::TuyaEndpoint;

    void ShutdownClusters() override
    {
        if (mBooleanState.IsConstructed())
        {
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mBooleanState.Cluster()));
            mBooleanState.Destroy();
        }
    }

    void ApplyContact(bool closed) override
    {
        VerifyOrReturn(mBooleanState.IsConstructed());
        // Also emits the BooleanState StateChange event when the value actually changes.
        mBooleanState.Cluster().SetStateValue(closed);
    }

protected:
    void DoInitClusters(EndpointId endpoint) override
    {
        mBooleanState.Create(endpoint);
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mBooleanState.Registration()));
    }

private:
    LazyRegisteredServerCluster<BooleanStateCluster> mBooleanState;
};

// ---------------------------------------------------------------------------
// 5.5 — Occupancy Sensor (0x0107): OccupancySensing
// ---------------------------------------------------------------------------
class TuyaOccupancySensorEndpoint : public TuyaEndpoint
{
public:
    using TuyaEndpoint::TuyaEndpoint;

    void ShutdownClusters() override
    {
        if (mOccupancy.IsConstructed())
        {
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mOccupancy.Cluster()));
            mOccupancy.Destroy();
        }
    }

    void ApplyOccupancy(bool occupied) override
    {
        VerifyOrReturn(mOccupancy.IsConstructed());
        mOccupancy.Cluster().SetOccupancy(occupied);
    }

protected:
    void DoInitClusters(EndpointId endpoint) override
    {
        // PIR is what a Tuya `pir` motion sensor is. HoldTime is deliberately not
        // configured: the cloud decides when motion clears and pushes that state, so a
        // local hold timer would only fight it.
        mOccupancy.Create(OccupancySensingCluster::Config(endpoint).WithFeatures(OccupancySensing::Feature::kPassiveInfrared));
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mOccupancy.Registration()));
    }

private:
    LazyRegisteredServerCluster<OccupancySensingCluster> mOccupancy;
};

// ---------------------------------------------------------------------------
// 5.6 — Temperature Sensor (0x0302), optionally + Humidity Sensor (0x0307)
// ---------------------------------------------------------------------------
class TuyaTemperatureSensorEndpoint : public TuyaEndpoint
{
public:
    TuyaTemperatureSensorEndpoint(const char * dni, const char * label, bool hasHumidity) :
        TuyaEndpoint(dni, label), mHasHumidity(hasHumidity)
    {}

    void ShutdownClusters() override
    {
        if (mHumidity.IsConstructed())
        {
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mHumidity.Cluster()));
            mHumidity.Destroy();
        }
        if (mTemperature.IsConstructed())
        {
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mTemperature.Cluster()));
            mTemperature.Destroy();
        }
    }

    void ApplyTemperature(int16_t centiDegrees) override
    {
        VerifyOrReturn(mTemperature.IsConstructed());
        // Clamp instead of letting SetMeasuredValue reject: a bogus cloud reading should
        // not leave the attribute stuck at its previous value with no indication.
        const int16_t clamped = std::clamp<int16_t>(centiDegrees, kMinTemperature, kMaxTemperature);
        LogErrorOnFailure(mTemperature.Cluster().SetMeasuredValue(DataModel::Nullable<int16_t>(clamped)));
    }

    void ApplyHumidity(uint16_t hundredthsPercent) override
    {
        VerifyOrReturn(mHumidity.IsConstructed());
        mHumidity.Cluster().ReportMeasuredValue(hundredthsPercent);
    }

protected:
    // -40.00 °C .. +125.00 °C: the range of the sensors Tuya ships.
    static constexpr int16_t kMinTemperature = -4000;
    static constexpr int16_t kMaxTemperature = 12500;

    void DoInitClusters(EndpointId endpoint) override
    {
        mTemperature.Create(endpoint, TemperatureMeasurementCluster::OptionalAttributeSet(),
                            TemperatureMeasurementCluster::StartupConfiguration{
                                .minMeasuredValue = DataModel::Nullable<int16_t>(kMinTemperature),
                                .maxMeasuredValue = DataModel::Nullable<int16_t>(kMaxTemperature),
                                .tolerance        = 0 });
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mTemperature.Registration()));

        if (mHasHumidity)
        {
            mHumidity.Create(endpoint);
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mHumidity.Registration()));
        }
    }

private:
    const bool mHasHumidity;
    LazyRegisteredServerCluster<TemperatureMeasurementCluster> mTemperature;
    LazyRegisteredServerCluster<TuyaClusters::RelativeHumidityCluster> mHumidity;
};

// ---------------------------------------------------------------------------
// 5.7 — Thermostat (0x0301)
// ---------------------------------------------------------------------------
class TuyaThermostatEndpoint : public TuyaEndpoint, public TuyaClusters::ThermostatDelegate
{
public:
    TuyaThermostatEndpoint(const char * dni, const char * label, bool supportsCooling) :
        TuyaEndpoint(dni, label), mSupportsCooling(supportsCooling)
    {}

    void ShutdownClusters() override
    {
        if (mThermostat.IsConstructed())
        {
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mThermostat.Cluster()));
            mThermostat.Destroy();
        }
    }

    // ThermostatDelegate — Matter thread, under StackLock.
    void OnSetpointChanged(bool heating, int16_t setpointCentiDegrees) override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostSetpoint(GetEndpointId(), heating, setpointCentiDegrees);
    }

    void OnSystemModeChanged(Thermostat::SystemModeEnum mode) override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostSystemMode(GetEndpointId(), static_cast<uint8_t>(mode));
        // Base-ops fallback for clients that never registered the extended ops.
        PostOnOff(GetEndpointId(), mode != Thermostat::SystemModeEnum::kOff);
    }

    // A thermostat's ambient reading arrives through the same report call a plain
    // temperature sensor uses.
    void ApplyTemperature(int16_t centiDegrees) override
    {
        VerifyOrReturn(mThermostat.IsConstructed());
        ApplyingReportScope scope(*this);
        mThermostat.Cluster().ReportLocalTemperature(centiDegrees);
    }

    // Cloud "on/off" for a thermostat means heat vs off; use hubm_report_system_mode to
    // report an exact mode (cool/auto) instead.
    void ApplyOnOff(bool on) override
    {
        VerifyOrReturn(mThermostat.IsConstructed());
        ApplyingReportScope scope(*this);
        mThermostat.Cluster().ReportSystemMode(on ? Thermostat::SystemModeEnum::kHeat : Thermostat::SystemModeEnum::kOff);
    }

    // hubm_report_level carries the heating setpoint in whole °C — the narrowest thing the
    // base report set can express. Clients that can be precise should use
    // hubm_report_setpoint (0.01 °C) instead.
    void ApplyLevel(uint8_t degreesCelsius) override
    {
        VerifyOrReturn(mThermostat.IsConstructed());
        ApplyingReportScope scope(*this);
        mThermostat.Cluster().ReportHeatingSetpoint(static_cast<int16_t>(degreesCelsius * 100));
    }

    void ApplySetpoint(bool heating, int16_t centiDegrees) override
    {
        VerifyOrReturn(mThermostat.IsConstructed());
        ApplyingReportScope scope(*this);
        if (heating)
            mThermostat.Cluster().ReportHeatingSetpoint(centiDegrees);
        else
            mThermostat.Cluster().ReportCoolingSetpoint(centiDegrees);
    }

    void ApplySystemMode(uint8_t systemMode) override
    {
        VerifyOrReturn(mThermostat.IsConstructed());
        ApplyingReportScope scope(*this);
        mThermostat.Cluster().ReportSystemMode(static_cast<Thermostat::SystemModeEnum>(systemMode));
    }

protected:
    void DoInitClusters(EndpointId endpoint) override
    {
        mThermostat.Create(endpoint, *this, mSupportsCooling);
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mThermostat.Registration()));
    }

private:
    const bool mSupportsCooling;
    LazyRegisteredServerCluster<TuyaClusters::ThermostatCluster> mThermostat;
};

// ---------------------------------------------------------------------------
// 5.8 — Window Covering (0x0202)
// ---------------------------------------------------------------------------
class TuyaWindowCoveringEndpoint : public TuyaEndpoint, public TuyaClusters::WindowCoveringDelegate
{
public:
    using TuyaEndpoint::TuyaEndpoint;

    void ShutdownClusters() override
    {
        if (mCovering.IsConstructed())
        {
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mCovering.Cluster()));
            mCovering.Destroy();
        }
    }

    // WindowCoveringDelegate — Matter thread, under StackLock.
    void OnLiftTargetChanged(uint8_t percent) override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostLiftPercent(GetEndpointId(), percent);
        PostLevel(GetEndpointId(), percent); // base-ops fallback
    }

    void OnStopRequested() override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostStop(GetEndpointId());
    }

    // Cloud position report: hubm_report_level(percent closed).
    void ApplyLevel(uint8_t percent) override
    {
        VerifyOrReturn(mCovering.IsConstructed());
        ApplyingReportScope scope(*this);
        mCovering.Cluster().ReportCurrentLiftPercent(percent);
    }

    // A curtain motor's plain on/off state maps to the two end positions.
    void ApplyOnOff(bool open) override
    {
        VerifyOrReturn(mCovering.IsConstructed());
        ApplyingReportScope scope(*this);
        mCovering.Cluster().ReportCurrentLiftPercent(open ? 0 : 100);
    }

protected:
    void DoInitClusters(EndpointId endpoint) override
    {
        mCovering.Create(endpoint, *this);
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mCovering.Registration()));
    }

private:
    LazyRegisteredServerCluster<TuyaClusters::WindowCoveringCluster> mCovering;
};

// ---------------------------------------------------------------------------
// 5.9 — Fan (0x002B)
// ---------------------------------------------------------------------------
class TuyaFanEndpoint : public TuyaEndpoint, public TuyaClusters::FanControlDelegate
{
public:
    using TuyaEndpoint::TuyaEndpoint;

    void ShutdownClusters() override
    {
        if (mFan.IsConstructed())
        {
            LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Unregister(&mFan.Cluster()));
            mFan.Destroy();
        }
    }

    // FanControlDelegate — Matter thread, under StackLock.
    void OnFanModeChanged(FanControl::FanModeEnum mode) override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostFanMode(GetEndpointId(), static_cast<uint8_t>(mode));
        PostOnOff(GetEndpointId(), mode != FanControl::FanModeEnum::kOff); // base-ops fallback
    }

    void OnPercentChanged(uint8_t percent) override
    {
        VerifyOrReturn(!IsApplyingReport());
        PostFanPercent(GetEndpointId(), percent);
        PostLevel(GetEndpointId(), percent); // base-ops fallback
    }

    void ApplyOnOff(bool on) override
    {
        VerifyOrReturn(mFan.IsConstructed());
        ApplyingReportScope scope(*this);
        mFan.Cluster().ReportFanMode(on ? FanControl::FanModeEnum::kHigh : FanControl::FanModeEnum::kOff);
    }

    void ApplyLevel(uint8_t percent) override
    {
        VerifyOrReturn(mFan.IsConstructed());
        ApplyingReportScope scope(*this);
        mFan.Cluster().ReportPercent(percent);
    }

    void ApplyFanMode(uint8_t fanMode) override
    {
        VerifyOrReturn(mFan.IsConstructed());
        ApplyingReportScope scope(*this);
        mFan.Cluster().ReportFanMode(static_cast<FanControl::FanModeEnum>(fanMode));
    }

protected:
    void DoInitClusters(EndpointId endpoint) override
    {
        mFan.Create(endpoint, *this);
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(mFan.Registration()));
    }

private:
    LazyRegisteredServerCluster<TuyaClusters::FanControlCluster> mFan;
};

// ---------------------------------------------------------------------------
// Device type tables. Every bridged device also carries Bridged Node (0x0013) so the
// controller renders it as a child of the Aggregator.
// ---------------------------------------------------------------------------
constexpr EmberAfDeviceType kOnOffPlugDeviceTypes[] = {
    { 0x010A /* ma_onoffpluginunit */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

constexpr EmberAfDeviceType kDimmableLightDeviceTypes[] = {
    { 0x0100 /* ma_dimmablelight */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

constexpr EmberAfDeviceType kColorLightDeviceTypes[] = {
    { 0x010D /* ma_extendedcolorlight */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

constexpr EmberAfDeviceType kContactSensorDeviceTypes[] = {
    { 0x0015 /* ma_contactsensor */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

constexpr EmberAfDeviceType kOccupancySensorDeviceTypes[] = {
    { 0x0107 /* ma_occupancysensor */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

constexpr EmberAfDeviceType kTemperatureSensorDeviceTypes[] = {
    { 0x0302 /* ma_tempsensor */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

// A Tuya temp+humidity sensor is one cloud device with two measurements, so it is bridged
// as one endpoint carrying both device types (the alternative — two endpoints — would show
// up as two unrelated things in the app).
constexpr EmberAfDeviceType kTemperatureHumiditySensorDeviceTypes[] = {
    { 0x0302 /* ma_tempsensor */, 1 },
    { 0x0307 /* ma_humiditysensor */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

constexpr EmberAfDeviceType kThermostatDeviceTypes[] = {
    { 0x0301 /* ma_thermostat */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

constexpr EmberAfDeviceType kWindowCoveringDeviceTypes[] = {
    { 0x0202 /* ma_windowcovering */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

constexpr EmberAfDeviceType kFanDeviceTypes[] = {
    { 0x002B /* ma_fan */, 1 },
    { 0x0013 /* ma_bridgeddevice */, 1 },
};

// ---------------------------------------------------------------------------
// List helpers. All of these REQUIRE gDevicesMutex to be held.
// ---------------------------------------------------------------------------
TuyaEndpoint * FindByEndpointLocked(hubm_endpoint_t ep)
{
    for (auto & dev : gTuyaDevices)
    {
        if (static_cast<hubm_endpoint_t>(dev->GetEndpointId()) == ep)
            return dev.get();
    }
    return nullptr;
}

bool RemoveLocked(TuyaEndpoint * dev)
{
    VerifyOrReturnValue(dev != nullptr, false);

    // Endpoint teardown first (clears the data-model entry and unregisters the clusters
    // under StackLock), then drop our owning reference.
    TuyaBridge::RemoveBridgedDeviceEndpoint(dev, [dev] { dev->ShutdownClusters(); });

    for (auto it = gTuyaDevices.begin(); it != gTuyaDevices.end(); ++it)
    {
        if (it->get() == dev)
        {
            gTuyaDevices.erase(it);
            return true;
        }
    }
    return false;
}

// Run `fn` against the device that owns `ep`, with both the device list and the Matter
// stack locked. Holding gDevicesMutex for the whole call is what makes this safe against
// a concurrent hubm_remove_*: the device cannot be freed underneath `fn`. Lock order is
// always gDevicesMutex -> StackLock; nothing in this file ever takes them the other way.
template <typename Fn>
void WithDevice(hubm_endpoint_t ep, Fn && fn)
{
    std::lock_guard<std::mutex> guard(gDevicesMutex);
    TuyaEndpoint * dev = FindByEndpointLocked(ep);
    VerifyOrReturn(dev != nullptr);
    StackLock lock;
    fn(*dev);
}

bool RemoveByDniLocked(const std::string & dni)
{
    VerifyOrReturnValue(!dni.empty(), false);
    for (auto & dev : gTuyaDevices)
    {
        if (dev->GetDni() == dni)
            return RemoveLocked(dev.get());
    }
    return false;
}

void DispatchLoop()
{
    for (;;)
    {
        PendingCommand cmd;
        {
            std::unique_lock<std::mutex> lock(gQueueMutex);
            gQueueCv.wait(lock, [] { return !gQueue.empty() || !gDispatchRunning; });
            if (!gDispatchRunning && gQueue.empty())
                return;
            cmd = gQueue.front();
            gQueue.pop_front();
        }

        // Resolve the ops under gDevicesMutex and COPY them out: the device may be removed
        // the instant we let go, so the C callback must never run off a live reference into
        // the list. StackLock is deliberately not held here — the callback is expected to
        // do a blocking cloud round-trip.
        hubm_device_ops ops         = {};
        void * ctx                  = nullptr;
        hubm_device_ops_ext opsExt  = {};
        void * ctxExt               = nullptr;
        {
            std::lock_guard<std::mutex> guard(gDevicesMutex);
            TuyaEndpoint * dev = FindByEndpointLocked(static_cast<hubm_endpoint_t>(cmd.endpoint));
            if (dev == nullptr)
                continue;
            ops    = dev->Ops();
            ctx    = dev->Ctx();
            opsExt = dev->OpsExt();
            ctxExt = dev->CtxExt();
        }

        const hubm_endpoint_t ep = static_cast<hubm_endpoint_t>(cmd.endpoint);
        switch (cmd.kind)
        {
        case CmdKind::kOnOff:
            if (ops.on_onoff != nullptr)
                ops.on_onoff(ctx, ep, cmd.flag);
            break;
        case CmdKind::kLevel:
            if (ops.on_level != nullptr)
                ops.on_level(ctx, ep, cmd.a);
            break;
        case CmdKind::kColorHs:
            if (ops.on_color_hs != nullptr)
                ops.on_color_hs(ctx, ep, cmd.a, cmd.b);
            break;
        case CmdKind::kColorTemp:
            if (ops.on_colortemp != nullptr)
                ops.on_colortemp(ctx, ep, cmd.u16);
            break;
        case CmdKind::kSetpoint:
            if (opsExt.on_setpoint != nullptr)
                opsExt.on_setpoint(ctxExt, ep, cmd.i16, cmd.flag ? 1 : 0);
            break;
        case CmdKind::kSystemMode:
            if (opsExt.on_system_mode != nullptr)
                opsExt.on_system_mode(ctxExt, ep, cmd.a);
            break;
        case CmdKind::kLiftPercent:
            if (opsExt.on_lift_percent != nullptr)
                opsExt.on_lift_percent(ctxExt, ep, cmd.a);
            break;
        case CmdKind::kStop:
            if (opsExt.on_stop != nullptr)
                opsExt.on_stop(ctxExt, ep);
            break;
        case CmdKind::kFanMode:
            if (opsExt.on_fan_mode != nullptr)
                opsExt.on_fan_mode(ctxExt, ep, cmd.a);
            break;
        case CmdKind::kFanPercent:
            if (opsExt.on_fan_percent != nullptr)
                opsExt.on_fan_percent(ctxExt, ep, cmd.a);
            break;
        }
    }
}

void EnsureDispatchThread()
{
    std::lock_guard<std::mutex> guard(gQueueMutex);
    if (gDispatchRunning)
        return;
    gDispatchRunning = true;
    gDispatchThread  = std::thread(DispatchLoop);
}

void StopDispatchThread()
{
    {
        std::lock_guard<std::mutex> guard(gQueueMutex);
        if (!gDispatchRunning)
            return;
        gDispatchRunning = false;
        gQueue.clear();
    }
    gQueueCv.notify_all();
    if (gDispatchThread.joinable())
        gDispatchThread.join();
}

// Common tail of every hubm_add_*: claim a slot + endpoint id, register the clusters and
// take ownership. Returns the endpoint id (>0) or -1.
hubm_endpoint_t AddDevice(std::unique_ptr<TuyaEndpoint> dev, Span<const EmberAfDeviceType> deviceTypes,
                          const hubm_device_ops * ops, void * ctx)
{
    VerifyOrReturnValue(dev != nullptr, -1);

    EnsureDispatchThread();
    dev->SetOps(ops, ctx);

    std::lock_guard<std::mutex> guard(gDevicesMutex);

    // Re-adding a known device (cloud re-sync, credential refresh) replaces the old
    // endpoint rather than creating a duplicate.
    RemoveByDniLocked(dev->GetDni());

    TuyaEndpoint * raw = dev.get();
    EndpointId endpoint =
        TuyaBridge::AddBridgedDeviceEndpoint(raw, Span<DataVersion>(raw->mDataVersions), deviceTypes,
                                             [raw](EndpointId assigned) { raw->InitClusters(assigned); });
    if (endpoint == kInvalidEndpointId)
    {
        ChipLogError(DeviceLayer, "TuyaBridge: failed to add endpoint for '%s'", raw->GetDni().c_str());
        return -1;
    }

    ChipLogProgress(DeviceLayer, "TuyaBridge: device '%s' (dni=%s) bridged at endpoint %u", raw->GetName(),
                    raw->GetDni().c_str(), endpoint);
    gTuyaDevices.push_back(std::move(dev));
    return static_cast<hubm_endpoint_t>(endpoint);
}

} // namespace

// ---------------------------------------------------------------------------
// TuyaBridge::Shutdown — called from ApplicationShutdown after tuya_runtime_stop().
// ---------------------------------------------------------------------------
namespace TuyaBridge {

void Shutdown()
{
    AnchorHubmApi();

    // The dispatch thread can call into the Tuya C layer; stop it before anything else so
    // no callback outlives the runtime, and so it cannot resurrect a device we just freed.
    // The join waits for an in-flight callback to return — that is why tuya_runtime_stop()
    // must have run first (it cancels the cloud I/O those callbacks perform).
    StopDispatchThread();

    std::lock_guard<std::mutex> guard(gDevicesMutex);
    while (!gTuyaDevices.empty())
    {
        RemoveLocked(gTuyaDevices.back().get());
    }
}

} // namespace TuyaBridge

// ---------------------------------------------------------------------------
// hubm_* C API
// ---------------------------------------------------------------------------
extern "C" {

// The Matter stack is already up in this process, so there is no node to start or stop:
// hubm_start returns a non-null sentinel and every other entry point ignores the handle.
hubm_node * hubm_start(void)
{
    return reinterpret_cast<hubm_node *>(&gNodeSentinel);
}

void hubm_stop(hubm_node *) {}

hubm_endpoint_t hubm_add_onoff(hubm_node *, const char * dni, const char * label, const hubm_device_ops * ops, void * ctx)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    return AddDevice(std::make_unique<TuyaOnOffEndpoint>(dni, label), Span<const EmberAfDeviceType>(kOnOffPlugDeviceTypes), ops,
                     ctx);
}

hubm_endpoint_t hubm_add_dimmable(hubm_node *, const char * dni, const char * label, const hubm_device_ops * ops, void * ctx)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    return AddDevice(std::make_unique<TuyaDimmableEndpoint>(dni, label), Span<const EmberAfDeviceType>(kDimmableLightDeviceTypes),
                     ops, ctx);
}

hubm_endpoint_t hubm_add_color_light(hubm_node *, const char * dni, const char * label, const hubm_device_ops * ops, void * ctx)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    return AddDevice(std::make_unique<TuyaColorLightEndpoint>(dni, label), Span<const EmberAfDeviceType>(kColorLightDeviceTypes),
                     ops, ctx);
}

hubm_endpoint_t hubm_add_contact_sensor(hubm_node *, const char * dni, const char * label)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    return AddDevice(std::make_unique<TuyaContactSensorEndpoint>(dni, label),
                     Span<const EmberAfDeviceType>(kContactSensorDeviceTypes), nullptr, nullptr);
}

hubm_endpoint_t hubm_add_occupancy_sensor(hubm_node *, const char * dni, const char * label)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    return AddDevice(std::make_unique<TuyaOccupancySensorEndpoint>(dni, label),
                     Span<const EmberAfDeviceType>(kOccupancySensorDeviceTypes), nullptr, nullptr);
}

hubm_endpoint_t hubm_add_temp_sensor(hubm_node *, const char * dni, const char * label, bool has_humidity)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    return AddDevice(std::make_unique<TuyaTemperatureSensorEndpoint>(dni, label, has_humidity),
                     has_humidity ? Span<const EmberAfDeviceType>(kTemperatureHumiditySensorDeviceTypes)
                                  : Span<const EmberAfDeviceType>(kTemperatureSensorDeviceTypes),
                     nullptr, nullptr);
}

hubm_endpoint_t hubm_add_thermostat(hubm_node *, const char * dni, const char * label, const hubm_device_ops * ops, void * ctx)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    // Heating-only by default: a Tuya `wk` thermostat is a radiator/underfloor controller.
    // Cooling-capable models can be surfaced by extending the C-side descriptor later.
    return AddDevice(std::make_unique<TuyaThermostatEndpoint>(dni, label, /* supportsCooling = */ false),
                     Span<const EmberAfDeviceType>(kThermostatDeviceTypes), ops, ctx);
}

hubm_endpoint_t hubm_add_window_covering(hubm_node *, const char * dni, const char * label, const hubm_device_ops * ops,
                                         void * ctx)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    return AddDevice(std::make_unique<TuyaWindowCoveringEndpoint>(dni, label),
                     Span<const EmberAfDeviceType>(kWindowCoveringDeviceTypes), ops, ctx);
}

hubm_endpoint_t hubm_add_fan(hubm_node *, const char * dni, const char * label, const hubm_device_ops * ops, void * ctx)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    return AddDevice(std::make_unique<TuyaFanEndpoint>(dni, label), Span<const EmberAfDeviceType>(kFanDeviceTypes), ops, ctx);
}

int hubm_set_device_ops_ext(int endpoint, const hubm_device_ops_ext * ops, void * ctx)
{
    std::lock_guard<std::mutex> guard(gDevicesMutex);
    TuyaEndpoint * dev = FindByEndpointLocked(static_cast<hubm_endpoint_t>(endpoint));
    VerifyOrReturnValue(dev != nullptr, -1);
    dev->SetOpsExt(ops, ctx);
    return 0;
}

int hubm_remove_endpoint(hubm_node *, hubm_endpoint_t ep)
{
    std::lock_guard<std::mutex> guard(gDevicesMutex);
    TuyaEndpoint * dev = FindByEndpointLocked(ep);
    VerifyOrReturnValue(dev != nullptr, -1);
    ChipLogProgress(DeviceLayer, "TuyaBridge: removing endpoint %d (dni=%s)", ep, dev->GetDni().c_str());
    return RemoveLocked(dev) ? 0 : -1;
}

int hubm_remove_tuya_device(hubm_node *, const char * dni)
{
    VerifyOrReturnValue(dni != nullptr && dni[0] != '\0', -1);
    std::lock_guard<std::mutex> guard(gDevicesMutex);
    ChipLogProgress(DeviceLayer, "TuyaBridge: removing device dni=%s", dni);
    return RemoveByDniLocked(dni) ? 0 : -1;
}

void hubm_report_onoff(hubm_node *, hubm_endpoint_t ep, bool on)
{
    WithDevice(ep, [on](TuyaEndpoint & dev) { dev.ApplyOnOff(on); });
}

void hubm_report_level(hubm_node *, hubm_endpoint_t ep, uint8_t level)
{
    WithDevice(ep, [level](TuyaEndpoint & dev) { dev.ApplyLevel(level); });
}

void hubm_report_color_hs(hubm_node *, hubm_endpoint_t ep, uint8_t hue, uint8_t sat)
{
    WithDevice(ep, [hue, sat](TuyaEndpoint & dev) { dev.ApplyColorHs(hue, sat); });
}

void hubm_report_colortemp(hubm_node *, hubm_endpoint_t ep, uint16_t mireds)
{
    WithDevice(ep, [mireds](TuyaEndpoint & dev) { dev.ApplyColorTemp(mireds); });
}

void hubm_report_contact(hubm_node *, hubm_endpoint_t ep, bool closed)
{
    WithDevice(ep, [closed](TuyaEndpoint & dev) { dev.ApplyContact(closed); });
}

void hubm_report_temperature(hubm_node *, hubm_endpoint_t ep, int16_t centideg)
{
    WithDevice(ep, [centideg](TuyaEndpoint & dev) { dev.ApplyTemperature(centideg); });
}

void hubm_report_humidity(hubm_node *, hubm_endpoint_t ep, uint16_t hundredths_pct)
{
    WithDevice(ep, [hundredths_pct](TuyaEndpoint & dev) { dev.ApplyHumidity(hundredths_pct); });
}

// Declared in the camera half of the glue header (OccupancySensing motion), but a Tuya
// PIR sensor reports through exactly the same cluster, so one implementation serves both.
void hubm_report_motion(hubm_node *, hubm_endpoint_t ep, bool active)
{
    WithDevice(ep, [active](TuyaEndpoint & dev) { dev.ApplyOccupancy(active); });
}

// Extended reports (tuya-bridge-endpoints.h), for state the base report set cannot carry.
void hubm_report_setpoint(int endpoint, int16_t centideg, int heating)
{
    WithDevice(static_cast<hubm_endpoint_t>(endpoint),
               [centideg, heating](TuyaEndpoint & dev) { dev.ApplySetpoint(heating != 0, centideg); });
}

void hubm_report_system_mode(int endpoint, uint8_t system_mode)
{
    WithDevice(static_cast<hubm_endpoint_t>(endpoint), [system_mode](TuyaEndpoint & dev) { dev.ApplySystemMode(system_mode); });
}

void hubm_report_fan_mode(int endpoint, uint8_t fan_mode)
{
    WithDevice(static_cast<hubm_endpoint_t>(endpoint), [fan_mode](TuyaEndpoint & dev) { dev.ApplyFanMode(fan_mode); });
}

} // extern "C"

namespace {

// The bridge links with -ffunction-sections/--gc-sections, and the Tuya C layer does not
// call the hubm_* API yet, so without a reference from something the binary does keep the
// linker garbage-collects every entry point above — the API would be silently absent from
// the shipped binary (and from `nm`). Taking their addresses from TuyaBridge::Shutdown(),
// which camera-bridge-main.cpp always calls, pins them. Remove this once the Tuya runtime
// has real call sites.
void AnchorHubmApi()
{
    static const void * const kAnchors[] = {
        reinterpret_cast<const void *>(&hubm_start),
        reinterpret_cast<const void *>(&hubm_stop),
        reinterpret_cast<const void *>(&hubm_add_onoff),
        reinterpret_cast<const void *>(&hubm_add_dimmable),
        reinterpret_cast<const void *>(&hubm_add_color_light),
        reinterpret_cast<const void *>(&hubm_add_contact_sensor),
        reinterpret_cast<const void *>(&hubm_add_occupancy_sensor),
        reinterpret_cast<const void *>(&hubm_add_temp_sensor),
        reinterpret_cast<const void *>(&hubm_add_thermostat),
        reinterpret_cast<const void *>(&hubm_add_window_covering),
        reinterpret_cast<const void *>(&hubm_add_fan),
        reinterpret_cast<const void *>(&hubm_set_device_ops_ext),
        reinterpret_cast<const void *>(&hubm_remove_endpoint),
        reinterpret_cast<const void *>(&hubm_remove_tuya_device),
        reinterpret_cast<const void *>(&hubm_report_onoff),
        reinterpret_cast<const void *>(&hubm_report_level),
        reinterpret_cast<const void *>(&hubm_report_color_hs),
        reinterpret_cast<const void *>(&hubm_report_colortemp),
        reinterpret_cast<const void *>(&hubm_report_contact),
        reinterpret_cast<const void *>(&hubm_report_temperature),
        reinterpret_cast<const void *>(&hubm_report_humidity),
        reinterpret_cast<const void *>(&hubm_report_motion),
        reinterpret_cast<const void *>(&hubm_report_setpoint),
        reinterpret_cast<const void *>(&hubm_report_system_mode),
        reinterpret_cast<const void *>(&hubm_report_fan_mode),
    };

    // The store must be to a volatile, or the optimiser deletes the whole loop (and with
    // it the only reference keeping these functions alive).
    static const void * volatile sKeepAlive = nullptr;
    for (const void * anchor : kAnchors)
    {
        sKeepAlive = anchor;
        (void) sKeepAlive;
    }
}

} // namespace


#endif // HAVE_TUYA

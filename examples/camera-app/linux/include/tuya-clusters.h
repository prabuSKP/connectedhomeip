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

// Cluster servers for the Tuya bridged endpoints that upstream does not provide in a
// form this binary can use.
//
// Why these exist: this bridge serves every functional cluster from the server-cluster
// REGISTRY on endpoints created with emberAfSetDynamicEndpoint, because the camera-app
// ZAP data model contains none of the clusters a Tuya device needs. Upstream's OnOff,
// LevelControl, BooleanState, OccupancySensing and TemperatureMeasurement servers are
// already code-driven `ServerCluster`s and are used as-is. The remaining four —
// ColorControl, Thermostat, WindowCovering, FanControl — are still ember/ZAP-bound
// (emberAfGetClusterServerEndpointIndex + generated attribute storage + a
// MATTER_DM_*_CLUSTER_SERVER_ENDPOINT_COUNT delegate table), so they cannot serve a
// dynamic endpoint here; RelativeHumidityMeasurement has no server implementation at
// all. Those five are implemented below directly against ServerClusterInterface.
//
// Deliberate simplification, applies to all of them: transitions are INSTANT. Every
// transitionTime/rate argument is accepted (so the command list stays spec-shaped) but
// the new value is applied immediately, because the backing Tuya Cloud API is set-point
// only and has no notion of a ramp. RemainingTime is therefore not advertised.

#pragma once

#ifdef HAVE_TUYA

#include <app/server-cluster/DefaultServerCluster.h>
#include <clusters/ColorControl/Enums.h>
#include <clusters/ColorControl/Ids.h>
#include <clusters/FanControl/Enums.h>
#include <clusters/FanControl/Ids.h>
#include <clusters/RelativeHumidityMeasurement/Ids.h>
#include <clusters/Thermostat/Enums.h>
#include <clusters/Thermostat/Ids.h>
#include <clusters/WindowCovering/Enums.h>
#include <clusters/WindowCovering/Ids.h>
#include <lib/core/DataModelTypes.h>

namespace TuyaClusters {

// ---------------------------------------------------------------------------
// ColorControl (0x0300) — HueAndSaturation + ColorTemperature features.
// ---------------------------------------------------------------------------
class ColorControlDelegate
{
public:
    virtual ~ColorControlDelegate() = default;
    virtual void OnHueSaturationChanged(uint8_t hue, uint8_t saturation) = 0;
    virtual void OnColorTemperatureChanged(uint16_t mireds)              = 0;
};

class ColorControlCluster : public chip::app::DefaultServerCluster
{
public:
    // Mired bounds advertised as the physical limits. 153 mired = 6535 K (cool),
    // 500 mired = 2000 K (warm) — the range virtually every tunable-white bulb covers.
    static constexpr uint16_t kPhysicalMinMireds = 153;
    static constexpr uint16_t kPhysicalMaxMireds = 500;
    static constexpr uint8_t kMaxHue             = 254;
    static constexpr uint8_t kMaxSaturation      = 254;

    ColorControlCluster(chip::EndpointId endpointId, ColorControlDelegate & delegate);

    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest & request,
                                                           chip::app::AttributeValueEncoder & encoder) override;
    chip::app::DataModel::ActionReturnStatus WriteAttribute(const chip::app::DataModel::WriteAttributeRequest & request,
                                                            chip::app::AttributeValueDecoder & decoder) override;
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath & path,
                          chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> & builder) override;
    CHIP_ERROR AcceptedCommands(const chip::app::ConcreteClusterPath & path,
                                chip::ReadOnlyBufferBuilder<chip::app::DataModel::AcceptedCommandEntry> & builder) override;
    std::optional<chip::app::DataModel::ActionReturnStatus> InvokeCommand(const chip::app::DataModel::InvokeRequest & request,
                                                                          chip::TLV::TLVReader & input_arguments,
                                                                          chip::app::CommandHandler * handler) override;

    // Cloud -> Matter. Updates the attributes without calling the delegate back.
    void ReportHueSaturation(uint8_t hue, uint8_t saturation);
    void ReportColorTemperature(uint16_t mireds);

    uint8_t GetCurrentHue() const { return mCurrentHue; }
    uint8_t GetCurrentSaturation() const { return mCurrentSaturation; }
    uint16_t GetColorTemperature() const { return mColorTemperatureMireds; }

private:
    // Applies + notifies, honouring mSuppressDelegate.
    void ApplyHueSaturation(uint8_t hue, uint8_t saturation);
    void ApplyColorTemperature(uint16_t mireds);

    ColorControlDelegate & mDelegate;
    uint8_t mCurrentHue           = 0;
    uint8_t mCurrentSaturation    = 0;
    uint16_t mColorTemperatureMireds = 250; // 4000 K
    chip::app::Clusters::ColorControl::ColorModeEnum mColorMode =
        chip::app::Clusters::ColorControl::ColorModeEnum::kCurrentHueAndCurrentSaturation;
    chip::app::Clusters::ColorControl::EnhancedColorModeEnum mEnhancedColorMode =
        chip::app::Clusters::ColorControl::EnhancedColorModeEnum::kCurrentHueAndCurrentSaturation;
    chip::BitMask<chip::app::Clusters::ColorControl::OptionsBitmap> mOptions;
    bool mSuppressDelegate = false;
};

// ---------------------------------------------------------------------------
// RelativeHumidityMeasurement (0x0405) — attributes only, no commands.
// ---------------------------------------------------------------------------
class RelativeHumidityCluster : public chip::app::DefaultServerCluster
{
public:
    explicit RelativeHumidityCluster(chip::EndpointId endpointId);

    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest & request,
                                                           chip::app::AttributeValueEncoder & encoder) override;
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath & path,
                          chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> & builder) override;

    // value is in 0.01 %RH, i.e. 5000 == 50.00 %RH. Out-of-range values are clamped.
    void ReportMeasuredValue(uint16_t hundredthsPercent);

private:
    static constexpr uint16_t kMinMeasuredValue = 0;
    static constexpr uint16_t kMaxMeasuredValue = 10000;

    chip::app::DataModel::Nullable<uint16_t> mMeasuredValue;
};

// ---------------------------------------------------------------------------
// Thermostat (0x0201) — Heating (+ optional Cooling) features.
// ---------------------------------------------------------------------------
class ThermostatDelegate
{
public:
    virtual ~ThermostatDelegate() = default;
    // setpoint in 0.01 °C, `heating` tells which setpoint changed.
    virtual void OnSetpointChanged(bool heating, int16_t setpointCentiDegrees) = 0;
    virtual void OnSystemModeChanged(chip::app::Clusters::Thermostat::SystemModeEnum mode) = 0;
};

class ThermostatCluster : public chip::app::DefaultServerCluster
{
public:
    static constexpr int16_t kAbsMinHeatSetpoint = 500;  //  5.00 °C
    static constexpr int16_t kAbsMaxHeatSetpoint = 3000; // 30.00 °C
    static constexpr int16_t kAbsMinCoolSetpoint = 1600; // 16.00 °C
    static constexpr int16_t kAbsMaxCoolSetpoint = 3200; // 32.00 °C

    ThermostatCluster(chip::EndpointId endpointId, ThermostatDelegate & delegate, bool supportsCooling);

    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest & request,
                                                           chip::app::AttributeValueEncoder & encoder) override;
    chip::app::DataModel::ActionReturnStatus WriteAttribute(const chip::app::DataModel::WriteAttributeRequest & request,
                                                            chip::app::AttributeValueDecoder & decoder) override;
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath & path,
                          chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> & builder) override;
    CHIP_ERROR AcceptedCommands(const chip::app::ConcreteClusterPath & path,
                                chip::ReadOnlyBufferBuilder<chip::app::DataModel::AcceptedCommandEntry> & builder) override;
    std::optional<chip::app::DataModel::ActionReturnStatus> InvokeCommand(const chip::app::DataModel::InvokeRequest & request,
                                                                          chip::TLV::TLVReader & input_arguments,
                                                                          chip::app::CommandHandler * handler) override;

    // Cloud -> Matter.
    void ReportLocalTemperature(int16_t centiDegrees);
    void ReportHeatingSetpoint(int16_t centiDegrees);
    void ReportCoolingSetpoint(int16_t centiDegrees);
    void ReportSystemMode(chip::app::Clusters::Thermostat::SystemModeEnum mode);

private:
    chip::app::DataModel::ActionReturnStatus SetHeatingSetpoint(int16_t value, bool fromCloud);
    chip::app::DataModel::ActionReturnStatus SetCoolingSetpoint(int16_t value, bool fromCloud);

    ThermostatDelegate & mDelegate;
    const bool mSupportsCooling;
    chip::app::DataModel::Nullable<int16_t> mLocalTemperature;
    int16_t mOccupiedHeatingSetpoint = 2000; // 20.00 °C
    int16_t mOccupiedCoolingSetpoint = 2600; // 26.00 °C
    chip::app::Clusters::Thermostat::SystemModeEnum mSystemMode = chip::app::Clusters::Thermostat::SystemModeEnum::kOff;
};

// ---------------------------------------------------------------------------
// WindowCovering (0x0102) — Lift + PositionAwareLift.
// ---------------------------------------------------------------------------
class WindowCoveringDelegate
{
public:
    virtual ~WindowCoveringDelegate() = default;
    // 0 % = fully open, 100 % = fully closed (Matter's lift-percentage convention).
    virtual void OnLiftTargetChanged(uint8_t percent) = 0;
    virtual void OnStopRequested()                    = 0;
};

class WindowCoveringCluster : public chip::app::DefaultServerCluster
{
public:
    WindowCoveringCluster(chip::EndpointId endpointId, WindowCoveringDelegate & delegate);

    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest & request,
                                                           chip::app::AttributeValueEncoder & encoder) override;
    chip::app::DataModel::ActionReturnStatus WriteAttribute(const chip::app::DataModel::WriteAttributeRequest & request,
                                                            chip::app::AttributeValueDecoder & decoder) override;
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath & path,
                          chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> & builder) override;
    CHIP_ERROR AcceptedCommands(const chip::app::ConcreteClusterPath & path,
                                chip::ReadOnlyBufferBuilder<chip::app::DataModel::AcceptedCommandEntry> & builder) override;
    std::optional<chip::app::DataModel::ActionReturnStatus> InvokeCommand(const chip::app::DataModel::InvokeRequest & request,
                                                                          chip::TLV::TLVReader & input_arguments,
                                                                          chip::app::CommandHandler * handler) override;

    // Cloud -> Matter: the covering reached/reports this position (0 % open .. 100 % closed).
    void ReportCurrentLiftPercent(uint8_t percent);

private:
    chip::app::DataModel::ActionReturnStatus GoToLiftPercent(uint8_t percent);

    WindowCoveringDelegate & mDelegate;
    chip::app::DataModel::Nullable<uint8_t> mCurrentPositionLiftPercentage;
    chip::app::DataModel::Nullable<uint16_t> mCurrentPositionLiftPercent100ths;
    chip::app::DataModel::Nullable<uint16_t> mTargetPositionLiftPercent100ths;
    chip::BitMask<chip::app::Clusters::WindowCovering::Mode> mMode;
};

// ---------------------------------------------------------------------------
// FanControl (cluster 0x0202; the ma_fan DEVICE TYPE is 0x002B, used in
// kFanDeviceTypes in tuya-bridge-endpoints.cpp — not this cluster) — MultiSpeed + Step.
// ---------------------------------------------------------------------------
class FanControlDelegate
{
public:
    virtual ~FanControlDelegate() = default;
    virtual void OnFanModeChanged(chip::app::Clusters::FanControl::FanModeEnum mode) = 0;
    virtual void OnPercentChanged(uint8_t percent)                                   = 0;
};

class FanControlCluster : public chip::app::DefaultServerCluster
{
public:
    static constexpr uint8_t kSpeedMax = 3;

    FanControlCluster(chip::EndpointId endpointId, FanControlDelegate & delegate);

    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest & request,
                                                           chip::app::AttributeValueEncoder & encoder) override;
    chip::app::DataModel::ActionReturnStatus WriteAttribute(const chip::app::DataModel::WriteAttributeRequest & request,
                                                            chip::app::AttributeValueDecoder & decoder) override;
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath & path,
                          chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> & builder) override;
    CHIP_ERROR AcceptedCommands(const chip::app::ConcreteClusterPath & path,
                                chip::ReadOnlyBufferBuilder<chip::app::DataModel::AcceptedCommandEntry> & builder) override;
    std::optional<chip::app::DataModel::ActionReturnStatus> InvokeCommand(const chip::app::DataModel::InvokeRequest & request,
                                                                          chip::TLV::TLVReader & input_arguments,
                                                                          chip::app::CommandHandler * handler) override;

    // Cloud -> Matter.
    void ReportFanMode(chip::app::Clusters::FanControl::FanModeEnum mode);
    void ReportPercent(uint8_t percent);

private:
    // Keeps FanMode/PercentSetting/SpeedSetting consistent, as the spec requires.
    void ApplyFanMode(chip::app::Clusters::FanControl::FanModeEnum mode, bool fromCloud);
    void ApplyPercent(uint8_t percent, bool fromCloud);

    FanControlDelegate & mDelegate;
    chip::app::Clusters::FanControl::FanModeEnum mFanMode = chip::app::Clusters::FanControl::FanModeEnum::kOff;
    uint8_t mPercentSetting                               = 0;
    uint8_t mSpeedSetting                                 = 0;
};

} // namespace TuyaClusters

#endif // HAVE_TUYA

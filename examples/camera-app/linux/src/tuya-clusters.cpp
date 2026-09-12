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

// See tuya-clusters.h for why these cluster servers are implemented here instead of
// being reused from src/app/clusters.

#ifdef HAVE_TUYA

#include "tuya-clusters.h"

#include <app/AttributeValueDecoder.h>
#include <app/AttributeValueEncoder.h>
#include <app/data-model/Decode.h>
#include <app/server-cluster/AttributeListBuilder.h>
#include <clusters/ColorControl/Attributes.h>
#include <clusters/ColorControl/Commands.h>
#include <clusters/ColorControl/Metadata.h>
#include <clusters/FanControl/Attributes.h>
#include <clusters/FanControl/Commands.h>
#include <clusters/FanControl/Metadata.h>
#include <clusters/RelativeHumidityMeasurement/Attributes.h>
#include <clusters/RelativeHumidityMeasurement/Metadata.h>
#include <clusters/Thermostat/Attributes.h>
#include <clusters/Thermostat/Commands.h>
#include <clusters/Thermostat/Metadata.h>
#include <clusters/WindowCovering/Attributes.h>
#include <clusters/WindowCovering/Commands.h>
#include <clusters/WindowCovering/Metadata.h>
#include <lib/support/CodeUtils.h>

#include <algorithm>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using chip::Protocols::InteractionModel::Status;

namespace TuyaClusters {

namespace {

// Hue is a 0..254 ring: stepping past either end wraps around.
uint8_t WrapHue(int32_t hue)
{
    constexpr int32_t kRing = ColorControlCluster::kMaxHue + 1; // 255 distinct values
    hue %= kRing;
    if (hue < 0)
        hue += kRing;
    return static_cast<uint8_t>(hue);
}

uint8_t ClampSaturation(int32_t saturation)
{
    return static_cast<uint8_t>(std::clamp<int32_t>(saturation, 0, ColorControlCluster::kMaxSaturation));
}

uint16_t ClampMireds(int32_t mireds)
{
    return static_cast<uint16_t>(
        std::clamp<int32_t>(mireds, ColorControlCluster::kPhysicalMinMireds, ColorControlCluster::kPhysicalMaxMireds));
}

} // namespace

// ---------------------------------------------------------------------------
// ColorControl
// ---------------------------------------------------------------------------

ColorControlCluster::ColorControlCluster(EndpointId endpointId, ColorControlDelegate & delegate) :
    DefaultServerCluster({ endpointId, ColorControl::Id }), mDelegate(delegate)
{}

DataModel::ActionReturnStatus ColorControlCluster::ReadAttribute(const DataModel::ReadAttributeRequest & request,
                                                                 AttributeValueEncoder & encoder)
{
    using namespace ColorControl::Attributes;

    switch (request.path.mAttributeId)
    {
    case ClusterRevision::Id:
        return encoder.Encode(ColorControl::kRevision);
    case FeatureMap::Id:
        return encoder.Encode<uint32_t>(
            BitMask<ColorControl::Feature>(ColorControl::Feature::kHueAndSaturation, ColorControl::Feature::kColorTemperature)
                .Raw());
    case CurrentHue::Id:
        return encoder.Encode(mCurrentHue);
    case CurrentSaturation::Id:
        return encoder.Encode(mCurrentSaturation);
    case ColorMode::Id:
        return encoder.Encode(mColorMode);
    case EnhancedColorMode::Id:
        return encoder.Encode(mEnhancedColorMode);
    case Options::Id:
        return encoder.Encode(mOptions);
    case NumberOfPrimaries::Id:
        // Nullable: we do not model the physical primaries of a cloud bulb.
        return encoder.EncodeNull();
    case ColorCapabilities::Id:
        return encoder.Encode<uint16_t>(
            BitMask<ColorControl::ColorCapabilitiesBitmap>(ColorControl::ColorCapabilitiesBitmap::kHueSaturation,
                                                           ColorControl::ColorCapabilitiesBitmap::kColorTemperature)
                .Raw());
    case ColorTemperatureMireds::Id:
        return encoder.Encode(mColorTemperatureMireds);
    case ColorTempPhysicalMinMireds::Id:
        return encoder.Encode<uint16_t>(kPhysicalMinMireds);
    case ColorTempPhysicalMaxMireds::Id:
        return encoder.Encode<uint16_t>(kPhysicalMaxMireds);
    default:
        return Status::UnsupportedAttribute;
    }
}

DataModel::ActionReturnStatus ColorControlCluster::WriteAttribute(const DataModel::WriteAttributeRequest & request,
                                                                  AttributeValueDecoder & decoder)
{
    if (request.path.mAttributeId == ColorControl::Attributes::Options::Id)
    {
        BitMask<ColorControl::OptionsBitmap> options;
        ReturnErrorOnFailure(decoder.Decode(options));
        SetAttributeValue(mOptions, options, ColorControl::Attributes::Options::Id);
        return Status::Success;
    }
    return DefaultServerCluster::WriteAttribute(request, decoder);
}

CHIP_ERROR ColorControlCluster::Attributes(const ConcreteClusterPath & path,
                                           ReadOnlyBufferBuilder<DataModel::AttributeEntry> & builder)
{
    using namespace ColorControl::Attributes;

    // Everything this implementation serves: the unconditionally-mandatory set plus the
    // attributes made mandatory by the HS and CT features.
    const DataModel::AttributeEntry kServed[] = {
        ColorMode::kMetadataEntry,
        Options::kMetadataEntry,
        NumberOfPrimaries::kMetadataEntry,
        EnhancedColorMode::kMetadataEntry,
        ColorCapabilities::kMetadataEntry,
        CurrentHue::kMetadataEntry,
        CurrentSaturation::kMetadataEntry,
        ColorTemperatureMireds::kMetadataEntry,
        ColorTempPhysicalMinMireds::kMetadataEntry,
        ColorTempPhysicalMaxMireds::kMetadataEntry,
    };

    AttributeListBuilder listBuilder(builder);
    return listBuilder.Append(Span<const DataModel::AttributeEntry>(kServed), {});
}

CHIP_ERROR ColorControlCluster::AcceptedCommands(const ConcreteClusterPath & path,
                                                 ReadOnlyBufferBuilder<DataModel::AcceptedCommandEntry> & builder)
{
    using namespace ColorControl::Commands;

    static constexpr DataModel::AcceptedCommandEntry kCommands[] = {
        MoveToHue::kMetadataEntry,
        MoveHue::kMetadataEntry,
        StepHue::kMetadataEntry,
        MoveToSaturation::kMetadataEntry,
        MoveSaturation::kMetadataEntry,
        StepSaturation::kMetadataEntry,
        MoveToHueAndSaturation::kMetadataEntry,
        StopMoveStep::kMetadataEntry,
        MoveToColorTemperature::kMetadataEntry,
        MoveColorTemperature::kMetadataEntry,
        StepColorTemperature::kMetadataEntry,
    };
    return builder.ReferenceExisting(kCommands);
}

std::optional<DataModel::ActionReturnStatus> ColorControlCluster::InvokeCommand(const DataModel::InvokeRequest & request,
                                                                                TLV::TLVReader & input_arguments,
                                                                                CommandHandler * handler)
{
    using namespace ColorControl::Commands;
    using ColorControl::MoveModeEnum;
    using ColorControl::StepModeEnum;

    switch (request.path.mCommandId)
    {
    case MoveToHue::Id: {
        MoveToHue::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(data.hue <= kMaxHue, Status::ConstraintError);
        ApplyHueSaturation(data.hue, mCurrentSaturation);
        return Status::Success;
    }
    case MoveToSaturation::Id: {
        MoveToSaturation::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(data.saturation <= kMaxSaturation, Status::ConstraintError);
        ApplyHueSaturation(mCurrentHue, data.saturation);
        return Status::Success;
    }
    case MoveToHueAndSaturation::Id: {
        MoveToHueAndSaturation::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(data.hue <= kMaxHue && data.saturation <= kMaxSaturation, Status::ConstraintError);
        ApplyHueSaturation(data.hue, data.saturation);
        return Status::Success;
    }
    case MoveHue::Id: {
        // No ramping: "start moving" resolves to "go to the end of the ring in that
        // direction", "stop" is a no-op because nothing is in flight.
        MoveHue::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        if (data.moveMode == MoveModeEnum::kStop)
            return Status::Success;
        VerifyOrReturnError(data.moveMode == MoveModeEnum::kUp || data.moveMode == MoveModeEnum::kDown, Status::InvalidCommand);
        ApplyHueSaturation(WrapHue(mCurrentHue + (data.moveMode == MoveModeEnum::kUp ? data.rate : -data.rate)),
                           mCurrentSaturation);
        return Status::Success;
    }
    case StepHue::Id: {
        StepHue::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(data.stepMode == StepModeEnum::kUp || data.stepMode == StepModeEnum::kDown, Status::InvalidCommand);
        ApplyHueSaturation(WrapHue(mCurrentHue + (data.stepMode == StepModeEnum::kUp ? data.stepSize : -data.stepSize)),
                           mCurrentSaturation);
        return Status::Success;
    }
    case MoveSaturation::Id: {
        MoveSaturation::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        if (data.moveMode == MoveModeEnum::kStop)
            return Status::Success;
        VerifyOrReturnError(data.moveMode == MoveModeEnum::kUp || data.moveMode == MoveModeEnum::kDown, Status::InvalidCommand);
        ApplyHueSaturation(mCurrentHue, data.moveMode == MoveModeEnum::kUp ? kMaxSaturation : 0);
        return Status::Success;
    }
    case StepSaturation::Id: {
        StepSaturation::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(data.stepMode == StepModeEnum::kUp || data.stepMode == StepModeEnum::kDown, Status::InvalidCommand);
        ApplyHueSaturation(mCurrentHue,
                           ClampSaturation(mCurrentSaturation +
                                           (data.stepMode == StepModeEnum::kUp ? data.stepSize : -data.stepSize)));
        return Status::Success;
    }
    case MoveToColorTemperature::Id: {
        MoveToColorTemperature::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        ApplyColorTemperature(ClampMireds(data.colorTemperatureMireds));
        return Status::Success;
    }
    case MoveColorTemperature::Id: {
        MoveColorTemperature::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        if (data.moveMode == MoveModeEnum::kStop)
            return Status::Success;
        VerifyOrReturnError(data.moveMode == MoveModeEnum::kUp || data.moveMode == MoveModeEnum::kDown, Status::InvalidCommand);
        const int32_t bound = (data.moveMode == MoveModeEnum::kUp)
            ? (data.colorTemperatureMaximumMireds != 0 ? data.colorTemperatureMaximumMireds : kPhysicalMaxMireds)
            : (data.colorTemperatureMinimumMireds != 0 ? data.colorTemperatureMinimumMireds : kPhysicalMinMireds);
        ApplyColorTemperature(ClampMireds(bound));
        return Status::Success;
    }
    case StepColorTemperature::Id: {
        StepColorTemperature::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(data.stepMode == StepModeEnum::kUp || data.stepMode == StepModeEnum::kDown, Status::InvalidCommand);
        const int32_t stepped =
            mColorTemperatureMireds + (data.stepMode == StepModeEnum::kUp ? data.stepSize : -data.stepSize);
        ApplyColorTemperature(ClampMireds(stepped));
        return Status::Success;
    }
    case StopMoveStep::Id:
        // Transitions are instant, so there is never anything to stop.
        return Status::Success;
    default:
        return Status::UnsupportedCommand;
    }
}

void ColorControlCluster::ApplyHueSaturation(uint8_t hue, uint8_t saturation)
{
    SetAttributeValue(mCurrentHue, hue, ColorControl::Attributes::CurrentHue::Id);
    SetAttributeValue(mCurrentSaturation, saturation, ColorControl::Attributes::CurrentSaturation::Id);
    SetAttributeValue(mColorMode, ColorControl::ColorModeEnum::kCurrentHueAndCurrentSaturation,
                      ColorControl::Attributes::ColorMode::Id);
    SetAttributeValue(mEnhancedColorMode, ColorControl::EnhancedColorModeEnum::kCurrentHueAndCurrentSaturation,
                      ColorControl::Attributes::EnhancedColorMode::Id);

    if (!mSuppressDelegate)
    {
        mDelegate.OnHueSaturationChanged(hue, saturation);
    }
}

void ColorControlCluster::ApplyColorTemperature(uint16_t mireds)
{
    SetAttributeValue(mColorTemperatureMireds, mireds, ColorControl::Attributes::ColorTemperatureMireds::Id);
    SetAttributeValue(mColorMode, ColorControl::ColorModeEnum::kColorTemperatureMireds, ColorControl::Attributes::ColorMode::Id);
    SetAttributeValue(mEnhancedColorMode, ColorControl::EnhancedColorModeEnum::kColorTemperatureMireds,
                      ColorControl::Attributes::EnhancedColorMode::Id);

    if (!mSuppressDelegate)
    {
        mDelegate.OnColorTemperatureChanged(mireds);
    }
}

void ColorControlCluster::ReportHueSaturation(uint8_t hue, uint8_t saturation)
{
    mSuppressDelegate = true;
    ApplyHueSaturation(WrapHue(hue), ClampSaturation(saturation));
    mSuppressDelegate = false;
}

void ColorControlCluster::ReportColorTemperature(uint16_t mireds)
{
    mSuppressDelegate = true;
    ApplyColorTemperature(ClampMireds(mireds));
    mSuppressDelegate = false;
}

// ---------------------------------------------------------------------------
// RelativeHumidityMeasurement
// ---------------------------------------------------------------------------

RelativeHumidityCluster::RelativeHumidityCluster(EndpointId endpointId) :
    DefaultServerCluster({ endpointId, RelativeHumidityMeasurement::Id })
{}

DataModel::ActionReturnStatus RelativeHumidityCluster::ReadAttribute(const DataModel::ReadAttributeRequest & request,
                                                                     AttributeValueEncoder & encoder)
{
    using namespace RelativeHumidityMeasurement::Attributes;

    switch (request.path.mAttributeId)
    {
    case ClusterRevision::Id:
        return encoder.Encode(RelativeHumidityMeasurement::kRevision);
    case FeatureMap::Id:
        return encoder.Encode<uint32_t>(0);
    case MeasuredValue::Id:
        return encoder.Encode(mMeasuredValue);
    case MinMeasuredValue::Id:
        return encoder.Encode(DataModel::Nullable<uint16_t>(kMinMeasuredValue));
    case MaxMeasuredValue::Id:
        return encoder.Encode(DataModel::Nullable<uint16_t>(kMaxMeasuredValue));
    default:
        return Status::UnsupportedAttribute;
    }
}

CHIP_ERROR RelativeHumidityCluster::Attributes(const ConcreteClusterPath & path,
                                               ReadOnlyBufferBuilder<DataModel::AttributeEntry> & builder)
{
    AttributeListBuilder listBuilder(builder);
    return listBuilder.Append(Span<const DataModel::AttributeEntry>(RelativeHumidityMeasurement::Attributes::kMandatoryMetadata),
                              {});
}

void RelativeHumidityCluster::ReportMeasuredValue(uint16_t hundredthsPercent)
{
    DataModel::Nullable<uint16_t> value(std::clamp<uint16_t>(hundredthsPercent, kMinMeasuredValue, kMaxMeasuredValue));
    SetAttributeValue(mMeasuredValue, value, RelativeHumidityMeasurement::Attributes::MeasuredValue::Id);
}

// ---------------------------------------------------------------------------
// Thermostat
// ---------------------------------------------------------------------------

ThermostatCluster::ThermostatCluster(EndpointId endpointId, ThermostatDelegate & delegate, bool supportsCooling) :
    DefaultServerCluster({ endpointId, Thermostat::Id }), mDelegate(delegate), mSupportsCooling(supportsCooling)
{}

DataModel::ActionReturnStatus ThermostatCluster::ReadAttribute(const DataModel::ReadAttributeRequest & request,
                                                               AttributeValueEncoder & encoder)
{
    using namespace Thermostat::Attributes;

    switch (request.path.mAttributeId)
    {
    case ClusterRevision::Id:
        return encoder.Encode(Thermostat::kRevision);
    case FeatureMap::Id: {
        BitMask<Thermostat::Feature> features(Thermostat::Feature::kHeating);
        if (mSupportsCooling)
            features.Set(Thermostat::Feature::kCooling);
        return encoder.Encode<uint32_t>(features.Raw());
    }
    case LocalTemperature::Id:
        return encoder.Encode(mLocalTemperature);
    case ControlSequenceOfOperation::Id:
        return encoder.Encode(mSupportsCooling ? Thermostat::ControlSequenceOfOperationEnum::kCoolingAndHeating
                                               : Thermostat::ControlSequenceOfOperationEnum::kHeatingOnly);
    case SystemMode::Id:
        return encoder.Encode(mSystemMode);
    case OccupiedHeatingSetpoint::Id:
        return encoder.Encode(mOccupiedHeatingSetpoint);
    case AbsMinHeatSetpointLimit::Id:
        return encoder.Encode<int16_t>(kAbsMinHeatSetpoint);
    case AbsMaxHeatSetpointLimit::Id:
        return encoder.Encode<int16_t>(kAbsMaxHeatSetpoint);
    case OccupiedCoolingSetpoint::Id:
        VerifyOrReturnValue(mSupportsCooling, Status::UnsupportedAttribute);
        return encoder.Encode(mOccupiedCoolingSetpoint);
    case AbsMinCoolSetpointLimit::Id:
        VerifyOrReturnValue(mSupportsCooling, Status::UnsupportedAttribute);
        return encoder.Encode<int16_t>(kAbsMinCoolSetpoint);
    case AbsMaxCoolSetpointLimit::Id:
        VerifyOrReturnValue(mSupportsCooling, Status::UnsupportedAttribute);
        return encoder.Encode<int16_t>(kAbsMaxCoolSetpoint);
    default:
        return Status::UnsupportedAttribute;
    }
}

DataModel::ActionReturnStatus ThermostatCluster::WriteAttribute(const DataModel::WriteAttributeRequest & request,
                                                                AttributeValueDecoder & decoder)
{
    using namespace Thermostat::Attributes;

    switch (request.path.mAttributeId)
    {
    case OccupiedHeatingSetpoint::Id: {
        int16_t value;
        ReturnErrorOnFailure(decoder.Decode(value));
        return SetHeatingSetpoint(value, /* fromCloud = */ false);
    }
    case OccupiedCoolingSetpoint::Id: {
        VerifyOrReturnValue(mSupportsCooling, Status::UnsupportedAttribute);
        int16_t value;
        ReturnErrorOnFailure(decoder.Decode(value));
        return SetCoolingSetpoint(value, /* fromCloud = */ false);
    }
    case SystemMode::Id: {
        Thermostat::SystemModeEnum mode;
        ReturnErrorOnFailure(decoder.Decode(mode));
        // Only the modes this thermostat's feature set can actually perform.
        const bool supported = (mode == Thermostat::SystemModeEnum::kOff) || (mode == Thermostat::SystemModeEnum::kHeat) ||
            (mSupportsCooling && (mode == Thermostat::SystemModeEnum::kCool || mode == Thermostat::SystemModeEnum::kAuto));
        VerifyOrReturnValue(supported, Status::ConstraintError);

        if (SetAttributeValue(mSystemMode, mode, SystemMode::Id))
        {
            mDelegate.OnSystemModeChanged(mode);
        }
        return Status::Success;
    }
    default:
        return DefaultServerCluster::WriteAttribute(request, decoder);
    }
}

CHIP_ERROR ThermostatCluster::Attributes(const ConcreteClusterPath & path,
                                         ReadOnlyBufferBuilder<DataModel::AttributeEntry> & builder)
{
    using namespace Thermostat::Attributes;

    const DataModel::AttributeEntry kHeating[] = {
        LocalTemperature::kMetadataEntry,        ControlSequenceOfOperation::kMetadataEntry,
        SystemMode::kMetadataEntry,              OccupiedHeatingSetpoint::kMetadataEntry,
        AbsMinHeatSetpointLimit::kMetadataEntry, AbsMaxHeatSetpointLimit::kMetadataEntry,
    };
    const DataModel::AttributeEntry kHeatingAndCooling[] = {
        LocalTemperature::kMetadataEntry,        ControlSequenceOfOperation::kMetadataEntry,
        SystemMode::kMetadataEntry,              OccupiedHeatingSetpoint::kMetadataEntry,
        AbsMinHeatSetpointLimit::kMetadataEntry, AbsMaxHeatSetpointLimit::kMetadataEntry,
        OccupiedCoolingSetpoint::kMetadataEntry, AbsMinCoolSetpointLimit::kMetadataEntry,
        AbsMaxCoolSetpointLimit::kMetadataEntry,
    };

    AttributeListBuilder listBuilder(builder);
    return listBuilder.Append(mSupportsCooling ? Span<const DataModel::AttributeEntry>(kHeatingAndCooling)
                                               : Span<const DataModel::AttributeEntry>(kHeating),
                              {});
}

CHIP_ERROR ThermostatCluster::AcceptedCommands(const ConcreteClusterPath & path,
                                               ReadOnlyBufferBuilder<DataModel::AcceptedCommandEntry> & builder)
{
    static constexpr DataModel::AcceptedCommandEntry kCommands[] = {
        Thermostat::Commands::SetpointRaiseLower::kMetadataEntry,
    };
    return builder.ReferenceExisting(kCommands);
}

std::optional<DataModel::ActionReturnStatus> ThermostatCluster::InvokeCommand(const DataModel::InvokeRequest & request,
                                                                              TLV::TLVReader & input_arguments,
                                                                              CommandHandler * handler)
{
    if (request.path.mCommandId != Thermostat::Commands::SetpointRaiseLower::Id)
    {
        return Status::UnsupportedCommand;
    }

    Thermostat::Commands::SetpointRaiseLower::DecodableType data;
    VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);

    // `amount` is in 0.1 °C steps; the setpoint attributes are in 0.01 °C.
    const int32_t deltaCentiDegrees = static_cast<int32_t>(data.amount) * 10;
    const bool touchHeat =
        data.mode == Thermostat::SetpointRaiseLowerModeEnum::kHeat || data.mode == Thermostat::SetpointRaiseLowerModeEnum::kBoth;
    const bool touchCool = mSupportsCooling &&
        (data.mode == Thermostat::SetpointRaiseLowerModeEnum::kCool || data.mode == Thermostat::SetpointRaiseLowerModeEnum::kBoth);
    VerifyOrReturnValue(touchHeat || touchCool, Status::InvalidCommand);

    if (touchHeat)
    {
        SetHeatingSetpoint(static_cast<int16_t>(std::clamp<int32_t>(mOccupiedHeatingSetpoint + deltaCentiDegrees,
                                                                   kAbsMinHeatSetpoint, kAbsMaxHeatSetpoint)),
                           /* fromCloud = */ false);
    }
    if (touchCool)
    {
        SetCoolingSetpoint(static_cast<int16_t>(std::clamp<int32_t>(mOccupiedCoolingSetpoint + deltaCentiDegrees,
                                                                   kAbsMinCoolSetpoint, kAbsMaxCoolSetpoint)),
                           /* fromCloud = */ false);
    }
    return Status::Success;
}

DataModel::ActionReturnStatus ThermostatCluster::SetHeatingSetpoint(int16_t value, bool fromCloud)
{
    VerifyOrReturnValue(value >= kAbsMinHeatSetpoint && value <= kAbsMaxHeatSetpoint, Status::ConstraintError);
    if (SetAttributeValue(mOccupiedHeatingSetpoint, value, Thermostat::Attributes::OccupiedHeatingSetpoint::Id) && !fromCloud)
    {
        mDelegate.OnSetpointChanged(/* heating = */ true, value);
    }
    return Status::Success;
}

DataModel::ActionReturnStatus ThermostatCluster::SetCoolingSetpoint(int16_t value, bool fromCloud)
{
    VerifyOrReturnValue(value >= kAbsMinCoolSetpoint && value <= kAbsMaxCoolSetpoint, Status::ConstraintError);
    if (SetAttributeValue(mOccupiedCoolingSetpoint, value, Thermostat::Attributes::OccupiedCoolingSetpoint::Id) && !fromCloud)
    {
        mDelegate.OnSetpointChanged(/* heating = */ false, value);
    }
    return Status::Success;
}

void ThermostatCluster::ReportLocalTemperature(int16_t centiDegrees)
{
    SetAttributeValue(mLocalTemperature, DataModel::Nullable<int16_t>(centiDegrees),
                      Thermostat::Attributes::LocalTemperature::Id);
}

void ThermostatCluster::ReportHeatingSetpoint(int16_t centiDegrees)
{
    SetHeatingSetpoint(static_cast<int16_t>(std::clamp<int16_t>(centiDegrees, kAbsMinHeatSetpoint, kAbsMaxHeatSetpoint)),
                       /* fromCloud = */ true);
}

void ThermostatCluster::ReportCoolingSetpoint(int16_t centiDegrees)
{
    SetCoolingSetpoint(static_cast<int16_t>(std::clamp<int16_t>(centiDegrees, kAbsMinCoolSetpoint, kAbsMaxCoolSetpoint)),
                       /* fromCloud = */ true);
}

void ThermostatCluster::ReportSystemMode(Thermostat::SystemModeEnum mode)
{
    SetAttributeValue(mSystemMode, mode, Thermostat::Attributes::SystemMode::Id);
}

// ---------------------------------------------------------------------------
// WindowCovering
// ---------------------------------------------------------------------------

WindowCoveringCluster::WindowCoveringCluster(EndpointId endpointId, WindowCoveringDelegate & delegate) :
    DefaultServerCluster({ endpointId, WindowCovering::Id }), mDelegate(delegate)
{}

DataModel::ActionReturnStatus WindowCoveringCluster::ReadAttribute(const DataModel::ReadAttributeRequest & request,
                                                                   AttributeValueEncoder & encoder)
{
    using namespace WindowCovering::Attributes;

    switch (request.path.mAttributeId)
    {
    case ClusterRevision::Id:
        return encoder.Encode(WindowCovering::kRevision);
    case FeatureMap::Id:
        return encoder.Encode<uint32_t>(
            BitMask<WindowCovering::Feature>(WindowCovering::Feature::kLift, WindowCovering::Feature::kPositionAwareLift).Raw());
    case Type::Id:
        return encoder.Encode(WindowCovering::Type::kRollerShade);
    case EndProductType::Id:
        return encoder.Encode(WindowCovering::EndProductType::kRollerShade);
    case ConfigStatus::Id:
        return encoder.Encode(BitMask<WindowCovering::ConfigStatus>(WindowCovering::ConfigStatus::kOperational,
                                                                    WindowCovering::ConfigStatus::kLiftPositionAware));
    case OperationalStatus::Id:
        // Motion is driven (and reported) by the cloud; nothing is ever "in motion" from
        // this cluster's point of view, so the covering always reads as stopped.
        return encoder.Encode(BitMask<WindowCovering::OperationalStatus>());
    case Mode::Id:
        return encoder.Encode(mMode);
    case CurrentPositionLiftPercentage::Id:
        return encoder.Encode(mCurrentPositionLiftPercentage);
    case CurrentPositionLiftPercent100ths::Id:
        return encoder.Encode(mCurrentPositionLiftPercent100ths);
    case TargetPositionLiftPercent100ths::Id:
        return encoder.Encode(mTargetPositionLiftPercent100ths);
    default:
        return Status::UnsupportedAttribute;
    }
}

DataModel::ActionReturnStatus WindowCoveringCluster::WriteAttribute(const DataModel::WriteAttributeRequest & request,
                                                                    AttributeValueDecoder & decoder)
{
    if (request.path.mAttributeId == WindowCovering::Attributes::Mode::Id)
    {
        BitMask<WindowCovering::Mode> mode;
        ReturnErrorOnFailure(decoder.Decode(mode));
        SetAttributeValue(mMode, mode, WindowCovering::Attributes::Mode::Id);
        return Status::Success;
    }
    return DefaultServerCluster::WriteAttribute(request, decoder);
}

CHIP_ERROR WindowCoveringCluster::Attributes(const ConcreteClusterPath & path,
                                             ReadOnlyBufferBuilder<DataModel::AttributeEntry> & builder)
{
    using namespace WindowCovering::Attributes;

    const DataModel::AttributeEntry kServed[] = {
        Type::kMetadataEntry,
        ConfigStatus::kMetadataEntry,
        OperationalStatus::kMetadataEntry,
        EndProductType::kMetadataEntry,
        Mode::kMetadataEntry,
        CurrentPositionLiftPercentage::kMetadataEntry,
        CurrentPositionLiftPercent100ths::kMetadataEntry,
        TargetPositionLiftPercent100ths::kMetadataEntry,
    };

    AttributeListBuilder listBuilder(builder);
    return listBuilder.Append(Span<const DataModel::AttributeEntry>(kServed), {});
}

CHIP_ERROR WindowCoveringCluster::AcceptedCommands(const ConcreteClusterPath & path,
                                                   ReadOnlyBufferBuilder<DataModel::AcceptedCommandEntry> & builder)
{
    using namespace WindowCovering::Commands;

    static constexpr DataModel::AcceptedCommandEntry kCommands[] = {
        UpOrOpen::kMetadataEntry,
        DownOrClose::kMetadataEntry,
        StopMotion::kMetadataEntry,
        GoToLiftPercentage::kMetadataEntry,
    };
    return builder.ReferenceExisting(kCommands);
}

std::optional<DataModel::ActionReturnStatus> WindowCoveringCluster::InvokeCommand(const DataModel::InvokeRequest & request,
                                                                                  TLV::TLVReader & input_arguments,
                                                                                  CommandHandler * handler)
{
    using namespace WindowCovering::Commands;

    switch (request.path.mCommandId)
    {
    case UpOrOpen::Id:
        return GoToLiftPercent(0); // 0 % lift = fully open
    case DownOrClose::Id:
        return GoToLiftPercent(100);
    case GoToLiftPercentage::Id: {
        GoToLiftPercentage::DecodableType data;
        VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(data.liftPercent100thsValue <= 10000, Status::ConstraintError);
        return GoToLiftPercent(static_cast<uint8_t>(data.liftPercent100thsValue / 100));
    }
    case StopMotion::Id:
        mDelegate.OnStopRequested();
        return Status::Success;
    default:
        return Status::UnsupportedCommand;
    }
}

DataModel::ActionReturnStatus WindowCoveringCluster::GoToLiftPercent(uint8_t percent)
{
    percent = std::min<uint8_t>(percent, 100);

    SetAttributeValue(mTargetPositionLiftPercent100ths, DataModel::Nullable<Percent100ths>(static_cast<Percent100ths>(percent * 100)),
                      WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id);

    // Optimistic current position (transitions are instant here — see the header). A later
    // cloud position report corrects it if the covering ends up somewhere else.
    SetAttributeValue(mCurrentPositionLiftPercentage, DataModel::Nullable<Percent>(percent),
                      WindowCovering::Attributes::CurrentPositionLiftPercentage::Id);
    SetAttributeValue(mCurrentPositionLiftPercent100ths,
                      DataModel::Nullable<Percent100ths>(static_cast<Percent100ths>(percent * 100)),
                      WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id);

    mDelegate.OnLiftTargetChanged(percent);
    return Status::Success;
}

void WindowCoveringCluster::ReportCurrentLiftPercent(uint8_t percent)
{
    percent = std::min<uint8_t>(percent, 100);

    SetAttributeValue(mCurrentPositionLiftPercentage, DataModel::Nullable<Percent>(percent),
                      WindowCovering::Attributes::CurrentPositionLiftPercentage::Id);
    SetAttributeValue(mCurrentPositionLiftPercent100ths,
                      DataModel::Nullable<Percent100ths>(static_cast<Percent100ths>(percent * 100)),
                      WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id);
    SetAttributeValue(mTargetPositionLiftPercent100ths,
                      DataModel::Nullable<Percent100ths>(static_cast<Percent100ths>(percent * 100)),
                      WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id);
}

// ---------------------------------------------------------------------------
// FanControl
// ---------------------------------------------------------------------------

namespace {

uint8_t PercentToSpeed(uint8_t percent)
{
    if (percent == 0)
        return 0;
    // Round up so any non-zero percentage maps to a running speed.
    return static_cast<uint8_t>(std::min<uint32_t>(FanControlCluster::kSpeedMax,
                                                   (percent * FanControlCluster::kSpeedMax + 99) / 100));
}

uint8_t SpeedToPercent(uint8_t speed)
{
    return static_cast<uint8_t>(std::min<uint32_t>(100u, (speed * 100u) / FanControlCluster::kSpeedMax));
}

FanControl::FanModeEnum PercentToFanMode(uint8_t percent)
{
    if (percent == 0)
        return FanControl::FanModeEnum::kOff;
    if (percent <= 33)
        return FanControl::FanModeEnum::kLow;
    if (percent <= 66)
        return FanControl::FanModeEnum::kMedium;
    return FanControl::FanModeEnum::kHigh;
}

uint8_t FanModeToPercent(FanControl::FanModeEnum mode)
{
    switch (mode)
    {
    case FanControl::FanModeEnum::kOff:
        return 0;
    case FanControl::FanModeEnum::kLow:
        return 33;
    case FanControl::FanModeEnum::kMedium:
        return 66;
    default: // kHigh, kOn, kAuto, kSmart — all "run the fan"
        return 100;
    }
}

} // namespace

FanControlCluster::FanControlCluster(EndpointId endpointId, FanControlDelegate & delegate) :
    DefaultServerCluster({ endpointId, FanControl::Id }), mDelegate(delegate)
{}

DataModel::ActionReturnStatus FanControlCluster::ReadAttribute(const DataModel::ReadAttributeRequest & request,
                                                               AttributeValueEncoder & encoder)
{
    using namespace FanControl::Attributes;

    switch (request.path.mAttributeId)
    {
    case ClusterRevision::Id:
        return encoder.Encode(FanControl::kRevision);
    case FeatureMap::Id:
        return encoder.Encode<uint32_t>(
            BitMask<FanControl::Feature>(FanControl::Feature::kMultiSpeed, FanControl::Feature::kStep).Raw());
    case FanMode::Id:
        return encoder.Encode(mFanMode);
    case FanModeSequence::Id:
        return encoder.Encode(FanControl::FanModeSequenceEnum::kOffLowMedHigh);
    case PercentSetting::Id:
        return encoder.Encode(DataModel::Nullable<Percent>(mPercentSetting));
    case PercentCurrent::Id:
        return encoder.Encode<Percent>(mPercentSetting);
    case SpeedMax::Id:
        return encoder.Encode<uint8_t>(kSpeedMax);
    case SpeedSetting::Id:
        return encoder.Encode(DataModel::Nullable<uint8_t>(mSpeedSetting));
    case SpeedCurrent::Id:
        return encoder.Encode<uint8_t>(mSpeedSetting);
    default:
        return Status::UnsupportedAttribute;
    }
}

DataModel::ActionReturnStatus FanControlCluster::WriteAttribute(const DataModel::WriteAttributeRequest & request,
                                                                AttributeValueDecoder & decoder)
{
    using namespace FanControl::Attributes;

    switch (request.path.mAttributeId)
    {
    case FanMode::Id: {
        FanControl::FanModeEnum mode;
        ReturnErrorOnFailure(decoder.Decode(mode));
        VerifyOrReturnValue(mode <= FanControl::FanModeEnum::kHigh || mode == FanControl::FanModeEnum::kOn,
                            Status::ConstraintError);
        ApplyFanMode(mode, /* fromCloud = */ false);
        return Status::Success;
    }
    case PercentSetting::Id: {
        DataModel::Nullable<Percent> percent;
        ReturnErrorOnFailure(decoder.Decode(percent));
        VerifyOrReturnValue(!percent.IsNull(), Status::ConstraintError);
        VerifyOrReturnValue(percent.Value() <= 100, Status::ConstraintError);
        ApplyPercent(percent.Value(), /* fromCloud = */ false);
        return Status::Success;
    }
    case SpeedSetting::Id: {
        DataModel::Nullable<uint8_t> speed;
        ReturnErrorOnFailure(decoder.Decode(speed));
        VerifyOrReturnValue(!speed.IsNull(), Status::ConstraintError);
        VerifyOrReturnValue(speed.Value() <= kSpeedMax, Status::ConstraintError);
        ApplyPercent(SpeedToPercent(speed.Value()), /* fromCloud = */ false);
        return Status::Success;
    }
    default:
        return DefaultServerCluster::WriteAttribute(request, decoder);
    }
}

CHIP_ERROR FanControlCluster::Attributes(const ConcreteClusterPath & path,
                                         ReadOnlyBufferBuilder<DataModel::AttributeEntry> & builder)
{
    using namespace FanControl::Attributes;

    const DataModel::AttributeEntry kServed[] = {
        FanMode::kMetadataEntry,      FanModeSequence::kMetadataEntry, PercentSetting::kMetadataEntry,
        PercentCurrent::kMetadataEntry, SpeedMax::kMetadataEntry,      SpeedSetting::kMetadataEntry,
        SpeedCurrent::kMetadataEntry,
    };

    AttributeListBuilder listBuilder(builder);
    return listBuilder.Append(Span<const DataModel::AttributeEntry>(kServed), {});
}

CHIP_ERROR FanControlCluster::AcceptedCommands(const ConcreteClusterPath & path,
                                               ReadOnlyBufferBuilder<DataModel::AcceptedCommandEntry> & builder)
{
    static constexpr DataModel::AcceptedCommandEntry kCommands[] = {
        FanControl::Commands::Step::kMetadataEntry,
    };
    return builder.ReferenceExisting(kCommands);
}

std::optional<DataModel::ActionReturnStatus> FanControlCluster::InvokeCommand(const DataModel::InvokeRequest & request,
                                                                              TLV::TLVReader & input_arguments,
                                                                              CommandHandler * handler)
{
    if (request.path.mCommandId != FanControl::Commands::Step::Id)
    {
        return Status::UnsupportedCommand;
    }

    FanControl::Commands::Step::DecodableType data;
    VerifyOrReturnError(DataModel::Decode(input_arguments, data) == CHIP_NO_ERROR, Status::InvalidCommand);

    const bool wrap      = data.wrap.ValueOr(false);
    const bool lowestOff = data.lowestOff.ValueOr(false);
    const uint8_t lowest = lowestOff ? 0 : 1;

    int32_t speed = mSpeedSetting;
    if (data.direction == FanControl::StepDirectionEnum::kIncrease)
    {
        speed = (speed >= kSpeedMax) ? (wrap ? lowest : kSpeedMax) : speed + 1;
    }
    else if (data.direction == FanControl::StepDirectionEnum::kDecrease)
    {
        speed = (speed <= lowest) ? (wrap ? kSpeedMax : lowest) : speed - 1;
    }
    else
    {
        return Status::InvalidCommand;
    }

    ApplyPercent(SpeedToPercent(static_cast<uint8_t>(speed)), /* fromCloud = */ false);
    return Status::Success;
}

void FanControlCluster::ApplyFanMode(FanControl::FanModeEnum mode, bool fromCloud)
{
    const uint8_t percent = FanModeToPercent(mode);

    SetAttributeValue(mFanMode, mode, FanControl::Attributes::FanMode::Id);
    const bool percentChanged = SetAttributeValue(mPercentSetting, percent, FanControl::Attributes::PercentSetting::Id);
    if (percentChanged)
    {
        NotifyAttributeChanged(FanControl::Attributes::PercentCurrent::Id);
    }
    const uint8_t speed = PercentToSpeed(percent);
    if (SetAttributeValue(mSpeedSetting, speed, FanControl::Attributes::SpeedSetting::Id))
    {
        NotifyAttributeChanged(FanControl::Attributes::SpeedCurrent::Id);
    }

    if (!fromCloud)
    {
        mDelegate.OnFanModeChanged(mode);
    }
}

void FanControlCluster::ApplyPercent(uint8_t percent, bool fromCloud)
{
    percent = std::min<uint8_t>(percent, 100);

    if (SetAttributeValue(mPercentSetting, percent, FanControl::Attributes::PercentSetting::Id))
    {
        NotifyAttributeChanged(FanControl::Attributes::PercentCurrent::Id);
    }
    const uint8_t speed = PercentToSpeed(percent);
    if (SetAttributeValue(mSpeedSetting, speed, FanControl::Attributes::SpeedSetting::Id))
    {
        NotifyAttributeChanged(FanControl::Attributes::SpeedCurrent::Id);
    }
    // FanMode must stay consistent with PercentSetting (spec 4.4.6.1).
    SetAttributeValue(mFanMode, PercentToFanMode(percent), FanControl::Attributes::FanMode::Id);

    if (!fromCloud)
    {
        mDelegate.OnPercentChanged(percent);
    }
}

void FanControlCluster::ReportFanMode(FanControl::FanModeEnum mode)
{
    ApplyFanMode(mode, /* fromCloud = */ true);
}

void FanControlCluster::ReportPercent(uint8_t percent)
{
    ApplyPercent(percent, /* fromCloud = */ true);
}

} // namespace TuyaClusters

#endif // HAVE_TUYA

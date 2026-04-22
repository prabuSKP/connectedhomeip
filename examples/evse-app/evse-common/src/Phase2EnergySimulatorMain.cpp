/*
 *
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

#include <Phase2EnergySimulatorMain.h>

#include <CommodityMeteringMain.h>
#include <CommodityPriceMain.h>
#include <CommodityTariffMain.h>
#include <DeviceEnergyManagementDelegateImpl.h>
#include <DeviceEnergyManagementManager.h>
#include <ElectricalPowerMeasurementDelegateImpl.h>
#include <EnergyEvseDelegateImpl.h>
#include <EnergyEvseMain.h>
#include <EnergyEvseManager.h>
#include <EnergyEvseTargetsStore.h>
#include <EnergyManagementAppCmdLineOptions.h>
#include <Identify.h>
#include <MeterIdentificationInstance.h>
#include <PowerTopologyDelegateImpl.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/electrical-energy-measurement-server/CodegenIntegration.h>
#include <app/clusters/energy-evse-server/EnergyEvseTestEventTriggerHandler.h>
#include <app/data-model/Nullable.h>
#include <app/reporting/reporting.h>
#include <device-energy-management-modes.h>
#include <energy-evse-modes.h>
#include <lib/core/Optional.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>

#include <cstdlib>
#include <ctime>
#include <memory>
using namespace chip;
using namespace chip::app;
using namespace chip::app::DataModel;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::DeviceEnergyManagement;
using namespace chip::app::Clusters::ElectricalPowerMeasurement;
using namespace chip::app::Clusters::ElectricalEnergyMeasurement;
using namespace chip::app::Clusters::PowerTopology;

namespace {

constexpr chip::EndpointId kElectricalSensorEndpoint = 1;
constexpr chip::EndpointId kDemEndpoint              = 2;
constexpr chip::EndpointId kElectricalMeterEndpoint  = 3;
constexpr chip::EndpointId kElectricalUtilityMeterEndpoint = 4;
constexpr chip::EndpointId kElectricalEnergyTariffEndpoint = 5;
constexpr chip::EndpointId kEnergyEvseEndpoint = 6;

constexpr uint32_t kTelemetryIntervalSec = 10;

// Telemetry state — accumulates over time
int64_t sSensorCumulativeEnergyMwh = 45'000'000; // 45 kWh
int64_t sMeterCumulativeEnergyMwh  = 52'000'000; // 52 kWh

const ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type kMeasurementAccuracyRanges[] = {
    { .rangeMin   = 0,
      .rangeMax   = 1'000'000'000'000'000,
      .percentMax = MakeOptional(static_cast<chip::Percent100ths>(500)),
      .percentMin = MakeOptional(static_cast<chip::Percent100ths>(50)) }
};

const ElectricalEnergyMeasurement::Structs::MeasurementAccuracyStruct::Type kMeasurementAccuracy = {
    .measurementType  = MeasurementTypeEnum::kElectricalEnergy,
    .measured         = true,
    .minMeasuredValue = 0,
    .maxMeasuredValue = 1'000'000'000'000'000,
    .accuracyRanges =
        DataModel::List<const ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(kMeasurementAccuracyRanges)
};

struct ElectricalMeasurementRuntime
{
    std::unique_ptr<ElectricalPowerMeasurementDelegate> epmDelegate;
    std::unique_ptr<ElectricalPowerMeasurementInstance> epmInstance;
    std::unique_ptr<ElectricalEnergyMeasurementAttrAccess> eemAttrAccess;
};

struct ElectricalSensorRuntime : public ElectricalMeasurementRuntime
{
    std::unique_ptr<PowerTopologyDelegate> powerTopologyDelegate;
    std::unique_ptr<PowerTopologyInstance> powerTopologyInstance;
};

struct DemRuntime
{
    std::unique_ptr<DeviceEnergyManagementDelegate> demDelegate;
    std::unique_ptr<DeviceEnergyManagementManager> demInstance;
};

struct EvseRuntime
{
    std::unique_ptr<EvseTargetsDelegate> targetsDelegate;
    std::unique_ptr<EnergyEvseDelegate> delegate;
    std::unique_ptr<EnergyEvseManager> instance;
};

struct EvseTestEventSaveData
{
    int64_t oldMaxHardwareChargeCurrentLimit    = 0;
    int64_t oldMaxHardwareDischargeCurrentLimit = 0;
    int64_t oldCircuitCapacity                  = 0;
    int64_t oldUserMaximumChargeCurrent         = 0;
    int64_t oldCableAssemblyLimit               = 0;
    EnergyEvse::StateEnum oldStateBasic         = EnergyEvse::StateEnum::kNotPluggedIn;
    EnergyEvse::StateEnum oldStatePluggedIn     = EnergyEvse::StateEnum::kNotPluggedIn;
    EnergyEvse::StateEnum oldStateDemand        = EnergyEvse::StateEnum::kNotPluggedIn;
};

ElectricalSensorRuntime gElectricalSensor;
DemRuntime gDem;
ElectricalMeasurementRuntime gElectricalMeter;
ElectricalMeasurementRuntime gElectricalUtilityMeter;
EvseRuntime gEvse;
EvseTestEventSaveData gEvseTestEventSaveData;

BitMask<ElectricalPowerMeasurement::Feature, uint32_t> GetElectricalPowerFeatures()
{
    return BitMask<ElectricalPowerMeasurement::Feature, uint32_t>(
        ElectricalPowerMeasurement::Feature::kDirectCurrent, ElectricalPowerMeasurement::Feature::kAlternatingCurrent,
        ElectricalPowerMeasurement::Feature::kPolyphasePower, ElectricalPowerMeasurement::Feature::kHarmonics,
        ElectricalPowerMeasurement::Feature::kPowerQuality);
}

BitMask<ElectricalPowerMeasurement::OptionalAttributes, uint32_t> GetElectricalPowerOptionalAttributes()
{
    return BitMask<ElectricalPowerMeasurement::OptionalAttributes, uint32_t>(
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRanges,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeVoltage,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeActiveCurrent,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeReactiveCurrent,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeApparentCurrent,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeReactivePower,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeApparentPower,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSVoltage,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSCurrent,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSPower,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeFrequency,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributePowerFactor,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeNeutralCurrent);
}

std::unique_ptr<ElectricalEnergyMeasurementAttrAccess> * GetElectricalEnergyMeasurementSlot(EndpointId endpointId)
{
    switch (endpointId)
    {
    case kElectricalSensorEndpoint:
        return &gElectricalSensor.eemAttrAccess;
    case kElectricalMeterEndpoint:
        return &gElectricalMeter.eemAttrAccess;
    default:
        return nullptr;
    }
}

void SeedElectricalPowerValues(ElectricalMeasurementRuntime & runtime, int64_t voltageMv, int64_t currentMa, int64_t powerMw)
{
    VerifyOrReturn(runtime.epmDelegate != nullptr);

    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetPowerMode(PowerModeEnum::kAc);
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetVoltage(Nullable<int64_t>(voltageMv));
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetActiveCurrent(Nullable<int64_t>(currentMa));
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetActivePower(Nullable<int64_t>(powerMw));
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetFrequency(Nullable<int64_t>(50'000));
}

void SeedElectricalEnergyValues(EndpointId endpointId, int64_t cumulativeImportedMwh, int64_t periodicImportedMwh)
{
    ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type cumulativeImported = {
        .energy = cumulativeImportedMwh,
    };
    ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type periodicImported = {
        .energy = periodicImportedMwh,
    };
    const Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> noExportedEnergy;

    NotifyCumulativeEnergyMeasured(endpointId, MakeOptional(cumulativeImported), noExportedEnergy);
    MatterReportingAttributeChangeCallback(endpointId, ElectricalEnergyMeasurement::Id,
                                           ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Id);

    NotifyPeriodicEnergyMeasured(endpointId, MakeOptional(periodicImported), noExportedEnergy);
    MatterReportingAttributeChangeCallback(endpointId, ElectricalEnergyMeasurement::Id,
                                           ElectricalEnergyMeasurement::Attributes::PeriodicEnergyImported::Id);
}

CHIP_ERROR InitElectricalSensorEndpoint()
{
    ReturnErrorOnFailure(ElectricalPowerMeasurementInit(kElectricalSensorEndpoint, gElectricalSensor.epmDelegate,
                                                        gElectricalSensor.epmInstance, GetElectricalPowerFeatures(),
                                                        GetElectricalPowerOptionalAttributes()));
    ReturnErrorOnFailure(
        PowerTopologyInit(kElectricalSensorEndpoint, gElectricalSensor.powerTopologyDelegate, gElectricalSensor.powerTopologyInstance));
    VerifyOrReturnError(gElectricalSensor.eemAttrAccess != nullptr, CHIP_ERROR_INCORRECT_STATE);

    SeedElectricalPowerValues(gElectricalSensor, 230'000, 6'500, 1'500'000);
    SeedElectricalEnergyValues(kElectricalSensorEndpoint, 45'000'000, 100'000);
    return CHIP_NO_ERROR;
}

CHIP_ERROR InitDemEndpoint()
{
    ReturnErrorOnFailure(DeviceEnergyManagementInit(kDemEndpoint, gDem.demDelegate, gDem.demInstance, GetFeatureMapFromCmdLine()));
    VerifyOrReturnError(gDem.demDelegate != nullptr, CHIP_ERROR_INCORRECT_STATE);

    TEMPORARY_RETURN_IGNORED gDem.demDelegate->SetESAState(ESAStateEnum::kOnline);
    TEMPORARY_RETURN_IGNORED gDem.demDelegate->SetAbsMinPower(0);
    TEMPORARY_RETURN_IGNORED gDem.demDelegate->SetAbsMaxPower(7'200'000);
    return CHIP_NO_ERROR;
}

CHIP_ERROR InitElectricalMeterEndpoint()
{
    ReturnErrorOnFailure(ElectricalPowerMeasurementInit(kElectricalMeterEndpoint, gElectricalMeter.epmDelegate,
                                                        gElectricalMeter.epmInstance, GetElectricalPowerFeatures(),
                                                        GetElectricalPowerOptionalAttributes()));
    VerifyOrReturnError(gElectricalMeter.eemAttrAccess != nullptr, CHIP_ERROR_INCORRECT_STATE);
    ReturnErrorOnFailure(CommodityMeteringInit(kElectricalMeterEndpoint));

    SeedElectricalPowerValues(gElectricalMeter, 230'000, 8'000, 1'840'000);
    SeedElectricalEnergyValues(kElectricalMeterEndpoint, 52'000'000, 250'000);
    return CHIP_NO_ERROR;
}

CHIP_ERROR InitElectricalUtilityMeterEndpoint()
{
    ReturnErrorOnFailure(MeterIdentificationInit(kElectricalUtilityMeterEndpoint));
    return CHIP_NO_ERROR;
}

CHIP_ERROR InitElectricalEnergyTariffEndpoint()
{
    ReturnErrorOnFailure(CommodityPriceInit(kElectricalEnergyTariffEndpoint));
    ReturnErrorOnFailure(CommodityTariffInit(kElectricalEnergyTariffEndpoint));
    ReturnErrorOnFailure(MeterIdentificationInit(kElectricalEnergyTariffEndpoint));
    ReturnErrorOnFailure(CommodityMeteringInit(kElectricalEnergyTariffEndpoint));
    return CHIP_NO_ERROR;
}

CHIP_ERROR InitEnergyEvseEndpoint()
{
    VerifyOrReturnError(!gEvse.targetsDelegate && !gEvse.delegate && !gEvse.instance, CHIP_ERROR_INCORRECT_STATE);

    gEvse.targetsDelegate = std::make_unique<EvseTargetsDelegate>();
    VerifyOrReturnError(gEvse.targetsDelegate != nullptr, CHIP_ERROR_NO_MEMORY);

    gEvse.delegate = std::make_unique<EnergyEvseDelegate>(*gEvse.targetsDelegate);
    VerifyOrReturnError(gEvse.delegate != nullptr, CHIP_ERROR_NO_MEMORY);

    gEvse.instance = std::make_unique<EnergyEvseManager>(
        EndpointId(kEnergyEvseEndpoint), *gEvse.delegate,
        BitMask<EnergyEvse::Feature, uint32_t>(EnergyEvse::Feature::kChargingPreferences, EnergyEvse::Feature::kRfid,
                                               EnergyEvse::Feature::kSoCReporting, EnergyEvse::Feature::kPlugAndCharge,
                                               EnergyEvse::Feature::kV2x),
        BitMask<EnergyEvse::OptionalAttributes, uint32_t>(EnergyEvse::OptionalAttributes::kSupportsUserMaximumChargingCurrent,
                                                          EnergyEvse::OptionalAttributes::kSupportsRandomizationWindow,
                                                          EnergyEvse::OptionalAttributes::kSupportsApproximateEvEfficiency),
        BitMask<EnergyEvse::OptionalCommands, uint32_t>(EnergyEvse::OptionalCommands::kSupportsStartDiagnostics));
    VerifyOrReturnError(gEvse.instance != nullptr, CHIP_ERROR_NO_MEMORY);

    ReturnErrorOnFailure(gEvse.instance->Init());
    gEvse.delegate->SetInstance(gEvse.instance.get());
    ReturnErrorOnFailure(gEvse.targetsDelegate->LoadTargets());

    TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetMaxHardwareChargeCurrentLimit(32000);
    TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetMaxHardwareDischargeCurrentLimit(32000);
    TEMPORARY_RETURN_IGNORED gEvse.instance->SetCircuitCapacity(32000);
    TEMPORARY_RETURN_IGNORED gEvse.instance->SetUserMaximumChargeCurrent(32000);
    TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetState(EnergyEvse::StateEnum::kNotPluggedIn);

    return CHIP_NO_ERROR;
}

void ShutdownElectricalMeasurementRuntime(ElectricalMeasurementRuntime & runtime)
{
    if (runtime.eemAttrAccess)
    {
        runtime.eemAttrAccess->Shutdown();
        runtime.eemAttrAccess.reset();
    }

    TEMPORARY_RETURN_IGNORED ElectricalPowerMeasurementShutdown(runtime.epmInstance, runtime.epmDelegate);
}

int64_t RandomVariation(int64_t base, int64_t halfRange)
{
    return base + (std::rand() % (2 * halfRange + 1)) - halfRange;
}

void UpdateEndpointTelemetry(ElectricalMeasurementRuntime & runtime, EndpointId endpoint,
                            int64_t baseVoltageMv, int64_t baseCurrentMa, int64_t basePowerMw,
                            int64_t & cumulativeEnergyMwh)
{
    if (runtime.epmDelegate == nullptr)
        return;

    int64_t voltage = RandomVariation(baseVoltageMv, 5'000);  // ±5 V
    int64_t current = RandomVariation(baseCurrentMa, 500);    // ±0.5 A
    int64_t power   = RandomVariation(basePowerMw, 100'000);  // ±100 W

    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetVoltage(Nullable<int64_t>(voltage));
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetActiveCurrent(Nullable<int64_t>(current));
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetActivePower(Nullable<int64_t>(power));

    MatterReportingAttributeChangeCallback(endpoint, ElectricalPowerMeasurement::Id,
                                           ElectricalPowerMeasurement::Attributes::Voltage::Id);
    MatterReportingAttributeChangeCallback(endpoint, ElectricalPowerMeasurement::Id,
                                           ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id);
    MatterReportingAttributeChangeCallback(endpoint, ElectricalPowerMeasurement::Id,
                                           ElectricalPowerMeasurement::Attributes::ActivePower::Id);

    // Accumulate energy: E = P × t / 3600 (mWh)
    int64_t energyIncrementMwh = (power * static_cast<int64_t>(kTelemetryIntervalSec)) / 3600;
    cumulativeEnergyMwh += energyIncrementMwh;

    SeedElectricalEnergyValues(endpoint, cumulativeEnergyMwh, energyIncrementMwh);
}

void TelemetryTimerHandler(chip::System::Layer * /*layer*/, void * /*appState*/)
{
    ChipLogDetail(AppServer, "Phase 2 Telemetry: tick (sensor cumE=%.1f kWh, meter cumE=%.1f kWh)",
                  static_cast<double>(sSensorCumulativeEnergyMwh) / 1'000'000.0,
                  static_cast<double>(sMeterCumulativeEnergyMwh) / 1'000'000.0);

    UpdateEndpointTelemetry(gElectricalSensor, kElectricalSensorEndpoint,
                            230'000, 6'500, 1'500'000, sSensorCumulativeEnergyMwh);
    UpdateEndpointTelemetry(gElectricalMeter, kElectricalMeterEndpoint,
                            230'000, 8'000, 1'840'000, sMeterCumulativeEnergyMwh);

    // Reschedule
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(kTelemetryIntervalSec * 1000),
        TelemetryTimerHandler, nullptr);
}

} // namespace

void emberAfElectricalEnergyMeasurementClusterInitCallback(chip::EndpointId endpointId)
{
    auto * slot = GetElectricalEnergyMeasurementSlot(endpointId);
    VerifyOrDie(slot != nullptr);
    VerifyOrDie(*slot == nullptr);

    auto attrAccess = std::make_unique<ElectricalEnergyMeasurementAttrAccess>(
        BitMask<ElectricalEnergyMeasurement::Feature, uint32_t>(ElectricalEnergyMeasurement::Feature::kImportedEnergy,
                                                                ElectricalEnergyMeasurement::Feature::kCumulativeEnergy,
                                                                ElectricalEnergyMeasurement::Feature::kPeriodicEnergy),
        BitMask<ElectricalEnergyMeasurement::OptionalAttributes, uint32_t>(
            ElectricalEnergyMeasurement::OptionalAttributes::kOptionalAttributeCumulativeEnergyReset),
        endpointId);
    VerifyOrDie(attrAccess != nullptr);
    VerifyOrDie(attrAccess->Init() == CHIP_NO_ERROR);

    ElectricalEnergyMeasurement::Structs::CumulativeEnergyResetStruct::Type resetStruct = {
        .importedResetTimestamp = MakeOptional(MakeNullable(static_cast<uint32_t>(0))),
        .exportedResetTimestamp = MakeOptional(MakeNullable(static_cast<uint32_t>(0))),
        .importedResetSystime   = MakeOptional(MakeNullable(static_cast<uint64_t>(0))),
        .exportedResetSystime   = MakeOptional(MakeNullable(static_cast<uint64_t>(0))),
    };

    TEMPORARY_RETURN_IGNORED SetMeasurementAccuracy(endpointId, kMeasurementAccuracy);
    TEMPORARY_RETURN_IGNORED SetCumulativeReset(endpointId, MakeOptional(resetStruct));
    *slot = std::move(attrAccess);
}

void Phase2EnergySimulatorInit()
{
    ChipLogDetail(AppServer, "Phase 2 Energy Simulator: Init");

    VerifyOrDie(InitElectricalSensorEndpoint() == CHIP_NO_ERROR);
    VerifyOrDie(InitDemEndpoint() == CHIP_NO_ERROR);
    VerifyOrDie(InitElectricalMeterEndpoint() == CHIP_NO_ERROR);
    VerifyOrDie(InitElectricalUtilityMeterEndpoint() == CHIP_NO_ERROR);
    VerifyOrDie(InitElectricalEnergyTariffEndpoint() == CHIP_NO_ERROR);
    VerifyOrDie(InitEnergyEvseEndpoint() == CHIP_NO_ERROR);

    // Start telemetry timer for dynamic attribute updates
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(kTelemetryIntervalSec * 1000),
        TelemetryTimerHandler, nullptr);
    ChipLogDetail(AppServer, "Phase 2 Energy Simulator: Telemetry timer started (%u sec interval)", kTelemetryIntervalSec);
}

void Phase2EnergySimulatorShutdown()
{
    ChipLogDetail(AppServer, "Phase 2 Energy Simulator: Shutdown");

    // Cancel telemetry timer
    chip::DeviceLayer::SystemLayer().CancelTimer(TelemetryTimerHandler, nullptr);

    TEMPORARY_RETURN_IGNORED MeterIdentificationShutdown();
    TEMPORARY_RETURN_IGNORED CommodityMeteringShutdown();
    TEMPORARY_RETURN_IGNORED CommodityTariffShutdown();
    TEMPORARY_RETURN_IGNORED CommodityPriceShutdown();
    if (gEvse.instance)
    {
        gEvse.instance->Shutdown();
        gEvse.instance.reset();
    }
    gEvse.delegate.reset();
    gEvse.targetsDelegate.reset();
    ShutdownElectricalMeasurementRuntime(gElectricalMeter);
    TEMPORARY_RETURN_IGNORED PowerTopologyShutdown(gElectricalSensor.powerTopologyInstance, gElectricalSensor.powerTopologyDelegate);
    ShutdownElectricalMeasurementRuntime(gElectricalSensor);
    DeviceEnergyManagementShutdown(gDem.demInstance, gDem.demDelegate);

    Clusters::DeviceEnergyManagementMode::Shutdown();
}

DeviceEnergyManagement::DeviceEnergyManagementDelegate * GetDEMDelegate()
{
    VerifyOrDieWithMsg(gDem.demDelegate.get() != nullptr, AppServer, "DEM Delegate is null");
    return gDem.demDelegate.get();
}

bool HandleEnergyEvseTestEventTrigger(uint64_t eventTrigger)
{
    VerifyOrReturnError(gEvse.delegate != nullptr && gEvse.instance != nullptr, false);

    switch (static_cast<chip::EnergyEvseTrigger>(eventTrigger))
    {
    case chip::EnergyEvseTrigger::kBasicFunctionality:
        gEvseTestEventSaveData.oldMaxHardwareChargeCurrentLimit    = gEvse.delegate->HwGetMaxHardwareChargeCurrentLimit();
        gEvseTestEventSaveData.oldMaxHardwareDischargeCurrentLimit = gEvse.delegate->HwGetMaxHardwareDischargeCurrentLimit();
        gEvseTestEventSaveData.oldCircuitCapacity                  = gEvse.instance->GetCircuitCapacity();
        gEvseTestEventSaveData.oldUserMaximumChargeCurrent         = gEvse.instance->GetUserMaximumChargeCurrent();
        gEvseTestEventSaveData.oldStateBasic                       = gEvse.delegate->HwGetState();

        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetMaxHardwareChargeCurrentLimit(32000);
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetMaxHardwareDischargeCurrentLimit(32000);
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetCircuitCapacity(32000);
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetUserMaximumChargeCurrent(32000);
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetState(EnergyEvse::StateEnum::kNotPluggedIn);
        return true;
    case chip::EnergyEvseTrigger::kBasicFunctionalityClear:
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetMaxHardwareChargeCurrentLimit(
            gEvseTestEventSaveData.oldMaxHardwareChargeCurrentLimit);
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetMaxHardwareDischargeCurrentLimit(
            gEvseTestEventSaveData.oldMaxHardwareDischargeCurrentLimit);
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetCircuitCapacity(gEvseTestEventSaveData.oldCircuitCapacity);
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetUserMaximumChargeCurrent(gEvseTestEventSaveData.oldUserMaximumChargeCurrent);
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetState(gEvseTestEventSaveData.oldStateBasic);
        return true;
    case chip::EnergyEvseTrigger::kEVPluggedIn:
        gEvseTestEventSaveData.oldCableAssemblyLimit = gEvse.delegate->HwGetCableAssemblyLimit();
        gEvseTestEventSaveData.oldStatePluggedIn     = gEvse.delegate->HwGetState();
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetCableAssemblyLimit(63000);
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetState(EnergyEvse::StateEnum::kPluggedInNoDemand);
        return true;
    case chip::EnergyEvseTrigger::kEVPluggedInClear:
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetCableAssemblyLimit(gEvseTestEventSaveData.oldCableAssemblyLimit);
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetState(gEvseTestEventSaveData.oldStatePluggedIn);
        return true;
    case chip::EnergyEvseTrigger::kEVChargeDemand:
        gEvseTestEventSaveData.oldStateDemand = gEvse.delegate->HwGetState();
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetState(EnergyEvse::StateEnum::kPluggedInDemand);
        return true;
    case chip::EnergyEvseTrigger::kEVChargeDemandClear:
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetState(gEvseTestEventSaveData.oldStateDemand);
        return true;
    case chip::EnergyEvseTrigger::kEVTimeOfUseMode:
    case chip::EnergyEvseTrigger::kEVTimeOfUseModeClear:
        // Mode changes are handled through the Energy EVSE Mode cluster commands.
        return true;
    case chip::EnergyEvseTrigger::kEVSEGroundFault:
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetFault(EnergyEvse::FaultStateEnum::kGroundFault);
        return true;
    case chip::EnergyEvseTrigger::kEVSEOverTemperatureFault:
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetFault(EnergyEvse::FaultStateEnum::kOverTemperature);
        return true;
    case chip::EnergyEvseTrigger::kEVSEFaultClear:
    case chip::EnergyEvseTrigger::kEVSEDiagnosticsComplete:
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetFault(EnergyEvse::FaultStateEnum::kNoError);
        return true;
    case chip::EnergyEvseTrigger::kEVSESetSoCLow:
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetStateOfCharge(DataModel::MakeNullable(static_cast<Percent>(20)));
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetBatteryCapacity(DataModel::MakeNullable(static_cast<int64_t>(70'000'000)));
        return true;
    case chip::EnergyEvseTrigger::kEVSESetSoCHigh:
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetStateOfCharge(DataModel::MakeNullable(static_cast<Percent>(95)));
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetBatteryCapacity(DataModel::MakeNullable(static_cast<int64_t>(70'000'000)));
        return true;
    case chip::EnergyEvseTrigger::kEVSESetSoCClear:
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetStateOfCharge(DataModel::NullNullable);
        TEMPORARY_RETURN_IGNORED gEvse.instance->SetBatteryCapacity(DataModel::NullNullable);
        return true;
    case chip::EnergyEvseTrigger::kEVSESetVehicleID: {
        CharSpan vehicleId = CharSpan::fromCharString("Test-Vehicle-ID-012345789-ABCDEF");
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetVehicleID(vehicleId);
        return true;
    }
    case chip::EnergyEvseTrigger::kEVSETriggerRFID: {
        constexpr uint8_t rfidData[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99 };
        TEMPORARY_RETURN_IGNORED gEvse.delegate->HwSetRFID(ByteSpan(rfidData));
        return true;
    }
    default:
        return false;
    }
}

EndpointId GetIdentifyEndpointId()
{
    return kElectricalSensorEndpoint;
}

} // namespace

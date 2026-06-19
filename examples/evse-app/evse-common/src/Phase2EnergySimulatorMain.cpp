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
#include <DeviceEnergyManagementDelegateImpl.h>
#include <DeviceEnergyManagementManager.h>
#include <ElectricalPowerMeasurementDelegateImpl.h>
#include <EnergyManagementAppCmdLineOptions.h>
#include <Identify.h>
#include <MeterIdentificationInstance.h>
#include <PowerTopologyDelegateImpl.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/electrical-energy-measurement-server/CodegenIntegration.h>
#include <app/data-model/Nullable.h>
#include <app/reporting/reporting.h>
#include <device-energy-management-modes.h>
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

constexpr chip::EndpointId kElectricalSensorEndpoint       = 1;
constexpr chip::EndpointId kDemEndpoint                    = 2;
constexpr chip::EndpointId kElectricalMeterEndpoint        = 3;
constexpr chip::EndpointId kElectricalUtilityMeterEndpoint = 4;

constexpr uint32_t kTelemetryIntervalSec = 10;

// Telemetry state — accumulates over time
int64_t sSensorCumulativeEnergyMwh        = 45'000'000; // 45 kWh imported
int64_t sMeterCumulativeEnergyImportedMwh = 52'000'000; // 52 kWh imported
int64_t sMeterCumulativeEnergyExportedMwh = 12'000'000; // 12 kWh exported (simulated back-feed)

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

ElectricalSensorRuntime gElectricalSensor;
DemRuntime gDem;
ElectricalMeasurementRuntime gElectricalMeter;

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
    // RMSVoltage (0x000B) and RMSCurrent (0x000C) are the ALTC feature attributes used by
    // the SmartThings driver for voltageMeasurement and currentMeasurement capabilities.
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetRMSVoltage(Nullable<int64_t>(voltageMv));
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetRMSCurrent(Nullable<int64_t>(currentMa));
}

void SeedElectricalEnergyValues(EndpointId endpointId, int64_t cumulativeImportedMwh, int64_t periodicImportedMwh,
                                int64_t cumulativeExportedMwh = 0, int64_t periodicExportedMwh = 0)
{
    ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type cumulativeImported = {
        .energy = cumulativeImportedMwh,
    };
    ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type periodicImported = {
        .energy = periodicImportedMwh,
    };

    if (cumulativeExportedMwh > 0)
    {
        ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type cumulativeExported = {
            .energy = cumulativeExportedMwh,
        };
        ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type periodicExported = {
            .energy = periodicExportedMwh,
        };
        NotifyCumulativeEnergyMeasured(endpointId, MakeOptional(cumulativeImported), MakeOptional(cumulativeExported));
        MatterReportingAttributeChangeCallback(endpointId, ElectricalEnergyMeasurement::Id,
                                               ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Id);
        MatterReportingAttributeChangeCallback(endpointId, ElectricalEnergyMeasurement::Id,
                                               ElectricalEnergyMeasurement::Attributes::CumulativeEnergyExported::Id);

        NotifyPeriodicEnergyMeasured(endpointId, MakeOptional(periodicImported), MakeOptional(periodicExported));
        MatterReportingAttributeChangeCallback(endpointId, ElectricalEnergyMeasurement::Id,
                                               ElectricalEnergyMeasurement::Attributes::PeriodicEnergyImported::Id);
        MatterReportingAttributeChangeCallback(endpointId, ElectricalEnergyMeasurement::Id,
                                               ElectricalEnergyMeasurement::Attributes::PeriodicEnergyExported::Id);
    }
    else
    {
        const Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> noExportedEnergy;
        NotifyCumulativeEnergyMeasured(endpointId, MakeOptional(cumulativeImported), noExportedEnergy);
        MatterReportingAttributeChangeCallback(endpointId, ElectricalEnergyMeasurement::Id,
                                               ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Id);
        NotifyPeriodicEnergyMeasured(endpointId, MakeOptional(periodicImported), noExportedEnergy);
        MatterReportingAttributeChangeCallback(endpointId, ElectricalEnergyMeasurement::Id,
                                               ElectricalEnergyMeasurement::Attributes::PeriodicEnergyImported::Id);
    }
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
    // Seed both imported and exported energy: the electrical-meter profile exposes
    // separate importedEnergy and exportedEnergy components per the reviewer's Confluence doc.
    SeedElectricalEnergyValues(kElectricalMeterEndpoint,
                               sMeterCumulativeEnergyImportedMwh, 250'000,
                               sMeterCumulativeEnergyExportedMwh, 60'000);
    return CHIP_NO_ERROR;
}

CHIP_ERROR InitElectricalUtilityMeterEndpoint()
{
    ReturnErrorOnFailure(MeterIdentificationInit(kElectricalUtilityMeterEndpoint));
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

void UpdateEndpointTelemetry(ElectricalMeasurementRuntime & runtime, EndpointId endpoint, int64_t baseVoltageMv,
                             int64_t baseCurrentMa, int64_t basePowerMw, int64_t & cumulativeEnergyMwh,
                             int64_t * cumulativeExportedMwh = nullptr)
{
    if (runtime.epmDelegate == nullptr)
        return;

    int64_t voltage = RandomVariation(baseVoltageMv, 5'000); // ±5 V
    int64_t current = RandomVariation(baseCurrentMa, 500);   // ±0.5 A
    int64_t power   = RandomVariation(basePowerMw, 100'000); // ±100 W

    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetVoltage(Nullable<int64_t>(voltage));
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetActiveCurrent(Nullable<int64_t>(current));
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetActivePower(Nullable<int64_t>(power));
    // SetRMSVoltage/SetRMSCurrent internally call MatterReportingAttributeChangeCallback for
    // attributes 0x000B/0x000C. These are what the ST driver subscribes to for
    // voltageMeasurement (RMSVoltage) and currentMeasurement (RMSCurrent) capabilities.
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetRMSVoltage(Nullable<int64_t>(voltage));
    TEMPORARY_RETURN_IGNORED runtime.epmDelegate->SetRMSCurrent(Nullable<int64_t>(current));

    MatterReportingAttributeChangeCallback(endpoint, ElectricalPowerMeasurement::Id,
                                           ElectricalPowerMeasurement::Attributes::Voltage::Id);
    MatterReportingAttributeChangeCallback(endpoint, ElectricalPowerMeasurement::Id,
                                           ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id);
    MatterReportingAttributeChangeCallback(endpoint, ElectricalPowerMeasurement::Id,
                                           ElectricalPowerMeasurement::Attributes::ActivePower::Id);

    // Accumulate energy: E = P × t / 3600 (mWh)
    int64_t energyIncrementMwh = (power * static_cast<int64_t>(kTelemetryIntervalSec)) / 3600;
    cumulativeEnergyMwh += energyIncrementMwh;

    if (cumulativeExportedMwh != nullptr)
    {
        // Simulate export at ~25% of the import increment (e.g. net metering back-feed)
        int64_t exportIncrementMwh = energyIncrementMwh / 4;
        *cumulativeExportedMwh += exportIncrementMwh;
        SeedElectricalEnergyValues(endpoint, cumulativeEnergyMwh, energyIncrementMwh,
                                   *cumulativeExportedMwh, exportIncrementMwh);
    }
    else
    {
        SeedElectricalEnergyValues(endpoint, cumulativeEnergyMwh, energyIncrementMwh);
    }
}

void TelemetryTimerHandler(chip::System::Layer * /*layer*/, void * /*appState*/)
{
    ChipLogDetail(AppServer,
                  "Phase 2 Telemetry: tick — EP1 sensor cumE=%.1f kWh | EP3 meter import=%.1f kWh export=%.1f kWh",
                  static_cast<double>(sSensorCumulativeEnergyMwh) / 1'000'000.0,
                  static_cast<double>(sMeterCumulativeEnergyImportedMwh) / 1'000'000.0,
                  static_cast<double>(sMeterCumulativeEnergyExportedMwh) / 1'000'000.0);

    UpdateEndpointTelemetry(gElectricalSensor, kElectricalSensorEndpoint, 230'000, 6'500, 1'500'000,
                            sSensorCumulativeEnergyMwh);
    // Pass pointer to export accumulator so EP3 (Electrical Meter) also updates
    // CumulativeEnergyExported — required for the exportedEnergy profile component.
    UpdateEndpointTelemetry(gElectricalMeter, kElectricalMeterEndpoint, 230'000, 8'000, 1'840'000,
                            sMeterCumulativeEnergyImportedMwh, &sMeterCumulativeEnergyExportedMwh);

    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(kTelemetryIntervalSec * 1000),
                                                TelemetryTimerHandler, nullptr);
}

} // namespace

void emberAfElectricalEnergyMeasurementClusterInitCallback(chip::EndpointId endpointId)
{
    auto * slot = GetElectricalEnergyMeasurementSlot(endpointId);
    VerifyOrDie(slot != nullptr);
    VerifyOrDie(*slot == nullptr);

    // Electrical Meter (EP3) must advertise IMPE + EXPE + CUME per the Confluence doc:
    //   CumulativeEnergyImported  requires IMPE + CUME
    //   CumulativeEnergyExported  requires EXPE + CUME  ← spec-gated; needs EXPE feature bit
    //   CumulativeEnergyReset     requires CUME
    // Electrical Sensor (EP1) is import-only: IMPE + CUME + PERE.
    BitMask<ElectricalEnergyMeasurement::Feature, uint32_t> eemFeatures;
    if (endpointId == kElectricalMeterEndpoint)
    {
        eemFeatures.Set(ElectricalEnergyMeasurement::Feature::kImportedEnergy)
                   .Set(ElectricalEnergyMeasurement::Feature::kExportedEnergy)
                   .Set(ElectricalEnergyMeasurement::Feature::kCumulativeEnergy)
                   .Set(ElectricalEnergyMeasurement::Feature::kPeriodicEnergy);
    }
    else
    {
        eemFeatures.Set(ElectricalEnergyMeasurement::Feature::kImportedEnergy)
                   .Set(ElectricalEnergyMeasurement::Feature::kCumulativeEnergy)
                   .Set(ElectricalEnergyMeasurement::Feature::kPeriodicEnergy);
    }

    auto attrAccess = std::make_unique<ElectricalEnergyMeasurementAttrAccess>(
        eemFeatures,
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

    std::srand(static_cast<unsigned>(std::time(nullptr)));
    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(kTelemetryIntervalSec * 1000),
                                                TelemetryTimerHandler, nullptr);
    ChipLogDetail(AppServer, "Phase 2 Energy Simulator: Telemetry timer started (%u sec interval)", kTelemetryIntervalSec);
}

void Phase2EnergySimulatorShutdown()
{
    ChipLogDetail(AppServer, "Phase 2 Energy Simulator: Shutdown");

    chip::DeviceLayer::SystemLayer().CancelTimer(TelemetryTimerHandler, nullptr);

    TEMPORARY_RETURN_IGNORED MeterIdentificationShutdown();
    TEMPORARY_RETURN_IGNORED CommodityMeteringShutdown();
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

EndpointId GetIdentifyEndpointId()
{
    return kElectricalSensorEndpoint;
}

/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
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

#include <MeterIdentificationInstance.h>
#include <app/util/af-types.h>
#include <lib/support/CodeUtils.h>

#include <memory>

using namespace chip::app::Clusters;
using namespace chip::app::Clusters::MeterIdentification;

namespace {
static std::unique_ptr<Instance> gMeterIdentificationCluster;
static chip::EndpointId gMeterIdentificationEndpoint = chip::kInvalidEndpointId;
} // namespace

Instance * MeterIdentification::GetInstance()
{
    return gMeterIdentificationCluster.get();
}

CHIP_ERROR MeterIdentificationInit(chip::EndpointId endpointId)
{
    if (gMeterIdentificationCluster)
    {
        return (gMeterIdentificationEndpoint == endpointId) ? CHIP_NO_ERROR : CHIP_ERROR_INCORRECT_STATE;
    }

    gMeterIdentificationCluster =
        std::make_unique<Instance>(endpointId, chip::BitMask<Feature, uint32_t>(Feature::kPowerThreshold));
    VerifyOrReturnError(gMeterIdentificationCluster != nullptr, CHIP_ERROR_NO_MEMORY);

    CHIP_ERROR err = gMeterIdentificationCluster->Init();
    if (err != CHIP_NO_ERROR)
    {
        gMeterIdentificationCluster.reset();
        return err;
    }

    gMeterIdentificationEndpoint = endpointId;
    return CHIP_NO_ERROR;
}

CHIP_ERROR MeterIdentificationShutdown()
{
    if (gMeterIdentificationCluster)
    {
        gMeterIdentificationCluster->Shutdown();
        gMeterIdentificationCluster.reset(nullptr);
        gMeterIdentificationEndpoint = chip::kInvalidEndpointId;
    }

    return CHIP_NO_ERROR;
}

void emberAfMeterIdentificationClusterInitCallback(chip::EndpointId endpointId)
{
    VerifyOrDie(MeterIdentificationInit(endpointId) == CHIP_NO_ERROR);
}

void emberAfMeterIdentificationClusterShutdownCallback(chip::EndpointId endpointId)
{
    VerifyOrReturn(gMeterIdentificationEndpoint == endpointId || gMeterIdentificationEndpoint == chip::kInvalidEndpointId);
    TEMPORARY_RETURN_IGNORED MeterIdentificationShutdown();
}

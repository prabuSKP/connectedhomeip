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

#include <AppMain.h>
#include <EnergyManagementAppCmdLineOptions.h>
#include <Identify.h>
#include <Phase2EnergySimulatorMain.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <lib/support/BitMask.h>

#include <cstdlib>
#include <cstring>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::DeviceEnergyManagement;

static uint32_t ParseNumber(const char * pString);
static bool EnergyAppOptionHandler(const char * aProgram, chip::ArgParser::OptionSet * aOptions, int aIdentifier,
                                   const char * aName, const char * aValue);

constexpr uint16_t kOptionFeatureMap = 0xffd1;

static chip::ArgParser::OptionDef sEnergyAppOptionDefs[] = {
    { "featureSet", chip::ArgParser::kArgumentRequired, kOptionFeatureMap }, { nullptr }
};

static chip::ArgParser::OptionSet sCmdLineOptions = { EnergyAppOptionHandler,
                                                      sEnergyAppOptionDefs,
                                                      "PROGRAM OPTIONS",
                                                      "-f, --featureSet <value>\n" };

namespace chip {
namespace app {
namespace Clusters {
namespace DeviceEnergyManagement {

static chip::BitMask<Feature> sFeatureMap(Feature::kPowerAdjustment, Feature::kPowerForecastReporting,
                                          Feature::kStartTimeAdjustment, Feature::kPausable, Feature::kForecastAdjustment,
                                          Feature::kConstraintBasedAdjustment);

chip::BitMask<Feature> GetFeatureMapFromCmdLine()
{
    return sFeatureMap;
}

} // namespace DeviceEnergyManagement
} // namespace Clusters
} // namespace app
} // namespace chip

static uint32_t ParseNumber(const char * pString)
{
    if (strlen(pString) > 2 && pString[0] == '0' && pString[1] == 'x')
    {
        return static_cast<uint32_t>(strtoul(&pString[2], nullptr, 16));
    }

    return static_cast<uint32_t>(strtoul(pString, nullptr, 10));
}

void ApplicationInit()
{
    ChipLogDetail(AppServer, "Phase 2 Energy Simulator: ApplicationInit()");
    SuccessOrDie(IdentifyInit());
    Phase2EnergySimulatorInit();
}

void ApplicationShutdown()
{
    ChipLogDetail(AppServer, "Phase 2 Energy Simulator: ApplicationShutdown()");
    Phase2EnergySimulatorShutdown();
}

static bool EnergyAppOptionHandler(const char * aProgram, chip::ArgParser::OptionSet * aOptions, int aIdentifier,
                                   const char * aName, const char * aValue)
{
    bool retval = true;

    switch (aIdentifier)
    {
    case kOptionFeatureMap:
        sFeatureMap = BitMask<chip::app::Clusters::DeviceEnergyManagement::Feature>(ParseNumber(aValue));
        ChipLogDetail(Support, "Using FeatureMap 0x%04x", sFeatureMap.Raw());
        break;
    default:
        ChipLogError(Support, "%s: INTERNAL ERROR: Unhandled option: %s\n", aProgram, aName);
        retval = false;
        break;
    }

    return retval;
}

int main(int argc, char * argv[])
{
    if (ChipLinuxAppInit(argc, argv, &sCmdLineOptions) != 0)
    {
        return -1;
    }

    ChipLinuxAppMainLoop();
    return 0;
}

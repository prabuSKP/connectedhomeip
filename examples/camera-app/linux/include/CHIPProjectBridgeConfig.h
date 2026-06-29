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

#pragma once

// Inherit all camera-app settings (camera clusters, feature flags, etc.)
// then override bridge-specific values below.
#include <CHIPProjectAppConfig.h>

// ---------------------------------------------------------------------------
// Bridge / Aggregator overrides
// ---------------------------------------------------------------------------

// Advertise as Aggregator (0x000E): the node is a Matter bridge whose bridged
// camera endpoints live under an Aggregator endpoint.  Commissioning via QR /
// manual pairing code works regardless of this hint; it tells the controller to
// enumerate the bridged devices and create a card per camera.
#undef CHIP_DEVICE_CONFIG_DEVICE_TYPE
#define CHIP_DEVICE_CONFIG_DEVICE_TYPE 0x000E // Aggregator / Bridge

#undef CHIP_DEVICE_CONFIG_DEVICE_NAME
#define CHIP_DEVICE_CONFIG_DEVICE_NAME "ONVIF Camera Bridge"

// Maximum number of bridged cameras.  Each occupies one dynamic endpoint slot.
// Raise if more than 16 cameras are needed; lower to reduce RAM on constrained hubs.
#undef CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT
#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT 16

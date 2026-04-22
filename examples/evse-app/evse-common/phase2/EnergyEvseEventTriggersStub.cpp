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

#include <cstdint>

// The phase2 simulator reuses the shared Linux app-main layer, which registers
// the Energy EVSE test-event handler when that trigger family is enabled.
// This binary does not host the EVSE app stack, so these triggers are treated
// as unsupported instead of linking in the full EVSE implementation.
bool HandleEnergyEvseTestEventTrigger(uint64_t)
{
    return false;
}

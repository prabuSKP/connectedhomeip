# Phase 2 Virtual Energy Simulator Extension Plan

## Overview
This document outlines a comprehensive plan to extend the Phase 2 Virtual Energy Simulator to support all electrical device types and create a dedicated testing script similar to `scripts/tools/phase2_energy_simulator_smoke.sh`.

## Current State Analysis

### Supported Device Types in Phase 2 Virtual Energy Simulator
1. **Endpoint 0**: Root Node Device (MA-rootdevice)
2. **Endpoint 1**: Electrical Sensor (MA-electricalsensor)
3. **Endpoint 2**: Device Energy Management
4. **Endpoint 3**: Electrical Meter (MA-electrical-meter)
5. **Endpoint 4**: Electrical Utility Meter (MA-electrical-utility-meter)

### Supported Clusters
- Descriptor
- Basic Information
- Electrical Power Measurement
- Electrical Energy Measurement
- Power Topology
- Device Energy Management
- Device Energy Management Mode
- Commodity Metering
- Meter Identification

### Missing Clusters
- Commodity Price
- Commodity Tariff
- Energy EVSE
- Energy EVSE Mode

## Extension Plan

### 1. Add Commodity Price Support
**Objective**: Add support for the Commodity Price cluster (cluster ID: 149) to enable pricing information for energy commodities.

**Implementation Steps**:
1. Add a new endpoint for Commodity Price device type
2. Configure the Commodity Price cluster with required attributes:
   - TariffUnit
   - Currency
   - CurrentPrice
   - PriceForecast
3. Implement event handling for PriceChange events
4. Add command support for GetDetailedPriceRequest/Response

### 2. Add Commodity Tariff Support
**Objective**: Add support for the Commodity Tariff cluster (cluster ID: 1792) to enable tariff information management.

**Implementation Steps**:
1. Add a new endpoint for Commodity Tariff device type
2. Configure the Commodity Tariff cluster with required attributes:
   - TariffInfo
   - TariffUnit
   - StartDate
   - DayEntries
   - DayPatterns
   - CalendarPeriods
   - IndividualDays
   - CurrentDay
   - NextDay
   - CurrentDayEntry
   - CurrentDayEntryDate
   - NextDayEntry
   - NextDayEntryDate
   - TariffComponents
   - TariffPeriods
   - CurrentTariffComponents
   - NextTariffComponents
   - DefaultRandomizationOffset
   - DefaultRandomizationType
3. Implement command support for GetTariffComponent and GetDayEntry

### 3. Add Energy EVSE Support
**Objective**: Add support for the Energy EVSE (Electric Vehicle Supply Equipment) cluster (cluster ID: 153) to simulate EV charging stations.

**Implementation Steps**:
1. Add a new endpoint for Energy EVSE device type
2. Configure the Energy EVSE cluster with required attributes:
   - State
   - SupplyState
   - FaultState
   - ChargingTime
   - DischargingTime
   - SessionID
   - SessionDuration
   - ChargingCurrent
   - ChargingCurrentPhase1
   - ChargingCurrentPhase2
   - ChargingCurrentPhase3
   - DischargingCurrent
   - DischargingCurrentPhase1
   - DischargingCurrentPhase2
   - DischargingCurrentPhase3
   - SessionEnergyCharged
   - SessionEnergyDischarged
   - LifetimeEnergyCharged
   - LifetimeEnergyDischarged
   - Temperature
   - Setpoint
   - RandomizationDelay
   - ActivePower
   - RMSVoltage
   - RMSCurrent
3. Implement command support for Disable and EnableCharging

### 4. Add Energy EVSE Mode Support
**Objective**: Add support for the Energy EVSE Mode cluster (cluster ID: 157) to enable mode selection for EVSE devices.

**Implementation Steps**:
1. Add the Energy EVSE Mode cluster to the Energy EVSE endpoint
2. Configure the cluster with required attributes:
   - SupportedModes
   - CurrentMode
3. Implement command support for ChangeToMode

## New Device Types to Add

### 1. Electrical Energy Tariff Device
**Device Type ID**: 1299 (MA-electrical-energy-tariff)
**Purpose**: Device that provides energy tariff information
**Required Clusters**:
- Commodity Price (Server)
- Commodity Tariff (Server)
- Meter Identification (Server)
- Commodity Metering (Server)
- Electrical Grid Conditions (Server)

### 2. Energy EVSE Device
**Device Type ID**: 1292 (Energy EVSE)
**Purpose**: Device that simulates an electric vehicle charging station
**Required Clusters**:
- Energy EVSE (Server)
- Energy EVSE Mode (Server)
- Device Energy Management (Server)
- Device Energy Management Mode (Server)
- Electrical Power Measurement (Server)
- Electrical Energy Measurement (Server)

## Implementation Approach

### 1. ZAP Configuration Updates
1. Modify `examples/evse-app/evse-common/phase2-energy-simulator.zap` to include new endpoints
2. Add new device types and their associated clusters
3. Configure all required attributes with appropriate default values
4. Enable all necessary commands and events

### 2. Code Generation
1. Run ZAP code generation tool to update auto-generated source files:
   ```bash
   ./scripts/tools/zap/generate.py examples/evse-app/evse-common/phase2-energy-simulator.zap -o examples/evse-app/evse-common/
   ```

### 3. Application Logic Implementation
1. Update the main application to handle new device types
2. Implement simulation logic for each new cluster
3. Add appropriate data structures for storing cluster state
4. Implement command handlers for new clusters

### 4. Build Process
1. Rebuild the Phase 2 Virtual Energy Simulator:
   ```bash
   ./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build
   ```

## Comprehensive Testing Script Design

### Script Name
`scripts/tools/phase2_energy_simulator_comprehensive_test.sh`

### Script Features
1. **Modular Design**: Separate functions for each device type testing
2. **Configurable Parameters**: Support for custom node IDs, passcodes, discriminators
3. **Detailed Reporting**: Comprehensive output with PASS/FAIL status for each test
4. **Flexible Execution**: Options to run specific test suites or all tests
5. **Error Handling**: Robust error handling with meaningful error messages
6. **Logging**: Detailed logging to file for debugging purposes

### Command Line Options
```
--node-id <id>              Node ID to use (default: 0x12344321)
--passcode <code>           Setup passcode (default: 20202021)
--discriminator <disc>      Setup discriminator (default: 3840)
--pairing-mode <mode>       Pairing mode: onnetwork-long | onnetwork | code | skip
--recommission              Unpair the node before pairing
--test-suite <suite>        Run specific test suite (all, electrical-sensor, dem, electrical-meter, utility-meter, commodity-price, commodity-tariff, evse)
--verbose                   Enable verbose output
--log-file <file>           Log file path (default: /tmp/phase2-comprehensive-test.log)
--timeout <seconds>         Timeout for each test (default: 30)
--storage-directory <path>  chip-tool storage directory (default: /tmp/chip-tool-phase2-comprehensive)
-h, --help                  Show help
```

### Test Suites

#### 1. Device Discovery Tests
- Verify all endpoints are discoverable
- Check device type lists for each endpoint
- Validate server lists for each endpoint

#### 2. Electrical Sensor Tests
- Read PowerMode attribute
- Read ActivePower attribute
- Read Voltage attribute
- Read ActiveCurrent attribute
- Read CumulativeEnergyImported attribute
- Subscribe to ActivePower changes

#### 3. Device Energy Management Tests
- Read ESAState attribute
- Read SupportedModes attribute
- Read CurrentMode attribute
- Test ChangeToMode command
- Verify Device Energy Management Mode cluster functionality

#### 4. Electrical Meter Tests
- Read ActivePower attribute
- Read CumulativeEnergyImported attribute
- Read MeteredQuantity attribute
- Verify Commodity Metering cluster functionality

#### 5. Electrical Utility Meter Tests
- Read MeterSerialNumber attribute
- Read MeterType attribute
- Read PointOfDelivery attribute
- Verify Meter Identification cluster functionality

#### 6. Commodity Price Tests (New)
- Read TariffUnit attribute
- Read Currency attribute
- Read CurrentPrice attribute
- Read PriceForecast attribute
- Test GetDetailedPriceRequest command
- Verify PriceChange event handling

#### 7. Commodity Tariff Tests (New)
- Read TariffInfo attribute
- Read TariffUnit attribute
- Read StartDate attribute
- Read CurrentDay attribute
- Read CurrentDayEntry attribute
- Test GetTariffComponent command
- Test GetDayEntry command

#### 8. Energy EVSE Tests (New)
- Read State attribute
- Read SupplyState attribute
- Read FaultState attribute
- Read ChargingTime attribute
- Read SessionEnergyCharged attribute
- Test EnableCharging command
- Test Disable command
- Verify Energy EVSE Mode cluster functionality

### Script Structure

```bash
#!/usr/bin/env bash

# Global variables
SCRIPT_NAME="$(basename "$0")"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

CHIP_TOOL="${REPO_ROOT}/out/linux-x64-chip-tool/chip-tool"
STORAGE_DIR="/tmp/chip-tool-phase2-comprehensive"
LOG_FILE="/tmp/phase2-comprehensive-test.log"

# Default parameters
NODE_ID="0x12344321"
PASSCODE="20202021"
DISCRIMINATOR="3840"
PAIRING_MODE="onnetwork-long"
RECOMMISSION=0
TEST_SUITE="all"
VERBOSE=0
TIMEOUT=30

# Test counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
TEST_NUMBER=0

# Function definitions
print_usage() {
    # Implementation
}

log() {
    # Implementation
}

verbose_log() {
    # Implementation
}

run_chip_tool() {
    # Implementation
}

run_test() {
    # Implementation
}

pair_device() {
    # Implementation
}

# Test suite functions
run_device_discovery_tests() {
    # Implementation
}

run_electrical_sensor_tests() {
    # Implementation
}

run_device_energy_management_tests() {
    # Implementation
}

run_electrical_meter_tests() {
    # Implementation
}

run_electrical_utility_meter_tests() {
    # Implementation
}

run_commodity_price_tests() {
    # Implementation
}

run_commodity_tariff_tests() {
    # Implementation
}

run_evse_tests() {
    # Implementation
}

# Main execution
main() {
    # Parse command line arguments
    # Initialize test environment
    # Run selected test suites
    # Report results
}

main "$@"
```

## Benefits of This Extension

1. **Complete Coverage**: Support for all electrical device types and clusters
2. **Comprehensive Testing**: Detailed test script covering all functionality
3. **Modular Design**: Easy to extend and maintain
4. **Industry Standard Compliance**: Adherence to Matter specification
5. **Developer Friendly**: Clear documentation and examples
6. **Robust Testing**: Comprehensive error handling and reporting

## Timeline and Milestones

### Phase 1: Planning and Design (1 week)
- Finalize device type and cluster requirements
- Design ZAP configuration updates
- Create detailed implementation plan

### Phase 2: Implementation (2 weeks)
- Update ZAP configuration files
- Generate code and implement application logic
- Build and test basic functionality

### Phase 3: Testing Script Development (1 week)
- Develop comprehensive testing script
- Implement all test suites
- Validate script functionality

### Phase 4: Integration and Validation (1 week)
- Full integration testing
- Documentation updates
- Final validation

## Conclusion
This extension plan will significantly enhance the Phase 2 Virtual Energy Simulator by adding support for all electrical device types and clusters, along with a comprehensive testing script. This will provide developers with a complete virtual testing environment for energy management applications, enabling better development and validation of Matter-based energy solutions.
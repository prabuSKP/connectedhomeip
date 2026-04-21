# Phase 2 Virtual Energy Simulator: Endpoints, Device Types, and Chip-Tool Command Usage Guide

## Overview

The Phase 2 Virtual Energy Simulator is a Matter-based virtual device that simulates various electrical energy management components. It runs on Linux and can be used for testing without physical hardware. The simulator has 5 endpoints, each with specific device types and cluster support.

## Endpoints and Device Types

### Endpoint 0: Root Node Device
- **Device Type**: Root Node (MA-rootdevice)
- **Purpose**: This is the base endpoint that all Matter devices have. It provides core device information and management capabilities.

#### Key Clusters:
1. **Descriptor**: Provides device type and cluster information
2. **Basic Information**: Device identification (vendor, product, serial number, etc.)
3. **Access Control**: Security and access permissions
4. **General Commissioning**: Device setup and commissioning
5. **Network Commissioning**: Network connectivity configuration
6. **Operational Credentials**: Security certificate management
7. **OTA Software Update Provider/Requestor**: Firmware update capabilities

### Endpoint 1: Electrical Sensor
- **Device Type**: Electrical Sensor (MA-electricalsensor)
- **Purpose**: Simulates an electrical sensor that measures power and energy consumption parameters.

#### Key Clusters:
1. **Electrical Power Measurement**: Real-time power measurements (voltage, current, active power, etc.)
2. **Electrical Energy Measurement**: Energy consumption tracking (cumulative and periodic)
3. **Power Topology**: Power connection information
4. **Identify**: Device identification (used for commissioning)

#### Key Attributes:
- **PowerMode**: Indicates the current power mode (AC, DC, etc.)
- **ActivePower**: Real-time active power consumption in milliwatts
- **Voltage**: Voltage measurement in millivolts
- **ActiveCurrent**: Current measurement in milliamperes
- **CumulativeEnergyImported**: Total energy consumed over time

### Endpoint 2: Device Energy Management
- **Device Type**: Device Energy Management
- **Purpose**: Simulates a device that can manage energy consumption, such as adjusting power usage based on grid conditions.

#### Key Clusters:
1. **Device Energy Management**: Controls and manages energy consumption (ESA state, power adjustment capabilities)
2. **Device Energy Management Mode**: Different operational modes for energy management
3. **Identify**: Device identification

#### Key Attributes:
- **ESAState**: Current energy state of the device (offline, online, etc.)
- **SupportedModes**: List of supported operational modes
- **CurrentMode**: Currently active mode

### Endpoint 3: Electrical Meter
- **Device Type**: Electrical Meter (MA-electrical-meter)
- **Purpose**: Simulates an electrical meter that measures power and energy consumption with additional commodity tracking.

#### Key Clusters:
1. **Electrical Power Measurement**: Real-time power measurements
2. **Electrical Energy Measurement**: Energy consumption tracking
3. **Commodity Metering**: Tracks metered quantities of commodities (electricity, gas, water, etc.)
4. **Identify**: Device identification

#### Key Attributes:
- **MeteredQuantity**: Array of measured commodity quantities
- **MeteredQuantityTimestamp**: Timestamp of the last measurement

### Endpoint 4: Electrical Utility Meter
- **Device Type**: Electrical Utility Meter (MA-electrical-utility-meter)
- **Purpose**: Simulates a utility meter with identification information.

#### Key Clusters:
1. **Meter Identification**: Meter information (serial number, type, point of delivery)
2. **Identify**: Device identification

#### Key Attributes:
- **MeterSerialNumber**: Unique serial number of the meter
- **MeterType**: Type of meter (electric, gas, water, etc.)
- **PointOfDelivery**: Location where the meter is installed

## Chip-Tool Command Usage Guide

Chip-tool is a command-line utility that allows you to interact with Matter devices for commissioning, reading/writing attributes, and sending commands.

### Basic Command Structure
The general structure of chip-tool commands is:
```
./out/linux-x64-chip-tool/chip-tool [cluster_name] [command_name] [parameters]
```

### Commissioning Commands

#### Pairing with onnetwork-long
This command commissions a device that is already on the network:
```
./out/linux-x64-chip-tool/chip-tool pairing onnetwork-long node-id setup-pin-code discriminator [optional-parameters]
```

**Required Parameters:**
- `node-id`: 64-bit identifier to assign to the device (e.g., 0x12344321)
- `setup-pin-code`: The PIN code for the device (e.g., 20202021)
- `discriminator`: 12-bit device identifier for commissioning (e.g., 3840)

**Common Optional Parameters:**
- `--storage-directory`: Directory to store chip-tool's data (default: /tmp)
- `--commissioner-nodeid`: Node ID for chip-tool itself (default: 112233)
- `--timeout`: Timeout in seconds for the command

**Example:**
```bash
./out/linux-x64-chip-tool/chip-tool pairing onnetwork-long 0x12344321 20202021 3840 --storage-directory /tmp/chip-tool-data
```

#### Unpairing a Device
Removes a device from the commissioned list:
```
./out/linux-x64-chip-tool/chip-tool pairing unpair node-id [optional-parameters]
```

**Example:**
```bash
./out/linux-x64-chip-tool/chip-tool pairing unpair 0x12344321
```

### Reading Attributes

#### Basic Read Command
Reads an attribute from a specific cluster on a device:
```
./out/linux-x64-chip-tool/chip-tool [cluster-name] read [attribute-name] destination-id endpoint-ids [optional-parameters]
```

**Required Parameters:**
- `cluster-name`: Name of the cluster (e.g., electricalpowermeasurement)
- `attribute-name`: Name of the attribute to read (e.g., active-power)
- `destination-id`: 64-bit node identifier of the device (e.g., 0x12344321)
- `endpoint-ids`: Endpoint number(s) to read from (e.g., 1)

**Common Optional Parameters:**
- `--storage-directory`: Directory for chip-tool data storage
- `--fabric-filtered`: Boolean for fabric-filtered reads (default: true)
- `--timeout`: Timeout in seconds for the command
- `--trace_decode`: Enable human-readable trace output

**Example:**
```bash
# Read active power from endpoint 1 of device 0x12344321
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read active-power 0x12344321 1 --trace_decode 1

# Read meter serial number from endpoint 4
./out/linux-x64-chip-tool/chip-tool meteridentification read meter-serial-number 0x12344321 4
```

### Subscribing to Attributes

#### Subscribe Command
Subscribes to attribute changes for real-time updates:
```
./out/linux-x64-chip-tool/chip-tool [cluster-name] subscribe [attribute-name] min-interval max-interval destination-id endpoint-ids [optional-parameters]
```

**Required Parameters:**
- `min-interval`: Minimum seconds between reports
- `max-interval`: Maximum seconds between reports
- All other parameters same as read command

**Additional Optional Parameters:**
- `--keepSubscriptions`: Keep existing subscriptions (default: false)
- `--auto-resubscribe`: Automatically resubscribe if connection lost (default: false)

**Example:**
```bash
# Subscribe to active power changes (1-60 seconds reporting interval)
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement subscribe active-power 1 60 0x12344321 1 --keepSubscriptions true
```

### Writing Attributes

#### Write Command
Writes a value to an attribute:
```
./out/linux-x64-chip-tool/chip-tool [cluster-name] write [attribute-name] [value] destination-id endpoint-ids [optional-parameters]
```

**Example:**
```bash
# Write node label (example)
./out/linux-x64-chip-tool/chip-tool basicinformation write node-label "Test Device" 0x12344321 0
```

### Common Optional Parameters (All Commands)

#### Security and Certification
- `--paa-trust-store-path`: Path to Product Attestation Authority certificates
- `--cd-trust-store-path`: Path to Certificate Declaration certificates
- `--use-max-sized-certs`: Use maximum-sized operational certificates
- `--only-allow-trusted-cd-keys`: Only allow trusted CD verifying keys
- `--bypass-attestation-verifier`: Bypass attestation verification

#### Commissioning Parameters
- `--commissioner-name`: Fabric name ("alpha", "beta", "gamma", or integer ≥4)
- `--commissioner-nodeid`: Node ID for chip-tool
- `--commissioner-vendor-id`: Vendor ID for chip-tool
- `--case-auth-tags`: CATs to encode in NOC

#### Tracing and Debugging
- `--trace_file`: Enable trace file output
- `--trace_log`: Enable trace log output
- `--trace_decode`: Enable human-readable trace decoding
- `--trace-to`: Trace destinations (json:log, json:<path>, perfetto, perfetto:<path>)

#### Storage and Session
- `--storage-directory`: Directory for chip-tool storage files
- `--ble-controller`: BLE controller selector
- `--allow-large-payload`: Allow large payloads (requires TCP)

#### ICD (Intermittently Connected Device) Parameters
- `--icd-registration`: Register for ICD check-ins
- `--icd-check-in-nodeid`: Check-in node ID for ICD
- `--icd-symmetric-key`: 16-byte ICD symmetric key

## Practical Examples from Smoke Test

### Device Discovery and Commissioning
```bash
# Commission the device
./out/linux-x64-chip-tool/chip-tool pairing onnetwork-long 0x12344321 20202021 3840

# Verify endpoints
./out/linux-x64-chip-tool/chip-tool descriptor read parts-list 0x12344321 0
./out/linux-x64-chip-tool/chip-tool descriptor read server-list 0x12344321 1
```

### Reading Electrical Measurements
```bash
# Read active power from electrical sensor (endpoint 1)
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read active-power 0x12344321 1

# Read cumulative energy imported
./out/linux-x64-chip-tool/chip-tool electricalenergymeasurement read cumulative-energy-imported 0x12344321 1

# Read commodity metered quantity
./out/linux-x64-chip-tool/chip-tool commoditymetering read metered-quantity 0x12344321 3

# Read meter serial number
./out/linux-x64-chip-tool/chip-tool meteridentification read meter-serial-number 0x12344321 4
```

### Subscribing to Changes
```bash
# Subscribe to active power changes
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement subscribe active-power 5 300 0x12344321 1
```

## Error Handling
Chip-tool returns specific error codes:
- `0x0000002F`: Invalid arguments or usage
- Connection errors: Device unreachable or commissioning issues
- Timeout errors: Command didn't complete in specified time

Use `--timeout` parameter to adjust command timeout values as needed.

## Best Practices
1. Always specify `--storage-directory` to keep data organized
2. Use `--trace_decode 1` for human-readable output during debugging
3. Set appropriate min/max intervals for subscriptions based on your needs
4. Use fabric-filtered reads unless you specifically need all-fabric data
5. Clean up old commissioning data with `pairing unpair` when needed

## Running the Simulator

### Building the Simulator
```bash
# Build the Phase 2 Virtual Energy Simulator
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build

# Build chip-tool
./scripts/build/build_examples.py --target linux-x64-chip-tool build
```

### Running the Simulator
```bash
# Run the simulator
./out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app \
  --discriminator 3840 \
  --passcode 20202021 \
  --secured-device-port 5540 \
  --KVS /tmp/chip-phase2-kvs \
  --enable-key 000102030405060708090a0b0c0d0e0f
```

### Running the Smoke Test
```bash
# Run the automated smoke test
./scripts/tools/phase2_energy_simulator_smoke.sh \
  --recommission \
  --pair-timeout 30 \
  --check-timeout 5 \
  --response-lines 6
```

This comprehensive guide provides all the information needed to understand the Phase 2 Virtual Energy Simulator's endpoints and device types, as well as how to use chip-tool to interact with the simulator for testing and development purposes.
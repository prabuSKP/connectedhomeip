# Phase 2 Virtual Energy Simulator Extension

## Overview
This document describes the Phase 2 Virtual Energy Simulator that exposes the
electrical device types supported by the `phase2-energy-simulator.zap` data
model, along with a comprehensive testing script that covers every cluster on
every endpoint.

## Supported Device Types and Endpoints

The Phase 2 Virtual Energy Simulator publishes the following endpoints:

| Endpoint | Device type                       | Server clusters                                                                                   |
|---------:|-----------------------------------|---------------------------------------------------------------------------------------------------|
| 0        | MA-rootdevice (+ OTA requestor)   | Descriptor, Access Control, Basic Information, OTA Requestor, and other standard root clusters   |
| 1        | MA-electricalsensor               | Identify, Descriptor, ElectricalPowerMeasurement, ElectricalEnergyMeasurement, PowerTopology      |
| 2        | Device Energy Management (1293)   | Identify, Descriptor, DeviceEnergyManagement, DeviceEnergyManagementMode                          |
| 3        | MA-electrical-meter               | Identify, Descriptor, ElectricalPowerMeasurement, ElectricalEnergyMeasurement, CommodityMetering  |
| 4        | MA-electrical-utility-meter       | Identify, Descriptor, MeterIdentification                                                         |

## Extending to Additional Device Types

The plan document (`phase2_energy_simulator_extension_plan.md`) outlines two
additional endpoints (MA-electrical-energy-tariff and Energy EVSE). Adding them
requires regenerating `phase2-energy-simulator.matter` from the ZAP file with
the ZAP tool:

```bash
./scripts/tools/zap/generate.py examples/evse-app/evse-common/phase2-energy-simulator.zap \
    -o examples/evse-app/evse-common/
```

Once the matter file contains the new endpoints (and the corresponding entries
are added back to `Phase2EnergySimulatorMain.cpp` / the build graph), the
Commodity Price, Commodity Tariff, Energy EVSE, and Energy EVSE Mode clusters
can be re-enabled. The ZAP regeneration step has not been run in this change
because `zap-cli` is not available in the current build environment; the ZAP
and matter files are therefore kept in lockstep at five endpoints so that the
simulator compiles cleanly.

## Implementation Files

- **ZAP configuration**: `examples/evse-app/evse-common/phase2-energy-simulator.zap`
- **Matter IDL**: `examples/evse-app/evse-common/phase2-energy-simulator.matter`
- **Application entry point**: `examples/evse-app/evse-common/src/Phase2EnergySimulatorMain.cpp`
- **Linux main**: `examples/evse-app/linux/phase2_main.cpp`
- **Build graph**: `examples/evse-app/evse-common/phase2/BUILD.gn`, `examples/evse-app/linux/BUILD.gn`
- **Smoke test script**: `scripts/tools/phase2_energy_simulator_smoke.sh`
- **Comprehensive test script**: `scripts/tools/phase2_energy_simulator_comprehensive_test.sh`
- **Extension plan**: `phase2_energy_simulator_extension_plan.md`

## Building the Simulator

```bash
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build
```

## Running the Simulator

```bash
./out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app \
  --discriminator 3840 \
  --passcode 20202021 \
  --secured-device-port 5540 \
  --KVS /tmp/chip-phase2-kvs \
  --enable-key 000102030405060708090a0b0c0d0e0f
```

## Comprehensive Test Script

`scripts/tools/phase2_energy_simulator_comprehensive_test.sh` exercises every
cluster on every endpoint exposed by the simulator. Each test is reported with
PASS/FAIL status and totals are printed at the end.

### Test suites

| Suite                 | Target endpoint | Cluster(s) exercised                                              |
|-----------------------|-----------------|-------------------------------------------------------------------|
| device-discovery      | all             | Descriptor `parts-list`, `server-list`, `device-type-list`        |
| basic-information     | 0               | Basic Information attributes (vendor/product/software/unique id)  |
| electrical-sensor     | 1               | ElectricalPowerMeasurement, ElectricalEnergyMeasurement           |
| power-topology        | 1               | PowerTopology attributes                                          |
| dem                   | 2               | DeviceEnergyManagement attributes                                 |
| dem-mode              | 2               | DeviceEnergyManagementMode attributes                             |
| electrical-meter      | 3               | ElectricalPowerMeasurement, ElectricalEnergyMeasurement           |
| commodity-metering    | 3               | CommodityMetering attributes                                      |
| utility-meter         | 4               | Descriptor layout for the utility meter device type               |
| meter-identification  | 4               | MeterIdentification attributes                                    |

The `all` suite (default) runs every suite above in order.

### Usage examples

Run every suite:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh
```

Run with recommissioning:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh --recommission
```

Run a single suite:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh --test-suite commodity-metering
```

Run with custom identity:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --node-id 0x12345678 --passcode 30303030 --discriminator 3841
```

### Command-line options

```
--node-id <id>              Node ID to use (default: 0x12344321)
--passcode <code>           Setup passcode (default: 20202021)
--discriminator <disc>      Setup discriminator (default: 3840)
--pairing-mode <mode>       Pairing mode: onnetwork-long | onnetwork | code | skip
--qr-payload <payload>      QR payload / manual code for --pairing-mode code
--recommission              Unpair the node before pairing
--test-suite <suite>        Run specific test suite (see table above)
--verbose                   Enable verbose output
--log-file <file>           Log file path (default: /tmp/phase2-comprehensive-test.log)
--timeout <seconds>         Timeout for each test (default: 30)
--storage-directory <path>  chip-tool storage directory (default: /tmp/chip-tool-phase2-comprehensive)
-h, --help                  Show help
```

## Integration with Existing Tools

- `scripts/tools/phase2_energy_simulator_smoke.sh` remains a lightweight smoke
  check that also covers the five endpoints above.
- `chip-tool` commands against the simulator are unchanged.
- The build process is unchanged aside from the smaller dependency graph for
  `chip-phase2-energy-simulator-app`.

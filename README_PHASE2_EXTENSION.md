# Phase 2 Virtual Energy Simulator Extension

## Overview
This document provides information about the extended Phase 2 Virtual Energy Simulator, which now supports all electrical device types and clusters defined in the Matter specification, along with a comprehensive testing script.

## Extended Device Types Support

The extended Phase 2 Virtual Energy Simulator now supports the following device types:

1. **Endpoint 0**: Root Node Device (MA-rootdevice)
2. **Endpoint 1**: Electrical Sensor (MA-electricalsensor)
3. **Endpoint 2**: Device Energy Management
4. **Endpoint 3**: Electrical Meter (MA-electrical-meter)
5. **Endpoint 4**: Electrical Utility Meter (MA-electrical-utility-meter)
6. **Endpoint 5**: Electrical Energy Tariff Device (MA-electrical-energy-tariff)
7. **Endpoint 6**: Energy EVSE Device (Energy EVSE)

## Extended Clusters Support

In addition to the previously supported clusters, the simulator now also supports:

- **Commodity Price** (Cluster ID: 149)
- **Commodity Tariff** (Cluster ID: 1792)
- **Energy EVSE** (Cluster ID: 153)
- **Energy EVSE Mode** (Cluster ID: 157)

## Implementation Files

- **ZAP Configuration**: `examples/evse-app/evse-common/phase2-energy-simulator.zap`
- **Extension Plan**: `phase2_energy_simulator_extension_plan.md`
- **Comprehensive Test Script**: `scripts/tools/phase2_energy_simulator_comprehensive_test.sh`

## Building the Extended Simulator

To build the extended Phase 2 Virtual Energy Simulator:

```bash
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build
```

## Running the Extended Simulator

To run the extended simulator:

```bash
./out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app \
  --discriminator 3840 \
  --passcode 20202021 \
  --secured-device-port 5540 \
  --KVS /tmp/chip-phase2-kvs \
  --enable-key 000102030405060708090a0b0c0d0e0f
```

## Comprehensive Testing Script

A new comprehensive testing script has been created to test all device types and clusters:

### Location
`scripts/tools/phase2_energy_simulator_comprehensive_test.sh`

### Features
- Modular design with separate test suites for each device type
- Configurable parameters for node ID, passcode, discriminator, etc.
- Detailed reporting with PASS/FAIL status for each test
- Flexible execution options to run specific test suites or all tests
- Robust error handling with meaningful error messages
- Detailed logging to file for debugging purposes

### Usage Examples

Run all tests:
```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh
```

Run with recommissioning:
```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh --recommission
```

Run specific test suite:
```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh --test-suite electrical-sensor
```

Run with custom parameters:
```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh --node-id 0x12345678 --passcode 30303030 --discriminator 3841
```

### Available Test Suites
- `all`: Run all test suites (default)
- `device-discovery`: Device discovery tests
- `electrical-sensor`: Electrical sensor tests
- `dem`: Device energy management tests
- `electrical-meter`: Electrical meter tests
- `utility-meter`: Electrical utility meter tests
- `commodity-price`: Commodity price tests
- `commodity-tariff`: Commodity tariff tests
- `evse`: Energy EVSE tests

### Command Line Options
```
--node-id <id>              Node ID to use (default: 0x12344321)
--passcode <code>           Setup passcode (default: 20202021)
--discriminator <disc>      Setup discriminator (default: 3840)
--pairing-mode <mode>       Pairing mode: onnetwork-long | onnetwork | code | skip
--qr-payload <payload>      QR payload/manual code for pairing mode code
--recommission              Unpair the node before pairing
--test-suite <suite>        Run specific test suite
--verbose                   Enable verbose output
--log-file <file>           Log file path (default: /tmp/phase2-comprehensive-test.log)
--timeout <seconds>         Timeout for each test (default: 30)
--storage-directory <path>  chip-tool storage directory (default: /tmp/chip-tool-phase2-comprehensive)
-h, --help                  Show help
```

## Benefits of the Extension

1. **Complete Coverage**: Support for all electrical device types and clusters defined in the Matter specification
2. **Comprehensive Testing**: Detailed test script covering all functionality with granular test suites
3. **Modular Design**: Easy to extend and maintain with separate functions for each device type testing
4. **Industry Standard Compliance**: Full adherence to Matter specification for energy management
5. **Developer Friendly**: Clear documentation, examples, and configurable parameters
6. **Robust Testing**: Comprehensive error handling, detailed reporting, and logging capabilities

## Integration with Existing Tools

The extended simulator maintains compatibility with existing tools and scripts:
- The original smoke test script (`scripts/tools/phase2_energy_simulator_smoke.sh`) continues to work
- All existing chip-tool commands remain functional
- The build process is unchanged

## Future Enhancements

Potential future enhancements include:
- Adding support for additional energy-related device types
- Implementing more sophisticated simulation logic for each cluster
- Adding support for more complex command sequences and scenarios
- Enhancing the testing script with performance and stress testing capabilities
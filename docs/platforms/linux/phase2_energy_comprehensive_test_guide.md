# Phase 2 Energy Simulator Comprehensive Test Guide

This guide explains how to run the full scripted validation for the Linux Phase 2
virtual energy simulator.

Script path:

- `scripts/tools/phase2_energy_simulator_comprehensive_test.sh`

## 1. What This Script Does

The comprehensive script is larger than the smoke test. It:

1. Pairs the simulator with `chip-tool`.
2. Runs grouped read checks across all exposed Phase 2 endpoints.
3. Prints each request and a focused response summary.
4. Returns a non-zero exit code if any test fails.

If you run:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite commodity-metering
```

you will only run the Commodity Metering suite, which contains 5 checks.

If you run:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite all
```

you will run the complete scripted coverage, which currently contains 49 checks.

## 2. Prerequisites

Build the simulator and `chip-tool`:

```bash
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build
./scripts/build/build_examples.py --target linux-x64-chip-tool build
```

## 3. Start The Simulator

In terminal 1:

```bash
cd /home/prabu/Desktop/connectedhomeip
source scripts/activate.sh -p linux

pkill -f chip-phase2-energy-simulator-app || true
rm -f /tmp/chip-phase2-kvs

./out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app \
  --discriminator 3840 \
  --passcode 20202021 \
  --secured-device-port 5540 \
  --KVS /tmp/chip-phase2-kvs \
  --enable-key 000102030405060708090a0b0c0d0e0f
```

Keep this terminal running.

## 4. Run The Complete Comprehensive Test

In terminal 2:

```bash
cd /home/prabu/Desktop/connectedhomeip
source scripts/activate.sh -p linux

rm -rf /tmp/chip-tool-phase2-comprehensive

./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite all
```

This is the command to run the full comprehensive flow.

## 5. Run One Specific Test Suite

To run only one group of checks:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite commodity-metering
```

Available suites:

- `all`
- `device-discovery`
- `basic-information`
- `electrical-sensor`
- `power-topology`
- `dem`
- `dem-mode`
- `electrical-meter`
- `commodity-metering`
- `utility-meter`
- `meter-identification`

## 6. Copy-Paste Commands For Individual Suites

Run these from a second terminal while the simulator is already running.

Common setup:

```bash
cd /home/prabu/Desktop/connectedhomeip
source scripts/activate.sh -p linux
```

### Full comprehensive run

```bash
rm -rf /tmp/chip-tool-phase2-comprehensive

./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite all
```

### Device discovery suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite device-discovery
```

### Basic information suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite basic-information
```

### Electrical sensor suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite electrical-sensor
```

### Power topology suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite power-topology
```

### Device Energy Management suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite dem
```

### Device Energy Management Mode suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite dem-mode
```

### Electrical meter suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite electrical-meter
```

### Commodity metering suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite commodity-metering
```

### Utility meter suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite utility-meter
```

### Meter identification suite

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite meter-identification
```

## 7. Useful Variants

Run with a longer timeout:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite all \
  --timeout 60
```

Run with verbose logging:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite all \
  --verbose
```

Skip pairing if already commissioned:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --pairing-mode skip \
  --test-suite all
```

## 8. Default Paths And Settings

By default the script uses:

- Node ID: `0x12344321`
- Passcode: `20202021`
- Discriminator: `3840`
- `chip-tool`: `out/linux-x64-chip-tool/chip-tool`
- Storage directory: `/tmp/chip-tool-phase2-comprehensive`
- Log file: `/tmp/phase2-comprehensive-test.log`

## 9. Reading The Output

Each test prints:

- `Request`: the exact `chip-tool` command
- `Response`: selected output lines
- `Result`: PASS or FAIL with the command exit code

For many simulator attributes, `Data = NULL` can still be a valid PASS if the
attribute is readable and the command succeeds.

## 10. Common Mistake

If you see only 5 tests, that usually means you selected:

```bash
--test-suite commodity-metering
```

That is expected. Use this instead for the complete run:

```bash
./scripts/tools/phase2_energy_simulator_comprehensive_test.sh \
  --test-suite all
```

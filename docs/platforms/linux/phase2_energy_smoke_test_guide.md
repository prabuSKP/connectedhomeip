# Phase 2 Energy Simulator Smoke Test Guide

This guide explains how to use the scripted smoke test for the Linux Phase 2
virtual energy simulator and how to read each step as a beginner.

Script path:

- `scripts/tools/phase2_energy_simulator_smoke.sh`

## 1. Why This Script Exists

The script runs a small but useful end-to-end validation:

1. Optionally recommission the device.
2. Pair with `chip-tool`.
3. Run a fixed set of descriptor and energy cluster reads.
4. Print each command in a request/response format.

This helps answer two questions quickly:

- Is the device reachable and commissioned correctly?
- Are the expected endpoints and energy attributes readable?

## 2. Prerequisites

Build and run the simulator:

```bash
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build
./out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app \
  --discriminator 3840 \
  --passcode 20202021 \
  --secured-device-port 5540 \
  --KVS /tmp/chip-phase2-kvs \
  --enable-key 000102030405060708090a0b0c0d0e0f
```

Build `chip-tool`:

```bash
./scripts/build/build_examples.py --target linux-x64-chip-tool build
```

## 3. Run A Clean Smoke Test

```bash
pkill -f chip-phase2-energy-simulator-app || true
rm -f /tmp/chip-phase2-kvs
rm -rf /tmp/chip-tool-phase2

./out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app \
  --discriminator 3840 \
  --passcode 20202021 \
  --secured-device-port 5540 \
  --KVS /tmp/chip-phase2-kvs \
  --enable-key 000102030405060708090a0b0c0d0e0f
```

In another terminal:

```bash
./scripts/tools/phase2_energy_simulator_smoke.sh \
  --recommission \
  --pair-timeout 30 \
  --check-timeout 5 \
  --response-lines 6
```

## 4. Output Format

Each operation prints three blocks:

- `Request`: the exact `chip-tool` command being executed.
- `Response`: focused lines (`Endpoint`, `Data`, or error lines).
- `Result`: PASS/FAIL and process exit code.

Typical successful output includes lines such as:

- `[TOO] Endpoint: ... Cluster: ... Attribute ...`
- `[DMG] Data = ...`

## 5. What Each Test Verifies

### Commissioning Steps

1. Recommission unpair (best effort)
- Why: removes stale pairing state if one exists.
- Note: timeout here can be normal on a fresh storage directory.

2. Pair device (`onnetwork-long` by default)
- Why: establishes secure operational session before any reads.

### Endpoint Discovery Checks

1. `descriptor read parts-list <node> 0`
- Why: verifies root endpoint declares expected child endpoints.

2. `descriptor read server-list <node> 1`
- Why: confirms endpoint 1 cluster exposure.

3. `descriptor read server-list <node> 2`
- Why: confirms endpoint 2 cluster exposure.

4. `descriptor read server-list <node> 3`
- Why: confirms endpoint 3 cluster exposure.

5. `descriptor read server-list <node> 4`
- Why: confirms endpoint 4 cluster exposure.

### Phase 2 Energy Checks

6. `electricalpowermeasurement read active-power <node> 1`
- Why: validates sensor-side instantaneous power read path.

7. `electricalenergymeasurement read cumulative-energy-imported <node> 1`
- Why: validates sensor-side accumulated energy read path.

8. `powertopology read feature-map <node> 1`
- Why: validates power topology feature support visibility.

9. `deviceenergymanagement read esastate <node> 2`
- Why: validates current device energy management state reporting.

10. `deviceenergymanagementmode read supported-modes <node> 2`
- Why: validates exposed DEM operating modes.

11. `electricalpowermeasurement read active-power <node> 3`
- Why: validates meter-side instantaneous power read path.

12. `electricalenergymeasurement read cumulative-energy-imported <node> 3`
- Why: validates meter-side accumulated energy read path.

13. `commoditymetering read metered-quantity <node> 3`
- Why: validates commodity metering attribute access.

14. `meteridentification read meter-serial-number <node> 4`
- Why: validates meter identity attribute access.

## 6. Interpreting PASS, FAIL, NULL, and Empty Data

- PASS means command execution and path resolution succeeded.
- FAIL means command failed (timeout, unsupported path, session/setup problem).
- `Data = NULL` may be valid simulator behavior for some attributes.
- An empty structure (`Data =` with nested fields) can still be valid.

## 7. Common Failure Patterns

- `CHIP Error 0x00000032: Timeout`
  - Usually discovery/session reachability or stale state.
- Storage init or path issues
  - Ensure `--storage-directory` points to a writable directory.

## 8. Useful Script Options

```bash
./scripts/tools/phase2_energy_simulator_smoke.sh --help
```

Common options:

- `--pairing-mode onnetwork-long|onnetwork|code|skip`
- `--recommission`
- `--pair-timeout <seconds>`
- `--check-timeout <seconds>`
- `--response-lines <count>`
- `--storage-directory <path>`

## 9. Exit Code Behavior

- Exit `0`: all smoke checks passed.
- Exit `1`: one or more smoke checks failed.

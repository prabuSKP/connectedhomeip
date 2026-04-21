# Virtual Matter Device Testing Plan

## Using Matter SDK (`connectedhomeip`) to Simulate Electrical Device Types

> **Purpose:** Set up virtual Matter devices on a Linux machine (or WSL) to test the `matter-energy` SmartThings Edge driver without physical hardware.
>
> **Related:** [ELECTRICAL_DEVICE_TYPE_MAPPING.md](./ELECTRICAL_DEVICE_TYPE_MAPPING.md) | [DESIGN_PLAN_ELECTRICAL_DEVICE_CLASS_v2.md](./DESIGN_PLAN_ELECTRICAL_DEVICE_CLASS_v2.md)
>
> **Last Updated:** April 15, 2026

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                    YOUR DEVELOPMENT MACHINE                         │
│                                                                     │
│  ┌──────────────────────┐    ┌──────────────────────┐               │
│  │  Virtual Matter       │    │  chip-tool             │              │
│  │  Device App           │    │  (Controller/Debug)    │              │
│  │  (energy-management   │    │                        │              │
│  │  -app or custom)      │    │  Read attributes       │              │
│  │                       │    │  Write commands         │              │
│  │  Runs on Linux/WSL    │    │  Subscribe events      │              │
│  │                       │    │                        │              │
│  │  Advertises via mDNS  │    │  Runs on Linux/WSL    │              │
│  │  on Wi-Fi             │    │                        │              │
│  └──────────┬────────────┘    └──────────────────────┘               │
│             │ Wi-Fi (same network)                                   │
└─────────────┼───────────────────────────────────────────────────────┘
              │
              │ Matter Protocol (UDP/IP)
              │
┌─────────────┼───────────────────────────────────────────────────────┐
│  ┌──────────▼────────────┐                                          │
│  │  SmartThings Hub       │   SmartThings Hub discovers the         │
│  │  (v2/v3/Station)       │   virtual device via mDNS, commissions │
│  │                        │   it, and routes to your Edge driver.   │
│  │  Runs: matter-energy   │                                         │
│  │  Edge Driver           │                                         │
│  └──────────┬────────────┘                                          │
│             │                                                       │
│  ┌──────────▼────────────┐                                          │
│  │  SmartThings App       │   Shows device in UI with capability    │
│  │  (Android/iOS)         │   values from your Edge driver.         │
│  └────────────────────────┘                                         │
│                                YOUR HOME NETWORK                    │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Prerequisites

### Hardware
| Item | Required? | Notes |
|---|---|---|
| SmartThings Hub (v2/v3/Station) | ✅ Yes | Must be on same Wi-Fi network as your dev machine |
| Android/iOS phone with SmartThings app | ✅ Yes | For commissioning the virtual device |
| Linux machine or Windows with WSL2 | ✅ Yes | To build and run the virtual Matter device |

### Software
| Item | Version | Purpose |
|---|---|---|
| Ubuntu (native or WSL2) | 22.04+ | Build environment for Matter SDK |
| Git | Latest | Clone the Matter SDK repository |
| Python | 3.11+ | Required by current Matter SDK bootstrap/build flow |
| GN + Ninja | (bundled with SDK) | C++ build system |
| SmartThings CLI | Latest | Deploy Edge driver to your hub |

---

## Phase 1: Environment Setup (One-time, ~1-2 hours)

### Step 1.1: Install WSL2 (Windows users only)

```powershell
# Run in PowerShell as Administrator
wsl --install -d Ubuntu-22.04
```

After restart, set up your Ubuntu username/password in the WSL terminal.

### Step 1.2: Clone the Matter SDK

```bash
# In WSL/Linux terminal
cd ~
git clone --recurse-submodules https://github.com/project-chip/connectedhomeip.git
cd connectedhomeip

# Checkout the Electrical simulator branch used by this plan
git checkout -b virtual-Eclectrical-device origin/virtual-Eclectrical-device

# Always-required submodules for bootstrap/activate
git submodule update --init --depth 1 \
  third_party/pigweed/repo \
  third_party/openthread/repo \
  third_party/editline/repo

# Linux-only submodule set for host simulation builds
python3 scripts/checkout_submodules.py --shallow --platform linux
```

> **⚠️ Important:** If you need every platform SDK, run `git submodule update -f --init --recursive` (large checkout). For Linux host simulation only, the platform checkout command above is sufficient and faster.

### Step 1.3: Install Dependencies

```bash
# Install system-level dependencies
sudo apt-get update
sudo apt-get install -y git gcc g++ pkg-config cmake curl libssl-dev libdbus-1-dev \
  libglib2.0-dev libavahi-client-dev ninja-build python3-pip unzip \
  libgirepository1.0-dev libcairo2-dev libreadline-dev libevent-dev default-jre \
  python3.11 python3.11-dev python3.11-venv

# Ensure python3 points to 3.11+
python3 --version

# Bootstrap the Matter build environment
source scripts/bootstrap.sh -p linux
```

> **Note:** `bootstrap.sh` takes 10-20 minutes on first run. It downloads toolchains and sets up the Python virtual environment.

### Step 1.4: Activate Environment

```bash
# MUST run this every time you open a new terminal
source scripts/activate.sh -p linux
```

> **Note:** Use `source scripts/activate.sh`. The path `source/activate.sh` is incorrect for this repository.
>
> If the host default `python3` is below 3.11, prepend a Python 3.11 shim path before activation:
>
> ```bash
> export PATH=/home/$USER/.local/py311-shim:$PATH
> source scripts/activate.sh -p linux
> ```

---

## Phase 2: Build Virtual Device Apps (~30-60 minutes)

You need to build **two things**: the virtual device app + the chip-tool controller.

> **Repo note for this local checkout:** the cloned `connectedhomeip` tree currently contains `examples/evse-app`, `examples/energy-gateway-app`, and an Android-only `examples/virtual-device-app`, but it does **not** contain the older `examples/energy-management-app` path referenced below. For the Phase 2 multi-endpoint simulator in this workspace, use [tools/generate_phase2_energy_zap.py](C:\Prabu\Application\SmartThingsEdgeDrivers\tools\generate_phase2_energy_zap.py), which writes [phase2-energy-simulator.zap](C:\Prabu\Application\SmartThingsEdgeDrivers\connectedhomeip\examples\evse-app\evse-common\phase2-energy-simulator.zap), and build the dedicated Linux target with `./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build`.

### Step 2.1: Build `chip-tool` (Matter Controller/Debugger)

```bash
cd ~/connectedhomeip

# Build chip-tool
./scripts/build/build_examples.py --target linux-x64-chip-tool build

# Verify
./out/linux-x64-chip-tool/chip-tool --help
```

### Step 2.2: Build Phase 2 Electrical Simulator (Recommended)

For `origin/virtual-Eclectrical-device`, build the dedicated Phase 2 simulator target.

```bash
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build

# Verify
ls -la ./out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app
```

For an end-to-end reproducibility check, run one clean build by deleting the old
target output first:

```bash
rm -rf ./out/linux-x64-phase2-energy-simulator
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build
```

### Step 2.3: (Optional) Build `all-clusters-app` as fallback

This app includes ALL standard Matter clusters — useful as a universal test device.

```bash
./scripts/build/build_examples.py --target linux-x64-all-clusters-app build
```

---

## Phase 3: Configure & Run Virtual Devices

### 3.1: Virtual Device — EVSE + Electrical Sensor (Default Config)

The `energy-management-app` ships with a default endpoint configuration that includes:

| Endpoint | Device Type | Clusters Included |
|---|---|---|
| Endpoint 0 | Root Node | Descriptor, Basic Information, etc. |
| Endpoint 1 | Energy EVSE (0x050C) | EnergyEvse, EnergyEvseMode, DeviceEnergyManagementMode |
| Endpoint 1 | Electrical Sensor (0x0510) | ElectricalPowerMeasurement, ElectricalEnergyMeasurement |
| Endpoint 1 | DEM (0x050D) | DeviceEnergyManagement |

#### Run the Virtual EVSE Device

```bash
# Terminal 1: Start the virtual device
cd ~/connectedhomeip
./out/linux-x64-energy-management-app/chip-energy-management-app \
  --discriminator 3840 \
  --KVS /tmp/chip-energy-kvs \
  --passcode 20202021 \
  --secured-device-port 5540 \
  --trace_decode 1
```

**Key parameters:**
| Parameter | Value | Purpose |
|---|---|---|
| `--discriminator` | `3840` | 12-bit device identifier for commissioning |
| `--passcode` | `20202021` | Setup PIN code (used during commissioning) |
| `--KVS` | `/tmp/chip-energy-kvs` | Key-value store file (persistent state) |
| `--secured-device-port` | `5540` | UDP port for Matter communication |

**Expected output:**
```
[1713456789.123][1234:1234] CHIP:SVR: Server initialization complete
[1713456789.456][1234:1234] CHIP:DL: Device Configuration:
[1713456789.789][1234:1234] CHIP:DL:   Serial Number: TEST_SN
[1713456789.012][1234:1234] CHIP:DL:   Vendor Id: 65521 (0xFFF1)
[1713456789.345][1234:1234] CHIP:DL:   Product Id: 32769 (0x8001)
[1713456789.678][1234:1234] CHIP:DL:   Setup Pin Code (Passcode): 20202021
[1713456789.901][1234:1234] CHIP:DL:   Setup Discriminator: 3840 (0x0F00)
[1713456789.234][1234:1234] CHIP:SVR: SetupQRCode: [MT:-24J0AFN00KA0648G00]
[1713456789.567][1234:1234] CHIP:SVR: Manual pairing code: [34970112332]
```

> **Save these values:** The Manual pairing code (`34970112332`) is needed for SmartThings commissioning.

### 3.2: Virtual Device — Standalone Electrical Sensor

To test a **standalone** Electrical Sensor (0x0510) without EVSE, you need to customize the ZAP configuration.

#### Step 1: Copy the ZAP config

```bash
cd ~/connectedhomeip
cp examples/energy-management-app/energy-management-common/energy-management-app.zap \
   examples/energy-management-app/energy-management-common/electrical-sensor-standalone.zap
```

#### Step 2: Edit with ZAP tool

```bash
# Launch the ZAP GUI configurator
./scripts/tools/zap/run_zaptool.sh \
  examples/energy-management-app/energy-management-common/electrical-sensor-standalone.zap
```

**In the ZAP GUI:**
1. Navigate to **Endpoint 1**
2. Change the **Device Type** to `Electrical Sensor (0x0510)`
3. **Remove** clusters: `EnergyEvse`, `EnergyEvseMode`, `DeviceEnergyManagement`
4. **Keep** clusters:
   - `ElectricalPowerMeasurement` (Server) — enable attributes: `PowerMode`, `ActivePower`, `Voltage`, `ActiveCurrent`
   - `ElectricalEnergyMeasurement` (Server) — enable attributes: `CumulativeEnergyImported`, `PeriodicEnergyImported`
   - `PowerTopology` (Server) — optional
5. Save the `.zap` file

#### Step 3: Regenerate code and rebuild

```bash
# Regenerate the auto-generated code from ZAP config
./scripts/tools/zap/generate.py \
  examples/energy-management-app/energy-management-common/electrical-sensor-standalone.zap \
  -o examples/energy-management-app/energy-management-common/

# Rebuild
./scripts/build/build_examples.py --target linux-x64-energy-management-app build
```

#### Step 4: Run standalone sensor

```bash
./out/linux-x64-energy-management-app/chip-energy-management-app \
  --discriminator 3841 \
  --KVS /tmp/chip-sensor-kvs \
  --passcode 20202021 \
  --secured-device-port 5541
```

### 3.3: Running Multiple Virtual Devices Simultaneously

You can run multiple virtual devices on the same machine by using different ports, discriminators, and KVS files:

```bash
# Terminal 1: EVSE device
./out/linux-x64-energy-management-app/chip-energy-management-app \
  --discriminator 3840 --KVS /tmp/chip-evse-kvs \
  --passcode 20202021 --secured-device-port 5540

# Terminal 2: Standalone Electrical Sensor
./out/linux-x64-energy-management-app/chip-energy-management-app \
  --discriminator 3841 --KVS /tmp/chip-sensor-kvs \
  --passcode 20202022 --secured-device-port 5541

# Terminal 3: All-Clusters (for DEM testing)
./out/linux-x64-all-clusters-app/chip-all-clusters-app \
  --discriminator 3842 --KVS /tmp/chip-all-kvs \
  --passcode 20202023 --secured-device-port 5542
```

---

## Phase 4: Commission into SmartThings

### 4.1: Via SmartThings App (Recommended)

1. Open **SmartThings App** on your phone
2. Tap **"+"** → **"Add device"**
3. Tap **"Scan QR code"** or **"Enter setup code manually"**
4. Enter the **Manual pairing code** from the virtual device output (e.g., `34970112332`)
5. The app will discover and commission the virtual device
6. Your `matter-energy` Edge driver will handle it based on the fingerprint match

> **⚠️ Network requirement:** Your phone, SmartThings Hub, and WSL/Linux machine must all be on the **same Wi-Fi network**.

### 4.2: Via chip-tool (For Multi-Admin / Debug)

```bash
# Commission the virtual device to chip-tool's fabric
./out/linux-x64-chip-tool/chip-tool pairing onnetwork 110 20202021

# 110 = node ID you assign
# 20202021 = the passcode from the virtual device
```

### 4.3: WSL2 Network Considerations

> **⚠️ WSL2 Networking:** By default, WSL2 uses a NAT network. For mDNS discovery to work between WSL2 and your SmartThings Hub, you may need to configure WSL2 in **mirrored networking mode**:

```ini
# Edit/create C:\Users\<YourName>\.wslconfig
[wsl2]
networkingMode=mirrored
```

Then restart WSL:
```powershell
wsl --shutdown
wsl
```

---

## Phase 5: Interact & Simulate Attribute Changes

### 5.1: Read Attributes with chip-tool

```bash
# Read ActivePower (Electrical Power Measurement cluster)
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read active-power 110 1
# 110 = node ID, 1 = endpoint ID

# Read PowerMode
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read power-mode 110 1

# Read CumulativeEnergyImported (Electrical Energy Measurement cluster)
./out/linux-x64-chip-tool/chip-tool electricalenergymeasurement read cumulative-energy-imported 110 1

# Read EVSE State
./out/linux-x64-chip-tool/chip-tool energyevse read state 110 1

# Read EVSE Supply State
./out/linux-x64-chip-tool/chip-tool energyevse read supply-state 110 1

# Read all attributes on an endpoint
./out/linux-x64-chip-tool/chip-tool any read-by-id 0xFFFFFFFF 0xFFFFFFFF 110 1
```

### 5.2: Subscribe to Attribute Reports

```bash
# Subscribe to ActivePower changes (min 1s, max 60s reporting interval)
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement subscribe active-power 1 60 110 1

# Subscribe to all EEM energy changes
./out/linux-x64-chip-tool/chip-tool electricalenergymeasurement subscribe cumulative-energy-imported 5 300 110 1
```

### 5.3: Send Commands to EVSE

```bash
# Enable Charging (EVSE command)
./out/linux-x64-chip-tool/chip-tool energyevse enable-charging 110 1 \
  --ChargingEnabledUntil 1735689600 \
  --MinimumChargeCurrent 6000 \
  --MaximumChargeCurrent 32000

# Disable Charging
./out/linux-x64-chip-tool/chip-tool energyevse disable 110 1

# Change EVSE Mode
./out/linux-x64-chip-tool/chip-tool energyevsemode change-to-mode 110 1 --NewMode 1
```

### 5.4: Simulate Attribute Value Changes (In-App)

The energy-management-app has a **built-in simulation** that periodically updates attribute values. To customize the simulation, modify the source code:

```bash
# Edit the simulation logic
nano ~/connectedhomeip/examples/energy-management-app/linux/main.cpp
```

Key modification points:

```cpp
// In the main loop or a timer callback, update attribute values:

#include <app/clusters/electrical-power-measurement-server/electrical-power-measurement-server.h>

// Simulate power fluctuation
void SimulatePowerReading() {
    int64_t activePower_mW = 1500000 + (rand() % 500000);  // 1500-2000W in mW
    
    // Update the attribute value internally
    ElectricalPowerMeasurement::Attributes::ActivePower::Set(
        1,               // endpoint ID
        activePower_mW   // value in milliwatts
    );
    
    // Notify subscribers of the change
    MatterReportingAttributeChangeCallback(
        1,                                                    // endpoint
        ElectricalPowerMeasurement::Id,                       // cluster
        ElectricalPowerMeasurement::Attributes::ActivePower::Id  // attribute
    );
}

// Simulate cumulative energy increase
void SimulateEnergyReading() {
    static int64_t totalEnergy_mWh = 45000000;  // Start at 45kWh
    totalEnergy_mWh += 100000;  // Add 100Wh per interval
    
    // Build the EnergyMeasurementStruct
    ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type measurement;
    measurement.energy = totalEnergy_mWh;
    
    ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Set(
        1, measurement
    );
    
    MatterReportingAttributeChangeCallback(
        1,
        ElectricalEnergyMeasurement::Id,
        ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Id
    );
}
```

After modifying, rebuild and restart:
```bash
./scripts/build/build_examples.py --target linux-x64-energy-management-app build
# Then run the app again
```

### 5.5: Factory Reset a Virtual Device

```bash
# Delete the KVS file to reset all state
rm /tmp/chip-energy-kvs

# Or use chip-tool to unpair
./out/linux-x64-chip-tool/chip-tool pairing unpair 110
```

---

## Phase 6: Test Matrix — Device Types × Edge Driver Scenarios

### Test Scenarios for Each MVP Device Type

#### Scenario A: EVSE (0x050C) — Regression Test

| Test # | What to Test | chip-tool Command | Expected SmartThings Capability Event |
|---|---|---|---|
| A1 | EVSE State → Plugged In | *(Built into app simulation)* | `evseState.state = pluggedInCharging` |
| A2 | Active Power = 7200W | Set `ActivePower = 7200000` | `powerMeter.power = 7200.0 W` |
| A3 | Energy Imported = 50kWh | Set `CumulativeEnergyImported.energy = 50000000` | `energyMeter.energy = 50000 Wh` |
| A4 | Enable Charging command | Send from SmartThings app | EVSE receives `EnableCharging` |
| A5 | Mode change | Change mode in SmartThings app | EVSE receives `ChangeToMode` |

#### Scenario B: Standalone Electrical Sensor (0x0510) — New

| Test # | What to Test | chip-tool Command | Expected SmartThings Capability Event |
|---|---|---|---|
| B1 | Power Mode = AC | Set `PowerMode = 1` | `powerSource.powerSource = mains` |
| B2 | Active Power = 1500W | Set `ActivePower = 1500000` | `powerMeter.power = 1500.0 W` |
| B3 | Voltage = 230V | Set `Voltage = 230000` | `voltageMeasurement.voltage = 230.0 V` |
| B4 | Current = 6.5A | Set `ActiveCurrent = 6500` | `currentMeasurement.current = 6.5 A` |
| B5 | Energy = 45kWh | Set `CumulativeEnergyImported.energy = 45000000000` | `energyMeter.energy = 45000000 Wh` |
| B6 | Periodic Energy delta | Set `PeriodicEnergyImported.energy = 100000` | `powerConsumptionReport.powerConsumption.deltaEnergy` |

#### Scenario C: DEM Standalone Mode-Only (0x050D) — New

| Test # | What to Test | chip-tool Command | Expected SmartThings Capability Event |
|---|---|---|---|
| C1 | Supported Modes list | Read `SupportedModes` | `mode.supportedModes = [...]` |
| C2 | Current Mode | Set `CurrentMode = 0` | `mode.mode = <first mode label>` |
| C3 | Change Mode command | Send from SmartThings app | DEM receives `ChangeToMode` |

---

## Phase 7: Automated Test with Python (TC scripts)

The Matter SDK includes Python test scripts specifically for electrical clusters:

```bash
# Run Electrical Power Measurement test cases
python3 src/python_testing/TC_EPM_2_1.py \
  --commissioning-method on-network \
  --discriminator 3840 \
  --passcode 20202021

# Run Electrical Energy Measurement test cases
python3 src/python_testing/TC_EEM_2_1.py \
  --commissioning-method on-network \
  --discriminator 3840 \
  --passcode 20202021

# Run Device Energy Management test cases
python3 src/python_testing/TC_DEM_2_1.py \
  --commissioning-method on-network \
  --discriminator 3840 \
  --passcode 20202021
```

---

## Quick Reference Card

### Common chip-tool Commands for Electrical Testing

```bash
# === ELECTRICAL POWER MEASUREMENT (0x0090) ===
chip-tool electricalpowermeasurement read active-power <node> <endpoint>
chip-tool electricalpowermeasurement read power-mode <node> <endpoint>
chip-tool electricalpowermeasurement read voltage <node> <endpoint>
chip-tool electricalpowermeasurement read active-current <node> <endpoint>
chip-tool electricalpowermeasurement subscribe active-power <min> <max> <node> <endpoint>

# === ELECTRICAL ENERGY MEASUREMENT (0x0091) ===
chip-tool electricalenergymeasurement read cumulative-energy-imported <node> <endpoint>
chip-tool electricalenergymeasurement read periodic-energy-imported <node> <endpoint>
chip-tool electricalenergymeasurement subscribe cumulative-energy-imported <min> <max> <node> <endpoint>

# === ENERGY EVSE (0x0099) ===
chip-tool energyevse read state <node> <endpoint>
chip-tool energyevse read supply-state <node> <endpoint>
chip-tool energyevse read fault-state <node> <endpoint>
chip-tool energyevse enable-charging <node> <endpoint> --ChargingEnabledUntil <epoch> --MinimumChargeCurrent <mA> --MaximumChargeCurrent <mA>
chip-tool energyevse disable <node> <endpoint>

# === DEVICE ENERGY MANAGEMENT MODE (0x009F) ===
chip-tool deviceenergymanagementmode read supported-modes <node> <endpoint>
chip-tool deviceenergymanagementmode read current-mode <node> <endpoint>
chip-tool deviceenergymanagementmode change-to-mode <node> <endpoint> --NewMode <id>

# === GENERAL ===
chip-tool descriptor read device-type-list <node> <endpoint>
chip-tool descriptor read server-list <node> <endpoint>
chip-tool any read-by-id 0xFFFFFFFF 0xFFFFFFFF <node> <endpoint>
```

### Virtual Device Default Credentials

| Parameter | Default Value |
|---|---|
| Vendor ID | `0xFFF1` (Test) |
| Product ID | `0x8001` (Test) |
| Passcode | `20202021` |
| Discriminator | `3840` |
| Manual Pairing Code | *(shown in device output)* |

---

## Troubleshooting

| Problem | Solution |
|---|---|
| SmartThings Hub can't discover virtual device | Ensure same Wi-Fi network. For WSL2, enable mirrored networking. Check `avahi-daemon` is running: `sudo systemctl start avahi-daemon` |
| `chip-tool` commissioning fails | Clear state: `rm -rf /tmp/chip_*` and `rm /tmp/chip-energy-kvs`. Restart both tools. |
| Build fails with missing dependencies | Re-run `source scripts/bootstrap.sh` then `source scripts/activate.sh` |
| ZAP tool won't launch | Install: `sudo apt-get install libatk1.0-0 libatk-bridge2.0-0 libgtk-3-0` |
| WSL2 mDNS not working | Add `networkingMode=mirrored` to `.wslconfig` and restart WSL |
| Multiple virtual devices conflict | Use different `--discriminator`, `--secured-device-port`, and `--KVS` values |
| Virtual device not matching Edge driver | Check `fingerprints.yml` — device type IDs must match what ZAP configured |

---

*Document Version: 1.0*
*Last Updated: April 15, 2026*

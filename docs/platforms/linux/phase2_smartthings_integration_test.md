# SmartThings Matter 1.5 Electrical Device Type — End-to-End Test Procedure

**Driver:** `matter-energy` (`SmartThingsEdgeDrivers/drivers/SmartThings/matter-energy`)
**Simulator:** Phase 2 Virtual Energy Simulator (`connectedhomeip`, branch `virtual-Eclectrical-device`)
**Last Updated:** June 2026

---

## Overview

This document describes the complete procedure for validating the `matter-energy` SmartThings Edge
Driver against the Phase 2 Virtual Energy Simulator. The simulator runs on a Linux PC and exposes
four Matter 1.5 electrical device type endpoints. The driver is installed on a SmartThings Hub on
the same LAN.

### Simulator Endpoints

| Endpoint | Device Type | ID | Driver Profile Expected |
|---|---|---|---|
| EP0 | Root Node | 0x0016 | — (not directly surfaced) |
| EP1 | Electrical Sensor | 0x0510 | `electrical-sensor-voltage-current` |
| EP2 | Device Energy Management | 0x050D | `dem-standalone` |
| EP3 | Electrical Meter | 0x0514 | `electrical-meter` |
| EP4 | Electrical Utility Meter | 0x0511 | **Not supported** (no fingerprint) |

---

## Prerequisites

### Hardware / Accounts

- SmartThings Hub (any v3 or later) **on the same LAN** as the Linux PC
- SmartThings account signed in to both the mobile app and the CLI
- Android or iOS device running the SmartThings app

### Software on Linux PC

- SmartThings CLI installed and authenticated (`smartthings --version`)
- `connectedhomeip` repo checked out at branch `virtual-Eclectrical-device`
- `SmartThingsEdgeDrivers` repo checked out

Verify CLI login:

```bash
smartthings whoami
```

### Simulator Already Built

The Phase 2 simulator binary must already exist:

```bash
ls -l out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app
```

If not, build it first:

```bash
source scripts/activate.sh -p linux
./scripts/build/build_examples.py --target linux-x64-phase2-energy-simulator build
```

For the full build guide see: [phase2_virtual_energy_simulator.md](./phase2_virtual_energy_simulator.md)

---

## Step 1 — Start the Phase 2 Virtual Energy Simulator

Open **Terminal A** (from the `connectedhomeip` repo root) and run:

```bash
# Kill any existing simulator instance holding port 5540, then clear stale state
pkill -f chip-phase2-energy-simulator || true
sleep 1
rm -f /tmp/chip-phase2-kvs

./out/linux-x64-phase2-energy-simulator/chip-phase2-energy-simulator-app \
  --discriminator 3840 \
  --passcode 20202021 \
  --secured-device-port 5540 \
  --KVS /tmp/chip-phase2-kvs \
  --enable-key 000102030405060708090a0b0c0d0e0f \
  2>&1 | tee /tmp/simulator-run.log
```

The `tee` command writes simulator output to `/tmp/simulator-run.log` while still displaying
it in the terminal. Keep Terminal A running throughout the test.

### Expected Startup Output

```
[DIS] Advertise commission parameter vendorID=65521 productID=32769 discriminator=3840/15 cm=1
[DIS] CHIP minimal mDNS configured as 'Commissionable node device'
[DIS] mDNS service published: _matterc._udp
[EM]  Phase 2 Energy Simulator: Init
```

Followed by periodic telemetry ticks every ~10 seconds:

```
Phase 2 Telemetry: tick — EP1 active_power=<mW>mW voltage=<mV>mV current=<mA>mA
```

If you do **not** see the mDNS advertisement lines, the simulator is not discoverable.
Clear the KVS and restart (see [Section 9 of the simulator guide](./phase2_virtual_energy_simulator.md#9-re-run-and-re-commission-tips)).

---

## Step 2 — Install the matter-energy Edge Driver

Open **Terminal B**.

### 2.1 Package and install (fast path)

The CLI must be run from the driver directory that contains `config.yml`:

```bash
cd /home/test/EdgeClient/SmartThingsEdgeDrivers/drivers/SmartThings/matter-energy
smartthings edge:drivers:package . --install
```

The CLI will prompt you to:
1. Select a channel (choose your development channel)
2. Select a hub (choose the hub on the same LAN as the simulator)

### 2.2 Verify installation and open logcat

```bash
smartthings edge:drivers:installed
```

Expected: `matter-energy` listed with status `LIVE`.

> **Important:** Open the logcat stream **now, before proceeding to Step 3** (commissioning).
> Profile selection and device lifecycle events happen at commissioning time. If logcat is
> started after the device is already paired, those events will only be visible in the full
> `hub-agent.log` on the hub — not in the terminal stream.

```bash
smartthings edge:drivers:logcat --log-level info
```

Expected driver startup line in logcat:

```
[INFO] matter-energy: Starting matter-energy driver, with dispatcher: 0x...
```

Keep the logcat stream open in Terminal B throughout the test.

---

## Step 3 — Commission the Simulator in SmartThings App

1. Open the **SmartThings app** on your phone.
2. Tap **+** → **Add device** → **Matter**.
3. When prompted to scan a QR code, use the manual pairing code instead:
   - Manual code: `34970112332`  
   *(derived from discriminator=3840, passcode=20202021)*
4. The app will discover and commission the simulator.
5. Accept all prompts. Allow 30–60 seconds for commissioning to complete.

> **Network requirement:** Your phone, SmartThings Hub, and Linux PC must all be on the
> same subnet. mDNS discovery will fail across subnets.

### What SmartThings Creates

Because the simulator exposes multiple device type endpoints, SmartThings will register
**up to three separate devices** in your account — one for each endpoint that matches a
driver fingerprint:

| ST Device Name | Endpoint | Profile Assigned |
|---|---|---|
| Matter Electrical Sensor | EP1 | `electrical-sensor-voltage-current` |
| Matter Device Energy Management | EP2 | `dem-standalone` |
| Matter Electrical Meter | EP3 | `electrical-meter` |
| *(EP4 Utility Meter — no device created)* | EP4 | Not matched |

> **Observed behavior (June 2026 run):** The simulator was commissioned as a single Matter
> fabric node and SmartThings created **one device** (displayed in the app as **"Matter Device"**) rather
> than three. The driver detected all three types (`Sensor: 1, Meter: 1, DEM: 1`) on the
> single device object and selected the `electrical-sensor` profile. EP2 (DEM) and EP3
> (Meter) data was processed under the same device rather than separate ones. See the
> [Actual Test Run Analysis](#actual-test-run-analysis--june-9-2026) section for the full
> breakdown.

---

## Step 4 — Verify Driver Fingerprint and Profile Selection

In Terminal B (logcat), look for these lines immediately after commissioning:

### 4.1 Driver recognizes the device

```
[INFO] matter-energy: Device init: <device-id>, endpoints: <count>
[INFO] matter-energy: doConfigure: Starting profile selection for <device-id>
```

### 4.2 Expected profile selection per device type

**Electrical Sensor (EP1 / 0x0510):**

```
[DEBUG] matter-energy: Device type detection - EVSE: 0, Solar: 0, Battery: 0, Sensor: 1, Meter: 0, DEM: 0
[DEBUG] matter-energy: Standalone Electrical Sensor detected on endpoints: 1
[DEBUG] matter-energy: Voltage attribute found on endpoint 1
[DEBUG] matter-energy: Voltage detection result: YES, selecting profile: electrical-sensor-voltage-current
[INFO]  matter-energy: Updating device profile to electrical-sensor-voltage-current
```

**Device Energy Management (EP2 / 0x050D):**

```
[DEBUG] matter-energy: Device type detection - EVSE: 0, Solar: 0, Battery: 0, Sensor: 0, Meter: 0, DEM: 1
[DEBUG] matter-energy: Standalone DEM detected on endpoints: 2
[INFO]  matter-energy: Updating device profile to dem-standalone
```

**Electrical Meter (EP3 / 0x0514):**

```
[DEBUG] matter-energy: Device type detection - EVSE: 0, Solar: 0, Battery: 0, Sensor: 0, Meter: 1, DEM: 0
[DEBUG] matter-energy: Standalone Electrical Meter detected on endpoints: 3
[INFO]  matter-energy: Updating device profile to electrical-meter
```

### 4.3 Profile selection verification (check this first)

Immediately after `doConfigure`, verify which profile was actually selected by looking for:

```
[INFO] matter-energy: Updating device profile to <profile-name>
```

| Profile selected | Meaning |
|---|---|
| `electrical-sensor-voltage-current` | ✅ Voltage detected — full sensor capabilities available |
| `electrical-sensor` | ❌ Voltage not detected — no voltage/current tiles in app |
| `electrical-meter` | ✅ Electrical Meter profile (EP3 standalone) |
| `dem-standalone` | ✅ DEM profile (EP2 standalone) |

> **Known issue:** In the June 2026 test run the driver selected `electrical-sensor` instead
> of `electrical-sensor-voltage-current` because the voltage attribute scan during
> `doConfigure` returned NO. If you see this, cross-check with chip-tool to confirm the
> attribute is present on the device:
>
> ```bash
> ./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read voltage \
>   0x12344321 1 --storage-directory /tmp/chip-tool-phase2
> ```
>
> If chip-tool returns a value but the driver still selects `electrical-sensor`, the issue
> is a timing/detection gap in the driver's `doConfigure` voltage check (see
> [Actionable Issues](#actionable-issues-identified) in the analysis section).

### 4.4 Energy measurement mode detection

For both the Sensor and Meter devices you should also see:

```
[INFO] matter-energy: Energy measurement: 2 endpoints found, 2 support cumulative energy
```

This confirms the simulator supports `CumulativeEnergyImported` and the driver will use it
(not the periodic fallback).

If instead you see:

```
[WARN] matter-energy: Cumulative energy NOT supported - using periodic reporting fallback
```

… the driver has fallen back to accumulating periodic reports. Values will still update but
will be less accurate between reporting windows.

---

## Step 5 — Validate Each Device in SmartThings App

Open each newly created device in the SmartThings app and verify capabilities.
Cross-check values by reading the same attributes with chip-tool in Terminal B.

### 5.1 Electrical Sensor (EP1, profile: `electrical-sensor-voltage-current`)

**Capabilities exposed in SmartThings app:**

| Capability | Attribute | Maps from Matter |
|---|---|---|
| `powerMeter` | `power` (W) | EPM `ActivePower` ÷ 1000 |
| `energyMeter` | `energy` (Wh) | EEM `CumulativeEnergyImported.Energy` ÷ 1000 |
| `powerConsumptionReport` | `powerConsumption` | EEM cumulative/periodic |
| `voltageMeasurement` | `voltage` (V) | EPM `Voltage` ÷ 1000 |
| `currentMeasurement` | `current` (A) | EPM `ActiveCurrent` ÷ 1000 |
| `powerSource` | `powerSource` | EPM `PowerMode` |

**Cross-check with chip-tool (run from `connectedhomeip` repo root):**

```bash
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read active-power   0x12344321 1 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read voltage         0x12344321 1 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read active-current  0x12344321 1 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read power-mode      0x12344321 1 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalenergymeasurement read cumulative-energy-imported 0x12344321 1 --storage-directory /tmp/chip-tool-phase2
```

**Expected values (based on simulator telemetry):**

| Attribute | chip-tool raw | SmartThings display |
|---|---|---|
| `ActivePower` | ~1,500,000–1,600,000 mW | ~1500–1600 W |
| `Voltage` | ~224,000–226,000 mV | ~224–226 V |
| `ActiveCurrent` | ~6,400–6,600 mA | ~6.4–6.6 A |
| `PowerMode` | 2 (AC) | `powerSource: mains` |
| `CumulativeEnergyImported.Energy` | increases over time (mWh) | increasing Wh value |

**Logcat lines to watch:**

```
[DEBUG] matter-energy: ActivePower received: EP=1, Value=1540767 mW (1540.77 W)
[DEBUG] matter-energy: EPM[EP1]: V=225.22V I=6.50A P=1540.77W
[DEBUG] matter-energy: Cumulative Energy Imported: EP=1, Raw=<mWh> mWh
[DEBUG] matter-energy: Energy report: Imported=<Wh> Wh, Total=<Wh> Wh, Component=main
```

> **Note on energy update frequency:** The driver applies a 15-minute minimum interval
> between `energyMeter` and `powerConsumptionReport` updates in SmartThings. `powerMeter`
> updates every time a new `ActivePower` report arrives from the device.

---

### 5.2 Device Energy Management (EP2, profile: `dem-standalone`)

**Capabilities exposed in SmartThings app:**

| Capability | Attribute | Maps from Matter |
|---|---|---|
| `mode` | `mode` (string) | DEMMode `CurrentMode` → mode label |
| `mode` | `supportedModes` (list) | DEMMode `SupportedModes` |

**Cross-check with chip-tool:**

```bash
./out/linux-x64-chip-tool/chip-tool deviceenergymanagementmode read supported-modes 0x12344321 2 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool deviceenergymanagementmode read current-mode    0x12344321 2 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool deviceenergymanagement read esastate             0x12344321 2 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool deviceenergymanagement read abs-max-power        0x12344321 2 --storage-directory /tmp/chip-tool-phase2
```

**Expected values:**

| Attribute | chip-tool raw | SmartThings display |
|---|---|---|
| `CurrentMode` | 0 | First mode label in supported list |
| `SupportedModes` | array of mode structs | Dropdown list in `mode` capability |
| `ESAState` | 1 (online) | *(not surfaced — DEM cluster not mapped)* |
| `AbsMaxPower` | 7,200,000 mW | *(not surfaced — DEM cluster not mapped)* |

**Logcat lines to watch:**

```
[INFO]  matter-energy: Updating device profile to dem-standalone
[DEBUG] matter-energy: Device init: <id>, endpoints: 1
```

> **Current state gap:** The `dem-standalone` profile exposes only the `mode` capability
> (DEMMode cluster). The full `DeviceEnergyManagement` cluster (ESA state, power limits,
> opt-out state, power adjustment commands) is **not yet mapped** to a SmartThings capability.
> The mode dropdown in the app is the only interactive element.

---

### 5.3 Electrical Meter (EP3, profile: `electrical-meter`)

**Capabilities exposed in SmartThings app:**

| Capability | Attribute | Maps from Matter |
|---|---|---|
| `powerMeter` | `power` (W) | EPM `ActivePower` ÷ 1000 |
| `energyMeter` | `energy` (Wh) | EEM `CumulativeEnergyImported.Energy` ÷ 1000 |
| `powerConsumptionReport` | `powerConsumption` | EEM cumulative/periodic |
| `voltageMeasurement` | `voltage` (V) | EPM `Voltage` ÷ 1000 |
| `currentMeasurement` | `current` (A) | EPM `ActiveCurrent` ÷ 1000 |
| `powerSource` | `powerSource` | EPM `PowerMode` |

**Cross-check with chip-tool:**

```bash
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read active-power   0x12344321 3 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read voltage         0x12344321 3 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read active-current  0x12344321 3 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalenergymeasurement read cumulative-energy-imported 0x12344321 3 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool commoditymetering read metered-quantity         0x12344321 3 --storage-directory /tmp/chip-tool-phase2
```

**Expected values:**

| Attribute | chip-tool raw | SmartThings display |
|---|---|---|
| `ActivePower` | ~1,750,000–1,900,000 mW | ~1750–1900 W |
| `Voltage` | ~228,000–231,000 mV | ~228–231 V |
| `ActiveCurrent` | ~8,000–8,500 mA | ~8.0–8.5 A |
| `PowerMode` | 2 (AC) | `powerSource: mains` |
| `CumulativeEnergyImported.Energy` | increases over time | increasing Wh value |
| `MeteredQuantity` | NULL | *(not surfaced — returns NULL)* |

**Logcat lines to watch:**

```
[DEBUG] matter-energy: ActivePower received: EP=3, Value=1866025 mW (1866.03 W)
[DEBUG] matter-energy: EPM[EP3]: V=229.83V I=8.46A P=1866.03W
[DEBUG] matter-energy: Cumulative Energy Imported: EP=3, Raw=<mWh> mWh
[DEBUG] matter-energy: Energy report: Imported=<Wh> Wh, Total=<Wh> Wh, Component=main
```

> **Current state gap:** The `CommodityMetering` cluster (0x0B07) on EP3 is present in the
> simulator but all measurement attributes (`MeteredQuantity`, `MeteredQuantityTimestamp`,
> `TariffUnit`) return `NULL`. The driver does not map CommodityMetering to any SmartThings
> capability yet. Neither gap blocks the power/energy display.

---

### 5.4 Electrical Utility Meter (EP4, 0x0511) — Not Supported

EP4 exposes the `MeterIdentification` cluster (0x0B06) with device type `0x0511`.

**Current state:** The `fingerprints.yml` for `matter-energy` does **not** contain an entry
for device type `0x0511`. SmartThings will not create a device for EP4, and no hub logs for
this endpoint will appear from the `matter-energy` driver.

**Ideal state (future work):** EP4 should produce a "Matter Electrical Utility Meter" device
with meter identity fields (serial number, meter type, point of delivery) surfaced via custom
capabilities or `MeterIdentification` cluster support.

---

## Step 6 — Observe Live Updates

### 6.1 Power updates (immediate)

The simulator updates electrical measurements every ~10 seconds. To watch live:

**In logcat (Terminal B):**

```bash
smartthings edge:drivers:logcat --log-level debug | grep -E "ActivePower received|EPM\[EP|Energy report"
```

**With chip-tool subscribe:**

```bash
# Subscribe to active-power on EP1 (5–60 second report interval)
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement subscribe active-power 5 60 \
  0x12344321 1 --storage-directory /tmp/chip-tool-phase2 --keepSubscriptions true
```

The `powerMeter.power` value in the SmartThings app should refresh within a few seconds of
each new telemetry tick from the simulator.

### 6.2 Energy updates (wait 15 minutes)

The `energyMeter` (cumulative kWh) and `powerConsumptionReport` capabilities are throttled
by a **15-minute minimum interval**. To verify energy reporting:

1. Leave the simulator running and the device commissioned.
2. Wait at least **15 minutes** after commissioning.
3. Watch logcat for the first cumulative energy emission:

```
[DEBUG] matter-energy: Cumulative Energy Imported: EP=1, Raw=<mWh> mWh
[DEBUG] matter-energy: Energy report: Imported=<Wh> Wh, Total=<Wh> Wh, Component=main
[INFO]  matter-energy: emitting event: {"capability_id":"energyMeter","state":{"value":...}}
```

4. Check the SmartThings app — the `energyMeter` tile should show a non-zero, increasing value.

> **Note:** `powerMeter` (live watts) updates every ~10 s throughout. `energyMeter` (total
> kWh) updates at most once every 15 minutes regardless of how often the device reports.

### 6.3 Cross-check Voltage and Current with chip-tool

If the profile was selected as `electrical-sensor` (instead of
`electrical-sensor-voltage-current`), the ST app will show no voltage/current tiles. You can
still confirm the device is correctly reporting these values at the Matter layer:

```bash
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read voltage \
  0x12344321 1 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read active-current \
  0x12344321 1 --storage-directory /tmp/chip-tool-phase2
```

Expected: Voltage ~224,000–226,000 mV; ActiveCurrent ~6,400–6,600 mA. If these return
valid values but the ST app shows no tiles, the gap is in the driver profile selection (not
the device).

---

## Step 7 — SmartThings App Device Card Checklist

Open each created device in the SmartThings app and confirm:

> **Before checking the app:** Confirm in logcat which profile was selected
> (`Updating device profile to <name>`). Known gaps listed below may cause some tiles to be
> absent even when the device is working correctly at the Matter layer.

### Electrical Sensor card

- [ ] Device name: **Matter Device** *(app commissioning-time name; driver logs call it "Matter Electrical Sensor")*
- [ ] `powerMeter` tile shows a power value in watts (non-zero, updating every ~10 s)
- [ ] `powerSource` shows **AC power supply** (display text for `mains`)
- [ ] `energyMeter` tile shows a cumulative energy value (non-zero, increasing — requires 15 min wait; shows "– kWh" until then)
- [ ] Toast "This device hasn't updated all of its status information yet" dismisses once `energyMeter` receives its first value
- [ ] Activity history shows energy consumption entries updating over time
- [ ] `voltageMeasurement` tile shows ~225 V *(only if profile is `electrical-sensor-voltage-current`; absent if `electrical-sensor` — known gap)*
- [ ] `currentMeasurement` tile shows ~6–7 A *(same condition as voltage above)*

### Device Energy Management card

> **Known gap:** In the current driver + single-device commissioning topology, a separate
> DEM device may not be created. If it is absent from the app, check logcat for
> `Updating device profile to dem-standalone`. If not present, DEM is handled by the same
> device as the Electrical Sensor (EP2 not separately subscribed).

- [ ] Device name: **Matter Device Energy Management** *(may not appear — see note above)*
- [ ] `mode` tile shows a mode label (e.g., "Normal" or mode index 0)
- [ ] Tapping the mode tile opens a mode selector (list from `SupportedModes`)
- [ ] Selecting a different mode sends a `ChangeToMode` command (verify in logcat)

### Electrical Meter card

> **Known gap:** In the current driver + single-device commissioning topology, a separate
> Electrical Meter device may not be created. EP3 power data may appear aggregated into
> the Electrical Sensor device's `powerMeter` tile instead.

- [ ] Device name: **Matter Electrical Meter** *(may not appear — see note above)*
- [ ] `powerMeter` tile shows a power value (non-zero, slightly different from sensor)
- [ ] `energyMeter` tile shows cumulative energy (non-zero, increasing)
- [ ] `voltageMeasurement` tile shows ~230 V
- [ ] `currentMeasurement` tile shows ~8–9 A
- [ ] `powerSource` shows `mains`

---

## Expected Results Summary

### Ideal vs Current State

| Device / Feature | Ideal (Full Matter 1.5 Spec) | Current Code State |
|---|---|---|
| **EP1 Electrical Sensor** | | |
| Power (W) | Real-time W from EPM ActivePower | ✅ Working |
| Voltage (V) | Real-time V from EPM Voltage | ✅ Working |
| Current (A) | Real-time A from EPM ActiveCurrent | ✅ Working |
| Cumulative energy | Wh from EEM CumulativeEnergyImported | ✅ Working |
| Periodic energy | Wh fallback if no cumulative feature | ✅ Working (fallback) |
| Power source mode | AC/DC from EPM PowerMode | ✅ Working |
| **EP2 Device Energy Management** | | |
| Mode selection | Mode list + change via DEMMode cluster | ✅ Working |
| ESA state visible | Online/Offline/Fault state in UI | ❌ Not surfaced |
| Power limits | AbsMinPower / AbsMaxPower in UI | ❌ Not surfaced |
| DEM commands | Power adjustment, opt-out commands | ❌ Not implemented |
| **EP3 Electrical Meter** | | |
| Power (W) | Same as Sensor via EPM | ✅ Working |
| Voltage / Current | From EPM Voltage/ActiveCurrent | ✅ Working |
| Cumulative energy | From EEM | ✅ Working |
| Commodity metered quantity | CommodityMetering MeteredQuantity | ❌ Simulator returns NULL; driver has no mapping |
| Tariff unit | CommodityMetering TariffUnit | ❌ Simulator returns NULL; driver has no mapping |
| **EP4 Electrical Utility Meter** | | |
| Device created in ST | Yes, as Utility Meter device | ❌ No fingerprint — no device created |
| Meter type | From MeterIdentification cluster | ❌ Not implemented |
| Serial number / POD | From MeterIdentification cluster | ❌ Not implemented |

---

## Hub Log Quick Reference

| What you want to confirm | Log pattern to grep |
|---|---|
| Driver started | `Starting matter-energy driver` |
| Device recognized | `Device init: <id>` |
| Profile selected | `Updating device profile to` |
| Power reports flowing | `ActivePower received: EP=` |
| Voltage/current flowing | `EPM\[EP` |
| Energy reports flowing | `Energy report: Imported=` |
| Cumulative energy supported | `support cumulative energy` |
| Fallback to periodic | `Cumulative energy NOT supported` |
| No device type matched | `No matching device type found` |

---

## Troubleshooting

### Simulator not discovered by SmartThings app during commissioning

- Confirm phone, hub, and Linux PC are on the same LAN/subnet.
- Check mDNS: `avahi-browse -a | grep matter`
- Confirm port 5540 is not blocked: `ss -ulnp | grep 5540`
- If KVS is stale, restart the simulator with a clean KVS (see Step 1).

### Wrong device count after commissioning

If fewer than 3 devices appear, one device type may not have been matched. Check logcat for:

```
[WARN] matter-energy: doConfigure: No matching device type found - using default profile
```

Then verify `fingerprints.yml` in the `matter-energy` driver includes the expected device type IDs.

### Device shows but values are zero or frozen

Check logcat for `ActivePower received` lines. If absent, the subscription was not established.
Try removing and re-adding the device. Confirm the simulator is still running and telemetry
ticks appear in Terminal A.

### Energy value in app never updates (stays zero)

The 15-minute throttle gate is applied to `energyMeter` and `powerConsumptionReport`. For
initial commissioning, the first report should arrive within 15 minutes. If after 15 minutes
it is still zero, check for:

```
[WARN] matter-energy: Cumulative energy NOT supported - using periodic reporting fallback
```

If this appears and no periodic reports arrive, verify the simulator's
`ElectricalEnergyMeasurement` cluster has at least the `PERIODIC_ENERGY` feature bit set.

### mode capability shows no modes / mode selector empty

The `SupportedModes` attribute on DEMMode (EP2) must be reported before the UI populates.
Trigger a `refresh` from the SmartThings app or wait for the initial subscription report.

### After re-running the test: pairing fails

After a `pairing unpair`, the simulator stops advertising. Always clear the KVS and restart:

```bash
pkill -f chip-phase2-energy-simulator
rm -f /tmp/chip-phase2-kvs
# Then re-run the simulator command from Step 1
```

---

## Cleanup

When testing is complete:

```bash
# Stop the simulator
pkill -f chip-phase2-energy-simulator

# Optional: unpair from chip-tool's fabric
./out/linux-x64-chip-tool/chip-tool pairing unpair 0x12344321 --storage-directory /tmp/chip-tool-phase2

# Clean up temp files
rm -f /tmp/chip-phase2-kvs
rm -rf /tmp/chip-tool-phase2
```

Remove the devices from SmartThings app:
- Open each device → ⋮ menu → **Delete device**

To uninstall the driver from the hub:

```bash
smartthings edge:drivers:installed
smartthings edge:drivers:uninstall <driver-id> --hub <hub-id>
```

---

## Known Gaps and Open Issues

The following issues were identified during the June 2026 test run. They represent gaps
between the expected behavior described in this document and what the current driver code
actually produces.

| # | Issue | Symptoms | Area |
|---|---|---|---|
| 1 | **Power aggregation**: EP1 + EP3 summed into one `powerMeter` | App shows ~3200–3500 W instead of two separate readings | `init.lua` power routing |
| 2 | **Voltage detection gap**: `doConfigure` returns NO for Voltage despite attribute being present | Profile `electrical-sensor` selected instead of `electrical-sensor-voltage-current`; no V/A tiles | `init.lua` doConfigure |
| 3 | **0x0511 Utility Meter not in fingerprints** | No ST device created for EP4; no meter-identification data | `fingerprints.yml` |
| 4 | **DEM attributes not handled in single-device topology** | No `DeviceEnergyManagementMode` capability when Sensor + DEM on same node | `init.lua` DEM subscription |

For full root cause details see the [Actual Test Run Analysis](#actual-test-run-analysis--june-9-2026) section.

---

## Related Documents

| Document | Location |
|---|---|
| Phase 2 simulator build + run guide | [phase2_virtual_energy_simulator.md](./phase2_virtual_energy_simulator.md) |
| Smoke test guide | [phase2_energy_smoke_test_guide.md](./phase2_energy_smoke_test_guide.md) |
| Comprehensive test guide | [phase2_energy_comprehensive_test_guide.md](./phase2_energy_comprehensive_test_guide.md) |
| Hub log reference | `SmartThingsEdgeDrivers/docs/MATTER_ENERGY_DRIVER_LOG_REFERENCE.md` |
| Device type mapping | `SmartThingsEdgeDrivers/docs/ELECTRICAL_DEVICE_TYPE_MAPPING.md` |
| Phase 2 driver verification | `SmartThingsEdgeDrivers/docs/PHASE2_EDGE_DRIVER_VERIFICATION.md` |
| Original copy of this document | `SmartThingsEdgeDrivers/docs/PHASE2_ST_END_TO_END_TEST.md` |

---

## Actual Test Run Analysis — June 9, 2026

This section documents findings from a real test run performed on June 9, 2026.

**Log sources analysed:**

| File | Contents |
|---|---|
| `terminalEdgeDriverLogsJune9` (in ST repo) | ST CLI `logcat --log-level debug` output, 404 lines, 14:22:47–14:25:24 IST |
| `terminalVirtualEnergylogsJune9` (in this repo) | Identical to above — same logcat session saved under a second name; no separate virtual simulator console was captured |
| `hub-agent.log` (in ST repo) | Full hub-side driver log; device events confirmed at lines 51970–51981 matching terminal log |
| `Screenshot_20260609_142320_SmartThings.jpg` (in ST repo) | SmartThings app device card screenshot taken at 14:23 IST (~1 min after commissioning) |

**Test window:** ~2 min 37 s of live data (driver logcat started after the device was already commissioned).

---

### Device Card Screenshot — 14:23 IST

Screenshot taken approximately 1 minute after commissioning. Annotated observations:

| What the screenshot shows | Analysis |
|---|---|
| Device name: **"Matter Device"** | App display name is generic — differs from driver log name "Matter Electrical Sensor". ST uses the commissioning-time name; the fingerprint `deviceLabel` appears only in driver-internal logs, not the UI heading. |
| Location: **My Home – Living room** | Device placed in Living Room during commissioning. |
| Power meter tile (blue, prominent): **3.4 kW** | Confirms the EP1+EP3 aggregated value. 3.4 kW ≈ EP1 ~1515 W + EP3 ~1859 W. Matches logs exactly. |
| Energy meter tile: **"– kWh"** (dash) | Null — 15-minute throttle not yet expired at ~1 min post-commission. Expected. |
| Power source: **"AC power supply"** | Correct. This is the ST app display string for `powerSource: mains`. |
| **No voltage tile** | Confirms profile `electrical-sensor` was selected (not `electrical-sensor-voltage-current`). |
| **No current tile** | Same reason — profile lacks `currentMeasurement` capability. |
| **Only 3 capabilities visible** (Power meter, Energy meter, Power source) | No DEM mode, no separate Electrical Meter device, no V/A tiles. All gaps confirmed visually. |
| Toast: **"This device hasn't updated all of its status information yet. Check again later."** | ST shows this when any capability has no initial value. Caused by `energyMeter = null` (waiting for 15-min throttle). Disappears after the first cumulative energy report is emitted. |

> **Device name correction for the checklist:** The driver logs refer to the device as
> "Matter Electrical Sensor" (the fingerprint `deviceLabel`), but the ST app displayed
> it as **"Matter Device"**. The app title uses the name assigned at commissioning time;
> Step 7 checklist items referencing "Matter Electrical Sensor" should instead say
> "Matter Device" (or whatever name was given during commissioning).

---

### Finding 1: Single ST device created, not three ❌

**Expected:** Three separate SmartThings devices — Electrical Sensor (EP1), Electrical Meter (EP3), Device Energy Management (EP2).

**Actual:** One device was created to represent the entire simulator. The ST app displayed it as **"Matter Device"** (commissioning-time name); driver logs refer to it internally as "Matter Electrical Sensor" (fingerprint `deviceLabel`). Device ID: `2e93edc0-96e0-4a8a-ad41-667bbc583961`.

**Evidence:**
```
Device init: 2e93edc0-..., endpoints: 5
Device type detection - EVSE: 0, Solar: 0, Battery: 0, Sensor: 1, Meter: 1, DEM: 1
```

**Root cause:** Matter commissions the simulator as a single fabric node (one device ID, multiple endpoints). SmartThings uses fingerprint matching on the primary device type (EP0 DeviceTypeList) to select one driver and creates one ST device. The driver then processes all endpoints under that single device object — it does not spin up child devices for EP2 or EP3.

---

### Finding 2: Profile `electrical-sensor` selected, not `electrical-sensor-voltage-current` ❌

**Expected:** Profile `electrical-sensor-voltage-current` (enables Voltage and Current capability tiles) because EP1 exposes `Voltage` and `Current` attributes.

**Actual:** Profile `electrical-sensor` selected.

**Evidence:**
```
Voltage detection result: NO, selecting profile: electrical-sensor
Updating device profile to electrical-sensor.
```

**Root cause:** During `doConfigure`, the driver queries the EP1 cluster attribute list for the presence of `Voltage`. At the time of profile selection, the voltage attribute did not appear in the cluster attribute scan result (possibly a timing issue or the attribute list was not yet cached). Chip-tool reads confirm `Voltage` is present on EP1 in the spec, so this is a driver-side detection gap.

**Consequence:** The ST device card has no voltage or current tiles. Volt and amp values from EP1 are never emitted even though the simulator provides them.

---

### Finding 3: Power aggregation bug — EP1 + EP3 summed into one powerMeter ❌

**Expected:** EP1 (Electrical Sensor, ~1400–1600 W) and EP3 (Electrical Meter, ~1750–1950 W) reported as separate power readings on separate devices.

**Actual:** Both EP values are accumulated into a single `TOTAL_ACTIVE_POWER` field and emitted as one combined `powerMeter` event on the single "Matter Electrical Sensor" device.

**Observed pattern (repeats every ~10 s):**
```
ActivePower received: EP=1, Value=1515516 mW (1515.52 W)
Power aggregation: EP1=1515.52W, Total=1515.52W    ← emitted first
ActivePower received: EP=3, Value=1859419 mW (1859.42 W)
Power aggregation: EP3=1859.42W, Total=3374.94W   ← emitted second (EP1+EP3)
```

The ST device card shows a fluctuating combined value of approximately **3150–3550 W** instead of two separate readings. EP1 range: 1400–1600 W. EP3 range: 1750–1950 W.

**Root cause:** The driver sets `is_standalone=true` for the device (because no parent aggregator endpoint was found) and accumulates all endpoint power into `TOTAL_ACTIVE_POWER`. When `is_standalone=true`, the code still sums EP1 and EP3 together rather than separating them, because both endpoints map to the same `powerMeter` capability on the same ST device.

---

### Finding 4: Periodic energy reports correctly ignored (cumulative supported) ✅

**Expected:** Periodic energy reports from EP1 and EP3 are ignored when `CumulativeEnergyImported` is supported.

**Actual:** Correct behaviour confirmed throughout the entire log window.

**Evidence:**
```
Energy measurement: 2 endpoints found, 2 support cumulative energy
Periodic Energy Imported: EP=1, Raw=4209 mWh
Energy report: Periodic report ignored (cumulative supported)
Periodic Energy Imported: EP=3, Raw=5165 mWh
Energy report: Periodic report ignored (cumulative supported)
```

Periodic values received every ~10 s for both EP1 (~3900–4500 mWh) and EP3 (~4900–5400 mWh); all correctly discarded.

---

### Finding 5: `energyMeter` / `powerConsumptionReport` not yet emitted (throttle pending) ⏳

**Expected:** `energyMeter` (cumulative kWh) and `powerConsumptionReport` capability events emitted after the 15-minute minimum interval.

**Actual:** Neither capability was emitted during the ~2.5-minute log window. This is correct — the 15-minute throttle (`MINIMUM_ST_ENERGY_REPORT_INTERVAL = 900 s`) had not elapsed. To observe these emissions the device must remain commissioned for at least 15 minutes after the first `CumulativeEnergyImported` report is received.

---

### Finding 6: `powerSource: mains` emitted correctly ✅

**Expected:** `powerSource` capability set to `"mains"` (PowerMode = AC).

**Actual:** Emitted twice at startup (once for EP1, once for EP3 PowerMode=AC), then stable.

**Evidence:**
```
emitting event: {"attribute_id":"powerSource","capability_id":"powerSource","component_id":"main","state":{"value":"mains"}}
```

---

### Finding 7: No DEM (Device Energy Management) attributes handled ❌

**Expected:** DEM device with `DeviceEnergyManagementMode` capability showing current mode and supported modes list.

**Actual:** No DEM-specific attribute subscriptions made; no DEM capability emitted. EP2 (DEM endpoint, 0x050D) is detected (`DEM: 1`) but since only one ST device is created with profile `electrical-sensor`, there is no DEM profile or capability handler active.

---

### Finding 8: Utility Meter (EP4, 0x0511) not handled ❌

**Expected:** Utility Meter endpoint contributes billing/tariff or meter-identification data.

**Actual:** 0x0511 is absent from `fingerprints.yml`. No subscription to EP4 attributes; no capability events from EP4 at all.

---

### Finding 9: Voltage and Current never emitted ❌

**Expected:** `voltageMeasurement` and `currentMeasurement` capability tiles on the Electrical Sensor device card.

**Actual:** Never emitted. Two reasons compound here: (a) profile `electrical-sensor` does not include those capability IDs, and (b) the subscription only requests `ActivePower`, `PowerMode`, `PeriodicEnergyImported`, and `PeriodicEnergyExported` — Voltage and Current are never subscribed to.

---

### Finding 10: No errors or warnings observed ✅

No `ERROR`, `CRITICAL`, or `WARN` lines from the `matter-energy` driver appeared in any log source during this test run. The driver ran cleanly throughout.

---

### Summary Table

| Test Point | Expected | Actual | Status |
|---|---|---|---|
| ST device count | 3 devices (Sensor, Meter, DEM) | 1 device (app: "Matter Device", logs: "Matter Electrical Sensor") | ❌ |
| Device profile | `electrical-sensor-voltage-current` | `electrical-sensor` (voltage not detected) | ❌ |
| Power reading — EP1 | ~1400–1600 W separate | Combined EP1+EP3 total ~3150–3550 W | ❌ |
| Power reading — EP3 | ~1750–1950 W separate | Included in combined total above | ❌ |
| Periodic energy handling | Ignored (cumulative supported) | Correctly ignored | ✅ |
| Cumulative energy (energyMeter) | After 15 min | Not yet (throttle) | ⏳ |
| PowerSource | `mains` | `mains` emitted correctly | ✅ |
| DEM mode/capabilities | DeviceEnergyManagementMode | Not emitted | ❌ |
| Utility Meter (EP4) | Meter identification data | Not handled (no fingerprint) | ❌ |
| Voltage / Current | voltageMeasurement + currentMeasurement | Not emitted | ❌ |
| Driver errors | None | None observed | ✅ |

---

### Actionable Issues Identified

1. **Power aggregation**: `is_standalone=true` path sums all endpoint powers instead of using the most representative single endpoint (EP1 for Electrical Sensor). Fix: when `is_standalone=true` and device type is `electrical-sensor`, use only EP1 power; do not accumulate EP3.

2. **Voltage detection at configure time**: The `doConfigure` attribute scan for Voltage is returning NO despite the attribute being present. May need a read-with-retry or a deferred re-check after subscription confirmation.

3. **0x0511 Utility Meter fingerprint missing**: Add `{ "deviceType": "0x0511", "deviceLabel": "Electrical Utility Meter" }` to `fingerprints.yml` and a corresponding device profile.

4. **EP2 DEM handling in single-device topology**: When `DEM: 1` is detected on the same fabric node as `Sensor: 1`, the driver should activate DEM attribute subscriptions and emit `DeviceEnergyManagementMode` capability even on the single combined device.

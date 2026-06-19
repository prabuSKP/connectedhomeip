# SmartThings Matter 1.5 Electrical Meter — End-to-End Test Procedure

**Driver:** `matter-energy` (`SmartThingsEdgeDrivers/drivers/SmartThings/matter-energy`)
**Profile:** `electrical-meter` (3 components: `main`, `importedEnergy`, `exportedEnergy`)
**Simulator:** Phase 2 Virtual Energy Simulator (`connectedhomeip`, branch `virtual-Eclectrical-device`)
**Last Updated:** June 2026

---

## Overview

This document describes the complete procedure for validating the `matter-energy` SmartThings Edge
Driver against the Phase 2 Virtual Energy Simulator, focused on the **Electrical Meter** device
type (0x0514) as specified in the reviewer's Confluence doc.

Per the reviewer's direction, the driver supports:
- **Electrical Meter (0x0514)** → `electrical-meter` profile ✅
- **Electrical Utility Meter (0x0511)** → mapped to the same `electrical-meter` profile ✅
- ~~Electrical Sensor (0x0510)~~ — dropped as a standalone type
- ~~Device Energy Management (0x050D)~~ — dropped as a standalone type

### Simulator Endpoints

The Phase 2 simulator exposes five endpoints. The updated driver only fingerprint-matches the
Electrical Meter endpoints:

| Endpoint | Device Type | ID | Driver Match |
|---|---|---|---|
| EP0 | Root Node | 0x0016 | — |
| EP1 | Electrical Sensor | 0x0510 | ❌ No fingerprint in updated driver |
| EP2 | Device Energy Management | 0x050D | ❌ No fingerprint in updated driver |
| EP3 | Electrical Meter | 0x0514 | ✅ `electrical-meter` profile |
| EP4 | Electrical Utility Meter | 0x0511 | ✅ `electrical-meter` profile |

SmartThings will create **one device** for the node, matched via EP3's device type 0x0514.

### Electrical Meter Profile (3 Components)

| Component | Capabilities | Matter Source |
|---|---|---|
| `main` | `powerMeter`, `voltageMeasurement`, `currentMeasurement`, `powerSource`, `firmwareUpdate`, `refresh` | EPM cluster on EP3 |
| `importedEnergy` | `energyMeter`, `powerConsumptionReport` | EEM `CumulativeEnergyImported` on EP3 |
| `exportedEnergy` | `energyMeter`, `powerConsumptionReport` | EEM `CumulativeEnergyExported` on EP3 |

### Attribute → Capability Mapping

| Matter Attribute (EP3) | Cluster | Capability | Component |
|---|---|---|---|
| `ActivePower` (0x0008) | EPM | `powerMeter.power` (W) | `main` |
| `RMSVoltage` (0x000B) | EPM | `voltageMeasurement.voltage` (V) | `main` |
| `RMSCurrent` (0x000C) | EPM | `currentMeasurement.current` (A) | `main` |
| `PowerMode` (0x0000) | EPM | `powerSource.powerSource` | `main` |
| `CumulativeEnergyImported` (0x0000) | EEM | `energyMeter.energy` + `powerConsumptionReport` | `importedEnergy` |
| `CumulativeEnergyExported` (0x0001) | EEM | `energyMeter.energy` + `powerConsumptionReport` | `exportedEnergy` |
| `CumulativeEnergyReset` (0x0005) | EEM | clears stored totals in driver | — |

> **Note:** The driver uses `RMSVoltage` (0x000B) and `RMSCurrent` (0x000C) rather than
> `Voltage` (0x0004) / `ActiveCurrent` (0x0005). RMS attributes are part of the ALTC
> (AlternatingCurrent) feature and are the correct attributes for AC billing meters.

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
Phase 2 Telemetry: tick — EP1 sensor cumE=45.0 kWh | EP3 meter import=52.0 kWh export=12.0 kWh
```

The tick log shows both endpoints. Only EP3 (Electrical Meter) is matched by the driver.

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
> Profile selection and device lifecycle events happen at commissioning time.

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

The simulator is a single Matter fabric node. SmartThings matches it against `fingerprints.yml`
and creates **one device** using the first matching device type:

| ST Device Name | Matched Endpoint | Profile |
|---|---|---|
| Matter Electrical Meter | EP3 (0x0514) | `electrical-meter` |

EP1 (0x0510) and EP2 (0x050D) have no fingerprint in the updated driver and are ignored.
EP4 (0x0511) also matches `electrical-meter` but is on the same node; a second device may or
may not be created depending on ST commissioning behavior.

---

## Step 4 — Verify Driver Fingerprint and Profile Selection

In Terminal B (logcat), look for these lines immediately after commissioning:

### 4.1 Driver recognizes the device

```
[INFO] matter-energy: Device init: <device-id>, endpoints: 5
[INFO] matter-energy: doConfigure: Starting profile selection for <device-id>
```

### 4.2 Expected profile selection for Electrical Meter

```
[DEBUG] matter-energy: Device type detection - EVSE: 0, Solar: 0, Battery: 0, Meter: 1, DEM: 0
[DEBUG] matter-energy: Standalone Electrical Meter detected on endpoints: 3
[INFO]  matter-energy: Updating device profile to electrical-meter
```

### 4.3 Profile selection verification

Confirm the profile was selected correctly:

```
[INFO] matter-energy: Updating device profile to electrical-meter
```

| Profile selected | Meaning |
|---|---|
| `electrical-meter` | ✅ Correct — 3-component profile (main, importedEnergy, exportedEnergy) |
| anything else | ❌ Unexpected — check fingerprints.yml and driver version |

### 4.4 Energy measurement mode detection

```
[INFO] matter-energy: Energy measurement: 1 endpoints found, 1 support cumulative energy
```

This confirms EP3 advertises the CUME feature bit and the driver will use
`CumulativeEnergyImported` / `CumulativeEnergyExported` (not the periodic fallback).

---

## Step 5 — Validate the Electrical Meter Device in SmartThings App

Open the created device in the SmartThings app. Cross-check values against chip-tool.

### 5.1 Expected capabilities and attribute sources

| Capability | Attribute | Matter Source | Notes |
|---|---|---|---|
| `powerMeter.power` (W) | `ActivePower` ÷ 1000 | EPM EP3 | Updates every ~10 s |
| `voltageMeasurement.voltage` (V) | `RMSVoltage` ÷ 1000 | EPM EP3 attr 0x000B | ALTC feature |
| `currentMeasurement.current` (A) | `RMSCurrent` ÷ 1000 | EPM EP3 attr 0x000C | ALTC feature |
| `powerSource.powerSource` | `PowerMode` = AC | EPM EP3 | Static: `mains` |
| `energyMeter.energy` (Wh) on `importedEnergy` | `CumulativeEnergyImported.energy` ÷ 1000 | EEM EP3 | 15-min throttle |
| `powerConsumptionReport` on `importedEnergy` | delta energy + timestamps | EEM EP3 | 15-min throttle |
| `energyMeter.energy` (Wh) on `exportedEnergy` | `CumulativeEnergyExported.energy` ÷ 1000 | EEM EP3 | 15-min throttle |
| `powerConsumptionReport` on `exportedEnergy` | delta energy + timestamps | EEM EP3 | 15-min throttle |

### 5.2 Cross-check with chip-tool (run from `connectedhomeip` repo root)

All reads target **EP3** (Electrical Meter):

```bash
# Power measurement — ActivePower, RMSVoltage, RMSCurrent, PowerMode
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read active-power  0x12344321 3 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read rms-voltage   0x12344321 3 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read rms-current   0x12344321 3 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement read power-mode    0x12344321 3 --storage-directory /tmp/chip-tool-phase2

# Energy measurement — CumulativeEnergyImported, CumulativeEnergyExported
./out/linux-x64-chip-tool/chip-tool electricalenergymeasurement read cumulative-energy-imported 0x12344321 3 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalenergymeasurement read cumulative-energy-exported 0x12344321 3 --storage-directory /tmp/chip-tool-phase2
./out/linux-x64-chip-tool/chip-tool electricalenergymeasurement read cumulative-energy-reset    0x12344321 3 --storage-directory /tmp/chip-tool-phase2
```

**Expected values (based on simulator telemetry — EP3):**

| Attribute | chip-tool raw | SmartThings display |
|---|---|---|
| `ActivePower` | ~1,740,000–1,940,000 mW | ~1740–1940 W |
| `RMSVoltage` | ~225,000–235,000 mV | ~225–235 V |
| `RMSCurrent` | ~7,500–8,500 mA | ~7.5–8.5 A |
| `PowerMode` | 2 (AC) | `powerSource: mains` |
| `CumulativeEnergyImported.energy` | increasing mWh (starts ~52,000,000) | increasing Wh |
| `CumulativeEnergyExported.energy` | increasing mWh (starts ~12,000,000, ~25% of import rate) | increasing Wh |

> **Why RMSVoltage / RMSCurrent?** The old simulator seeded only `Voltage` (0x0004) and
> `ActiveCurrent` (0x0005). The updated simulator now also seeds `RMSVoltage` (0x000B) and
> `RMSCurrent` (0x000C) — the attributes the updated ST driver subscribes to for
> `voltageMeasurement` and `currentMeasurement`. Without this, those capability tiles would
> stay blank even with a working subscription.

### 5.3 Logcat lines to watch

```
[DEBUG] matter-energy: ActivePower received: EP=3, Value=1866025 mW (1866.03 W)
[DEBUG] matter-energy: RMSVoltage received: EP=3, Value=229830 mV (229.83 V)
[DEBUG] matter-energy: RMSCurrent received: EP=3, Value=8460 mA (8.46 A)
[DEBUG] matter-energy: PowerMode received: EP=3, Mode=AC
[DEBUG] matter-energy: Cumulative Energy Imported: EP=3, Raw=<mWh> mWh
[DEBUG] matter-energy: Energy report: Imported=<Wh> Wh, Total=<Wh> Wh, Component=importedEnergy
[DEBUG] matter-energy: Cumulative Energy Exported: EP=3, Raw=<mWh> mWh
[DEBUG] matter-energy: Energy report: Exported=<Wh> Wh, Total=<Wh> Wh, Component=exportedEnergy
```

---

## Step 6 — Observe Live Updates

### 6.1 Power updates (immediate)

The simulator updates electrical measurements every ~10 seconds:

```bash
smartthings edge:drivers:logcat --log-level debug | grep -E "ActivePower received|RMSVoltage received|RMSCurrent received|Energy report"
```

**With chip-tool subscribe (EP3):**

```bash
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement subscribe active-power 5 60 \
  0x12344321 3 --storage-directory /tmp/chip-tool-phase2 --keepSubscriptions true
```

The `powerMeter.power` value in the SmartThings app should refresh within a few seconds of
each new telemetry tick.

### 6.2 Voltage and current (immediate)

Verify RMSVoltage and RMSCurrent subscriptions:

```bash
./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement subscribe rms-voltage 5 60 \
  0x12344321 3 --storage-directory /tmp/chip-tool-phase2 --keepSubscriptions true

./out/linux-x64-chip-tool/chip-tool electricalpowermeasurement subscribe rms-current 5 60 \
  0x12344321 3 --storage-directory /tmp/chip-tool-phase2 --keepSubscriptions true
```

Both should report new values every ~10 seconds alongside active-power.

### 6.3 Energy updates (wait 15 minutes)

The `energyMeter` and `powerConsumptionReport` capabilities on both `importedEnergy` and
`exportedEnergy` components are throttled by a **15-minute minimum interval**.

1. Leave the simulator running and the device commissioned.
2. Wait at least **15 minutes** after commissioning.
3. Watch logcat for the first cumulative energy emission on both components:

```
[DEBUG] matter-energy: Energy report: Imported=<Wh> Wh, Total=<Wh> Wh, Component=importedEnergy
[INFO]  matter-energy: emitting event energyMeter on importedEnergy: {"value":<Wh>,"unit":"Wh"}
[DEBUG] matter-energy: Energy report: Exported=<Wh> Wh, Total=<Wh> Wh, Component=exportedEnergy
[INFO]  matter-energy: emitting event energyMeter on exportedEnergy: {"value":<Wh>,"unit":"Wh"}
```

4. Check the SmartThings app — both the `importedEnergy` and `exportedEnergy` component
   tiles should show non-zero, increasing energy values.

---

## Step 7 — SmartThings App Device Card Checklist

> **Before checking:** Confirm in logcat that `Updating device profile to electrical-meter`
> was logged. The 3-component profile presents as separate sections in the device card.

### Electrical Meter device card

- [ ] Device name: set during commissioning (e.g., "Matter Electrical Meter" or "Matter Device")
- [ ] Category: `PowerMeasurementSensor` (set in profile; visible in ST developer console)

**`main` component tiles:**
- [ ] `powerMeter` tile shows a power value in watts (~1740–1940 W), updating every ~10 s
- [ ] `voltageMeasurement` tile shows ~225–235 V, updating every ~10 s
- [ ] `currentMeasurement` tile shows ~7.5–8.5 A, updating every ~10 s
- [ ] `powerSource` shows **AC power supply** (display text for `mains`)

**`importedEnergy` component tiles** (requires 15-min wait):
- [ ] `energyMeter` tile shows a non-zero, increasing cumulative energy value (Wh)
- [ ] Activity history shows energy consumption entries for imported energy
- [ ] Toast "This device hasn't updated..." dismisses after the first `energyMeter` value arrives

**`exportedEnergy` component tiles** (requires 15-min wait):
- [ ] `energyMeter` tile shows a non-zero, increasing cumulative energy value (Wh)
- [ ] Exported energy value is roughly ~25% of the imported energy increment
- [ ] Activity history shows energy consumption entries for exported energy

---

## Expected Results Summary

| Feature | Matter 1.5 Spec / Confluence Doc | Test Status |
|---|---|---|
| Device profile | `electrical-meter` (3 components) | verify in logcat |
| Category | `PowerMeasurementSensor` | verify in ST console |
| `powerMeter` (ActivePower) | ✅ Mapped → `main` | verify in app |
| `voltageMeasurement` (RMSVoltage 0x000B) | ✅ Mapped → `main` | verify in app |
| `currentMeasurement` (RMSCurrent 0x000C) | ✅ Mapped → `main` | verify in app |
| `powerSource` (PowerMode) | ✅ Mapped → `main` | verify in app |
| Imported energy (CumulativeEnergyImported) | ✅ Mapped → `importedEnergy` | verify after 15 min |
| Exported energy (CumulativeEnergyExported) | ✅ Mapped → `exportedEnergy` | verify after 15 min |
| CumulativeEnergyReset clears stored totals | ✅ Handler registered | verify via event trigger |
| Periodic energy fallback | ✅ Used only when CUME absent | not needed for meter |
| Tariff clusters (Commodity Price, Grid Conditions) | Out of scope — not implemented | — |

---

## Hub Log Quick Reference

| What you want to confirm | Log pattern to grep |
|---|---|
| Driver started | `Starting matter-energy driver` |
| Device recognized | `Device init: <id>` |
| Profile selected | `Updating device profile to electrical-meter` |
| Power reports flowing | `ActivePower received: EP=3` |
| RMSVoltage flowing | `RMSVoltage received: EP=3` |
| RMSCurrent flowing | `RMSCurrent received: EP=3` |
| Imported energy reports | `Energy report: Imported=` |
| Exported energy reports | `Energy report: Exported=` |
| Cumulative energy supported | `support cumulative energy` |
| Fallback to periodic | `Cumulative energy NOT supported` |
| Meter reset received | `CumulativeEnergyReset received` |

---

## Troubleshooting

### Simulator not discovered by SmartThings app during commissioning

- Confirm phone, hub, and Linux PC are on the same LAN/subnet.
- Check mDNS: `avahi-browse -a | grep matter`
- Confirm port 5540 is not blocked: `ss -ulnp | grep 5540`
- If KVS is stale, restart the simulator with a clean KVS (see Step 1).

### Wrong profile selected (not `electrical-meter`)

With the updated driver, only 0x0514 and 0x0511 are fingerprinted. If a different profile
is selected, verify the driver version installed on the hub matches the updated codebase.
Check logcat for:
```
[DEBUG] matter-energy: Device type detection - Meter: 1
```
If `Meter: 0`, the descriptor cluster on EP3 is not advertising device type 0x0514 correctly.

### `voltageMeasurement` / `currentMeasurement` tiles are missing or blank

These capabilities are in the `main` component of `electrical-meter`. If missing:
1. Confirm profile was selected as `electrical-meter` (not an older profile version).
2. Confirm logcat shows `RMSVoltage received` and `RMSCurrent received` for EP=3.
3. If logcat shows neither, the simulator binary may predate the RMSVoltage/RMSCurrent
   seeding changes — rebuild the simulator.

### Device shows but values are zero or frozen

Check logcat for `ActivePower received: EP=3`. If absent, the subscription was not
established. Try removing and re-adding the device. Confirm the simulator is still running
and telemetry ticks appear in Terminal A.

### `energyMeter` / `powerConsumptionReport` never update (stay zero)

The 15-minute throttle gate is applied. For initial commissioning, the first report arrives
within 15 minutes. If still zero after 15 minutes, check:

```
[WARN] matter-energy: Cumulative energy NOT supported - using periodic reporting fallback
```

This should not appear for the Electrical Meter (simulator advertises CUME feature).

### `exportedEnergy` component always shows zero

Verify the simulator binary includes the exported energy seeding changes. Check chip-tool:

```bash
./out/linux-x64-chip-tool/chip-tool electricalenergymeasurement read cumulative-energy-exported \
  0x12344321 3 --storage-directory /tmp/chip-tool-phase2
```

If this returns `null` or zero, the simulator is not seeding exported energy — rebuild.

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

Remove the device from SmartThings app:
- Open the device → ⋮ menu → **Delete device**

To uninstall the driver from the hub:

```bash
smartthings edge:drivers:installed
smartthings edge:drivers:uninstall <driver-id> --hub <hub-id>
```

---

## Known Gaps and Open Items

| # | Item | Status |
|---|---|---|
| 1 | `CumulativeEnergyReset` handler clears stored totals | ✅ Implemented in driver |
| 2 | Null guard on `EnergyMeasurementStruct.energy` | ✅ Fixed in driver |
| 3 | `endSystime` extracted from `EnergyMeasurementStruct` for report timestamps | ✅ Fixed in driver |
| 4 | RMSVoltage (0x000B) / RMSCurrent (0x000C) seeded and updated in simulator | ✅ Fixed in simulator |
| 5 | `CumulativeEnergyExported` seeded and updated in simulator for EP3 | ✅ Fixed in simulator |
| 6 | Tariff clusters (Commodity Price, Grid Conditions, Commodity Tariff) | Out of scope per reviewer |
| 7 | `CommodityMetering` cluster on EP3 — `MeteredQuantity` returns NULL | Out of scope for MVP |
| 8 | `MeterIdentification` cluster on EP4 (Utility Meter) | Out of scope for MVP |

---

## Related Documents

| Document | Location |
|---|---|
| Phase 2 simulator build + run guide | [phase2_virtual_energy_simulator.md](./phase2_virtual_energy_simulator.md) |
| Energy reporting flow and spec compliance | `SmartThingsEdgeDrivers/drivers/SmartThings/matter-energy/ENERGY_REPORTING_FLOW.md` |
| Smoke test guide | [phase2_energy_smoke_test_guide.md](./phase2_energy_smoke_test_guide.md) |
| Comprehensive test guide | [phase2_energy_comprehensive_test_guide.md](./phase2_energy_comprehensive_test_guide.md) |
| Hub log reference | `SmartThingsEdgeDrivers/docs/MATTER_ENERGY_DRIVER_LOG_REFERENCE.md` |

---

## Actual Test Run Reference — June 9, 2026 (Pre-Reviewer-Changes)

> **Archived.** The June 9 test run predates the reviewer changes (Electrical Meter-only
> focus, RMSVoltage/RMSCurrent, 3-component profile). The findings documented below are
> preserved for historical reference but no longer represent the expected test behavior.
> The current test steps and expected results above reflect the updated driver and simulator.

**Summary of June 9 gaps (all now addressed):**

| Gap | Root Cause | Fix |
|---|---|---|
| Profile `electrical-sensor` selected | 0x0510 fingerprinted; EP1 matched first | Removed 0x0510 fingerprint; 0x0514 (EP3) now matches |
| No voltage / current tiles | Subscribed to `Voltage`/`ActiveCurrent`; profile had no V/A on old profile | Driver now subscribes to `RMSVoltage`/`RMSCurrent`; new 3-component profile has both |
| EP1 + EP3 power summed into one tile | Both endpoints mapped to same `powerMeter` on single device | EP3-only device now created; EP1 (0x0510) has no fingerprint match |
| No exported energy tile | Only `CumulativeEnergyImported` seeded in simulator | Simulator now seeds and tracks `CumulativeEnergyExported` on EP3 |
| Single device created (not three) | ST commissions one device per node | Expected and correct for updated design — one device, 3 components |

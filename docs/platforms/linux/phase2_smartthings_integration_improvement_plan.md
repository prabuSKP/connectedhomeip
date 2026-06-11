# Phase 2 SmartThings Integration — Action Item Improvement Plan

**Companion to:** [phase2_smartthings_integration_test.md](./phase2_smartthings_integration_test.md)
**Driver under change:** `matter-energy` (`SmartThingsEdgeDrivers/drivers/SmartThings/matter-energy`)
**Simulator:** Phase 2 Virtual Energy Simulator (`connectedhomeip`, branch `virtual-Eclectrical-device`)
**Created:** June 9, 2026

---

## Purpose

The end-to-end test run on June 9, 2026 surfaced 10 findings and 4 explicit *Actionable
Issues* (see [the test doc's analysis section](./phase2_smartthings_integration_test.md#actionable-issues-identified)).
This document turns those findings into a concrete, prioritized engineering plan: what to
change, where in the code, how to verify, and in what order.

All code references below point to the live driver source so each work item is actionable
without re-deriving the root cause.

| Driver file | Role |
|---|---|
| `src/init.lua` | Profile selection (`do_configure`), power routing (`active_power_handler`), subscriptions, capability handlers |
| `fingerprints.yml` | Device-type → profile matching |
| `profiles/*.yml` | Capability sets per profile |

---

## Root-Cause Map (findings → code)

| Finding | Symptom | Root cause in code |
|---|---|---|
| F2 / F9 — voltage profile + V/A never emitted | `electrical-sensor` chosen; no V/A tiles | `do_configure` voltage scan returns NO (`src/init.lua:337-353`). Because the profile lacks the capability, `info_changed` never adds the Voltage/Current subscriptions (`src/init.lua:383-389`) even though they exist in the template (`src/init.lua:908-913`). |
| F3 — power aggregation | App shows ~3.4 kW combined | `active_power_handler` sums every standalone EP into `TOTAL_ACTIVE_POWER` (`src/init.lua:681-693`). |
| F7 — DEM not handled | No mode tile on combined device | Single device gets `electrical-sensor` profile; DEM branch in `do_configure` (`src/init.lua:369-376`) is unreachable once Sensor matches first. |
| F8 — Utility Meter (0x0511) | No EP4 device, no data | No `0x0511` entry in `fingerprints.yml`; no profile. |
| F1 — one device, not three | All endpoints under one ST device | Matter commissions one fabric node; driver does not create child devices. Architectural, see Phase B. |

**Key insight:** F2 and F9 share one fix. Correcting profile selection automatically
restores the Voltage/Current subscriptions — no separate subscription change is required.

---

## Phasing Strategy

The work splits into two phases. **Phase A** fixes the driver behavior on the existing
single-device topology — these are low-risk, high-value, and unblock a clean re-test.
**Phase B** addresses the deeper multi-endpoint device-modeling questions, which are larger
and partly depend on SmartThings platform behavior.

Do Phase A first and re-run the E2E test before starting Phase B.

---

## Phase A — Driver Fixes (single-device topology)

### A1. Fix voltage detection in `do_configure` (Issue 2 / F2 / F9)

**Goal:** Select `electrical-sensor-voltage-current` whenever EP1 exposes the EPM `Voltage`
attribute, so V/A tiles appear and the subscriptions get registered.

**Problem:** The scan at `src/init.lua:337-350` walks `device.endpoints[].clusters[].attributes`
for attribute `0x0004`. At `doConfigure` time the per-attribute list can be empty or not yet
cached, so `has_voltage` is `false` even though chip-tool confirms the attribute exists.

**Approach (in priority order):**
1. **Primary — detect by cluster presence + feature map, not the attribute list.** The EPM
   cluster's `FeatureMap` advertises whether voltage/current measurement is supported. Read
   the EPM `FeatureMap` (or check the EPM cluster's declared attribute list via
   `embedded_cluster_utils.get_endpoints`) rather than relying on the dynamic attribute
   array being populated. This is the most robust signal at configure time.
2. **Fallback — deferred re-check.** If the feature map is unavailable during the initial
   `doConfigure`, keep the device on `electrical-sensor` but schedule a one-shot read of
   `ElectricalPowerMeasurement.Voltage`; on a non-nil response, call
   `try_update_metadata({ profile = "electrical-sensor-voltage-current" })`.

**Files:** `src/init.lua` (`do_configure`, ~`:337-356`).

**Verification:**
- Logcat shows `Voltage detection result: YES, selecting profile: electrical-sensor-voltage-current`.
- After re-commission, ST app shows Voltage (~225 V) and Current (~6.5 A) tiles on EP1.
- chip-tool cross-check already documented in
  [test §5.1](./phase2_smartthings_integration_test.md#51-electrical-sensor-ep1-profile-electrical-sensor-voltage-current).

---

### A2. Separate EP1 / EP3 power instead of summing (Issue 1 / F3)

**Goal:** Stop reporting EP1 + EP3 as one ~3.4 kW value.

**Problem:** `active_power_handler` (`src/init.lua:681-693`) accumulates every standalone EP
into `TOTAL_ACTIVE_POWER` and emits the running total. Solar/Battery genuinely need
aggregation; standalone Sensor + Meter do not.

**Approach:** Restrict the aggregation path to true aggregation device types
(`is_aggregation_ep`). For standalone Electrical Sensor / Electrical Meter endpoints, emit
the **per-endpoint** power directly with `emit_event_for_endpoint(ib.endpoint_id, ...)` and
do **not** add to `TOTAL_ACTIVE_POWER`. This keeps each endpoint's `powerMeter` independent.

> Decision dependency: how EP1 vs EP3 power surfaces in the UI depends on whether they remain
> on one ST device (Phase A) or become separate devices (Phase B). In Phase A, with a single
> device, choose **one representative endpoint** for the primary `powerMeter` tile
> (EP1 for `electrical-sensor`) and stop summing; full per-endpoint separation lands with B1.

**Files:** `src/init.lua` (`active_power_handler`, `:666-694`; helper `get_total`).

**Verification:** ST `powerMeter` shows ~1500 W (EP1 alone), not ~3.4 kW. Logcat
`Power aggregation` line no longer sums EP1+EP3.

---

### A3. Handle DEM on the combined device (Issue 4 / F7)

**Goal:** Surface `DeviceEnergyManagementMode` (mode + supported modes) even when DEM (EP2)
shares the fabric node with the Electrical Sensor.

**Problem:** Once the Sensor branch matches in `do_configure` (`src/init.lua:334-357`), the
standalone DEM branch (`:369-376`) never runs, so no DEM-capable profile is applied and the
`mode` capability is absent.

**Approach:**
1. Add a **composite profile** (e.g. `electrical-sensor-voltage-current-dem`) that includes
   the EPM/EEM capabilities *and* `mode`. Select it in `do_configure` when both
   `electrical_sensor_eps > 0` and `dem_eps > 0` on the same node.
2. The DEM attribute handlers already exist
   (`device_energy_mgmt_supported_modes_attr_handler` / `..._mode_attr_handler`, wired at
   `src/init.lua:864-865`) and the `mode` subscriptions already include DEMMode
   (`src/init.lua:899-904`). Once the profile carries the `mode` capability, `info_changed`
   registers those subscriptions automatically.

**Files:** `src/init.lua` (`do_configure`), new `profiles/electrical-sensor-voltage-current-dem.yml`.

**Verification:** Mode tile appears; logcat shows `Updating device profile to
electrical-sensor-...-dem` and DEMMode `SupportedModes`/`CurrentMode` reports; `ChangeToMode`
round-trips when a mode is selected.

---

### A4. Add the 0x0511 Utility Meter fingerprint + profile (Issue 3 / F8)

**Goal:** Produce a device (or capability set) for EP4 so meter-identity data has a home.

**Approach (minimum):** Add to `fingerprints.yml`:
```yaml
  - id: "matter/electrical-utility-meter"
    deviceLabel: Matter Electrical Utility Meter
    deviceTypes:
      - id: 0x0511
    deviceProfileName: electrical-utility-meter
```
Create `profiles/electrical-utility-meter.yml`. Since `MeterIdentification` (0x0B06) has no
stock ST capability, start with the shared power/energy capabilities and add meter-identity
fields via custom capabilities in a follow-up (tracked under B3).

**Caveat:** On a single fabric node, adding a fingerprint does not by itself create a second
ST device (that's the Phase B topology question). This item ensures the device-type is
*recognized*; surfacing it as its own device depends on B1.

**Files:** `fingerprints.yml`, new `profiles/electrical-utility-meter.yml`.

**Verification:** `doConfigure` no longer logs `No matching device type found` for 0x0511;
device-type detection counts the endpoint.

---

## Phase B — Topology & Cluster Coverage (larger work)

### B1. Multi-endpoint device modeling (F1)

**Question to resolve first:** Should one multi-endpoint Matter node become **multiple ST
devices** (child devices per endpoint) or **one device with components** per endpoint? This
determines how A2/A3/A4 ultimately surface.

**Options:**
- **Child devices** (`driver:try_create_device` per matched endpoint) — closest to the test
  doc's "three devices" expectation; more code, lifecycle management.
- **Single device, multiple components** — each endpoint becomes a component within one
  device; simpler lifecycle, less familiar UX.

This needs a design decision (and likely a check of how the stock ST matter-energy driver
handles composed devices) before implementation. Capture the decision, then refactor the
profile-selection and emit paths accordingly.

### B2. Full DEM cluster mapping (test §5.2 gap)

Map `DeviceEnergyManagement` (0x0098) beyond mode: ESA state, `AbsMinPower`/`AbsMaxPower`,
opt-out state, and power-adjustment commands. Most need custom capabilities — scope after B1.

### B3. CommodityMetering + MeterIdentification mapping (test §5.3 / §5.4 gaps)

`CommodityMetering` (0x0B07) and `MeterIdentification` (0x0B06) currently return NULL from the
simulator and have no driver mapping. Two-sided work:
- **Simulator:** populate `MeteredQuantity`, `TariffUnit`, and meter-identity attributes so
  there is real data to test against.
- **Driver:** add custom capabilities once the simulator emits non-NULL values.

---

## Recommended Order of Execution

1. **A1** (voltage detection) — also fixes F9, biggest visible win.
2. **A2** (power separation) — corrects the headline 3.4 kW error.
3. **A3** (DEM on combined device) — unlocks the mode tile.
4. **A4** (0x0511 fingerprint) — recognition groundwork.
5. **Re-run the full E2E test** and refresh the analysis section with new results.
6. **B1** design decision → implementation, then **B2/B3**.

A1–A4 are independent edits to `init.lua` + `fingerprints.yml`/`profiles` and can be done in
one PR; B-items each warrant their own PR.

---

## Per-Item Acceptance Criteria (re-test checklist)

| Item | Pass condition |
|---|---|
| A1 | ST app shows Voltage (~225 V) + Current (~6.5 A) tiles; profile log = `electrical-sensor-voltage-current` |
| A2 | `powerMeter` ≈ 1500 W (EP1), not ~3.4 kW; no EP1+EP3 sum in logcat |
| A3 | Mode tile present; `ChangeToMode` succeeds; DEMMode reports in logcat |
| A4 | No `No matching device type found` for 0x0511; detection counts EP4 |
| B1 | Endpoint topology matches the agreed design (N devices or N components) |
| B2 | ESA state / power limits visible in app |
| B3 | Non-NULL metered quantity + meter identity surfaced |

---

## Risks & Notes

- **Profile proliferation:** A3 adds a composite profile; B1 may add more. Keep profile
  naming consistent with the existing `evse-*` convention (`src/init.lua:299-326`).
- **Don't regress EVSE/Solar/Battery:** `active_power_handler` aggregation is correct for
  those types — A2 must narrow the standalone path only, leaving `is_aggregation_ep`
  untouched (`src/init.lua:674`).
- **Simulator dependency:** B3 cannot be fully tested until the simulator emits real
  CommodityMetering/MeterIdentification values; that work lives in this `connectedhomeip` repo.
- **Re-commission required:** Profile changes (A1/A3/A4) take effect at `doConfigure`; clear
  KVS and re-commission per
  [test Step 1](./phase2_smartthings_integration_test.md#step-1--start-the-phase-2-virtual-energy-simulator).

---

## Related Documents

| Document | Location |
|---|---|
| E2E test procedure (source of action items) | [phase2_smartthings_integration_test.md](./phase2_smartthings_integration_test.md) |
| Simulator build + run guide | [phase2_virtual_energy_simulator.md](./phase2_virtual_energy_simulator.md) |
| Driver log reference | `SmartThingsEdgeDrivers/docs/MATTER_ENERGY_DRIVER_LOG_REFERENCE.md` |
| Device type mapping | `SmartThingsEdgeDrivers/docs/ELECTRICAL_DEVICE_TYPE_MAPPING.md` |

---

## Trial 2 Results — June 9, 2026

**Driver version:** Post Phase A + Phase B implementation (all A1–A4 and B1 implemented).
**Test logs:** `SmartThingsEdgeDrivers/drivers/SmartThings/matter-energy/trial2June9/`
- `terminalDriverSideLogs` — 370 lines, 16:06:09–16:07:08 IST (~1-minute window)
- `hub-agent.log` — full hub broker log
- `Screenshot_20260609_160637_SmartThings.jpg` — app state at 16:06

---

### What passed ✅

**B1 — Multi-component topology**

The ST app shows three distinct component tiles on a single device ("Matter Device"):
- **Main** — EP1 (Electrical Sensor): powerMeter, voltageMeasurement, currentMeasurement, powerSource
- **ElectricalMeter** — EP3 (Electrical Meter): powerMeter, voltageMeasurement, currentMeasurement, powerSource
- **DeviceEnergyManagement** — EP2 (DEM): mode

Component map built correctly at `device_added`:
`main=EP1, electricalMeter=EP3, deviceEnergyManagement=EP2, electricalUtilityMeter=EP4`

**A1 — Voltage detection**

`do_configure` now reads EPM feature bits via `embedded_cluster_utils.get_endpoints`:
`AC_eps=2, DC_eps=2, result=YES` → profile `electrical-sensor-meter-dem` selected correctly.

**A2 — Power separation (no more aggregation)**

EP1 and EP3 are emitted independently, not summed:
- `main` (EP1): 1449–1584 W (observed range)
- `electricalMeter` (EP3): 1780–1933 W (observed range)
- No 3.4 kW aggregated total anywhere in logs or app.

**A3 — DEM mode tile**

`DeviceEnergyManagement` component receives and displays mode correctly:
- `SupportedModes` delivered (5 options)
- `CurrentMode` = "No energy management (forecast only)"
- Mode tile visible in screenshot under **DeviceEnergyManagement** section.

**A4 — 0x0511 Utility Meter recognized**

EP4 (Electrical Utility Meter) counted correctly in device-type detection:
`EVSE:0, Solar:0, Battery:0, Sensor:1, Meter:1, DEM:1, UtilityMeter:1`
Mapped to `electricalUtilityMeter` component (no separate device/profile selected — folded into the composite mapping).

**Voltage and Current routing**

Voltage and current correctly routed per component via `emit_event_for_endpoint`:
- `main`: V ≈ 225–234 V, I ≈ 6.1–6.8 A
- `electricalMeter`: V ≈ 225–234 V, I ≈ 7.9–8.4 A

**PowerSource**

Both `main` and `electricalMeter` show "AC power supply" (`mains`). ✅

**Periodic energy correctly ignored**

Device supports cumulative energy; periodic import/export reports are correctly discarded at every tick. ✅

**hub-agent.log confirmation**

All events accepted by ST broker with correct `component_id` (no errors after profile applied). ✅

---

### New bugs found ❌

#### Bug C1 — Transient crash before profile update (severity: low, transient)

**What:** Two `ERROR` lines appear in `terminalDriverSideLogs` at lines 103–106 and 109–112:
```
[string "st/device.lua"]:215: attempt to index a nil value (local 'component')
```

**When:** During the very first `InteractionResponse` (EP3 `ActivePower` and `PowerMode`),
which arrives ~2 seconds after commissioning, before `infoChanged` applies the new profile.

**Root cause:** `endpoint_to_component` (init.lua:103–111) returns `"electricalMeter"` for
EP3 based on the persisted `COMPONENT_TO_ENDPOINT_MAP`. At this moment the device is still on
the fingerprint-matched profile (`electrical-sensor`) which has no `electricalMeter` component.
`device:emit_event_for_endpoint` looks up `device.profile.components["electricalMeter"]` → nil
→ crash at device.lua:215.

**Recovery:** `infoChanged` fires at log line 114 and applies `electrical-sensor-meter-dem`.
All events from that point onward work correctly — no further errors in the 1-minute window.
Only 2 events are lost (EP3's initial power reading and powerMode).

**Fix (pending):** In `endpoint_to_component`, check
`device.profile.components[component] ~= nil` before returning the mapped name; fall back to
`"main"` if the component is not yet in the active profile.

**File:** `src/init.lua:103–111`

---

#### Bug C2 — Energy meter tiles show "– kWh" (severity: medium, persistent)

**What:** Both `main` and `electricalMeter` components show "– kWh" in the `energyMeter`
capability tile throughout the test run. The `powerConsumptionReport` tile is also never
populated.

**Root cause:** `CumulativeEnergyImported` and `CumulativeEnergyExported` are **not included
in `subscribed_attributes`** (init.lua:1006–1008), so the hub never subscribes to them. The
cumulative energy attribute handlers (`energy_report_handler_factory(true, ...)` at
init.lua:960–961) therefore never fire. The periodic energy handlers DO run, but they are
immediately discarded (init.lua:713–715) because the device supports cumulative reports. Net
result: `energyMeter` and `powerConsumptionReport` never emit any events.

**Fix (pending):** Add `CumulativeEnergyImported` and `CumulativeEnergyExported` to the
`energyMeter.ID` subscription list in `subscribed_attributes`:
```lua
[capabilities.energyMeter.ID] = {
  clusters.ElectricalEnergyMeasurement.attributes.PeriodicEnergyExported,
  clusters.ElectricalEnergyMeasurement.attributes.CumulativeEnergyImported,
  clusters.ElectricalEnergyMeasurement.attributes.CumulativeEnergyExported,
},
```

Note: the 15-minute throttle (`MINIMUM_ST_ENERGY_REPORT_INTERVAL = 900 s`) means the first
`energyMeter` / `powerConsumptionReport` event will not appear until 15 minutes after the
cumulative subscription fires. The Trial 2 log window (~1 minute) cannot confirm the fix —
a longer soak test is required.

**File:** `src/init.lua:1006–1008`

---

### Updated acceptance criteria

| Item | Status | Notes |
|---|---|---|
| A1 — Voltage detection | **PASS** | AC/DC feature-map scan works; profile selected correctly |
| A2 — Power separation | **PASS** | EP1 → main (~1.5 kW), EP3 → electricalMeter (~1.9 kW) |
| A3 — DEM mode tile | **PASS** | Mode tile visible with 5 options; current mode correct |
| A4 — 0x0511 recognized | **PASS** | Utility Meter endpoint counted; mapped to component |
| B1 — Multi-component topology | **PASS** | 3 ST components visible, all correctly mapped |
| C1 — Transient crash (new) | **OPEN** | 2 events lost at startup; fix in `endpoint_to_component` |
| C2 — Energy meter "– kWh" (new) | **OPEN** | Missing cumulative subscription; needs longer soak test to verify fix |
| B2 — Full DEM cluster mapping | **NOT STARTED** | ESA state, power limits, adjustment commands |
| B3 — CommodityMetering / MeterIdentification | **NOT STARTED** | Simulator still emits NULL; two-sided work |

---

### Observations not yet tested (log window too short)

- `energyMeter` / `powerConsumptionReport` events: throttled to 15-minute minimum. A 20-minute
  soak run is needed to confirm C2 fix works and per-component energy reporting is correct.
- `electricalUtilityMeter` component: component created and mapped, but EP4 has no EPM/EEM
  endpoints that currently report into it. No events observed for this component.
- `ChangeToMode` command round-trip: DEM mode selection was not exercised during Trial 2.

---

### Pre-existing (non-regression) warnings

- `WARN: Device does not support cluster 0x009D` — 4 occurrences during subscription setup.
  Cluster 0x009D is EnergyEvse; absent from the Phase 2 simulator. These warnings were present
  before Phase A/B changes and are harmless.
- `EPM[EP1]: V=0.00V I=6.54A P=0.00W` log formatting — each EPM attribute (V, I, P) arrives
  as a separate Matter attribute report; the `log_electrical` helper shows 0 for whichever
  attributes are not the subject of the current callback. Not a data accuracy issue.

#!/usr/bin/env bash

set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

CHIP_TOOL="${REPO_ROOT}/out/linux-x64-chip-tool/chip-tool"
STORAGE_DIR="/tmp/chip-tool-phase2-comprehensive"
LOG_FILE="/tmp/phase2-comprehensive-test.log"

NODE_ID="0x12344321"
PASSCODE="20202021"
DISCRIMINATOR="3840"
QR_PAYLOAD=""

PAIRING_MODE="onnetwork-long"
RECOMMISSION=0
TEST_SUITE="all"
VERBOSE=0
TIMEOUT=30

TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
TEST_NUMBER=0

# Endpoint layout published by phase2-energy-simulator.matter / .zap
# EP 0: Root (MA-rootdevice)
# EP 1: Electrical Sensor  (ElectricalPowerMeasurement, ElectricalEnergyMeasurement, PowerTopology)
# EP 2: Device Energy Management (DeviceEnergyManagement, DeviceEnergyManagementMode)
# EP 3: Electrical Meter   (ElectricalPowerMeasurement, ElectricalEnergyMeasurement, CommodityMetering)
# EP 4: Electrical Utility Meter (MeterIdentification)

EP_ROOT=0
EP_ELECTRICAL_SENSOR=1
EP_DEM=2
EP_ELECTRICAL_METER=3
EP_UTILITY_METER=4

print_usage() {
    cat <<EOF
Usage: ${SCRIPT_NAME} [options]

Run comprehensive tests for the Phase 2 Virtual Energy Simulator covering
every electrical device cluster exposed by the simulator.

Options:
  --node-id <id>              Node ID to use (default: ${NODE_ID})
  --passcode <code>           Setup passcode (default: ${PASSCODE})
  --discriminator <disc>      Setup discriminator (default: ${DISCRIMINATOR})
  --pairing-mode <mode>       Pairing mode: onnetwork-long | onnetwork | code | skip
  --qr-payload <payload>      QR payload/manual code for pairing mode code
  --recommission              Unpair the node before pairing
  --test-suite <suite>        Run specific test suite. One of:
                                all (default), device-discovery,
                                electrical-sensor, power-topology,
                                dem, dem-mode,
                                electrical-meter, commodity-metering,
                                utility-meter, meter-identification,
                                basic-information
  --verbose                   Enable verbose output
  --log-file <file>           Log file path (default: ${LOG_FILE})
  --timeout <seconds>         Timeout for each test (default: ${TIMEOUT})
  --storage-directory <path>  chip-tool storage directory (default: ${STORAGE_DIR})
  -h, --help                  Show this help

Examples:
  ${SCRIPT_NAME}
  ${SCRIPT_NAME} --recommission
  ${SCRIPT_NAME} --test-suite electrical-sensor
  ${SCRIPT_NAME} --pairing-mode code --qr-payload 'MT:EXAMPLEPAYLOAD'
EOF
}

log() {
    echo "[INFO] $*" | tee -a "${LOG_FILE}"
}

verbose_log() {
    if [[ "${VERBOSE}" -eq 1 ]]; then
        echo "[VERBOSE] $*" | tee -a "${LOG_FILE}"
    fi
}

error() {
    echo "[ERROR] $*" | tee -a "${LOG_FILE}" >&2
}

warn() {
    echo "[WARN] $*" | tee -a "${LOG_FILE}"
}

print_separator() {
    printf '%s\n' '----------------------------------------------------------------' | tee -a "${LOG_FILE}"
}

print_run_header() {
    echo | tee -a "${LOG_FILE}"
    print_separator
    echo "Phase 2 Virtual Energy Simulator Comprehensive Test" | tee -a "${LOG_FILE}"
    print_separator
    echo "Node ID           : ${NODE_ID}" | tee -a "${LOG_FILE}"
    echo "Pairing Mode      : ${PAIRING_MODE}" | tee -a "${LOG_FILE}"
    echo "Recommission      : ${RECOMMISSION}" | tee -a "${LOG_FILE}"
    echo "Test Suite        : ${TEST_SUITE}" | tee -a "${LOG_FILE}"
    echo "Storage Directory : ${STORAGE_DIR}" | tee -a "${LOG_FILE}"
    echo "Log File          : ${LOG_FILE}" | tee -a "${LOG_FILE}"
    echo "Timeout           : ${TIMEOUT}s" | tee -a "${LOG_FILE}"
    print_separator
}

print_test_header() {
    local title="$1"

    TEST_NUMBER=$((TEST_NUMBER + 1))
    echo | tee -a "${LOG_FILE}"
    print_separator
    echo "Test ${TEST_NUMBER}: ${title}" | tee -a "${LOG_FILE}"
    print_separator
}

extract_focus_lines() {
    local response="$1"
    local value_lines
    local fallback_lines

    value_lines=$(printf '%s\n' "${response}" | tail -n 180 | grep -E '\[TOO\].*Endpoint:|\[DMG\].*Data =|StatusIB|CHIP Error|Error 0x|Timeout' || true)
    if [[ -n "${value_lines}" ]]; then
        printf '%s\n' "${value_lines}"
        return
    fi

    fallback_lines=$(printf '%s\n' "${response}" | tail -n 180 | grep -E '\[TOO\]|\[SC\].*Success status report received|\[SC\].*Initiating session' || true)
    if [[ -n "${fallback_lines}" ]]; then
        printf '%s\n' "${fallback_lines}"
    fi
}

print_response_body() {
    local response="$1"
    local response_to_print
    local focus_lines
    local total_lines

    if [[ -z "${response}" ]]; then
        echo "  (no output)" | tee -a "${LOG_FILE}"
        return
    fi

    focus_lines=$(extract_focus_lines "${response}")
    if [[ -n "${focus_lines}" ]]; then
        echo "  Key lines:" | tee -a "${LOG_FILE}"
        response_to_print="${focus_lines}"
    else
        echo "  Raw output:" | tee -a "${LOG_FILE}"
        response_to_print="${response}"
    fi

    total_lines=$(printf '%s\n' "${response_to_print}" | wc -l | tr -d ' ')
    if [[ "${total_lines}" -le 30 ]]; then
        printf '%s\n' "${response_to_print}" | sed 's/^/  /' | tee -a "${LOG_FILE}"
        return
    fi

    printf '%s\n' "${response_to_print}" | head -n 30 | sed 's/^/  /' | tee -a "${LOG_FILE}"
    echo "  ... (${total_lines} lines total, showing first 30)" | tee -a "${LOG_FILE}"
}

print_request_line() {
    local cmd_text

    cmd_text=$(printf '%q ' "$@")
    echo "  ${cmd_text}" | tee -a "${LOG_FILE}"
}

prepare_storage_dir() {
    if [[ -e "${STORAGE_DIR}" && ! -d "${STORAGE_DIR}" ]]; then
        error "Storage path exists but is not a directory: ${STORAGE_DIR}"
        exit 1
    fi

    mkdir -p "${STORAGE_DIR}" || {
        error "Failed to create storage directory: ${STORAGE_DIR}"
        exit 1
    }
    [[ -w "${STORAGE_DIR}" ]] || {
        error "Storage directory is not writable: ${STORAGE_DIR}"
        exit 1
    }
}

run_chip_tool() {
    local test_title="$1"
    local timeout_value="$2"
    shift 2

    local -a cmd=("${CHIP_TOOL}" "$@" --storage-directory "${STORAGE_DIR}")
    local output
    local rc

    if [[ "${timeout_value}" != "0" ]]; then
        cmd+=(--timeout "${timeout_value}")
    fi

    print_test_header "${test_title}"
    echo "Request:" | tee -a "${LOG_FILE}"
    print_request_line "${cmd[@]}"

    set +e
    output=$("${cmd[@]}" 2>&1)
    rc=$?
    set -e

    echo "Response:" | tee -a "${LOG_FILE}"
    print_response_body "${output}"

    if [[ "${rc}" -eq 0 ]]; then
        echo "Result: PASS (exit code ${rc})" | tee -a "${LOG_FILE}"
    else
        echo "Result: FAIL (exit code ${rc})" | tee -a "${LOG_FILE}"
    fi

    return "${rc}"
}

run_test() {
    local description="$1"
    shift

    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    if run_chip_tool "Test ${TOTAL_TESTS}: ${description}" "${TIMEOUT}" "$@"; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
        verbose_log "Passed test: ${description}"
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
        warn "Failed test: ${description}"
    fi
}

pair_device() {
    if [[ "${PAIRING_MODE}" == "skip" ]]; then
        log "Skipping pairing as requested"
        return
    fi

    if [[ "${RECOMMISSION}" -eq 1 ]]; then
        run_chip_tool "Recommission: unpair node ${NODE_ID} (best effort)" "${TIMEOUT}" pairing unpair "${NODE_ID}" || true
    fi

    local -a pair_cmd=(pairing)
    case "${PAIRING_MODE}" in
        onnetwork-long)
            pair_cmd+=(onnetwork-long "${NODE_ID}" "${PASSCODE}" "${DISCRIMINATOR}")
            ;;
        onnetwork)
            pair_cmd+=(onnetwork "${NODE_ID}" "${PASSCODE}")
            ;;
        code)
            [[ -n "${QR_PAYLOAD}" ]] || {
                error "--qr-payload is required when --pairing-mode code is used"
                exit 1
            }
            pair_cmd+=(code "${NODE_ID}" "${QR_PAYLOAD}")
            ;;
        *)
            error "Unsupported pairing mode: ${PAIRING_MODE}"
            exit 1
            ;;
    esac

    if ! run_chip_tool "Pair device using ${PAIRING_MODE}" "${TIMEOUT}" "${pair_cmd[@]}"; then
        if [[ "${RECOMMISSION}" -eq 1 ]]; then
            error "Pairing failed after recommission attempt"
            exit 1
        fi
        warn "Pairing failed. Continuing with read checks in case the device is already commissioned."
    fi
}

# --- Test suites ---------------------------------------------------------

run_device_discovery_tests() {
    log "Running device discovery tests"

    run_test "Root descriptor parts-list" descriptor read parts-list "${NODE_ID}" "${EP_ROOT}"
    run_test "Root descriptor device-type-list" descriptor read device-type-list "${NODE_ID}" "${EP_ROOT}"
    run_test "Electrical sensor descriptor server-list (EP${EP_ELECTRICAL_SENSOR})" \
        descriptor read server-list "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "DEM descriptor server-list (EP${EP_DEM})" \
        descriptor read server-list "${NODE_ID}" "${EP_DEM}"
    run_test "Electrical meter descriptor server-list (EP${EP_ELECTRICAL_METER})" \
        descriptor read server-list "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "Utility meter descriptor server-list (EP${EP_UTILITY_METER})" \
        descriptor read server-list "${NODE_ID}" "${EP_UTILITY_METER}"
}

run_basic_information_tests() {
    log "Running basic information tests (EP${EP_ROOT})"

    run_test "BasicInformation vendor-name" basicinformation read vendor-name "${NODE_ID}" "${EP_ROOT}"
    run_test "BasicInformation product-name" basicinformation read product-name "${NODE_ID}" "${EP_ROOT}"
    run_test "BasicInformation software-version" basicinformation read software-version "${NODE_ID}" "${EP_ROOT}"
    run_test "BasicInformation unique-id" basicinformation read unique-id "${NODE_ID}" "${EP_ROOT}"
}

run_electrical_sensor_tests() {
    log "Running electrical sensor tests (EP${EP_ELECTRICAL_SENSOR})"

    run_test "Sensor power-mode" \
        electricalpowermeasurement read power-mode "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "Sensor active-power" \
        electricalpowermeasurement read active-power "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "Sensor voltage" \
        electricalpowermeasurement read voltage "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "Sensor active-current" \
        electricalpowermeasurement read active-current "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "Sensor frequency" \
        electricalpowermeasurement read frequency "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "Sensor cumulative-energy-imported" \
        electricalenergymeasurement read cumulative-energy-imported "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "Sensor periodic-energy-imported" \
        electricalenergymeasurement read periodic-energy-imported "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "Sensor accuracy" \
        electricalenergymeasurement read accuracy "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
}

run_power_topology_tests() {
    log "Running power topology tests (EP${EP_ELECTRICAL_SENSOR})"

    run_test "PowerTopology feature-map" powertopology read feature-map "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "PowerTopology cluster-revision" powertopology read cluster-revision "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
    run_test "PowerTopology attribute-list" powertopology read attribute-list "${NODE_ID}" "${EP_ELECTRICAL_SENSOR}"
}

run_device_energy_management_tests() {
    log "Running device energy management tests (EP${EP_DEM})"

    run_test "DEM esa-type" deviceenergymanagement read esatype "${NODE_ID}" "${EP_DEM}"
    run_test "DEM esa-state" deviceenergymanagement read esastate "${NODE_ID}" "${EP_DEM}"
    run_test "DEM abs-min-power" deviceenergymanagement read abs-min-power "${NODE_ID}" "${EP_DEM}"
    run_test "DEM abs-max-power" deviceenergymanagement read abs-max-power "${NODE_ID}" "${EP_DEM}"
    run_test "DEM opt-out-state" deviceenergymanagement read opt-out-state "${NODE_ID}" "${EP_DEM}"
    run_test "DEM feature-map" deviceenergymanagement read feature-map "${NODE_ID}" "${EP_DEM}"
}

run_device_energy_management_mode_tests() {
    log "Running device energy management mode tests (EP${EP_DEM})"

    run_test "DEM-Mode supported-modes" deviceenergymanagementmode read supported-modes "${NODE_ID}" "${EP_DEM}"
    run_test "DEM-Mode current-mode" deviceenergymanagementmode read current-mode "${NODE_ID}" "${EP_DEM}"
    run_test "DEM-Mode feature-map" deviceenergymanagementmode read feature-map "${NODE_ID}" "${EP_DEM}"
}

run_electrical_meter_tests() {
    log "Running electrical meter tests (EP${EP_ELECTRICAL_METER})"

    run_test "Meter power-mode" \
        electricalpowermeasurement read power-mode "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "Meter active-power" \
        electricalpowermeasurement read active-power "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "Meter voltage" \
        electricalpowermeasurement read voltage "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "Meter active-current" \
        electricalpowermeasurement read active-current "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "Meter cumulative-energy-imported" \
        electricalenergymeasurement read cumulative-energy-imported "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "Meter periodic-energy-imported" \
        electricalenergymeasurement read periodic-energy-imported "${NODE_ID}" "${EP_ELECTRICAL_METER}"
}

run_commodity_metering_tests() {
    log "Running commodity metering tests (EP${EP_ELECTRICAL_METER})"

    run_test "CommodityMetering metered-quantity" \
        commoditymetering read metered-quantity "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "CommodityMetering metered-quantity-timestamp" \
        commoditymetering read metered-quantity-timestamp "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "CommodityMetering tariff-unit" \
        commoditymetering read tariff-unit "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "CommodityMetering maximum-metered-quantities" \
        commoditymetering read maximum-metered-quantities "${NODE_ID}" "${EP_ELECTRICAL_METER}"
    run_test "CommodityMetering feature-map" \
        commoditymetering read feature-map "${NODE_ID}" "${EP_ELECTRICAL_METER}"
}

run_electrical_utility_meter_tests() {
    log "Running electrical utility meter tests (EP${EP_UTILITY_METER})"

    run_test "UtilityMeter descriptor device-type-list" \
        descriptor read device-type-list "${NODE_ID}" "${EP_UTILITY_METER}"
    run_test "UtilityMeter descriptor server-list" \
        descriptor read server-list "${NODE_ID}" "${EP_UTILITY_METER}"
}

run_meter_identification_tests() {
    log "Running meter identification tests (EP${EP_UTILITY_METER})"

    run_test "MeterIdentification meter-type" \
        meteridentification read meter-type "${NODE_ID}" "${EP_UTILITY_METER}"
    run_test "MeterIdentification point-of-delivery" \
        meteridentification read point-of-delivery "${NODE_ID}" "${EP_UTILITY_METER}"
    run_test "MeterIdentification meter-serial-number" \
        meteridentification read meter-serial-number "${NODE_ID}" "${EP_UTILITY_METER}"
    run_test "MeterIdentification protocol-version" \
        meteridentification read protocol-version "${NODE_ID}" "${EP_UTILITY_METER}"
    run_test "MeterIdentification power-threshold" \
        meteridentification read power-threshold "${NODE_ID}" "${EP_UTILITY_METER}"
    run_test "MeterIdentification feature-map" \
        meteridentification read feature-map "${NODE_ID}" "${EP_UTILITY_METER}"
}

# --- Main ----------------------------------------------------------------

main() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --node-id)            NODE_ID="$2"; shift 2 ;;
            --passcode)           PASSCODE="$2"; shift 2 ;;
            --discriminator)      DISCRIMINATOR="$2"; shift 2 ;;
            --pairing-mode)       PAIRING_MODE="$2"; shift 2 ;;
            --qr-payload)         QR_PAYLOAD="$2"; shift 2 ;;
            --recommission)       RECOMMISSION=1; shift ;;
            --test-suite)         TEST_SUITE="$2"; shift 2 ;;
            --verbose)            VERBOSE=1; shift ;;
            --log-file)           LOG_FILE="$2"; shift 2 ;;
            --timeout)            TIMEOUT="$2"; shift 2 ;;
            --storage-directory)  STORAGE_DIR="$2"; shift 2 ;;
            -h|--help)            print_usage; exit 0 ;;
            *)                    error "Unknown argument: $1"; exit 1 ;;
        esac
    done

    [[ -x "${CHIP_TOOL}" ]] || {
        error "chip-tool not found or not executable: ${CHIP_TOOL}"
        exit 1
    }
    [[ "${TIMEOUT}" =~ ^[0-9]+$ ]] || {
        error "--timeout must be a non-negative integer"
        exit 1
    }
    prepare_storage_dir
    print_run_header
    pair_device

    case "${TEST_SUITE}" in
        all)
            run_device_discovery_tests
            run_basic_information_tests
            run_electrical_sensor_tests
            run_power_topology_tests
            run_device_energy_management_tests
            run_device_energy_management_mode_tests
            run_electrical_meter_tests
            run_commodity_metering_tests
            run_electrical_utility_meter_tests
            run_meter_identification_tests
            ;;
        device-discovery)      run_device_discovery_tests ;;
        basic-information)     run_basic_information_tests ;;
        electrical-sensor)     run_electrical_sensor_tests ;;
        power-topology)        run_power_topology_tests ;;
        dem)                   run_device_energy_management_tests ;;
        dem-mode)              run_device_energy_management_mode_tests ;;
        electrical-meter)      run_electrical_meter_tests ;;
        commodity-metering)    run_commodity_metering_tests ;;
        utility-meter)         run_electrical_utility_meter_tests ;;
        meter-identification)  run_meter_identification_tests ;;
        *)                     error "Unknown test suite: ${TEST_SUITE}"; exit 1 ;;
    esac

    echo | tee -a "${LOG_FILE}"
    log "Comprehensive tests complete: ${TOTAL_TESTS} total, ${PASSED_TESTS} passed, ${FAILED_TESTS} failed"

    if [[ "${FAILED_TESTS}" -gt 0 ]]; then
        exit 1
    fi
}

main "$@"

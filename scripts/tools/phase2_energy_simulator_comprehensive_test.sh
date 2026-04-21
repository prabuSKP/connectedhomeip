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

print_usage() {
    cat <<EOF
Usage: ${SCRIPT_NAME} [options]

Run comprehensive tests for the Phase 2 Virtual Energy Simulator.

Options:
  --node-id <id>              Node ID to use (default: ${NODE_ID})
  --passcode <code>           Setup passcode (default: ${PASSCODE})
  --discriminator <disc>      Setup discriminator (default: ${DISCRIMINATOR})
  --pairing-mode <mode>       Pairing mode: onnetwork-long | onnetwork | code | skip
  --qr-payload <payload>      QR payload/manual code for pairing mode code
  --recommission              Unpair the node before pairing
  --test-suite <suite>        Run specific test suite (all, device-discovery, electrical-sensor, dem, 
                              electrical-meter, utility-meter, commodity-price, commodity-tariff, evse)
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
    echo "[ERROR] $*" >&2 | tee -a "${LOG_FILE}"
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

# Test suite functions
run_device_discovery_tests() {
    log "Running device discovery tests"
    
    run_test "Descriptor parts list (endpoint 0)" descriptor read parts-list "${NODE_ID}" 0
    run_test "Descriptor server list (endpoint 1)" descriptor read server-list "${NODE_ID}" 1
    run_test "Descriptor server list (endpoint 2)" descriptor read server-list "${NODE_ID}" 2
    run_test "Descriptor server list (endpoint 3)" descriptor read server-list "${NODE_ID}" 3
    run_test "Descriptor server list (endpoint 4)" descriptor read server-list "${NODE_ID}" 4
    
    # If we have the new endpoints, test them too
    run_test "Descriptor server list (endpoint 5)" descriptor read server-list "${NODE_ID}" 5 || true
    run_test "Descriptor server list (endpoint 6)" descriptor read server-list "${NODE_ID}" 6 || true
    run_test "Descriptor server list (endpoint 7)" descriptor read server-list "${NODE_ID}" 7 || true
}

run_electrical_sensor_tests() {
    log "Running electrical sensor tests (endpoint 1)"
    
    run_test "Sensor power mode" electricalpowermeasurement read power-mode "${NODE_ID}" 1
    run_test "Sensor active power" electricalpowermeasurement read active-power "${NODE_ID}" 1
    run_test "Sensor voltage" electricalpowermeasurement read voltage "${NODE_ID}" 1
    run_test "Sensor active current" electricalpowermeasurement read active-current "${NODE_ID}" 1
    run_test "Sensor cumulative energy imported" electricalenergymeasurement read cumulative-energy-imported "${NODE_ID}" 1
    run_test "Sensor power topology feature map" powertopology read feature-map "${NODE_ID}" 1
}

run_device_energy_management_tests() {
    log "Running device energy management tests (endpoint 2)"
    
    run_test "DEM ESA state" deviceenergymanagement read esastate "${NODE_ID}" 2
    run_test "DEM supported modes" deviceenergymanagementmode read supported-modes "${NODE_ID}" 2
    run_test "DEM current mode" deviceenergymanagementmode read current-mode "${NODE_ID}" 2
}

run_electrical_meter_tests() {
    log "Running electrical meter tests (endpoint 3)"
    
    run_test "Meter active power" electricalpowermeasurement read active-power "${NODE_ID}" 3
    run_test "Meter cumulative energy imported" electricalenergymeasurement read cumulative-energy-imported "${NODE_ID}" 3
    run_test "Meter commodity metered quantity" commoditymetering read metered-quantity "${NODE_ID}" 3
}

run_electrical_utility_meter_tests() {
    log "Running electrical utility meter tests (endpoint 4)"
    
    run_test "Meter serial number" meteridentification read meter-serial-number "${NODE_ID}" 4
    run_test "Meter type" meteridentification read meter-type "${NODE_ID}" 4 || true
    run_test "Meter point of delivery" meteridentification read point-of-delivery "${NODE_ID}" 4 || true
}

run_commodity_price_tests() {
    log "Running commodity price tests (endpoint 5)"
    
    run_test "Commodity price tariff unit" commodityprice read tariff-unit "${NODE_ID}" 5
    run_test "Commodity price currency" commodityprice read currency "${NODE_ID}" 5
    run_test "Commodity current price" commodityprice read current-price "${NODE_ID}" 5
    run_test "Commodity price forecast" commodityprice read price-forecast "${NODE_ID}" 5
}

run_commodity_tariff_tests() {
    log "Running commodity tariff tests (endpoint 6)"
    
    run_test "Commodity tariff info" commoditytariff read tariff-info "${NODE_ID}" 6
    run_test "Commodity tariff unit" commoditytariff read tariff-unit "${NODE_ID}" 6
    run_test "Commodity start date" commoditytariff read start-date "${NODE_ID}" 6
    run_test "Commodity current day" commoditytariff read current-day "${NODE_ID}" 6
    run_test "Commodity current day entry" commoditytariff read current-day-entry "${NODE_ID}" 6
}

run_evse_tests() {
    log "Running EVSE tests (endpoint 7)"
    
    run_test "EVSE state" energyevse read state "${NODE_ID}" 7
    run_test "EVSE supply state" energyevse read supply-state "${NODE_ID}" 7
    run_test "EVSE fault state" energyevse read fault-state "${NODE_ID}" 7
    run_test "EVSE charging time" energyevse read charging-time "${NODE_ID}" 7
    run_test "EVSE session energy charged" energyevse read session-energy-charged "${NODE_ID}" 7
}

# Main execution
main() {
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --node-id)
                NODE_ID="$2"
                shift 2
                ;;
            --passcode)
                PASSCODE="$2"
                shift 2
                ;;
            --discriminator)
                DISCRIMINATOR="$2"
                shift 2
                ;;
            --pairing-mode)
                PAIRING_MODE="$2"
                shift 2
                ;;
            --qr-payload)
                QR_PAYLOAD="$2"
                shift 2
                ;;
            --recommission)
                RECOMMISSION=1
                shift
                ;;
            --test-suite)
                TEST_SUITE="$2"
                shift 2
                ;;
            --verbose)
                VERBOSE=1
                shift
                ;;
            --log-file)
                LOG_FILE="$2"
                shift 2
                ;;
            --timeout)
                TIMEOUT="$2"
                shift 2
                ;;
            --storage-directory)
                STORAGE_DIR="$2"
                shift 2
                ;;
            -h|--help)
                print_usage
                exit 0
                ;;
            *)
                error "Unknown argument: $1"
                exit 1
                ;;
        esac
    done

    # Validate tools and parameters
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

    # Pair device if needed
    pair_device

    # Run selected test suites
    case "${TEST_SUITE}" in
        all)
            run_device_discovery_tests
            run_electrical_sensor_tests
            run_device_energy_management_tests
            run_electrical_meter_tests
            run_electrical_utility_meter_tests
            run_commodity_price_tests
            run_commodity_tariff_tests
            run_evse_tests
            ;;
        device-discovery)
            run_device_discovery_tests
            ;;
        electrical-sensor)
            run_electrical_sensor_tests
            ;;
        dem)
            run_device_energy_management_tests
            ;;
        electrical-meter)
            run_electrical_meter_tests
            ;;
        utility-meter)
            run_electrical_utility_meter_tests
            ;;
        commodity-price)
            run_commodity_price_tests
            ;;
        commodity-tariff)
            run_commodity_tariff_tests
            ;;
        evse)
            run_evse_tests
            ;;
        *)
            error "Unknown test suite: ${TEST_SUITE}"
            exit 1
            ;;
    esac

    # Report results
    echo | tee -a "${LOG_FILE}"
    log "Comprehensive tests complete: ${TOTAL_TESTS} total, ${PASSED_TESTS} passed, ${FAILED_TESTS} failed"

    if [[ "${FAILED_TESTS}" -gt 0 ]]; then
        exit 1
    fi
}

main "$@"
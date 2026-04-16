#!/usr/bin/env bash

set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

CHIP_TOOL="${REPO_ROOT}/out/linux-x64-chip-tool/chip-tool"
STORAGE_DIR="/tmp/chip-tool-phase2"

NODE_ID="0x12344321"
PASSCODE="20202021"
DISCRIMINATOR="3840"
QR_PAYLOAD=""

PAIRING_MODE="onnetwork-long"
RECOMMISSION=0
RUN_SUBSCRIBE=0
SUBSCRIBE_MIN=0
SUBSCRIBE_MAX=30
PAIR_TIMEOUT=20
CHECK_TIMEOUT=10
RESPONSE_LINES=30

TOTAL_CHECKS=0
FAILED_CHECKS=0
STEP_NUMBER=0

print_usage() {
    cat <<EOF
Usage: ${SCRIPT_NAME} [options]

Run commissioning (optional) and phase2 energy simulator smoke checks.

Options:
  --node-id <id>              Node ID to use (default: ${NODE_ID})
  --passcode <code>           Setup passcode (default: ${PASSCODE})
  --discriminator <disc>      Setup discriminator (default: ${DISCRIMINATOR})
  --pairing-mode <mode>       Pairing mode: onnetwork-long | onnetwork | code | skip
  --qr-payload <payload>      QR payload/manual code for pairing mode code
  --recommission              Unpair the node before pairing
  --subscribe                 Start a live subscription after smoke checks
  --subscribe-min <seconds>   Subscription min interval (default: ${SUBSCRIBE_MIN})
  --subscribe-max <seconds>   Subscription max interval (default: ${SUBSCRIBE_MAX})
  --pair-timeout <seconds>    chip-tool timeout for pairing commands (default: ${PAIR_TIMEOUT})
  --check-timeout <seconds>   chip-tool timeout for smoke checks (default: ${CHECK_TIMEOUT})
  --response-lines <count>    Max response lines to print per step (default: ${RESPONSE_LINES})
  --chip-tool <path>          Path to chip-tool binary
  --storage-directory <path>  chip-tool storage directory (default: ${STORAGE_DIR})
  -h, --help                  Show this help

Examples:
  ${SCRIPT_NAME}
  ${SCRIPT_NAME} --recommission
  ${SCRIPT_NAME} --pairing-mode code --qr-payload 'MT:EXAMPLEPAYLOAD'
  ${SCRIPT_NAME} --subscribe
EOF
}

log() {
    echo "[INFO] $*"
}

warn() {
    echo "[WARN] $*" >&2
}

fail() {
    echo "[ERROR] $*" >&2
    exit 1
}

print_separator() {
    printf '%s\n' '----------------------------------------------------------------'
}

print_run_header() {
    echo
    print_separator
    echo "Phase2 Energy Simulator Smoke Check"
    print_separator
    echo "Node ID           : ${NODE_ID}"
    echo "Pairing Mode      : ${PAIRING_MODE}"
    echo "Recommission      : ${RECOMMISSION}"
    echo "Storage Directory : ${STORAGE_DIR}"
    echo "Pair Timeout      : ${PAIR_TIMEOUT}s"
    echo "Check Timeout     : ${CHECK_TIMEOUT}s"
    echo "Response Lines    : ${RESPONSE_LINES}"
    print_separator
}

print_step_header() {
    local title="$1"

    STEP_NUMBER=$((STEP_NUMBER + 1))
    echo
    print_separator
    echo "Step ${STEP_NUMBER}: ${title}"
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
        echo "  (no output)"
        return
    fi

    focus_lines=$(extract_focus_lines "${response}")
    if [[ -n "${focus_lines}" ]]; then
        echo "  Key lines:"
        response_to_print="${focus_lines}"
    else
        echo "  Raw output:"
        response_to_print="${response}"
    fi

    total_lines=$(printf '%s\n' "${response_to_print}" | wc -l | tr -d ' ')
    if [[ "${total_lines}" -le "${RESPONSE_LINES}" ]]; then
        printf '%s\n' "${response_to_print}" | sed 's/^/  /'
        return
    fi

    printf '%s\n' "${response_to_print}" | head -n "${RESPONSE_LINES}" | sed 's/^/  /'
    echo "  ... (${total_lines} lines total, showing first ${RESPONSE_LINES})"
}

print_request_line() {
    local cmd_text

    cmd_text=$(printf '%q ' "$@")
    echo "  ${cmd_text}"
}

prepare_storage_dir() {
    if [[ -e "${STORAGE_DIR}" && ! -d "${STORAGE_DIR}" ]]; then
        fail "Storage path exists but is not a directory: ${STORAGE_DIR}"
    fi

    mkdir -p "${STORAGE_DIR}" || fail "Failed to create storage directory: ${STORAGE_DIR}"
    [[ -w "${STORAGE_DIR}" ]] || fail "Storage directory is not writable: ${STORAGE_DIR}"
}

run_chip_tool() {
    local step_title="$1"
    local timeout_value="$2"
    shift 2

    local -a cmd=("${CHIP_TOOL}" "$@" --storage-directory "${STORAGE_DIR}")
    local output
    local rc

    if [[ "${timeout_value}" != "0" ]]; then
        cmd+=(--timeout "${timeout_value}")
    fi

    print_step_header "${step_title}"
    echo "Request:"
    print_request_line "${cmd[@]}"

    set +e
    output=$("${cmd[@]}" 2>&1)
    rc=$?
    set -e

    echo "Response:"
    print_response_body "${output}"

    if [[ "${rc}" -eq 0 ]]; then
        echo "Result: PASS (exit code ${rc})"
    else
        echo "Result: FAIL (exit code ${rc})"
    fi

    return "${rc}"
}

run_check() {
    local description="$1"
    shift

    TOTAL_CHECKS=$((TOTAL_CHECKS + 1))
    if ! run_chip_tool "Check ${TOTAL_CHECKS}: ${description}" "${CHECK_TIMEOUT}" "$@"; then
        FAILED_CHECKS=$((FAILED_CHECKS + 1))
        warn "Failed check: ${description}"
    fi
}

pair_device() {
    if [[ "${PAIRING_MODE}" == "skip" ]]; then
        log "Skipping pairing as requested"
        return
    fi

    if [[ "${RECOMMISSION}" -eq 1 ]]; then
        run_chip_tool "Recommission: unpair node ${NODE_ID} (best effort)" "${PAIR_TIMEOUT}" pairing unpair "${NODE_ID}" || true
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
            [[ -n "${QR_PAYLOAD}" ]] || fail "--qr-payload is required when --pairing-mode code is used"
            pair_cmd+=(code "${NODE_ID}" "${QR_PAYLOAD}")
            ;;
        *)
            fail "Unsupported pairing mode: ${PAIRING_MODE}"
            ;;
    esac

    if ! run_chip_tool "Pair device using ${PAIRING_MODE}" "${PAIR_TIMEOUT}" "${pair_cmd[@]}"; then
        if [[ "${RECOMMISSION}" -eq 1 ]]; then
            fail "Pairing failed after recommission attempt"
        fi
        warn "Pairing failed. Continuing with read checks in case the device is already commissioned."
    fi
}

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
        --subscribe)
            RUN_SUBSCRIBE=1
            shift
            ;;
        --subscribe-min)
            SUBSCRIBE_MIN="$2"
            shift 2
            ;;
        --subscribe-max)
            SUBSCRIBE_MAX="$2"
            shift 2
            ;;
        --pair-timeout)
            PAIR_TIMEOUT="$2"
            shift 2
            ;;
        --check-timeout)
            CHECK_TIMEOUT="$2"
            shift 2
            ;;
        --response-lines)
            RESPONSE_LINES="$2"
            shift 2
            ;;
        --chip-tool)
            CHIP_TOOL="$2"
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
            fail "Unknown argument: $1"
            ;;
    esac
done

[[ -x "${CHIP_TOOL}" ]] || fail "chip-tool not found or not executable: ${CHIP_TOOL}"
[[ "${PAIR_TIMEOUT}" =~ ^[0-9]+$ ]] || fail "--pair-timeout must be a non-negative integer"
[[ "${CHECK_TIMEOUT}" =~ ^[0-9]+$ ]] || fail "--check-timeout must be a non-negative integer"
[[ "${RESPONSE_LINES}" =~ ^[1-9][0-9]*$ ]] || fail "--response-lines must be a positive integer"
prepare_storage_dir
print_run_header

pair_device

echo
log "Running endpoint discovery checks"
run_check "Descriptor parts list (endpoint 0)" descriptor read parts-list "${NODE_ID}" 0
run_check "Descriptor server list (endpoint 1)" descriptor read server-list "${NODE_ID}" 1
run_check "Descriptor server list (endpoint 2)" descriptor read server-list "${NODE_ID}" 2
run_check "Descriptor server list (endpoint 3)" descriptor read server-list "${NODE_ID}" 3
run_check "Descriptor server list (endpoint 4)" descriptor read server-list "${NODE_ID}" 4

echo
log "Running phase2 energy attribute checks"
run_check "Sensor active power (endpoint 1)" electricalpowermeasurement read active-power "${NODE_ID}" 1
run_check "Sensor cumulative energy imported (endpoint 1)" electricalenergymeasurement read cumulative-energy-imported "${NODE_ID}" 1
run_check "Power topology feature map (endpoint 1)" powertopology read feature-map "${NODE_ID}" 1
run_check "DEM ESA state (endpoint 2)" deviceenergymanagement read esastate "${NODE_ID}" 2
run_check "DEM supported modes (endpoint 2)" deviceenergymanagementmode read supported-modes "${NODE_ID}" 2
run_check "Meter active power (endpoint 3)" electricalpowermeasurement read active-power "${NODE_ID}" 3
run_check "Meter cumulative energy imported (endpoint 3)" electricalenergymeasurement read cumulative-energy-imported "${NODE_ID}" 3
run_check "Commodity metered quantity (endpoint 3)" commoditymetering read metered-quantity "${NODE_ID}" 3
run_check "Meter serial number (endpoint 4)" meteridentification read meter-serial-number "${NODE_ID}" 4

echo
log "Smoke checks complete: ${TOTAL_CHECKS} total, ${FAILED_CHECKS} failed"

if [[ "${RUN_SUBSCRIBE}" -eq 1 ]]; then
    echo
    log "Starting live subscription on endpoint 1 active-power. Press Ctrl+C to stop."
    run_chip_tool "Subscribe endpoint 1 active-power" 0 electricalpowermeasurement subscribe active-power "${SUBSCRIBE_MIN}" "${SUBSCRIBE_MAX}" "${NODE_ID}" 1 --keepSubscriptions true
fi

if [[ "${FAILED_CHECKS}" -gt 0 ]]; then
    exit 1
fi

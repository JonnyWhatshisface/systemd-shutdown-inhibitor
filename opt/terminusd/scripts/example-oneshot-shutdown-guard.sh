#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2021 Jonathan D. Hall <jon@jonathandavidhall.com>
#
# Oneshot shutdown_guard example.
#
# Sends 4 one-packet probes to MONITOR_IP and exits 0 when at least 3 succeed
# (75% or better success rate). Exits non-zero otherwise.
#
# This is intended for shutdown_guard_type = oneshot. The daemon handles the
# thresholding and mask/unmask behavior based on the exit status.

set -u

MONITOR_IP="${MONITOR_IP:-192.168.1.225}"
PING_COUNT="${PING_COUNT:-4}"
PING_TIMEOUT="${PING_TIMEOUT:-5}"
MIN_SUCCESS_COUNT="${MIN_SUCCESS_COUNT:-3}"

log_info() {
    echo "INFO: $*" >&2
}

log_error() {
    echo "ERROR: $*" >&2
}

validate_config() {
    if [[ -z "$MONITOR_IP" ]]; then
        log_error "MONITOR_IP not set"
        exit 2
    fi

    if ! [[ "$PING_COUNT" =~ ^[0-9]+$ ]] || [[ "$PING_COUNT" -lt 1 ]]; then
        log_error "PING_COUNT must be a positive integer (got: $PING_COUNT)"
        exit 2
    fi

    if ! [[ "$PING_TIMEOUT" =~ ^[0-9]+$ ]] || [[ "$PING_TIMEOUT" -lt 1 ]]; then
        log_error "PING_TIMEOUT must be a positive integer (got: $PING_TIMEOUT)"
        exit 2
    fi

    if ! [[ "$MIN_SUCCESS_COUNT" =~ ^[0-9]+$ ]] || [[ "$MIN_SUCCESS_COUNT" -lt 0 || "$MIN_SUCCESS_COUNT" -gt "$PING_COUNT" ]]; then
        log_error "MIN_SUCCESS_COUNT must be between 0 and PING_COUNT (got: $MIN_SUCCESS_COUNT)"
        exit 2
    fi
}

ping_once() {
    local ping_output
    local received_count

    ping_output=$(ping -c 1 -W "$PING_TIMEOUT" "$MONITOR_IP" 2>&1)
    received_count=$(echo "$ping_output" | grep -oP '(?<=transmitted, )\d+(?= received)')

    if [[ -z "$received_count" ]]; then
        received_count=$(echo "$ping_output" | grep -oP '(?<=sent, )[0-9]+(?= ok)')
    fi

    [[ -n "$received_count" && "$received_count" -gt 0 ]]
}

main() {
    local successes=0
    local attempt

    validate_config

    for ((attempt = 1; attempt <= PING_COUNT; attempt++)); do
        if ping_once; then
            successes=$((successes + 1))
        fi
    done

    log_info "Reachability summary for $MONITOR_IP: ${successes}/${PING_COUNT} successful probes"

    if [[ "$successes" -ge "$MIN_SUCCESS_COUNT" ]]; then
        exit 0
    fi

    exit 1
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi

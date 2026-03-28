#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2021 Jonathan D. Hall <jon@jonathandavidhall.com>
#
# Persistent shutdown_guard example.
#
# Monitors reachability to MONITOR_IP. After FAILURE_THRESHOLD consecutive
# failed probes it emits the persist-mode control line:
#   shutdown_guard_disable_shutdown 1
# When connectivity recovers it emits:
#   shutdown_guard_disable_shutdown 0
#
# The daemon interprets those control lines and performs the actual masking
# or unmasking of shutdown-related systemd targets.

set -u

MONITOR_IP="${MONITOR_IP:-192.168.1.225}"
PING_INTERVAL="${PING_INTERVAL:-30}"
FAILURE_THRESHOLD="${FAILURE_THRESHOLD:-3}"
PING_TIMEOUT="${PING_TIMEOUT:-5}"

log_info() {
    echo "INFO: $*" >&2
}

log_warn() {
    echo "WARNING: $*" >&2
}

log_error() {
    echo "ERROR: $*" >&2
}

validate_config() {
    if [[ -z "$MONITOR_IP" ]]; then
        log_error "MONITOR_IP not set"
        exit 1
    fi

    if ! [[ "$FAILURE_THRESHOLD" =~ ^[0-9]+$ ]] || [[ "$FAILURE_THRESHOLD" -lt 1 ]]; then
        log_error "FAILURE_THRESHOLD must be a positive integer (got: $FAILURE_THRESHOLD)"
        exit 1
    fi

    if ! [[ "$PING_INTERVAL" =~ ^[0-9]+$ ]] || [[ "$PING_INTERVAL" -lt 1 ]]; then
        log_error "PING_INTERVAL must be a positive integer (got: $PING_INTERVAL)"
        exit 1
    fi

    if ! [[ "$PING_TIMEOUT" =~ ^[0-9]+$ ]] || [[ "$PING_TIMEOUT" -lt 1 ]]; then
        log_error "PING_TIMEOUT must be a positive integer (got: $PING_TIMEOUT)"
        exit 1
    fi

    log_info "Configuration: IP=$MONITOR_IP interval=${PING_INTERVAL}s threshold=$FAILURE_THRESHOLD timeout=${PING_TIMEOUT}s"
}

ping_ip() {
    local ping_output
    local received_count

    ping_output=$(ping -c 1 -W "$PING_TIMEOUT" "$MONITOR_IP" 2>&1)
    received_count=$(echo "$ping_output" | grep -oP '(?<=transmitted, )\d+(?= received)')

    if [[ -z "$received_count" ]]; then
        received_count=$(echo "$ping_output" | grep -oP '(?<=sent, )[0-9]+(?= ok)')
    fi

    [[ -n "$received_count" && "$received_count" -gt 0 ]]
}

emit_disable_shutdown() {
    local desired_state="$1"
    printf 'shutdown_guard_disable_shutdown %s\n' "$desired_state"
}

main() {
    local consecutive_failures=0
    local shutdown_disabled=0

    validate_config

    log_info "Persistent shutdown guard started for $MONITOR_IP"

    while true; do
        if ping_ip; then
            if [[ "$consecutive_failures" -gt 0 ]]; then
                log_info "IP $MONITOR_IP is responding again after $consecutive_failures consecutive failures"
            fi

            if [[ "$shutdown_disabled" -eq 1 ]]; then
                log_info "Connectivity restored; requesting shutdown enable"
                emit_disable_shutdown 0
                shutdown_disabled=0
            fi

            consecutive_failures=0
        else
            consecutive_failures=$((consecutive_failures + 1))

            if [[ "$consecutive_failures" -lt "$FAILURE_THRESHOLD" ]]; then
                log_warn "IP $MONITOR_IP is not responding (failure $consecutive_failures/$FAILURE_THRESHOLD)"
            elif [[ "$consecutive_failures" -eq "$FAILURE_THRESHOLD" ]]; then
                log_warn "Failure threshold reached for $MONITOR_IP; requesting shutdown disable"
                emit_disable_shutdown 1
                shutdown_disabled=1
            else
                log_warn "IP $MONITOR_IP still unreachable (consecutive failures: $consecutive_failures)"
            fi
        fi

        sleep "$PING_INTERVAL"
    done
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi

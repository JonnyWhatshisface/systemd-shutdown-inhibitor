#!/usr/bin/env bash

# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2021 Jonathan D. Hall <jon@jonathandavidhall.com>
#
# This file is part of terminusd.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Terminate logged-in user sessions during inhibited shutdown while updates run.
#
# This script intentionally targets logind sessions via:
#   loginctl terminate-session <session-id>
#
# That scope is limited to user login sessions (TTY/SSH/desktop) and avoids
# broad process matching that could disrupt system services such as terminusd
# itself or non-session daemon child processes.

set -euo pipefail

NOLOGIN_COMPAT_FILE="${NOLOGIN_COMPAT_FILE:-/run/nolgin}"
NOLOGIN_FILE="${NOLOGIN_FILE:-/run/nologin}"
TERMINATE_TIMEOUT_SEC="${TERMINATE_TIMEOUT_SEC:-15}"

log_info() {
    echo "INFO: $*"
}

log_warn() {
    echo "WARNING: $*" >&2
}

log_error() {
    echo "ERROR: $*" >&2
}

require_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        log_error "This script must be run as root."
        exit 1
    fi
}

require_loginctl() {
    if ! command -v loginctl >/dev/null 2>&1; then
        log_error "loginctl not found; cannot safely enumerate/terminate sessions."
        exit 1
    fi
}

block_new_logins() {
    printf '%s\n' 'System is preparing for reboot. Logins are temporarily disabled.' > "${NOLOGIN_COMPAT_FILE}"
    chmod 0644 "${NOLOGIN_COMPAT_FILE}"
    log_info "Created ${NOLOGIN_COMPAT_FILE}"

    # /run/nologin is the standard PAM gate checked by login programs.
    printf '%s\n' 'System is preparing for reboot. Logins are temporarily disabled.' > "${NOLOGIN_FILE}"
    chmod 0644 "${NOLOGIN_FILE}"
    log_info "Created ${NOLOGIN_FILE}"
}

session_exists() {
    local sid="$1"
    loginctl show-session "${sid}" >/dev/null 2>&1
}

wait_for_session_termination() {
    local sid="$1"
    local deadline=$((SECONDS + TERMINATE_TIMEOUT_SEC))

    while (( SECONDS < deadline )); do
        if ! session_exists "${sid}"; then
            return 0
        fi
        sleep 1
    done

    return 1
}

terminate_login_sessions() {
    local sid class state remote user uid
    local total=0 terminated=0 skipped=0 failed=0

    while read -r sid _rest; do
        [[ -z "${sid}" ]] && continue
        total=$((total + 1))

        class="$(loginctl show-session "${sid}" -p Class --value 2>/dev/null || true)"
        state="$(loginctl show-session "${sid}" -p State --value 2>/dev/null || true)"
        remote="$(loginctl show-session "${sid}" -p Remote --value 2>/dev/null || true)"
        user="$(loginctl show-session "${sid}" -p Name --value 2>/dev/null || true)"
        uid="$(loginctl show-session "${sid}" -p User --value 2>/dev/null || true)"

        # Target real user login sessions only.
        if [[ "${class}" != "user" ]]; then
            skipped=$((skipped + 1))
            continue
        fi

        # Ignore sessions already closing or inactive.
        if [[ "${state}" == "closing" ]]; then
            skipped=$((skipped + 1))
            continue
        fi

        log_info "Terminating session ${sid} user=${user:-unknown} uid=${uid:-unknown} state=${state:-unknown} remote=${remote:-unknown}"

        if ! loginctl terminate-session "${sid}" >/dev/null 2>&1; then
            log_error "Failed to request termination for session ${sid}"
            failed=$((failed + 1))
            continue
        fi

        if wait_for_session_termination "${sid}"; then
            log_info "Session ${sid} terminated"
            terminated=$((terminated + 1))
        else
            log_error "Timed out waiting for session ${sid} to terminate"
            failed=$((failed + 1))
        fi
    done < <(loginctl list-sessions --no-legend 2>/dev/null)

    log_info "Session termination summary: total_seen=${total} terminated=${terminated} skipped=${skipped} failed=${failed}"

    if (( failed > 0 )); then
        return 1
    fi

    return 0
}

main() {
    require_root
    require_loginctl

    if ! [[ "${TERMINATE_TIMEOUT_SEC}" =~ ^[0-9]+$ ]] || (( TERMINATE_TIMEOUT_SEC < 1 )); then
        log_error "TERMINATE_TIMEOUT_SEC must be a positive integer (got: ${TERMINATE_TIMEOUT_SEC})"
        exit 1
    fi

    block_new_logins
    terminate_login_sessions
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi

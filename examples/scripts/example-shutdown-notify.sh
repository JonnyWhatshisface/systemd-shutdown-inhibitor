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
#
# Pre-shutdown notification script.
#
# This script sends notifications to all logged in users that the system
# is preparing to shut down. Notifications will reach all terminals and
# X sessions that are active.
#
# The script also adds the /run/nologin flag to prevent new sessions from
# logging in.
#
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "This script must be run as root." >&2
    exit 1
fi

MESSAGE_TITLE="System shutdown preparation"
MESSAGE_BODY="The system is preparing for shutdown. New logins are now blocked. Please save your work and log out."
NOLOGIN_FILE="/run/nologin"
NOTIFY_SEND_BIN=""
terminal_hits=0
desktop_hits=0

send_tty_notice() {
    local tty_path="$1"

    [[ -z "${tty_path}" ]] && return 1
    [[ ! -w "${tty_path}" ]] && return 1

    {
        printf '\n'
        printf '[terminusd] %s\n' "${MESSAGE_BODY}"
        printf '\n'
    } > "${tty_path}" 2>/dev/null || return 1

    terminal_hits=$((terminal_hits + 1))
    return 0
}

if command -v notify-send >/dev/null 2>&1; then
    NOTIFY_SEND_BIN="$(command -v notify-send)"
fi

printf '%s\n' "System is preparing for shutdown. New logins are now blocked." > "${NOLOGIN_FILE}"
chmod 0644 "${NOLOGIN_FILE}"
echo "Created ${NOLOGIN_FILE}"

# Notify terminal users with a wall message
if command -v wall >/dev/null 2>&1; then
    if wall "[terminusd] ${MESSAGE_BODY}"; then
        echo "Sent terminal broadcast with wall."
    else
        echo "wall returned a non-zero status; some users may not have received terminal broadcast." >&2
    fi
else
    echo "wall command not found; skipping terminal broadcast." >&2
fi

# Some terminal emulators (XTerm) or users may suppress wall messages.
# Write directly to each tty to make sure the message was received.
while read -r who_user who_tty _; do
    [[ -z "${who_tty}" ]] && continue
    tty_path="/dev/${who_tty}"
    send_tty_notice "${tty_path}" || true
done < <(who)

# Fallback: discover active process TTYs so terminals that don't register in
# utmp (i.e. XTerm tabs) still receive a message.
while read -r ps_user ps_tty; do
    [[ -z "${ps_tty}" || "${ps_tty}" == "?" ]] && continue
    tty_path="/dev/${ps_tty}"
    send_tty_notice "${tty_path}" || true
done < <(ps -eo user=,tty= 2>/dev/null | awk '{print $1, $2}')

echo "Sent direct terminal notice to ${terminal_hits} tty endpoint(s)."

# Send notifications to active graphical sessions.
if command -v loginctl >/dev/null 2>&1 && [[ -n "${NOTIFY_SEND_BIN}" ]]; then
    while read -r sid; do
        [[ -z "${sid}" ]] && continue

        user="$(loginctl show-session "${sid}" -p Name --value 2>/dev/null || echo "")"
        uid="$(loginctl show-session "${sid}" -p User --value 2>/dev/null || echo "")"
        session_type="$(loginctl show-session "${sid}" -p Type --value 2>/dev/null || echo "")"
        state="$(loginctl show-session "${sid}" -p State --value 2>/dev/null || echo "")"
        remote="$(loginctl show-session "${sid}" -p Remote --value 2>/dev/null || echo "")"
        leader="$(loginctl show-session "${sid}" -p Leader --value 2>/dev/null || echo "")"

        [[ -z "${user}" || -z "${uid}" ]] && continue
        [[ "${remote}" == "yes" ]] && continue

        runtime_dir="/run/user/${uid}"
        bus="unix:path=${runtime_dir}/bus"
        display=""
        wayland_display=""
        xauthority=""

        # Use the session leader process environment if available. Target
        # the same display/socket values as the logged-in desktop session.
        if [[ -n "${leader}" && -r "/proc/${leader}/environ" ]]; then
            while IFS= read -r -d '' kv; do
                case "${kv}" in
                    DISPLAY=*) display="${kv#DISPLAY=}" ;;
                    WAYLAND_DISPLAY=*) wayland_display="${kv#WAYLAND_DISPLAY=}" ;;
                    XAUTHORITY=*) xauthority="${kv#XAUTHORITY=}" ;;
                    DBUS_SESSION_BUS_ADDRESS=*) bus="${kv#DBUS_SESSION_BUS_ADDRESS=}" ;;
                esac
            done < "/proc/${leader}/environ"
        fi

        # Skip the non-graphical sessions when we have neither display hints
        # nor a user dbus socket for desktop notifications.
        if [[ -z "${display}" && -z "${wayland_display}" && ! -S "${runtime_dir}/bus" ]]; then
            continue
        fi

        if [[ ! -S "${runtime_dir}/bus" && "${bus}" == "unix:path=${runtime_dir}/bus" ]]; then
            echo "Skipping session ${sid} (${user}): user bus socket not found at ${runtime_dir}/bus." >&2
            continue
        fi

        if runuser -u "${user}" -- \
            env XDG_RUNTIME_DIR="${runtime_dir}" \
                DBUS_SESSION_BUS_ADDRESS="${bus}" \
                DISPLAY="${display}" \
                WAYLAND_DISPLAY="${wayland_display}" \
                XAUTHORITY="${xauthority}" \
                "${NOTIFY_SEND_BIN}" -u critical -a terminusd \
                "${MESSAGE_TITLE}" "${MESSAGE_BODY}"; then
            echo "Sent desktop notification for session ${sid} user ${user}."
            desktop_hits=$((desktop_hits + 1))
        else
            echo "Failed desktop notification for session ${sid} user ${user}." >&2
        fi
    done < <(
        loginctl list-sessions --no-legend 2>/dev/null | awk '{print $1}'
    )

    if [[ "${desktop_hits}" -eq 0 ]]; then
        echo "No desktop notifications were delivered. Check loginctl session fields Type/State for this host." >&2
    fi
else
    echo "loginctl or notify-send not available; skipping desktop notifications." >&2
fi

echo "Pre-shutdown user notification step finished."

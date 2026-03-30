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
# Update on shutdown script.
#
# This script detects whether the system uses dnf or apt and installs
# available package updates. It then removes the /system-update symlink
# if it still exists. This makes the script and daemon a direct drop-in
# for cases where the system-update-service method is in use.
#
# It supports allowing package downgrades as well as upgrades by setting
# the appropriate apt/dnf mode below.
#
# Variables use parameter expansion with defaults, so they can be overridden by
# environment variables when launching the script.
#
set -euo pipefail

# DNF update strategy.
# - upgrade: only upgrade packages, no downgrades allowed(default)
# - distro-sync: synchronize to repo state (can downgrade packages)
DNF_MODE="${DNF_MODE:-upgrade}"

# APT update strategy.
# - upgrade: only upgrade packages, no downgrades allowed (default)
# - dist-upgrade: allows downgrades to match repo state. Similar to dnf's distro-sync.
APT_MODE="${APT_MODE:-upgrade}"

if [[ "${EUID}" -ne 0 ]]; then
    echo "This script must be run as root." >&2
    exit 1
fi

# Install updates with the appropriate package manager.
# This example script supports both dnf and apt.
if command -v dnf >/dev/null 2>&1; then
    case "${DNF_MODE}" in
        upgrade)
            echo "Starting dnf upgrade..."
            dnf -y upgrade --refresh
            echo "dnf upgrade completed."
            ;;
        distro-sync|distrosync)
            echo "Starting dnf distro-sync..."
            dnf -y distro-sync --refresh
            echo "dnf distro-sync completed."
            ;;
        *)
            echo "Invalid DNF_MODE '${DNF_MODE}'. Expected 'upgrade' or 'distro-sync'." >&2
            exit 1
            ;;
    esac
elif command -v apt-get >/dev/null 2>&1; then
    apt-get update
    case "${APT_MODE}" in
        upgrade)
            echo "Starting apt package upgrade..."
            DEBIAN_FRONTEND=noninteractive apt-get -y upgrade
            echo "apt package upgrade completed."
            ;;
        dist-upgrade|distupgrade)
            echo "Starting apt package dist-upgrade (downgrades allowed)..."
            DEBIAN_FRONTEND=noninteractive apt-get -y -o APT::Get::Allow-Downgrades=true dist-upgrade
            echo "apt package dist-upgrade completed."
            ;;
        *)
            echo "Invalid APT_MODE '${APT_MODE}'. Expected 'upgrade' or 'dist-upgrade'." >&2
            exit 1
            ;;
    esac
else
    echo "No supported package manager found. Expected dnf or apt-get." >&2
    exit 1
fi

# If system-update is still armed, remove it as the final step.
if [[ -L /system-update ]]; then
    echo "Removing /system-update symlink."
    rm -f /system-update
fi

if [[ -L /etc/system-update ]]; then
    echo "Removing /etc/system-update symlink."
    rm -f /etc/system-update
fi

echo "Update script finished."

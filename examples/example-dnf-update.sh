#!/usr/bin/env bash
set -euo pipefail

# Example shutdown update script for use with systemd-shutdown-inhibitor.
#
# This script executes a dnf upgrade to install updates on the system, and
# then it removes the /system-update symlink if it still exists. This makes
# the script and daemon a direct drop-in for cases where the system-update-service
# method is in use.
#
# If the updates faill for whatever reason with this method, the
# system will simply reboot into the system-update state.

if [[ "${EUID}" -ne 0 ]]; then
    echo "This script must be run as root." >&2
    exit 1
fi

echo "Starting dnf upgrade..."
dnf -y upgrade --refresh
echo "dnf upgrade completed."

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

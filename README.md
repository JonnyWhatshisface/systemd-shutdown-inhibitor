


[![CI](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/ci.yml/badge.svg)](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/ci.yml)

[![Unit Test - Config Parser](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-config.yml/badge.svg)](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-config.yml)

[![Unit Test - DBus Test Mode](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-dbus-test-mode.yml/badge.svg)](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-dbus-test-mode.yml)

[![Unit Test - terminusctl CLI](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-terminusctl.yml/badge.svg)](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-terminusctl.yml)

# terminusd
terminusd gives you total control of shutdowns/reboots on your systems.

* Automate package and system updates at reboot/shutdown, without needing systemd offline-updates/dual reboots.

* Disable and enable shutdown, halt, reboot and kexec deterministically using live monitoring scripts to prevent unwanted shutdowns and reboots of systems.

* Automate actions on shutdown/reboot that may require systemctl commands, which cannot be accomplished using unit files while on the way down

## Why terminusd for shutdown/reboot actions?

terminusd was developed originally to address issues with performing various actions at shutdown/reboot that require `systemctl` commands, which cannot be done via `systemd units`. 

One such action is installing package updates via `apt` and `dnf`.

Many environments prior to systemd were installing package updates automatically at reboot/shutdown with rc scripts. When they switched to systemd, they found that upgrades and patches which appeared to have been working actually weren't, because the `service control` commands in the packages would fail once systemd knows it's in a `shutdown`, `poweroff`, `halt`, `reboot` or `kexec` state.

To resolve this, systemd introduced [`offline-updates`](https://www.freedesktop.org/software/systemd/man/latest/systemd.offline-updates.html), which puts the system into a special state that you can safely perform package installations with `systemctl` commands available. It's triggered by creating `/system-update` or `/etc/system-update`, and systemd will boot into this state on its next boot. Once your actions have finished (package installation, whatever else you're doing) - your scripting removes `/system-update` and/or `/etc/system-update` and the system reboots again normally.

There are two problems with this approach:

* It introduces dual reboots to automate package upgrades, making it not commonly used and thus widely misunderstood

* It only solves the problem of installing / upgrading / removing packages

This does not actually give the users the ability to do more complex operations on the way down that may  require `systemctl` related commands before the host goes down.


## How it works

### Shutdown Actions

`terminusd` registers itself as a systemd delay `inhibitor`, attaches itself to `DBus` and listens for the `PrepareForShutdown` signal that is sent whenever a command is issued that will result in the system either going down or rebooting. This keeps systemd in a state pre-shutdown and `systemctl` commands will still function as normal.

It then runs all user-configured scripts in priority-ascending order in what is called `priority groups`. Multiple scripts and commands may share a single `priority group` and those that do will be executed in *parallel*. Individual commands/actions can be marked as *critical* and any single action marked *critical* failing will result in not executing any further `priority groups` and the `inhibitor` being unlocked, allowing the shutdown operation to proceed.

### Shutdown Guard

The `Shutdown Guard` can leverage you own commands or scripts to deterministically enable or disable all shutdown/reboot methods on the system until the criteria is met to re-enable them.

It can operate from either executing a command periodically at a defined interval and checking the return code, or by running a realtime persistent script that can dynamically tell the `temrinusd` to enable or disable the shutdown/reboot ability of the system. 

## Setup and installation

Simply executing `make` and `make install` - or installing the packages available on the GitHub - is all that's needed. By default, nothing is configured to start happening with terminus until you configure it.

For more information on configuring and using terminusd, see [the documentation](https://jonnywhatshisface.github.io/systemd-shutdown-inhibitor/). Alternatively, read the configuration file in `/etc/terminusd.conf`.
  



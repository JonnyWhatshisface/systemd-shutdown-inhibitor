
# terminusd

[![CI](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/ci.yml/badge.svg)](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/ci.yml)

[![Unit Test - Config Parser](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-config.yml/badge.svg)](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-config.yml)

[![Unit Test - DBus Test Mode](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-dbus-test-mode.yml/badge.svg)](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-dbus-test-mode.yml)

[![Unit Test - terminusctl CLI](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-terminusctl.yml/badge.svg)](https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor/actions/workflows/unit-test-terminusctl.yml)

Deterministic shutdown orchestration and admission control for Linux.

terminusd gives you total control of shutdowns/reboots on your systems.

First, it orchestrates pre-shutdown actions in deterministic priority order with parallel execution inside each priority group - and critical gate keeping configuration - while systemd is still fully operational. Package updates, service operations and maintenance workflows can complete cleanly in the order you need them to.

Second, its  shutdown guard can deterministically enable or disable shutdown/reboot on a system entirely, based on your own defined scripted outcomes or directives.

## Why terminusd

Systemd offers a system-update path, but many environments need two deterministic controls that are hard to express with default shutdown behavior of systemd alone:

1. Deterministic shutdown action orchestration: run exactly the right actions, in the right order, before poweroff/reboot. This is critical for things such as package installations/updates that may require services be shutdown as part of the installation, which often require using the dual-reboot method of system-update in systemd.

2. Deterministic shutdown admission control: explicitly allow or deny shutdown/reboot based on real runtime conditions for your environment. For instance, avoiding shutting down the only member of an HA cluster while running in BCP mode.

**terminusd addresses these gaps.**

* Holds a delay inhibitor while shutdown begins to execute configured actions in strict priority order and critical dependencies that abort further actions on failure. The delay timeout is entirely configurable, and during this time, systemd is not in a shutdown state, allowing for full systemctl command execution and safe package installation and upgrades, with the ability to selectively execute actions before, after and in-between to suit your environment.

* Built in `shutdown_guard` can entirely disable shutdown/reboot commands based on script outcomes or directives.
  
## Core capabilities


- Ordered shutdown execution with priority groups.

- Parallel execution within the same priority.

- Critical step support to abort remaining work if a key action fails.

- Script-driven shutdown guard that can deterministically enable/disable shutdown and reboot.

- Runtime control interface (`terminusctl`) to override, reconfigure and control state from the cli

- Config drop-ins (`/etc/terminus.d`) for composable configuration management.

  

## How it works

### Shutdown Actions:  

1.  `terminusd` starts and acquires a systemd delay inhibitor lock.

2. It subscribes to `PrepareForShutdown` on D-Bus.

3. On shutdown/reboot, it runs configured sections by ascending `priority`.

4. If a critical section fails, remaining groups are skipped.

5. The inhibitor is released and system shutdown continues.

### Shutdown Guard

1. persist - Runs user-defined script/program in a persist mode that it actively monitors, allowing the script/program to dynamically enable and disable system shutdown/reboot on the machine based on your criteria. (Yes, it even disables shutdown/reboot from root!)

2. oneshot - Run your script/program at defined intervals with a defined threshold. Non-zero exit code indicates a failure, and at X failures, system shutdown/reboots will be disabled system-wide. A zero exit code will re-enable them.

### terminusctl

CLI control for terminus to override its state, [re]enabled or disable shutdowns, reload configuration, update the max inhibit time and print out the ordered exeuction of your configured scripts to visualize what will happen on shutdown.
  

## Installation

  

### Build from source

  

```bash
make
sudo  make  install
```
This installs:

-  `terminusd` and `terminusctl` to `/usr/sbin`
- service unit to `/usr/lib/systemd/system/terminusd.service`
- man pages to `/usr/share/man/man8`
- default config to `/etc/terminusd.conf`
- drop-in examples to `/etc/terminus.d`
- script examples to `/opt/terminusd/scripts`

### Enable and start

```bash
sudo  systemctl  daemon-reload
sudo  systemctl  enable  --now  terminusd.service
```
OR
```bash
temrinusctl start
```

## Configuration

  

Primary config: `/etc/terminusd.conf`

Optional drop-ins: `/etc/terminus.d/*.conf` (loaded in lexical order after the main file).

### Minimal example

```ini

[main]
# Set the InhibitDelayMaxSec value in login.d
# to max_inhibit_delay (if not already set)
set_max_inhibit_delay = true
# Max amount of time the inhibitor can hold
# systemd in a pre-shutdown state
max_inhibit_delay = 2700
# Scripts to execute on shutdown/reboot.
# Scripts with maching priority execute
# in parallel. Full arguments can be put
# in the command. Critical scripts failing
# result in cancelling running of any further
# priority groups and let the reboot happen
# immediately after completion of the group.
[notifyusers]
command = /opt/terminusd/scripts/example-shutdown-notify.sh
priority = 100
[stop_certain_services]
command = /usr/local/bin/svc-stop-on-shutdown.sh --force
priority = 100
critical = true
[applyupdates]
command = /opt/terminusd/scripts/example-package-updates.sh
priority = 1000
critical = true
[firmware_updates]
command = /usr/local/bin/update_firmwares.sh --all
priority = 2000
```

### Important main options

-  `set_max_inhibit_delay`: write daemon's configured delay into `/etc/systemd/logind.conf` if the value is not already matching `max_inhibit_delay`

-  `max_inhibit_delay`: desired inhibit timeout window in seconds. Used for set and for checking the value.

-  `restart_logind_after_set`: immediately apply delay by restarting logind (use cautiously!)

### Script section options
  
-  `command` 
Full command-line with path and arguments.

-  `priority` (default `1000`)
Multiple actions with the same priority become a "priority group" and execute in parallel.

-  `user`, `group` (optional)
The user and group to run the script as. By default, they run as root.

- `env` (optional - inherits daemons env by default)
Full path to a file containing KEY=VALUE lines to use as the environment for the execution.

-  `critical` (default `false`)
Tasks marked as critical must exit non-zero in order for the next priority group to be executed. Any single task marked critical failing will fail the entire priority group, and no further tasks will be executed, letting go of the inhibitor and rebooting/shutting down the machine immediately.

-  `enabled` (default `true`)
Any drop-ins or entries with `enabled = false` will not be loaded or executed.
  
## Runtime control (terminusctl)

 
terminusctl gives flexible control and override capabilities to terminusd, allowing you to visualize the actions, reconfigure it without reload, override the shutdown/reboot admission state and trigger reboots directly from it, optionally skipping the configured shutdown actions.

It is usable only by root.

```bash
$ ./terminusctl --help
Usage: ./terminusctl [options] <command> [args]

Commands:
  start                 Start the terminusd daemon
  stop                  Stop the terminusd daemon
  status                Show current daemon runtime status
  reload-config         Reload scripts/config while daemon is running
  shutdown-guard        enable|disable shutdown guard
  shutdown-commands     enable|disable shutdown commands
  system-reboot         Request reboot through daemon control path
  system-shutdown       Request shutdown through daemon control path

Options:
  -f, --force           Skip confirmation prompt for reboot/shutdown
  --set-logind-inhibitor-delay
                        Write daemon's configured max inhibit delay to logind.conf
  --skip-scripts        With system-reboot/system-shutdown, skip script execution
  -h, --help            Show this help

```

  

## Shutdown guard modes

`shutdown_guard` supports two modes:

-  `oneshot`: run command every interval; mask shutdown targets after threshold consecutive failures.

-  `persist`: keep a long-running process active and let it emit directives:

-  `shutdown_guard_disable_shutdown 1`

-  `shutdown_guard_disable_shutdown 0`

This gives the ultimate flexibility for making decisions on when and why to disable reboots on a system, giving total control to do so based on your environments needs that go beyond just user permission. **Avoid embarrassing outages from accidental reboots during reduced capacity in a BCP event.**

Example guard scripts are installed under `/opt/terminusd/scripts`.

## Safety model

 
- Delay inhibitors are bounded by `InhibitDelayMaxSec`, so shutdown cannot be held indefinitely. When this value is exceeded, the system proceeds with shutting down regardless of status of actions. 

- Inhibitor is released immediately when work completes.

- Critical sections protect against partial maintenance states.


## Testing

Run all tests:

```bash
make  test
```
Run specific suites:

```bash
make  test-config
make  test-test-mode
make  test-terminusctl
```

## Packaging

Build Debian package:

```bash
bash  packaging/build-deb.sh
```

Build RPM package:

```bash
bash  packaging/build-rpm.sh
```

## Man pages

-  `man 8 terminusd`
-  `man 8 terminusctl`
  
## License

terminusd is proudly licensed under GPL-3.0-or-later. See [LICENSE](LICENSE).

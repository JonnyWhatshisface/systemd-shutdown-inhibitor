# Systemd Shutdown/Reboot Update Inhibitor

An inhibitor that waits for PrepareForShutdown() signal on DBUS and executions desired scripts/actions. Can be used for system updates on shutdown.

# Background

Systemd introduced a dual-reboot mechanism via the system-update state in order to install updates on reboots. The downside to this is the dual-reboot. Large systems can sometimes take a substantial amount of time to complete POST and rebooting twice isn't exactly ideal in these scenarios.

The reason the dual-reboots were introduced was because during the time of Systemd shutdown, executing various systemctl commands would fail due to being in a shutdown state. This would result in packages being in a broken state when attempting to update on shutdown, given many packages tend to execute systemctl commands as part of their pre and post actions.

Systemd supports inhibitors that will allow you to do exactly as the name implies - inhibit systemd during shutdowns and reboots. While in this state all systemctl commands function normally, including service start and stop. This is because the shutdown/reboot is inhibited before systemd is actually in a shutdown/reboot state, thus making it ideal for installing system updates at this stage.

## How does the inhibitor work?

The inhibitor works by first registering itself as a **delay** inhibitor, and then hooking in to DBus to listen for a **PrepareForShutdown()** message. The callback is executed once the **PrepareForShutdown()** message is seen. This function should be used to perform any actions you wish, including system updates via your package manager, before the system goes down.

## Why use a delay inhibitor instead of a block inhibitor?

The block inhibitors are ignored when shutdown or reboot commands are issued by root. I've tried working with the systemd community in the past to add a configuration option for root to honor block inhibitors, but it was rejected and argued repeatedly. Delay inhibitors will eventually time out based on the of **InhibitDelayMaxSec** in **/etc/systemd/logind.conf**. When the value there is reached, the inhibitor will let go even if whatever tasks it's doing is not completed, and the system will continue with its shutdown.

This makes setting an apporopriate value critical. If your tasks complete before the **InhibitDelayMaxSec** value is reached, then the inhibitor will let go and the shutdown will continue, so adding a high value does not delay the reboot/shutdown sequence any longer than is needed by the script/tasks you're executing.

## What if I'm already using the system-update service?

The inhibitor is a drop-in that can act as either a replacement or an enhancement to the current dual-reboot mechanisms in use today. It can run in parallel with them, keeping the dual-reboot method as a backup, or it can replace it entirely. Integration is implementation specific to how you've leveraged the dual-reboot update mechanism, but it generally works via some systemd unit file creating the /system-update symlink. Just use the same logic that would occur via the inhibitor by creating the /system-update symlink and removing it when your update mechanism finishes. This way, if the inhibitor is removed or shutdown, the /system-update symlink will still exist and the system will enter update state on the next boot.

## How do I implement it?

You simply run it. There's a bit of setup required, obviously, but once that's done - it's just the `system-update-inhibitor` daemon you execute.

### Setup inhibitor Script

Implement your update procedure in an external script/binary and point the daemon config at it with `shutdown_script`.

The daemon executes `shutdown_script` when `PrepareForShutdown()` is received, waits for it to fully complete, and logs stdout/stderr plus the final exit result to syslog (`daemon` facility).

Example config:

> set_max_inhibit_delay = true
>
> shutdown_script = /usr/local/sbin/pre-shutdown.sh
>
> max_inhibit_delay = 1800
>
> # shutdown_script_user = root
>
> # shutdown_script_group = wheel

If `shutdown_script_user` and/or `shutdown_script_group` are set in the config,
the daemon applies those credentials in the child process immediately before
executing the configured script or binary. If they are left commented out,
the script runs with the daemon's existing credentials.

### Setup Systemd

This implementation uses a **Delay** inhibitor which will only inhibit systemd's shutdown state up to a pre-defined period of time. If the update script finishes sooner than this time the inhibitor will exit and release the delay allowing the system to move forward. If the update exceeds the amount of time specified then the system will forcefully reboot regardless of whether the scripts executed by the inhibitor have finished.

The daemon uses `max_inhibit_delay` from its config (default `1800`) as the desired shutdown inhibit window. It only updates **/etc/systemd/logind.conf** when `set_max_inhibit_delay = true` is present in the config. If that flag is omitted or set to `false`, the daemon leaves `logind.conf` unchanged and does not restart `systemd-logind.service`.

When `set_max_inhibit_delay = true`, the daemon checks **/etc/systemd/logind.conf** and ensures **InhibitDelayMaxSec** matches the configured value. If it does not, the daemon updates `logind.conf`. Restarting `systemd-logind.service` is separately controlled by `restart_logind_after_set = true`; if that flag is omitted or set to `false`, the daemon does not restart logind.

Manual equivalent (if you want to preconfigure it yourself):

>InhibitDelayMaxSec = 1800

In environments where firmware updates and package updates are taking place, it's recommended to use sane values here (ie 45 minutes or so) to ensure those firmware updates are not interrupted. The delay is only kicked in if the updates do not finish before it's reached. Otherwise, the system continues the reboot/shutdown as soon as your commands return.

### Start the Inhibitor

At this point you only need to start the inhibitor. You can use a unit file or any other method you wish to execute `system-update-inhibitor`, so long as it's running as **root**.

### What if I'm already using system-update-service?

There's no configuration changing required. This is a direct drop-in. The only thing you need to do is in the script being executed by the inhibitor, remove the /etc/system-update symlink. The benefit to this is that if the update script fails for any reason, the original dual-reboot system-update method will resume as normal.

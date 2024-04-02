# Systemd Shutdown Inhibitor

An inhibitor that waits for PrepareForShutdown() signal on DBUS and executions desired scripts/actions. Can be used for system updates on shutdown.

# Background

Systemd introduced a dual-reboot mechanism via the system-update state in order to install updates on reboots. The downside to this is the dual-reboot. Large systems can sometimes take a substantial amount of time to complete POST and rebooting twice isn't exactly ideal in these scenarios.

The reason the dual-reboots were introduced was because during the time of Systemd shutdown, executing various systemctl commands would fail due to being in a shutdown state. This would result in packages being in a broken state when attempting to update on shutdown.

Systemd supports inhibitors that will allow you to do exactly as the name implies - inhibit systemd during shutdowns and reboots. While in this state all systemctl commands function normally, including service start and stop. So it's ideal for installing system updates at this stage.

## How does the inhibitor work?

The inhibitor works by first registering itself as a **delay** inhibitor, and then hooking in to DBus to listen for a **PrepareForShutdown()** message. The callback is executed once the **PrepareForShutdown()** message is seen. This function should be used to perform any actions you wish, including system updates via your package manager, before the system goes down.

## What if I'm already using the system-update service?

The inhibitor is a drop-in that can act as either a replacement or an enhancement to the current dual-reboot mechanisms in use today. It can run in parallel with them, keeping the dual-reboot method as a backup, or it can replace it entirely. Integration is implementation specific to how you've leveraged the dual-reboot update mechanism, but it generally works via some systemd unit file creating the /system-update symlink. Just use the same logic that would occur via the inhibitor by creating the /system-update symlink and removing it when your update mechanism finishes. This way, if the inhibitor is removed or shutdown, the /system-update symlink will still exist and the system will enter update state on the next boot.

## How do I implement it?

You simply run it. There's a bit of setup required, obviously, but once that's done - it's just a python script you execute. Of course, there's a little bit of setup needed in advance.

### Setup inhibitor Script

Implement your update procedure under the **handle_shutdown()** function in the script. Calling external scripts is allowed, or you can do it all directly in the function.

### Setup Systemd

This implementation uses a **Delay** inhibitor which will only inhibit systemd's shutdown state up to a pre-defined period of time. If the update script finishes sooner than this time the inhibitor will exit and release the delay allowing the system to move forward. If the update exceeds the amount of time specified then the system will forcefully reboot regardless of whether the scripts executed by the inhibitor have finished.

To set the maximum inhibit time you must edit **/etc/systemd/logind.conf** and set the **InhibitDelayMaxSec** value.  For example, to set the maximum time to 1800 seconds:

>InhibitDelayMaxSec = 1800

In environments where firmware updates and package updates are taking place, it's recommended to use sane values here (ie 45 minutes or so) to ensure those firmware updates are not interrupted. The delay is only kicked in if the updates do not finish before it's reached. Otherwise, the system continues the reboot/shutdown as soon as your commands return.

### Start the Inhibitor

At this point you only need to start the inhibitor. You can use a unit file or any other method you wish to execute the inhibitor, so long as it's running as **root**.

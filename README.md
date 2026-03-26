
# Systemd Shutdown/Reboot Update Inhibitor

An inhibitor that waits for PrepareForShutdown() signal on DBUS and inhibits the shutdown/reboot until desired scripts/binaries/actions have completed.

The inhibitors hold the current state of systemd so it's not in a shutdown state at the time the actions are being executed. This allows for scripts to install system and package updates on shutdown/reboot without the packages failing due to systemctl commands failing.

# Background

Systemd introduced a system-update state for installing updates automatically when shutting down or rebooting. This unfortunately introduced a dual-reboot mechanic. The dual-reboot occurs because once systemd is in a shutdown state, **systemctl** commands will fail. Given many packages will execute **systemctl** commands as part of their pre/post installations options, this could leave the package in a broken or corrupted state. 

The solution of using a dual-reboot does indeed get around this problem, because the updates are installed while the system is on its first boot and enters the system-update-service. However, the dual-reboot has a significant impact in some organizations.

Large systems can sometimes take a substantial amount of time to complete POST, bringing significant reboot times.


Systemd supports inhibitors that will allow you to do exactly as the name implies - inhibit systemd during shutdowns and reboots so the state of systemd is not yet in a shutdown state while the inhibitor is held in place. 

While inhibited, all systemctl commands function normally, including service start, start, enable, disable and mask. Any and all other systemctl commands also work as normal. Thus, this becomes a good state to install package updates and anything else desired.
  
## How does the inhibitor work?
  
The inhibitor works by first registering itself as a **delay** inhibitor, and then hooking in to DBus to listen for the **PrepareForShutdown()** message. A signal is registered to execute a callback once the **PrepareForShutdown()** message is seen.  The callback is where we execute our actions.

## Why use a delay inhibitor instead of a block inhibitor?

The block inhibitors are ignored when shutdown or reboot commands are issued by root. I've tried working with the systemd community in the past to add a configuration option for root to honor block inhibitors, but it was rejected and argued repeatedly. 

Delay inhibitors are inherently safer because they will eventually time out based on the of **InhibitDelayMaxSec** in **/etc/systemd/logind.conf**. When the value there is reached, the inhibitor will let go even if whatever tasks it's doing is not completed, and the system will continue with its shutdown.

However, this behaviour makes setting an appropriate value critical. If your tasks complete before the **InhibitDelayMaxSec** value is reached, then the inhibitor will let go and the shutdown will continue whether your actions have completed or not.

## Is there any risk with adding too high of a value for InhibitDelayMaxSec?

Aadding a high value does not delay the reboot/shutdown sequence any longer than is needed by the script/tasks you're executing. When the tasks/scripts are finished, and the file descriptor for the inhibitor is closed, the system will resume rebooting or halting as normal.

The **InhibitDelayMaxSec** is only used as a fail-safe to force resuming the shutdown/reboot operation if the inhibitor has held the lock beyond that duration of time.

## I'm already using the system-update mechanism with dual reboots. Do I have to change anything to use this? 

The inhibitor is a drop-in that can act as either a replacement or an enhancement to the current dual-reboot mechanisms in use today.

Integration may be implementation specific depending on how you've leveraged the dual-reboot update mechanism, but it generally works via some systemd unit file creating the /system-update symlink which is seen at boot time to enter the system-update state.

The inibitor itself can be dropped in right along-side that implementation, provided the script you execute to do the updates is properly removing the /system-update symlink when it's finished. 

With this method, if the inhibitor is stopped, removed or the updates fail during that time, the /system-update symlink will still exist and the system will enter update state on the next boot.

## How do I implement it?

You simply run it and make sure you have your update script in place.

There are two versions included in this repository:

* system-update-inhibitor daemon
* inhibitor Python script

The **Python script** is a stand-alone inhibitor that can be edited/modified directly in the script.

The **Daemon** includes a configuration file (*/etc/system-update-inhibitor.conf*) which sould be configured accordingly.

All that needs to happen after this is running the inhibitor as root to register the delay inhibitor.

### Setup system-update-inhibitor

Implement your update procedure in an external script/binary and point the daemon config at it with `shutdown_script`. By default, the daemon installs its configuration at `/etc/system-update-inhibitor.conf` and also adds a `/usr/local/sbin/update-on-shutdown.sh` which runs *dnf upgrade* to install package updates on shutdown/reboot.

You can either modify `/usr/local/sbin/update-on-shutdown.sh` or replace the path in `/etc/system-update-inhibitor.conf` with your own.

The daemon executes `shutdown_script` when `PrepareForShutdown()` is received, waits for it to fully complete, logging stdout/stderr plus the final exit result to syslog (`daemon` facility).

Example config:

> set_max_inhibit_delay = true
> shutdown_script = /usr/local/sbin/pre-shutdown.sh
>  shutdown_script_user = root
>  shutdown_script_group = wheel

If `shutdown_script_user` and/or `shutdown_script_group` are set in the config, the daemon applies those credentials in the child process immediately before executing the configured script or binary. If they are left commented out, the script runs with the daemon's existing credentials.

### Setup Systemd

This implementation uses a **Delay** inhibitor which will only inhibit systemd's shutdown state up to a pre-defined period of time. If the update script finishes sooner than this time the inhibitor will exit and release the delay allowing the system to move forward. If the update exceeds the amount of time specified then the system will forcefully reboot regardless of whether the scripts executed by the inhibitor have finished.

For easier configuration, the daemon can read `max_inhibit_delay` from its config file as the desired shutdown inhibit window and can automatically update **/etc/systemd/logind.conf** if `set_max_inhibit_delay = true` in the config. If that flag is omitted or set to `false`, the daemon leaves `logind.conf` unchanged. 

Additionally, the daemon can immediately apply the new value for `max_inhibit_delay` by restarting `systemd-logind.service` **only if** `restart_logind_after_set = true`. **This option should be used with caution, because it will end all current logind sessions upon doing so.**

### If you want to configure it yourself in systemd-logind:

Make sure you set the `InhibitDelayMaxSec` to the value you want in `/etc/systemd/logind.conf`. Example:

>InhibitDelayMaxSec = 1800

In environments where firmware updates and package updates are taking place, it's recommended to use sane values here (ie 45 minutes or so) to ensure those firmware updates are not interrupted and you don't end up bricking a device. 

### Start the Inhibitor

At this point you only need to start the inhibitor.

The daemon and python versions both include a systemd unit file for convenience.

#### For the daemon:
Enable the unit file with:
>systemctl enable system-update-inhibitor.service

If you wish to start the daemon without rebooting, then simply start it with systemctl:
>systemctl start system-update-inhibitor.service

#### Running the Python script

There is a unit file included that you can manually install by copying it to `/etc/systemd/system` and then running:
>systemctl enable system-update-inhibitor.service
>systemctl start system-update-inhibitor.service

You can also test the functionality by simply running it as root. As long as the inhibitor is running, it will be triggered on shutdown/reboot. To unregister it, simply kill the process/stop the service.


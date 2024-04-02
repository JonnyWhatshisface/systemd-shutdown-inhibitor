#!/usr/bin/python
# Copyright (c) 2021 Jonathan D. Hall <jon@jonathandavidhall.com>
#
# This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <https://www.gnu.org/licenses/>.

import time, os, sys
import dbus
from gi.repository import GLib
from dbus.mainloop.glib import DBusGMainLoop

def handle_shutdown(*args):
    # Replace this code with what you want to execute
    # on shutdown/reboot.
    #
    # For YUM based distributions, I use distro-sync, personally.
    #
    # You may want to leverage subprocess perhaps. But implementation
    # is really up to you.
    #
    print("Inhibit the shutdown for 30 seconds...")
    time.sleep(30)
    print("Letting go of inhibit lock")
    os.close(inhibitor_fd)

DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
# Connect to DBus to listen for messages
login = bus.get_object("org.freedesktop.login1", "/org/freedesktop/login1")
manager = dbus.Interface(login, dbus_interface='org.freedesktop.login1.Manager')
# Register program as an inhibitor. Since we use
# a delay inhibitor, you must make sure to set
# InhibitMaxDelay to a sane value. This configuration
# parameter is in /etc/systemd/logind.conf
inhibitor = manager.Inhibit("shutdown", "Update Script", "Trigger updates on shutdown/reboot", "delay")
inhibitor_fd = inhibitor.take()
# Add a signal to call the handle_shutdown() function when
# PrepareForshutdown() is sent out on DBus.
bus.add_signal_receiver(handle_shutdown, dbus_interface="org.freedesktop.login1.Manager", signal_name="PrepareForShutdown")

loop = GLib.MainLoop()
loop.run()

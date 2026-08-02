#!/usr/bin/env python3
"""A portal client that stays connected, for testing the Inhibit backend.

`busctl call` cannot test this interface. An inhibition lives for as long as
the connection that asked for it: xdg-desktop-portal closes the Request when
the application drops off the bus, and asteroidz watches for that itself so a
crashed portal cannot leave the machine awake forever (see
src/ipc/inhibit-portal.h). busctl exits the instant its call returns, so every
inhibition it takes is reclaimed a millisecond later -- correctly, and
uselessly. What a test needs is a client that asks and then waits.

It speaks the *impl* interface directly rather than going through
xdg-desktop-portal, standing in for what xdp itself would send. That keeps the
test about the compositor: no portal daemon to install, configure or race, and
the object paths are ours to choose, so a test can call Close on a handle it
knows.

  --inhibit FLAGS   take an inhibition (1 logout, 2 user-switch, 4 suspend,
                    8 idle; add them together) and hold it
  --monitor         create a monitoring session and print every StateChanged
                    it receives, one JSON object per line

Prints `ready` on stdout once every request has been answered, then blocks.
Exits on SIGTERM/SIGINT -- which is itself a case worth testing, since it is
how an application dying is spelled.
"""

import argparse
import json
import os
import signal
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

BUS_NAME = "org.freedesktop.impl.portal.desktop.asteroidz"
OBJ_PATH = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.impl.portal.Inhibit"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--address", default=os.environ.get("DBUS_SESSION_BUS_ADDRESS"))
    ap.add_argument("--inhibit", type=int, default=0, help="flags bitmask")
    ap.add_argument("--app-id", default="org.example.Tester")
    ap.add_argument("--reason", default="")
    ap.add_argument(
        "--handle",
        default="/org/freedesktop/portal/desktop/request/tester/1",
        help="the Request object path; the test calls Close on this",
    )
    ap.add_argument("--monitor", action="store_true")
    ap.add_argument(
        "--session",
        default="/org/freedesktop/portal/desktop/session/tester/1",
        help="the Session object path for --monitor",
    )
    args = ap.parse_args()

    if not args.address:
        sys.exit("no bus address: pass --address or set DBUS_SESSION_BUS_ADDRESS")

    bus = Gio.DBusConnection.new_for_address_sync(
        args.address,
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
        | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None,
        None,
    )

    # Subscribed BEFORE CreateMonitor: the backend answers a new monitor with
    # the current state immediately (a monitor created while the screen is
    # already locked would otherwise believe it is not), and subscribing
    # afterwards would miss exactly that signal.
    if args.monitor:
        def on_state(_conn, _sender, _path, _iface, _signal, params):
            session, state = params.unpack()
            print(json.dumps({"session": session, "state": state}), flush=True)

        bus.signal_subscribe(
            None, IFACE, "StateChanged", OBJ_PATH, None,
            Gio.DBusSignalFlags.NONE, on_state,
        )

    if args.inhibit:
        options = {}
        if args.reason:
            options["reason"] = GLib.Variant("s", args.reason)
        bus.call_sync(
            BUS_NAME, OBJ_PATH, IFACE, "Inhibit",
            GLib.Variant(
                "(ossua{sv})",
                (args.handle, args.app_id, "", args.inhibit, options),
            ),
            None, Gio.DBusCallFlags.NONE, 5000, None,
        )

    if args.monitor:
        bus.call_sync(
            BUS_NAME, OBJ_PATH, IFACE, "CreateMonitor",
            GLib.Variant("(ooss)", (args.handle, args.session, args.app_id, "")),
            GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, 5000, None,
        )

    print("ready", flush=True)

    loop = GLib.MainLoop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, lambda: (loop.quit(), True)[1])
    loop.run()


if __name__ == "__main__":
    main()

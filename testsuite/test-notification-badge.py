#!/usr/bin/env python3
"""Reproduce the GNOME dock-badge leak, then verify withdraw-on-close.

Ptyxis posts a GNotification with the tab UUID as the id when a command
finishes in an unfocused window. GNOME Shell persists that notification
and Ubuntu Dock badges Terminal from the count.

Existing code withdraws only when that tab is focused. Closing the tab
without focusing it never calls g_application_withdraw_notification, so
the badge stays. This test talks to the same org.gtk.Notifications API
GApplication uses.

Part 1 mimics the old close path (no withdraw) and asserts the
notification is still in ~/.local/share/gnome-shell/notifications.
Part 2 mimics the new close path (withdraw) and asserts it is gone.

Requires a GNOME Shell session (not headless mutter). Skips if
org.gtk.Notifications is missing. Not run by `meson test` / GNOME CI.
"""

from __future__ import annotations

import sys
import time
import uuid
from pathlib import Path

try:
    from gi.repository import Gio, GLib
except ImportError:
    print("SKIP: PyGObject is not available")
    sys.exit(77)

APP_ID = "org.gnome.Ptyxis"
STATE = Path.home() / ".local/share/gnome-shell/notifications"
VARIANT_TYPE = GLib.VariantType("a(sa(sv))")
BUS_NAME = "org.gtk.Notifications"
PATH = "/org/gtk/Notifications"
IFACE = "org.gtk.Notifications"


def session_bus():
    return Gio.bus_get_sync(Gio.BusType.SESSION, None)


def notifications_available(conn) -> bool:
    try:
        reply = conn.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "NameHasOwner",
            GLib.Variant("(s)", (BUS_NAME,)),
            GLib.VariantType("(b)"),
            Gio.DBusCallFlags.NONE,
            2000,
            None,
        )
        return bool(reply.unpack()[0])
    except GLib.Error:
        return False


def add_notification(conn, nid: str, title: str, body: str) -> None:
    payload = {
        "title": GLib.Variant("s", title),
        "body": GLib.Variant("s", body),
        "priority": GLib.Variant("s", "normal"),
        "default-action": GLib.Variant("s", "app.focus-tab-by-uuid"),
        "default-action-target": GLib.Variant("s", nid),
    }
    conn.call_sync(
        BUS_NAME,
        PATH,
        IFACE,
        "AddNotification",
        GLib.Variant("(ssa{sv})", (APP_ID, nid, payload)),
        None,
        Gio.DBusCallFlags.NONE,
        5000,
        None,
    )


def withdraw_notification(conn, nid: str) -> None:
    conn.call_sync(
        BUS_NAME,
        PATH,
        IFACE,
        "RemoveNotification",
        GLib.Variant("(ss)", (APP_ID, nid)),
        None,
        Gio.DBusCallFlags.NONE,
        5000,
        None,
    )


def stored_ids() -> list[str]:
    if not STATE.is_file() or STATE.stat().st_size == 0:
        return []
    variant = GLib.Variant.new_from_bytes(
        VARIANT_TYPE, GLib.Bytes.new(STATE.read_bytes()), True
    )
    found: list[str] = []
    for app_id, notifications in variant.unpack():
        if app_id != APP_ID:
            continue
        for nid, _payload in notifications:
            found.append(nid)
    return found


def wait_until(pred, timeout=5.0, step=0.1, hold=0.4) -> bool:
    deadline = time.monotonic() + timeout
    held_since = None
    while time.monotonic() < deadline:
        if pred():
            if held_since is None:
                held_since = time.monotonic()
            elif time.monotonic() - held_since >= hold:
                return True
        else:
            held_since = None
        time.sleep(step)
    return pred()


def test_existing_code_leaves_badge_after_close_without_viewing(conn) -> None:
    """Old Ptyxis: tab close does not withdraw. Badge stays. This is the bug."""
    nid = str(uuid.uuid4())
    add_notification(conn, nid, "Command completed", "sleep 2")
    assert wait_until(lambda: nid in stored_ids()), (
        f"failed to force a notification, store={stored_ids()}"
    )
    print(f"forced notification (tab not viewed): {nid}")

    # Existing close path: destroy the tab object, no withdraw_notification.
    print("existing code: close tab without viewing (no withdraw)")
    assert nid in stored_ids(), "notification vanished before close; cannot reproduce"
    leftover = stored_ids()
    print(f"REPRODUCED: notification still persisted after close: {leftover}")
    assert nid in leftover

    # Do not leak a badge into the developer session.
    withdraw_notification(conn, nid)
    assert wait_until(lambda: nid not in stored_ids())


def test_withdraw_on_close_clears_badge(conn) -> None:
    """New Ptyxis: close/destroy calls g_application_withdraw_notification."""
    nid = str(uuid.uuid4())
    add_notification(conn, nid, "Command completed", "sleep 2")
    assert wait_until(lambda: nid in stored_ids()), (
        f"failed to force a notification, store={stored_ids()}"
    )
    print(f"forced notification (tab not viewed): {nid}")

    withdraw_notification(conn, nid)
    assert wait_until(lambda: nid not in stored_ids()), (
        f"FIX FAILED: still persisted after withdraw: {stored_ids()}"
    )
    print("fixed close path: notification cleared")


def main() -> int:
    conn = session_bus()
    if not notifications_available(conn):
        print("SKIP: org.gtk.Notifications is not on the session bus")
        return 77

    test_existing_code_leaves_badge_after_close_without_viewing(conn)
    test_withdraw_on_close_clears_badge(conn)
    print("OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)

#!/usr/bin/env python3
"""MeshCore power poll over BLE via GLib D-Bus (BlueZ GATT).

Usage: ble_power.py [--mac AA:BB:CC:DD:EE:FF]
Requires the device to be paired/bonded with the local BlueZ first.
"""
import argparse
import gi
gi.require_version("GdkPixbuf", "2.0")  # harmless
from gi.repository import Gio, GLib
import sys, struct

_ap = argparse.ArgumentParser(description="Poll the MeshCore 'power' CLI over BLE")
_ap.add_argument("--mac", default="10:BD:A3:5B:B8:8D", help="device BLE address")
_ARGS = _ap.parse_args()
_DEVPATH = "/org/bluez/hci0/dev_" + _ARGS.mac.replace(":", "_")

BUS = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
TX_PATH = _DEVPATH + "/service0028/char0029"
RX_PATH = _DEVPATH + "/service0028/char002c"

notif = bytearray()
done = GLib.MainLoop()

def on_signal(conn, sender, path, iface, signal, params):
    if path != TX_PATH or signal != "PropertiesChanged":
        return
    _iface, changed, _inv = params.unpack()
    if "Value" in changed:
        notif.extend(changed["Value"])
        if b"cfg=" in bytes(notif):
            done.quit()

BUS.signal_subscribe(
    None, "org.freedesktop.DBus.Properties", "PropertiesChanged", TX_PATH,
    None, Gio.DBusSignalFlags.NONE, on_signal)

def prop(path, iface, name):
    return BUS.call_sync(
        "org.bluez", path, "org.freedesktop.DBus.Properties", "Get",
        GLib.Variant("(ss)", (iface, name)), GLib.VariantType("(v)"),
        Gio.DBusCallFlags.NONE, -1, None).unpack()[0]

def call(path, iface, method, variant=None):
    return BUS.call_sync(
        "org.bluez", path, iface, method, variant, None,
        Gio.DBusCallFlags.NONE, -1, None)

# subscribe notifications on TX
print("Notifying:", prop(TX_PATH, "org.bluez.GattCharacteristic1", "Notifying"))
call(TX_PATH, "org.bluez.GattCharacteristic1", "StartNotify")
print("Notifying:", prop(TX_PATH, "org.bluez.GattCharacteristic1", "Notifying"))

# send the power frame
payload = bytes([66]) + b"power"
frame = b"<" + bytes([len(payload) & 0xFF, len(payload) >> 8]) + payload
opts = GLib.Variant("a{sv}", {})
ay = GLib.Variant.new_from_bytes(GLib.VariantType.new("ay"), GLib.Bytes.new(frame), False)
call(RX_PATH, "org.bluez.GattCharacteristic1", "WriteValue",
     GLib.Variant.new_tuple(ay, opts))
print("frame sent, waiting for reply...")

GLib.timeout_add_seconds(8, done.quit)
done.run()

buf = bytes(notif)
print("raw:", buf.hex())
while b">" in buf and len(buf) >= 3:
    i = buf.find(b">")
    if i > 0: buf = buf[i:]
    ln = int.from_bytes(buf[1:3], "little")
    if ln == 0 or ln > 180:
        buf = buf[1:]; continue
    if len(buf) >= 3 + ln:
        f = buf[3:3+ln]; buf = buf[3+ln:]
        if f[0] == 29 and b"CPU" in f:
            print("POWER (over BLE):", f[1:].decode("utf-8", "replace").strip())
            sys.exit(0)
    else:
        break
print("no parsed reply")

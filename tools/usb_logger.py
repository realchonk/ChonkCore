#!/usr/bin/env python3
"""
MeshCore USB companion-protocol logger.

Attaches to a companion device's USB serial port (ENABLE_USB_INTERFACE builds)
and logs the traffic the device pushes to connected clients.

Modes:
  passive (default)  Just listens. Safe to run while the phone app is also
                     connected -- it never pops messages off the device's
                     offline queue, so the app still receives everything.
                     Logs: message-waiting tickles, send confirmations (ACKs),
                     raw LoRa RX activity (SNR/RSSI), adverts, and any frames
                     mirrored from the phone app's session.

  sync               Acts like a real client: announces itself (CMD_APP_START),
                     fetches the contact list for name resolution, and drains
                     the message queue (CMD_SYNC_NEXT_MESSAGE) whenever the
                     device signals new messages. Logs full message text.
                     WARNING: syncing removes messages from the device queue
                     permanently -- do NOT run this while the phone app is
                     connected, they will race for messages.

Wire format: frames are '>' + len_lo + len_hi + payload (device -> client)
             and      '<' + len_lo + len_hi + payload (client -> device).

Usage:
  python3 usb_logger.py                        # passive sniffing
  python3 usb_logger.py --mode sync            # full message content (standalone)
  python3 usb_logger.py --logfile mesh.log     # also write plain-text log
  python3 usb_logger.py --hexdump              # show raw frames too
"""

import argparse
import datetime
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

# ---- protocol constants (examples/companion_radio/MyMesh.cpp) ----

CMD_APP_START = 1
CMD_SEND_TXT_MSG = 2
CMD_GET_CONTACTS = 4
CMD_SYNC_NEXT_MESSAGE = 10

RESP_NAMES = {
    0: "OK", 1: "ERR", 2: "CONTACTS_START", 3: "CONTACT", 4: "END_OF_CONTACTS",
    5: "SELF_INFO", 6: "SENT", 7: "CONTACT_MSG_RECV", 8: "CHANNEL_MSG_RECV",
    9: "CURR_TIME", 10: "NO_MORE_MESSAGES", 11: "EXPORT_CONTACT",
    12: "BATT_AND_STORAGE", 13: "DEVICE_INFO", 14: "PRIVATE_KEY", 15: "DISABLED",
    16: "CONTACT_MSG_RECV_V3", 17: "CHANNEL_MSG_RECV_V3", 18: "CHANNEL_INFO",
    19: "SIGN_START", 20: "SIGNATURE", 21: "CUSTOM_VARS", 22: "ADVERT_PATH",
    23: "TUNING_PARAMS", 24: "STATS", 25: "AUTOADD_CONFIG", 27: "CHANNEL_DATA_RECV",
    28: "DEFAULT_FLOOD_SCOPE", 29: "CLI_REPLY",
    0x80: "PUSH_ADVERT", 0x81: "PUSH_PATH_UPDATED", 0x82: "PUSH_SEND_CONFIRMED",
    0x83: "PUSH_MSG_WAITING", 0x84: "PUSH_RAW_DATA", 0x85: "PUSH_LOGIN_SUCCESS",
    0x86: "PUSH_LOGIN_FAIL", 0x87: "PUSH_STATUS_RESPONSE", 0x88: "PUSH_LOG_RX_DATA",
    0x89: "PUSH_TRACE_DATA", 0x8A: "PUSH_NEW_ADVERT", 0x8B: "PUSH_TELEMETRY_RESPONSE",
    0x8C: "PUSH_BINARY_RESPONSE",
}

TXT_TYPES = {0: "plain", 1: "signed", 2: "sig-req", 3: "cli-data", 4: "cli-cmd"}
PUB_KEY_SIZE = 32
MAX_PATH_SIZE = 64
CONTACT_FRAME_NAME_OFF = 1 + PUB_KEY_SIZE + 3 + MAX_PATH_SIZE   # -> 32-byte name field

# pubkey 6-byte prefix -> contact name
contacts = {}
# expected-ack hash -> short description of the send (for ACK correlation)
pending_acks = {}


def now():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def log(line, logfile=None):
    print(line, flush=True)
    if logfile:
        logfile.write(line + "\n")
        logfile.flush()


def hexdump(b, limit=48):
    h = " ".join(f"{x:02x}" for x in b[:limit])
    return h + (" ..." if len(b) > limit else "")


def fmt_time(ts):
    try:
        return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")
    except (ValueError, OSError, OverflowError):
        return f"@{ts}"


def name_for(pub6):
    return contacts.get(pub6.hex(), pub6.hex())


def decode_contact_frame(f):
    """[code][pub32][type][flags][path_len][path64][name32][advert4][lat4][lon4][lastmod4]"""
    if len(f) < CONTACT_FRAME_NAME_OFF + 32:
        return None
    pub = f[1:1 + PUB_KEY_SIZE]
    ctype = f[33]
    name = f[CONTACT_FRAME_NAME_OFF:CONTACT_FRAME_NAME_OFF + 32].split(b"\0")[0].decode("utf-8", "replace")
    contacts[pub[:6].hex()] = name or pub[:6].hex()
    return pub, ctype, name


def decode_message_frame(f, logfile):
    """Handle RESP_CODE_CONTACT_MSG_RECV(_V3) and CHANNEL variants."""
    code = f[0]
    v3 = code in (16, 17)
    channel = code in (8, 17)
    i = 1
    snr = None
    if v3:
        snr = int.from_bytes([f[1]], "little", signed=True) / 4.0
        i = 4
    if channel:
        chan = f[i]
        i += 1
        who = f"channel#{chan}"
    else:
        pub6 = f[i:i + 6]
        i += 6
        who = name_for(pub6)
    path_len = f[i]
    i += 1
    txt_type = f[i]
    i += 1
    ts = int.from_bytes(f[i:i + 4], "little")
    i += 4
    text = f[i:].decode("utf-8", "replace")
    hops = "flood" if path_len != 0xFF else "direct"
    extra = f" SNR {snr:+.1f}dB" if snr is not None else ""
    log(f"[{now()}] RX MSG  from {who} ({hops}, {TXT_TYPES.get(txt_type, txt_type)}, "
        f"sent {fmt_time(ts)}){extra}: {text!r}", logfile)


def decode(f, logfile, verbose):
    if len(f) == 0:
        return
    code = f[0]
    name = RESP_NAMES.get(code, f"?0x{code:02x}")

    if code == 16 or code == 7:
        decode_message_frame(f, logfile)
    elif code == 17 or code == 8:
        decode_message_frame(f, logfile)
    elif code == 6:  # RESP_CODE_SENT (mirrored from any client's send)
        flood = f[1]
        ack = int.from_bytes(f[2:6], "little")
        pending_acks[ack] = "send"
        log(f"[{now()}] TX MSG  sent by a connected client ({'flood' if flood else 'direct'}), "
            f"waiting for ack {ack:08x}", logfile)
    elif code == 0x82:  # PUSH_SEND_CONFIRMED
        ack = int.from_bytes(f[1:5], "little")
        trip = int.from_bytes(f[5:9], "little")
        log(f"[{now()}] ACK     delivery confirmed (ack {ack:08x}, round-trip {trip} ms)", logfile)
        pending_acks.pop(ack, None)
    elif code == 0x83:  # PUSH_MSG_WAITING
        log(f"[{now()}] WAITING device has message(s) in the offline queue"
            + (" -- run with --mode sync (without the app connected) to see the content"
               if not verbose else ""), logfile)
    elif code == 0x88:  # PUSH_LOG_RX_DATA: raw LoRa packet
        snr = int.from_bytes([f[1]], "little", signed=True) / 4.0
        rssi = int.from_bytes([f[2]], "little", signed=True)
        log(f"[{now()}] LORA RX {len(f) - 3} bytes  SNR {snr:+.1f}dB  RSSI {rssi}dBm  {hexdump(f[3:])}",
            logfile)
    elif code == 0x8A:  # PUSH_NEW_ADVERT
        c = decode_contact_frame(f)
        if c:
            log(f"[{now()}] ADVERT  new contact: {c[2]!r} (type {c[1]})", logfile)
    elif code == 0x80:  # PUSH_ADVERT
        pub = f[1:1 + PUB_KEY_SIZE]
        log(f"[{now()}] ADVERT  {name_for(pub[:6])} re-advertised", logfile)
    elif code == 3:  # RESP_CODE_CONTACT (contacts sync)
        c = decode_contact_frame(f)
        if c and verbose:
            log(f"[{now()}] CONTACT {c[2]!r}", logfile)
    elif code == 5:  # SELF_INFO
        pub = f[5:5 + PUB_KEY_SIZE]
        log(f"[{now()}] SELF    node up, pubkey {pub[:6].hex()}", logfile)
    elif code == 10:
        if verbose:
            log(f"[{now()}] SYNC    queue drained", logfile)
    else:
        if verbose:
            log(f"[{now()}] FRAME   {name} ({len(f)} bytes)  {hexdump(f)}", logfile)


class FrameReader:
    """Extracts '>' frames from the mixed stream of protocol frames and
    console/debug text (the USB CDC port carries both; debug lines can
    contain '>' characters, so validate frame lengths before accepting).
    Printable text found in the discarded bytes is collected in .lines
    for console passthrough (e.g. MESH_PACKET_LOGGING output)."""

    MAX_FRAME = 176 + 4   # MAX_FRAME_SIZE + transport codes

    def __init__(self, ser):
        self.ser = ser
        self.buf = b""
        self.lines = []
        self._line = ""

    def _discard(self, data):
        for b in data:
            if b in (10, 13):
                if self._line:
                    self.lines.append(self._line)
                    self._line = ""
            elif 32 <= b < 127:
                self._line += chr(b)
            else:
                self._line = ""   # binary junk invalidates the line

    def _fill(self):
        try:
            n = max(1, self.ser.in_waiting)
            self.buf += self.ser.read(n)
        except serial.SerialException:
            # flaky USB: device dropped momentarily; wait for it to return
            time.sleep(0.5)

    def read(self, timeout=0.1):
        """Return the next plausible '>' frame payload, or None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if len(self.buf) < 3:
                self._fill()
                idx = self.buf.find(b">")
                if idx > 0:
                    self._discard(self.buf[:idx])
                    self.buf = self.buf[idx:]
                if len(self.buf) < 3:
                    continue
            ln = int.from_bytes(self.buf[1:3], "little")
            if ln == 0 or ln > self.MAX_FRAME:
                # bogus header (almost certainly debug text containing '>')
                self._discard(self.buf[:1])
                self.buf = self.buf[1:]
                continue
            if len(self.buf) < 3 + ln:
                self._fill()
                if len(self.buf) < 3 + ln:
                    continue
            frame = self.buf[3:3 + ln]
            self.buf = self.buf[3 + ln:]
            return frame
        return None


def send_cmd(ser, payload):
    ln = len(payload)
    ser.write(b"<" + bytes([ln & 0xFF, ln >> 8]) + payload)


def sync_drain(ser, reader, logfile, verbose):
    """Pull all queued messages (sync mode only)."""
    n = 0
    while True:
        send_cmd(ser, bytes([CMD_SYNC_NEXT_MESSAGE]))
        frame = None
        while frame is None:
            frame = reader.read(timeout=2.0)
        decode(frame, logfile, verbose)
        if frame[0] == 10:  # NO_MORE_MESSAGES
            break
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0", help="serial port (default /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--mode", choices=["passive", "sync"], default="passive",
                    help="passive: listen only (safe with the app connected); "
                         "sync: act as a client, drains the message queue (full text)")
    ap.add_argument("--verbose", action="store_true", help="log every frame, incl. unknown ones")
    ap.add_argument("--hexdump", action="store_true", help="hexdump all raw frames")
    ap.add_argument("--console", action="store_true",
                    help="also pass through console text lines (e.g. MESH_PACKET_LOGGING RX/TX lines)")
    ap.add_argument("--logfile", metavar="FILE", help="also append decoded lines to FILE")
    args = ap.parse_args()

    logfile = open(args.logfile, "a") if args.logfile else None
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"cannot open {args.port}: {e}")

    mode = f"{args.mode}"
    log(f"[{now()}] === meshcore usb logger on {args.port} ({mode} mode) ===", logfile)
    if args.mode == "sync":
        log(f"[{now()}] NOTE: sync mode pops messages off the device queue; "
            f"disconnect the phone app first.", logfile)
        # announce ourselves like a real client (app name, 7 reserved bytes)
        send_cmd(ser, bytes([CMD_APP_START]) + b"\0" * 7 + b"usb-logger")
        time.sleep(1.0)
        # fetch contacts for name resolution
        send_cmd(ser, bytes([CMD_GET_CONTACTS]))
        time.sleep(1.0)

    reader = FrameReader(ser)
    try:
        while True:
            frame = reader.read(timeout=0.5)
            if args.console and reader.lines:
                for line in reader.lines:
                    log(f"[{now()}] CONSOLE {line}", logfile)
                reader.lines.clear()
            if frame is None:
                continue
            if args.hexdump:
                log(f"[{now()}] RAW     {hexdump(frame, 64)}", logfile)
            decode(frame, logfile, args.verbose or args.mode == "sync")
            # in sync mode, a tickle means: drain the queue
            if args.mode == "sync" and frame[0] == 0x83:
                sync_drain(ser, reader, logfile, args.verbose)
    except KeyboardInterrupt:
        log(f"[{now()}] === stopped ===", logfile)
    finally:
        if logfile:
            logfile.close()
        ser.close()


if __name__ == "__main__":
    main()

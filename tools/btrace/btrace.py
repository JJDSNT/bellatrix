#!/usr/bin/env python3
# tools/btrace/btrace.py
#
# Captures JSON Lines bus trace output from the Bellatrix UART and
# writes it to stdout or a file.
#
# Usage:
#   python3 tools/btrace/btrace.py --port /dev/ttyUSB0 --baud 115200
#   python3 tools/btrace/btrace.py --port /dev/ttyUSB0 --save boot.jsonl
#   python3 tools/btrace/btrace.py --port /dev/ttyUSB0 --filter unimpl

import argparse
import json
import sys
import signal
import time
from datetime import datetime

try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False


def parse_args():
    p = argparse.ArgumentParser(description="Bellatrix bus trace capture")
    p.add_argument("--port", default="/dev/ttyUSB0", help="Serial port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--save", metavar="FILE", help="Save JSON Lines to file")
    p.add_argument("--filter", choices=["all", "unimpl", "cia", "chipset", "ipl", "state"],
                   default="all", help="Show only matching event types")
    p.add_argument("--timeout", type=float, default=None, help="Stop after N seconds")
    p.add_argument("--stdin", action="store_true", help="Read from stdin instead of serial port")
    return p.parse_args()


def should_show(event: dict, filter_name: str) -> bool:
    t = event.get("t", "")
    if filter_name == "all":
        return True
    if filter_name == "unimpl":
        return t == "btrace" and not event.get("impl", True)
    if filter_name == "cia":
        addr = event.get("addr", "0x0")
        try:
            a = int(addr, 16)
            return t == "btrace" and (0xBFD000 <= a <= 0xBFEFFF)
        except ValueError:
            return False
    if filter_name == "chipset":
        addr = event.get("addr", "0x0")
        try:
            a = int(addr, 16)
            return t == "btrace" and (0xDFF000 <= a <= 0xDFF1FF)
        except ValueError:
            return False
    if filter_name in ("ipl", "state"):
        return t == filter_name
    return True


def run(args):
    if not HAS_SERIAL and not args.stdin:
        print("ERROR: pyserial not installed. Run: pip install pyserial", file=sys.stderr)
        sys.exit(1)

    outfile = open(args.save, "w") if args.save else None
    start_time = time.monotonic()
    line_count = 0
    event_count = 0

    def cleanup(sig=None, frame=None):
        if outfile:
            outfile.close()
        print(f"\nCaptured {line_count} lines, {event_count} JSON events.", file=sys.stderr)
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)

    def handle_line(line: str):
        nonlocal line_count, event_count
        line = line.strip()
        if not line:
            return
        line_count += 1
        try:
            event = json.loads(line)
            event_count += 1
            if should_show(event, args.filter):
                formatted = json.dumps(event, separators=(",", ":"))
                print(formatted)
                sys.stdout.flush()
            if outfile:
                outfile.write(line + "\n")
                outfile.flush()
        except json.JSONDecodeError:
            # Non-JSON line (kprintf debug output) — pass through to stderr
            print(f"[UART] {line}", file=sys.stderr)

    if args.stdin:
        for line in sys.stdin:
            handle_line(line)
            if args.timeout and (time.monotonic() - start_time) > args.timeout:
                break
    else:
        with serial.Serial(args.port, args.baud, timeout=1) as ser:
            print(f"Connected to {args.port} at {args.baud} baud. Ctrl+C to stop.",
                  file=sys.stderr)
            while True:
                if args.timeout and (time.monotonic() - start_time) > args.timeout:
                    break
                raw = ser.readline()
                if raw:
                    try:
                        handle_line(raw.decode("utf-8", errors="replace"))
                    except Exception as e:
                        print(f"[ERR] {e}", file=sys.stderr)

    cleanup()


if __name__ == "__main__":
    run(parse_args())

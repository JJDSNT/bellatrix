#!/usr/bin/env python3
# tools/btrace/analyze.py
#
# Analyzes a btrace JSON Lines log and produces a structured report of:
#   - Total accesses
#   - Unimplemented registers (sorted by access count)
#   - IPL transitions
#   - VBL count
#   - Suggested next registers to implement
#
# Usage:
#   python3 tools/btrace/analyze.py boot.jsonl
#   python3 tools/btrace/analyze.py --report boot.jsonl > report.json
#   python3 tools/btrace/analyze.py --unimpl boot.jsonl

import argparse
import json
import sys
from collections import defaultdict

# Known Amiga register names (OCS subset for the MVP)
REGISTER_NAMES = {
    0xDFF002: "DMACONR",  0xDFF004: "VPOSR",    0xDFF006: "VHPOSR",
    0xDFF008: "DSKDATR",  0xDFF00A: "JOY0DAT",  0xDFF00C: "JOY1DAT",
    0xDFF00E: "CLXDAT",   0xDFF010: "ADKCONR",  0xDFF012: "POT0DAT",
    0xDFF014: "POT1DAT",  0xDFF016: "POTGOR",   0xDFF018: "SERDATR",
    0xDFF01A: "DSKBYTR",  0xDFF01C: "INTENAR",  0xDFF01E: "INTREQR",
    0xDFF020: "DSKPTH",   0xDFF022: "DSKPTL",
    0xDFF07E: "COPCON",   0xDFF080: "COP1LCH",  0xDFF082: "COP1LCL",
    0xDFF084: "COP2LCH",  0xDFF086: "COP2LCL",  0xDFF088: "COPJMP1",
    0xDFF08A: "COPJMP2",  0xDFF08C: "COPINS",   0xDFF08E: "DIWSTRT",
    0xDFF090: "DIWSTOP",  0xDFF092: "DDFSTRT",  0xDFF094: "DDFSTOP",
    0xDFF096: "DMACON",   0xDFF098: "CLXCON",   0xDFF09A: "INTENA",
    0xDFF09C: "INTREQ",   0xDFF09E: "ADKCON",
    0xDFF0A0: "AUD0LCH",  0xDFF0A2: "AUD0LCL",  0xDFF0A4: "AUD0LEN",
    0xDFF0A6: "AUD0PER",  0xDFF0A8: "AUD0VOL",  0xDFF0AA: "AUD0DAT",
    0xDFF0E0: "BPL1PTH",  0xDFF0E2: "BPL1PTL",  0xDFF0E4: "BPL2PTH",
    0xDFF0E6: "BPL2PTL",  0xDFF0E8: "BPL3PTH",  0xDFF0EA: "BPL3PTL",
    0xDFF100: "BPLCON0",  0xDFF102: "BPLCON1",  0xDFF104: "BPLCON2",
    0xDFF106: "BPLCON3",  0xDFF108: "BPL1MOD",  0xDFF10A: "BPL2MOD",
    0xDFF120: "SPR0PTH",  0xDFF122: "SPR0PTL",
    0xDFF180: "COLOR00",  0xDFF182: "COLOR01",  0xDFF184: "COLOR02",
    0xDFF186: "COLOR03",  0xDFF188: "COLOR04",  0xDFF18A: "COLOR05",
    0xDFF18C: "COLOR06",  0xDFF18E: "COLOR07",  0xDFF190: "COLOR08",
    0xDFF1BE: "COLOR31",
    0xDFF030: "SERDAT",   0xDFF032: "SERPER",
    # CIA-A
    0xBFE001: "CIAAPRA",  0xBFE101: "CIAAPRB",  0xBFE201: "CIAADDRA",
    0xBFE301: "CIAAPRB",  0xBFE401: "CIAATALO", 0xBFE501: "CIAATAHI",
    0xBFE601: "CIAATBLO", 0xBFE701: "CIAATBHI", 0xBFE801: "CIAATODLO",
    0xBFE901: "CIAATODMI",0xBFEA01: "CIAATODHI",0xBFED01: "CIAAICR",
    0xBFEE01: "CIAACRA",  0xBFEF01: "CIAACRB",
    # CIA-B
    0xBFDF00: "CIABPRA",  0xBFDF01: "CIABPRB",
}


def reg_name(addr: int) -> str:
    return REGISTER_NAMES.get(addr, f"UNKNOWN_{addr:06X}")


def is_cia(addr: int) -> bool:
    return 0xBFD000 <= addr <= 0xBFEFFF


def is_chipset(addr: int) -> bool:
    return 0xDFF000 <= addr <= 0xDFF1FF


def parse_args():
    p = argparse.ArgumentParser(description="Bellatrix btrace log analyzer")
    p.add_argument("logfile", nargs="?", default="-", help="JSON Lines log (default: stdin)")
    p.add_argument("--report", action="store_true", help="Output machine-readable JSON report")
    p.add_argument("--unimpl", action="store_true", help="Show only unimplemented registers")
    p.add_argument("--top", type=int, default=20, help="How many top registers to show")
    return p.parse_args()


def analyze(lines):
    stats = {
        "total_accesses": 0,
        "unimplemented": 0,
        "ipl_transitions": 0,
        "vbl_count": 0,
        "last_pc": None,
    }
    unimpl_regs = defaultdict(lambda: {"count": 0, "last_val": None, "dirs": set()})
    all_regs    = defaultdict(lambda: {"count": 0, "last_val": None, "dirs": set()})
    ipl_events  = []

    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            continue

        t = ev.get("t")

        if t == "btrace":
            stats["total_accesses"] += 1
            try:
                addr  = int(ev["addr"], 16)
                val   = ev.get("val", "0x0")
                impl  = ev.get("impl", False)
                d_dir = ev.get("dir", "?")
                pc    = ev.get("m68k_pc")
            except (KeyError, ValueError):
                continue

            if pc:
                stats["last_pc"] = pc

            all_regs[addr]["count"]    += 1
            all_regs[addr]["last_val"]  = val
            all_regs[addr]["dirs"].add(d_dir)

            if not impl:
                stats["unimplemented"]      += 1
                unimpl_regs[addr]["count"]   += 1
                unimpl_regs[addr]["last_val"] = val
                unimpl_regs[addr]["dirs"].add(d_dir)

        elif t == "ipl":
            stats["ipl_transitions"] += 1
            ipl_events.append(ev)

    return stats, unimpl_regs, all_regs, ipl_events


def main():
    args = parse_args()

    if args.logfile == "-":
        lines = sys.stdin
    else:
        try:
            lines = open(args.logfile)
        except FileNotFoundError:
            print(f"ERROR: file not found: {args.logfile}", file=sys.stderr)
            sys.exit(1)

    stats, unimpl_regs, all_regs, ipl_events = analyze(lines)

    # Build sorted unimplemented list
    unimpl_sorted = sorted(unimpl_regs.items(), key=lambda x: -x[1]["count"])
    suggested = [reg_name(addr) for addr, _ in unimpl_sorted[:5]]

    if args.report:
        report = {
            "summary": stats,
            "unimplemented_registers": [
                {
                    "addr":         f"0x{addr:06X}",
                    "name":         reg_name(addr),
                    "access_count": info["count"],
                    "last_val":     info["last_val"],
                    "dirs":         list(info["dirs"]),
                }
                for addr, info in unimpl_sorted
            ],
            "suggested_next": suggested,
        }
        print(json.dumps(report, indent=2))
        return

    if args.unimpl:
        print(f"Unimplemented registers ({len(unimpl_regs)} unique, {stats['unimplemented']} total accesses):\n")
        for addr, info in unimpl_sorted[:args.top]:
            dirs = "/".join(sorted(info["dirs"]))
            print(f"  0x{addr:06X}  {reg_name(addr):<20} {info['count']:5d} accesses  last={info['last_val']}  [{dirs}]")
        if suggested:
            print(f"\nSuggested next: {', '.join(suggested)}")
        return

    # Default: summary
    print("=== Bellatrix Bus Trace Analysis ===\n")
    print(f"  Total accesses    : {stats['total_accesses']}")
    print(f"  Unimplemented     : {stats['unimplemented']}")
    print(f"  IPL transitions   : {stats['ipl_transitions']}")
    print(f"  Last M68K PC      : {stats['last_pc'] or 'N/A'}")

    if unimpl_sorted:
        print(f"\n  Top unimplemented registers:")
        for addr, info in unimpl_sorted[:args.top]:
            dirs = "/".join(sorted(info["dirs"]))
            print(f"    0x{addr:06X}  {reg_name(addr):<20} {info['count']:5d}×  [{dirs}]")
        print(f"\n  Suggested next: {', '.join(suggested)}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
#
# Time one AROS boot under QEMU, from the start of emulation to Wanderer having
# drawn the volume icons, and print one JSON record.
#
#   scripts/boot-timing.py              one run
#   scripts/boot-timing.py -n 5         five serial runs, plus a summary
#   scripts/boot-timing.py --label ipl  tag the records so an A/B is separable
#
# Records are appended to out/boot-timing.jsonl. See AI_context/issues/ISSUE-0011.md.
#
# Each record carries its whole sample timeline as [t, state, title_delta,
# delta, arm_pc, pc_moved, arm_sp, pstate], which is what makes the verdict
# auditable after the fact rather than something to take on trust. A run that does not reach the
# icons also leaves a stall.txt beside its serial log: ARM state for all four
# cores and the instructions around the PC.
#
# The PC pair answered the question that split the whole stall investigation --
# is the guest executing, or waiting? Neither: the stalls sit at
# curr_el_spx_sync+0 with Emu68's ARM stack exhausted. SP is sampled alongside
# for the follow-up question, which is whether that stack drains steadily or
# collapses at one moment. Always on, not behind a flag: the run is the
# expensive part. See ISSUE-0007.
#
# Why Python and not bash, when every other script here is bash: this drives a
# process, talks a protocol over a socket and reads pixels, and splitting that
# across two languages would split the state that decides the verdict.
#
# It does not build its own QEMU command line. run.sh owns that, and this
# invokes run.sh -- a second copy would drift from it silently, which has
# already cost this project a measurement against a stale ELF.
#
# What it measures
# ----------------
# t0 is taken on the host immediately before launching QEMU. The guest console
# is deferred, so nothing the guest says about its own timing is used: every
# timestamp here is the moment this process saw something.
#
# t1 is the icons, not the screen, and the distinction is not cosmetic. Over the
# first ten runs this tool measured, the Workbench screen opened at 39-48 s in
# *every* run that got that far -- including four that then sat on an empty
# backdrop for the rest of the run, two of them for a full 900 s without a pixel
# changing. Screen-open is nearly deterministic; finishing is not. A detector
# that stopped at the grey would have called all eight of those a success.
#
# Grey alone is not enough for the first stage either. On a poisoned card the
# boot reaches a Workbench-grey screen that is *perfectly uniform* and stalls
# there; anchoring the baseline on that frame would then fire on the real
# screen's own chrome. So a frame counts as a screen only when the title band
# carries something. Flat fills make that an exact test: blank is exactly zero
# non-modal pixels, and anything drawn is not.
#
# The second stage is then self-calibrating rather than a magic constant. At the
# first frame that has a title band, the number of pixels in the backdrop that
# differ from the backdrop's own modal colour is recorded as delta0 -- window
# border, chrome, pointer, whatever this build happens to draw. Icons are
# declared when the count has grown by --icon-delta pixels *beyond* delta0.
# Nothing depends on where Wanderer places the icons or on how the desktop is
# themed, and every sample's raw counts are kept in the record, so the threshold
# can be re-checked against data instead of argued about. Measured margin on the
# first ten runs: the backdrop went from 0 to 3833 non-modal pixels in one
# sample, against a --icon-delta of 1500.
#
# A second signal fell out of the same data and is worth knowing about, though
# nothing depends on it: the title band's own count jumps from 9466 to 10870 in
# exactly the sample where the icons appear. That is the screen title changing
# from "Workbench Screen" to "Wanderer <n>M graphics mem", and it corroborates
# the verdict without reading a single character.

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from collections import Counter
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The two greys, and how far a pixel may sit from one and still count. Flat
# fills, so the tolerance is only there to survive a palette change.
LOGO_GREY = (0x78, 0x78, 0x78)
WORKBENCH_GREY = (0x98, 0x98, 0x98)
GREY_TOLERANCE = 12

# Fraction of the frame height treated as the title band. Split off from the
# backdrop for two reasons: it must not pollute the backdrop's activity count,
# and whether it is blank is itself the signal that a screen exists at all. The
# title's text genuinely changes when Wanderer finishes ("Workbench Screen" ->
# "Wanderer <n>M graphics mem"), but reading it means reading pixels as text;
# the backdrop count gets the same answer without an OCR dependency.
TITLE_FRACTION = 0.10

# How far apart the two ARM PC reads in one sample are taken. Two reads are
# enough to say "moving" or "not moving" without waiting for the next frame,
# which is what separates a guest that is executing from one that is waiting.
PC_PROBE_GAP = 0.2

# Where Emu68's ARM stack starts, as it reports at boot ("ARM stack top at
# 0xffffff8000080000"). Only used to turn a raw SP into "bytes consumed", which
# is the number worth reading.
ARM_STACK_TOP = 0xffffff8000080000

# Where Emu68 maps the guest's whole address space linearly, so guest memory can
# be read from the monitor. Used to capture the instruction a guest CPU
# exception froze on.
GUEST_ALIAS_BASE = 0xffffff9000000000


# --------------------------------------------------------------------------
# QEMU monitor
# --------------------------------------------------------------------------

class Monitor:
    """HMP over a unix socket. Commands are synchronous: send, read to prompt."""

    PROMPT = b"(qemu) "

    def __init__(self, path, timeout=20.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.buf = b""
        self._read_to_prompt()          # drain the banner

    def _read_to_prompt(self):
        while self.PROMPT not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("monitor closed")
            self.buf += chunk
        out, _, self.buf = self.buf.rpartition(self.PROMPT)
        return out.decode("utf-8", "replace")

    def cmd(self, line):
        self.sock.sendall(line.encode() + b"\n")
        return self._read_to_prompt()

    def arm_state(self):
        """Core 0's ARM PC, SP and PSTATE, or (None, None, None).

        Emu68 runs the m68k on core 0, so this is where the JIT is. A PC that
        moves between two reads says the guest is executing; one that does not
        says it is parked.

        SP is here because of what the PC probe found: stalls sit at
        `curr_el_spx_sync+0` with the ARM stack exhausted, and whether that
        stack drains steadily (a leak on some handler path) or collapses at one
        moment (genuine recursion) is a different defect with a different fix.
        The stack runs down from 0xffffff8000080000; watching the value across a
        whole run answers it. See ISSUE-0007.
        """
        try:
            out = self.cmd("info registers")
        except (OSError, ConnectionError, socket.timeout):
            return None, None, None
        # The monitor echoes the command back with terminal escapes, so match
        # the values rather than trusting the line layout.
        pc = re.search(r"\bPC=([0-9a-fA-F]+)", out)
        sp = re.search(r"\bSP=([0-9a-fA-F]+)", out)
        ps = re.search(r"\bPSTATE=([0-9a-fA-F]+)", out)
        return (pc.group(1) if pc else None), (sp.group(1) if sp else None), \
               (ps.group(1) if ps else None)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# Frames
# --------------------------------------------------------------------------

def read_ppm(path):
    """Parse the binary PPM (P6) that 'screendump' writes. Returns (w, h, rgb)."""
    with open(path, "rb") as fh:
        data = fh.read()

    if not data.startswith(b"P6"):
        raise ValueError("not a P6 PPM")

    # Header tokens are whitespace-separated and '#' starts a comment.
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1                            # the single whitespace after maxval

    width, height, maxval = fields
    if maxval != 255:
        raise ValueError(f"unsupported maxval {maxval}")
    return width, height, data[pos:pos + width * height * 3]


def near(colour, target, tolerance=GREY_TOLERANCE):
    return all(abs(a - b) <= tolerance for a, b in zip(colour, target))


def non_modal(pixels):
    """How many pixels are not this region's own most common colour."""
    counts = Counter(pixels[i:i + 3] for i in range(0, len(pixels), 3))
    return sum(counts.values()) - counts.most_common(1)[0][1], counts


def analyse(path):
    """Dominant colour, plus how busy the title band and the backdrop are."""
    width, height, rgb = read_ppm(path)
    if width == 0 or height == 0 or len(rgb) < width * height * 3:
        raise ValueError("truncated frame")

    _, counts = non_modal(rgb[:width * height * 3])
    dominant, dominant_n = counts.most_common(1)[0]

    split = int(height * TITLE_FRACTION) * width * 3
    title_delta, _ = non_modal(rgb[:split])
    delta, _ = non_modal(rgb[split:width * height * 3])

    return {
        "size": [width, height],
        "dominant": "#%02x%02x%02x" % tuple(dominant),
        "dominant_frac": round(dominant_n / (width * height), 4),
        "title_delta": title_delta,
        "delta": delta,
    }


def screen_state(frame):
    """logo | blank | screen | other -- the first stage, pixels only.

    'blank' is the boot's empty grey screen and 'screen' is one with a title
    band drawn on it. Both are Workbench-grey, and telling them apart is what
    keeps delta0 anchored to the right frame.
    """
    rgb = tuple(int(frame["dominant"][i:i + 2], 16) for i in (1, 3, 5))
    if near(rgb, LOGO_GREY):
        return "logo"
    if near(rgb, WORKBENCH_GREY):
        return "screen" if frame["title_delta"] > 0 else "blank"
    return "other"


# --------------------------------------------------------------------------
# Preconditions
# --------------------------------------------------------------------------

def other_qemus():
    """QEMU processes that are not ours. Concurrent runs change the outcome."""
    try:
        out = subprocess.run(["ps", "-eo", "pid,cmd"], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return -1                       # unknown; caller decides
    return sum(1 for line in out.splitlines()[1:] if "qemu-system" in line)


def sha_short(path, n=12):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()[:n]


def qemu_version():
    try:
        out = subprocess.run(["qemu-system-aarch64", "--version"],
                             capture_output=True, text=True, check=True).stdout
        m = re.search(r"version ([\d.]+)", out)
        return m.group(1) if m else "unknown"
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def regenerate_card(dist):
    """Fresh card. Returns None, or a message if it could not be made.

    A card is not optional while ISSUE-0009 is open: the handler poisons it, so
    one boot contaminates the next. The lean kernel-link build produces only the
    ELF, so this usually needs --dist (or BELLATRIX_SD_DIST) pointing at a full
    distribution tree -- see docs/known-good-baseline.md.
    """
    cmd = [os.path.join(ROOT, "scripts", "make-sdcard.sh")]
    if dist:
        cmd += ["--dist", dist]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout).strip().splitlines()
        return "make-sdcard.sh failed: " + (tail[-1] if tail else "no output")
    return None


# --------------------------------------------------------------------------
# One run
# --------------------------------------------------------------------------

def one_run(args, workdir):
    record = {
        "run": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "label": args.label,
        "interval": args.interval,
        "timeout": args.timeout,
        "icon_delta": args.icon_delta,
        "qemu": qemu_version(),
        "loadavg": round(os.getloadavg()[0], 2),
    }

    # Refuse immediately rather than waiting one out. A live QEMU here is
    # somebody else's -- the harness kills its own before returning -- and
    # measuring alongside it is the contamination this check exists to stop.
    strays = other_qemus()
    record["other_qemus"] = strays
    if strays != 0:
        record["verdict"] = "error"
        record["error"] = f"{strays} other qemu-system process(es) running"
        return record

    if args.card:
        failure = regenerate_card(args.dist)
        if failure:
            record["verdict"] = "error"
            record["error"] = failure
            return record
        record["sd"] = "regenerated"
    else:
        record["sd"] = "reused"

    for name, path in (("emu68", "out/images/Emu68.img"),
                       ("aros", "out/aros/aros-emu68-m68k.elf"),
                       ("sd", "out/aros/sd.img")):
        full = os.path.join(ROOT, path)
        if not os.path.exists(full):
            record["verdict"] = "error"
            record["error"] = f"missing {path}"
            return record
        record[name + "_sha"] = sha_short(full)

    sock_path = os.path.join(workdir, "monitor.sock")
    serial_path = os.path.join(workdir, "serial.log")
    env = dict(os.environ, BELLATRIX_QEMU_MONITOR=sock_path)

    t0 = time.monotonic()
    proc = subprocess.Popen(
        [os.path.join(ROOT, "run.sh"), "--headless", "--no-build",
         "--serial", serial_path],
        cwd=ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)

    try:
        mon = None
        while mon is None:
            if proc.poll() is not None:
                record["verdict"] = "error"
                record["error"] = f"qemu exited before the monitor came up ({proc.returncode})"
                return record
            if time.monotonic() - t0 > 30:
                record["verdict"] = "error"
                record["error"] = "monitor socket never appeared"
                return record
            try:
                mon = Monitor(sock_path)
            except (FileNotFoundError, ConnectionRefusedError, ConnectionError):
                time.sleep(0.25)

        samples = []
        state = "none"
        delta0 = None
        t_workbench = None
        verdict = None
        frame_path = os.path.join(workdir, "frame.ppm")

        while time.monotonic() - t0 < args.timeout:
            time.sleep(args.interval)

            if proc.poll() is not None:
                verdict = "dead"
                record["error"] = f"qemu exited during the run ({proc.returncode})"
                break

            if os.path.exists(frame_path):
                os.unlink(frame_path)
            try:
                mon.cmd(f"screendump {frame_path}")
                frame = analyse(frame_path)
            except (OSError, ValueError, ConnectionError, socket.timeout) as exc:
                samples.append([round(time.monotonic() - t0, 1), "error", str(exc)])
                continue

            # Two PC reads a moment apart: "pc" and whether it changed. Cheap
            # enough (two monitor round trips per frame) that it is always on,
            # so a stall carries its own diagnosis instead of needing the whole
            # series run again.
            pc1, sp1, pstate1 = mon.arm_state()
            time.sleep(PC_PROBE_GAP)
            pc2, _, _ = mon.arm_state()

            now = round(time.monotonic() - t0, 1)
            state = screen_state(frame)
            samples.append([now, state, frame["title_delta"], frame["delta"],
                            pc1, pc1 != pc2, sp1, pstate1])
            record["size"] = frame["size"]

            if state == "screen":
                if delta0 is None:
                    delta0 = frame["delta"]
                    t_workbench = now
                elif frame["delta"] >= delta0 + args.icon_delta:
                    verdict = "icons"
                    record["t_total"] = now
                    record["delta"] = frame["delta"]
                    break

        if verdict is None:
            # Ran out of time. What was on screen decides which kind of failure
            # this was -- collapsing these into "fail" throws away the only
            # distinction that made the detector hard to build.
            verdict = {"screen": "workbench", "blank": "blank",
                       "logo": "logo"}.get(state, "other")
            if not samples:
                verdict = "dead"
            record["delta"] = samples[-1][3] if samples else None

        # What the PC probe says about the last stretch of the run. Restricted
        # to the tail because a run that made progress and then stopped would
        # otherwise be averaged with the part where it was working.
        tail = [s for s in samples[-6:] if len(s) > 5 and s[4] is not None]
        if tail:
            record["pc_moving"] = any(s[5] for s in tail)
            record["pc_last"] = tail[-1][4]
            record["pc_distinct_tail"] = len({s[4] for s in tail})

        # Stack headroom over the whole run, in bytes below the top Emu68
        # reports at boot. A value that shrinks steadily is a leak; one that
        # holds and then collapses is recursion.
        # PSTATE bit 7 is the IRQ mask. Emu68's IRQ fast path sets it in SPSR
        # before eret, and the only `msr daifclr` in the whole binary is in the
        # PowerPC path -- so how ARM interrupts are ever re-enabled on the m68k
        # side is an open question, and any delivery mechanism depends on the
        # answer. Counting how often a healthy run is sampled with IRQs masked
        # is the cheap way to ask it.
        psts = [int(s[7], 16) for s in samples if len(s) > 7 and s[7]]
        if psts:
            record["pstate_irq_masked"] = sum(1 for p in psts if p & 0x80)
            record["pstate_samples"] = len(psts)

        sps = [int(s[6], 16) for s in samples if len(s) > 6 and s[6]]
        if sps:
            record["sp_first"] = "%x" % sps[0]
            record["sp_last"] = "%x" % sps[-1]
            record["sp_min"] = "%x" % min(sps)
            record["sp_used_max"] = ARM_STACK_TOP - min(sps)

        # A stall is worth a closer look while the machine is still up: full
        # ARM state for every core, and the instructions around the PC. If the
        # guest is parked on a `wfi`, this is where that shows.
        if verdict != "icons":
            try:
                dump = ["=== info status ===", mon.cmd("info status"),
                        "=== info cpus ===", mon.cmd("info cpus"),
                        "=== info registers -a ===", mon.cmd("info registers -a")]
                if record.get("pc_last"):
                    addr = "0x" + record["pc_last"]
                    dump += [f"=== x/8i {addr} ===", mon.cmd(f"x/8i {addr}")]
                # The stall is unbounded recursion through the exception vector,
                # so the stack is thousands of copies of one 160-byte frame.
                # Two windows into it are enough to read what keeps faulting;
                # taken well away from the ends so neither is the ragged edge.
                # ARM-side memory: the host and the ARM agree on byte order,
                # so words are fine here. Only *guest* memory needs bytes.
                for off in (0x20000, 0x40000):
                    where = "0x%x" % (ARM_STACK_TOP - off)
                    dump += [f"=== x/64gx {where} ===", mon.cmd(f"x/64gx {where}")]

                # If the guest reported a CPU exception, its trap handler froze
                # the machine with the m68k PC on the serial. Read the guest's
                # own memory there through the linear alias, so the instruction
                # that faulted is captured with the run that faulted rather
                # than needing the whole thing reproduced later.
                try:
                    with open(serial_path, "rb") as fh:
                        text = fh.read().decode("utf-8", "replace")
                except OSError:
                    text = ""
                for m in re.finditer(r"exception vector 0x([0-9a-f]+) at PC 0x([0-9a-f]+)",
                                     text):
                    record["guest_exception"] = {"vector": m.group(1),
                                                 "pc": m.group(2)}
                    addr = int(m.group(2), 16) & ~0xf
                    where = "0x%x" % (GUEST_ALIAS_BASE + addr - 16)
                    # Bytes, not halfwords or words. The guest is big-endian
                    # and the monitor reads little-endian, so anything wider
                    # comes back byte-swapped -- which cost one wrong reading
                    # already (0x80ff read as an opcode when the guest sees
                    # 0xff80). Bytes have no byte order to get wrong.
                    dump += [f"=== guest code at m68k PC 0x{m.group(2)} "
                             f"(alias {where}) ===", mon.cmd(f"x/32xb {where}")]

                # The trap handler prints the guest's registers, and the user
                # stack is where the return addresses are. Those *are* code, so
                # they symbolise against the m68k ELF this repository built --
                # which turns "the PC went somewhere" into a call chain.
                usp = re.search(r"USP 0x([0-9a-f]+)", text)
                if usp:
                    record["guest_usp"] = usp.group(1)
                    where = "0x%x" % (GUEST_ALIAS_BASE + int(usp.group(1), 16))
                    dump += [f"=== guest stack at USP 0x{usp.group(1)} ===",
                             mon.cmd(f"x/192xb {where}")]
                regs = dict(re.findall(r"\b(A[0-6]) 0x([0-9a-f]+)", text))
                record["guest_aregs"] = regs
                for name in ("A6", "A2", "A0"):
                    v = regs.get(name)
                    if not v:
                        continue
                    where = "0x%x" % (GUEST_ALIAS_BASE + (int(v, 16) & ~3))
                    dump += [f"=== guest memory at {name} 0x{v} ===",
                             mon.cmd(f"x/32xb {where}")]
                with open(os.path.join(workdir, "stall.txt"), "w") as fh:
                    fh.write("\n".join(dump))
            except (OSError, ConnectionError, socket.timeout) as exc:
                record["stall_dump_error"] = str(exc)

        record["verdict"] = verdict
        record["signal"] = "backdrop-delta"
        record["t_workbench"] = t_workbench
        record["delta0"] = delta0
        record["frames"] = len(samples)
        record["samples"] = samples
        try:
            record["serial_bytes"] = os.path.getsize(serial_path)
        except OSError:
            record["serial_bytes"] = 0
        if record["serial_bytes"] == 0 and verdict != "icons":
            record["verdict"] = "dead"

        # The serial log is always kept, and this was learned the expensive way:
        # the first ten runs measured the failure rate and threw away every byte
        # of serial with the temporary directory, so the follow-up question --
        # *where* does a stalled boot stop -- needed the whole series run again.
        # The run is the expensive part; the evidence is ~130 KB.
        keep = os.path.join(ROOT, "out", "boot-timing",
                            record["run"].replace(":", ""))
        os.makedirs(keep, exist_ok=True)
        wanted = ["serial.log", "stall.txt"] + (["frame.ppm"] if args.keep else [])
        for name in wanted:
            src = os.path.join(workdir, name)
            if os.path.exists(src):
                shutil.copy2(src, keep)
        record["kept"] = os.path.relpath(keep, ROOT)

        return record
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                proc.wait(timeout=10)


# --------------------------------------------------------------------------

def summarise(records):
    verdicts = Counter(r["verdict"] for r in records)
    times = sorted(r["t_total"] for r in records if r.get("t_total") is not None)
    line = "verdicts: " + ", ".join(f"{k}={v}" for k, v in verdicts.most_common())
    if times:
        line += (f" | t_total median={statistics.median(times):.1f}s"
                 f" min={times[0]:.1f}s max={times[-1]:.1f}s n={len(times)}")
    return line


def main():
    p = argparse.ArgumentParser(
        description="Time the AROS boot from emulation start to Wanderer's icons.")
    p.add_argument("-n", "--runs", type=int, default=1)
    p.add_argument("--label", default="", help="tag records so an A/B is separable")
    p.add_argument("--interval", type=float, default=5.0,
                   help="seconds between frames; this is the resolution (default 5)")
    p.add_argument("--timeout", type=float, default=900.0,
                   help="give up after this many seconds (default 900)")
    p.add_argument("--icon-delta", type=int, default=1500,
                   help="backdrop pixels that must appear beyond delta0 (default 1500)")
    p.add_argument("--no-card", dest="card", action="store_false",
                   help="do not regenerate the SD card first (see ISSUE-0009)")
    p.add_argument("--dist", default=os.environ.get("BELLATRIX_SD_DIST", ""),
                   help="distribution tree for make-sdcard.sh")
    p.add_argument("--log", default=os.path.join(ROOT, "out", "boot-timing.jsonl"))
    p.add_argument("--keep", action="store_true",
                   help="keep the serial log and last frame of each run")
    args = p.parse_args()

    os.makedirs(os.path.dirname(args.log), exist_ok=True)
    records = []

    for i in range(args.runs):
        with tempfile.TemporaryDirectory(prefix="bellatrix-boot-") as workdir:
            record = one_run(args, workdir)
        records.append(record)

        with open(args.log, "a") as fh:
            fh.write(json.dumps(record) + "\n")

        # One line per run on stderr so a long -n is followed live; the records
        # themselves go to stdout, where they can be piped.
        print(f"[{i + 1}/{args.runs}] {record['verdict']}"
              f" t_total={record.get('t_total')}"
              f" t_workbench={record.get('t_workbench')}"
              f" delta0={record.get('delta0')}"
              f"{' error=' + record['error'] if 'error' in record else ''}",
              file=sys.stderr, flush=True)
        print(json.dumps(record), flush=True)

    if args.runs > 1:
        print(summarise(records), file=sys.stderr)

    return 0 if any(r["verdict"] == "icons" for r in records) else 1


if __name__ == "__main__":
    sys.exit(main())

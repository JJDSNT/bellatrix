#!/usr/bin/env python3
"""hdf.py — Bellatrix HDF tooling (wrapper over external/amitools).

Subcommands:
  analyze <img.hdf>                     RDB + partition + filesystem report
  create  <img.hdf> <size> [--dostype DOS3] [--name DH0]
                                        create HDF with RDB + 1 partition, formatted
  ls      <img.hdf|img.adf> [part]      list files (any xdf image)
  adf2hdf <disk.adf> <img.hdf> [--dest /] [--part DH0]
                                        copy full ADF contents into the HDF
  iso2hdf <disc.iso> <img.hdf> [--dest /] [--part DH0]
                                        extract ISO (via 7z) and copy into the HDF
  dir2hdf <host-dir> <img.hdf> [--dest /] [--part DH0]
                                        copy a host directory tree into the HDF
                                        (sanitized copy; source is not modified)
  write   <img.hdf> <host-path> [ami-path] [--part DH0]
                                        copy a host file/dir into the HDF
  read    <img.hdf> <ami-path> [host-path] [--part DH0]
                                        copy a file/dir out of the HDF
  xdf     <args...>                     raw xdftool passthrough
  rdb     <args...>                     raw rdbtool passthrough

amitools is used from source at external/amitools (no install required).
"""

import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AMITOOLS = os.path.join(REPO, "external", "amitools")


def run(tool, *args, check=True, capture=False):
    env = dict(os.environ)
    env["PYTHONPATH"] = AMITOOLS + os.pathsep + env.get("PYTHONPATH", "")
    cmd = [sys.executable, "-m", f"amitools.tools.{tool}"] + [str(a) for a in args]
    return subprocess.run(cmd, env=env, check=check,
                          capture_output=capture, text=True)


def is_hdf(path):
    return not path.lower().endswith(".adf")


def part_args(img, part):
    """xdftool needs 'open part=<name>' before FS commands on RDB images."""
    if is_hdf(img):
        return ["open", f"part={part}", "+"]
    return []


def cmd_analyze(img):
    print(f"=== {img} ({os.path.getsize(img)} bytes) ===\n")
    if is_hdf(img):
        print("--- RDB ---")
        run("rdbtool", img, "info")
        print("\n--- Partitions ---")
        run("rdbtool", img, "show")
        r = run("rdbtool", img, "info", capture=True)
        parts = [l.split()[1] for l in r.stdout.splitlines()
                 if l.strip().startswith("Partition:")]
        for p in parts:
            print(f"\n--- Filesystem check: {p} ---")
            run("xdftool", img, "open", f"part={p}", "+", "info", check=False)
    else:
        run("xdftool", img, "info")


def cmd_create(img, size, dostype="DOS3", name="DH0"):
    if os.path.exists(img):
        sys.exit(f"error: {img} already exists")
    run("rdbtool", img, "create", f"size={size}", "+", "init", "+",
        "add", f"name={name}", "size=100%", f"dostype={dostype}",
        "max_transfer=0x1fe00", "bootable=true")
    fs = {"DOS0": "ofs", "DOS1": "ffs", "DOS2": "ofs+intl", "DOS3": "ffs+intl",
          "DOS4": "ofs+dircache", "DOS5": "ffs+dircache"}.get(dostype, "ffs")
    run("xdftool", img, "open", f"part={name}", "+", "format", name, fs)
    print(f"created {img}: 1 partition '{name}' ({dostype}), formatted")


def cmd_adf2hdf(adf, img, dest="/", part="DH0"):
    with tempfile.TemporaryDirectory() as tmp:
        run("xdftool", adf, "unpack", tmp)
        # unpack creates <tmp>/<volname>/ plus <volname>.blkdev/.xdfmeta files
        vol = next(os.path.join(tmp, e) for e in os.listdir(tmp)
                   if os.path.isdir(os.path.join(tmp, e)))
        entries = sorted(e for e in os.listdir(vol) if not e.endswith(".xdfmeta"))
        for e in entries:
            src = os.path.join(vol, e)
            ami = e if dest in ("/", "") else f"{dest.rstrip('/')}/{e}"
            run("xdftool", img, *part_args(img, part), "write", src, ami)
        print(f"copied {len(entries)} entries from {adf} to {img}:{dest}")


def copy_tree_into(img, srcdir, dest, part):
    entries = sorted(os.listdir(srcdir))
    for e in entries:
        src = os.path.join(srcdir, e)
        ami = e if dest in ("/", "") else f"{dest.rstrip('/')}/{e}"
        run("xdftool", img, *part_args(img, part), "write", src, ami)
    return len(entries)


def sanitize_for_ffs(tmp):
    """Make an extracted ISO tree valid for FFS: strip ISO9660 ';1'
    version suffixes, truncate names to 30 chars, and drop entries that
    collide case-insensitively (FFS namespace is case-insensitive)."""
    for root, dirs, files in os.walk(tmp, topdown=False):
        seen = {}
        for name in sorted(files) + sorted(dirs):
            path = os.path.join(root, name)
            new = name.split(";")[0]
            if len(new) > 30:
                stem, ext = os.path.splitext(new)
                new = (stem[:30 - len(ext)] + ext) if len(ext) < 8 else new[:30]
            rel = os.path.relpath(path, tmp)
            key = new.lower()
            if key in seen:
                print(f"warning: case-duplicate skipped: {rel} "
                      f"(kept {seen[key]})")
                shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
                continue
            seen[key] = new
            if new != name:
                print(f"warning: renamed for FFS: {rel} -> {new}")
                os.rename(path, os.path.join(root, new))


def cmd_iso2hdf(iso, img, dest="/", part="DH0"):
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["7z", "x", "-y", f"-o{tmp}", iso],
                       check=True, capture_output=True)
        sanitize_for_ffs(tmp)
        n = copy_tree_into(img, tmp, dest, part)
        print(f"copied {n} top-level entries from {iso} to {img}:{dest}")


def cmd_dir2hdf(srcdir, img, dest="/", part="DH0"):
    with tempfile.TemporaryDirectory() as tmp:
        work = os.path.join(tmp, "tree")
        shutil.copytree(srcdir, work, symlinks=False)
        sanitize_for_ffs(work)
        n = copy_tree_into(img, work, dest, part)
        print(f"copied {n} top-level entries from {srcdir} to {img}:{dest}")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    cmd, rest = args[0], args[1:]

    def opt(flag, default):
        if flag in rest:
            i = rest.index(flag)
            v = rest[i + 1]
            del rest[i:i + 2]
            return v
        return default

    if cmd == "analyze":
        cmd_analyze(rest[0])
    elif cmd == "create":
        cmd_create(rest[0], rest[1], opt("--dostype", "DOS3"), opt("--name", "DH0"))
    elif cmd == "ls":
        img = rest[0]
        part = rest[1] if len(rest) > 1 else "DH0"
        run("xdftool", img, *part_args(img, part), "list")
    elif cmd == "adf2hdf":
        p = opt("--part", "DH0")
        d = opt("--dest", "/")
        cmd_adf2hdf(rest[0], rest[1], d, p)
    elif cmd == "iso2hdf":
        p = opt("--part", "DH0")
        d = opt("--dest", "/")
        cmd_iso2hdf(rest[0], rest[1], d, p)
    elif cmd == "dir2hdf":
        p = opt("--part", "DH0")
        d = opt("--dest", "/")
        cmd_dir2hdf(rest[0], rest[1], d, p)
    elif cmd == "write":
        p = opt("--part", "DH0")
        img = rest[0]
        run("xdftool", img, *part_args(img, p), "write", *rest[1:])
    elif cmd == "read":
        p = opt("--part", "DH0")
        img = rest[0]
        run("xdftool", img, *part_args(img, p), "read", *rest[1:])
    elif cmd == "xdf":
        run("xdftool", *rest)
    elif cmd == "rdb":
        run("rdbtool", *rest)
    else:
        sys.exit(f"unknown command: {cmd}\n{__doc__}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""List upstream AROS changes that a pin bump does NOT deliver.

Code we compile from external/aros arrives with a bump. Code we hold a copy
of -- a fork of a driver, an override of an m68k-all file, a file adapted
from the aarch64 port -- does not: upstream can fix it and the fix never
reaches this port, silently. This finds those copies and reports what
upstream changed in them over a range.

A file of ours is paired with an upstream file when the two share a name
and their contents at the base commit are still recognisably the same
(difflib ratio >= --min-ratio). No list is maintained by hand, so a new fork
is picked up the day it is made.

    scripts/upstream-forks.py --to origin/master          # from the pin
    scripts/upstream-forks.py --base 78f2959e68 --to a399fe2fc7
    scripts/upstream-forks.py --to origin/master --pairs  # just the pairing

Paths under aros/ that setup.sh injects are skipped as upstream candidates:
external/aros/arch/m68k-emu68 is our own tree seen through a link.
"""

import argparse
import collections
import difflib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AROS = os.path.join(ROOT, "external", "aros")
OURS = os.path.join(ROOT, "aros")

# Names too generic to pair on.
GENERIC = {"mmakefile.src", "README.md", "README", "Makefile", ".gitignore",
           "makefile", "mmakefile"}
SOURCE_EXT = (".c", ".h", ".S", ".s", ".conf", ".cpp", ".py")


def git(*args):
    return subprocess.run(["git", "-C", AROS, *args], check=True,
                          capture_output=True, text=True).stdout


def upstream_files(rev):
    by_name = collections.defaultdict(list)
    for path in git("ls-tree", "-r", "--name-only", rev).splitlines():
        if path.startswith(("arch/m68k-emu68/", "contrib/")):
            continue
        by_name[os.path.basename(path)].append(path)
    return by_name


def our_files():
    for top, _dirs, files in os.walk(OURS):
        for name in files:
            if name in GENERIC or not name.endswith(SOURCE_EXT):
                continue
            yield os.path.join(top, name)


def read_text(data):
    return data.decode("utf-8", "replace").splitlines()


# Where an arch/m68k-emu68/<module>/<file> replaces upstream's by name. The
# build takes ours however little it still resembles the original, so these
# pair regardless of the ratio.
OVERRIDDEN = ("arch/m68k-all/{}", "arch/m68k-amiga/{}", "rom/{}")


def overridden(ours, names):
    rel = os.path.relpath(ours, os.path.join(OURS, "arch", "m68k-emu68"))
    if rel.startswith(".."):
        return []
    return [p.format(rel) for p in OVERRIDDEN if p.format(rel) in names]


def pairs(revs, min_ratio):
    """Pair at every rev given, so an origin that only appears inside the
    range -- a driver upstream adopted from us -- is found too."""
    by_name = collections.defaultdict(set)
    present = {}
    for rev in revs:
        for name, paths in upstream_files(rev).items():
            for path in paths:
                by_name[name].add(path)
                present.setdefault(path, rev)
    for ours in sorted(our_files()):
        cands = sorted(by_name.get(os.path.basename(ours), []))
        if not cands or len(cands) > 40:
            continue
        with open(ours, "rb") as f:
            mine = read_text(f.read())
        forced = set(overridden(ours, present))
        best = []
        for path in cands:
            theirs = read_text(subprocess.run(
                ["git", "-C", AROS, "show", f"{present[path]}:{path}"],
                check=True, capture_output=True).stdout)
            sm = difflib.SequenceMatcher(None, mine, theirs, autojunk=False)
            if path not in forced and (sm.real_quick_ratio() < min_ratio
                                       or sm.quick_ratio() < min_ratio):
                continue
            ratio = sm.ratio()
            if ratio >= min_ratio or path in forced:
                best.append((ratio, path, path in forced))
        if best:
            # The closest file is the origin; one barely further is a second
            # origin worth reading too. Anything else is a family resemblance,
            # except an override, which is kept whatever its ratio.
            best.sort(reverse=True)
            top = max(b[0] for b in best if not b[2]) if any(not b[2] for b in best) else 0
            best = [b for b in best if b[2] or b[0] >= top - 0.1]
            yield os.path.relpath(ours, ROOT), best


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="HEAD",
                    help="upstream commit our copies are compared at (default: the pin)")
    ap.add_argument("--to", required=True, help="end of the upstream range")
    ap.add_argument("--min-ratio", type=float, default=0.35)
    ap.add_argument("--pairs", action="store_true",
                    help="print the pairing only, no commit lookup")
    a = ap.parse_args()

    base = git("rev-parse", "--short=10", a.base).strip()
    to = git("rev-parse", "--short=10", a.to).strip()
    changed = 0
    for ours, best in pairs([base, to], a.min_ratio):
        lines = []
        for ratio, path, forced in best:
            log = "" if a.pairs else git(
                "log", "--first-parent", "--reverse",
                "--format=      %h %cd %s", "--date=short",
                f"{base}..{to}", "--", path)
            if a.pairs or log:
                kind = "override" if forced else f"{ratio:.2f}"
                lines.append(f"  <= {path}  ({kind})")
                if log:
                    lines.append(log.rstrip())
        if lines:
            changed += 1
            print(ours)
            print("\n".join(lines))
    if not a.pairs:
        print(f"\n{changed} file(s) of ours have upstream changes in "
              f"{base}..{to} that a pin bump does not deliver.", file=sys.stderr)


if __name__ == "__main__":
    main()

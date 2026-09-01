---
id: ISSUE-0087
title: "Retiring the Bluetooth port once upstream carries it"
status: open
priority: medium
type: chore
owner: unassigned
created_at: 2026-09-01
updated_at: 2026-09-01
tags:
  - bluetooth
  - upstream
  - submodules
blockers:
  - the AROS pull request has to be merged first
related_files:
  - aros/arch/arm-native/soc/broadcom/2708/bluetooth
  - patches/aros/0095-gpio-preserve-the-other-pins-when-selecting-a-function.patch
  - patches/aros/0099-2708-build-the-onboard-bluetooth-transport.patch
  - scripts/setup.sh
---

# What this is

The arm-native Bluetooth transport was written here and sent upstream as a
pull request against `aros-development-team/AROS` (branch
`bluetooth-pi-transport` on the `JJDSNT/AROS` fork, six commits over
`f04bb2c13e`). When it merges and the `external/aros` pin moves past it, this
repository is carrying the same code twice and has to let go of its copy.

That is two separate retirements, and only one of them announces itself.

# The silent half: the injected directory

`aros/arch/arm-native/soc/broadcom/2708/bluetooth/` reaches the build through
`setup.sh`'s symlink injection. `inject_dirs()` walks our tree and links at
**the first level that does not already exist upstream**. Today that is
`bluetooth`, because upstream has no such directory.

The day upstream has one, the walk descends *into* it instead: `brcmbt/` and
`h4bthci/` exist upstream too, so it descends again, finds no further
subdirectory of ours, and returns **nothing**. Simulated against the real
function:

```text
upstream without bluetooth/ ->  arch/arm-native/soc/broadcom/2708/bluetooth
upstream with    bluetooth/ ->  (no injection point at all)
```

So the link is silently not made, the build quietly uses upstream's copy, and
our files stay on disk looking exactly as alive as they do now. Editing them
would change nothing and say nothing. **`setup.sh --verify` will not report
this**: it verifies the patch series, not the injections.

Delete `aros/arch/arm-native/` outright at that point. There is nothing in it
that is ours any more, and leaving it is leaving a trap for whoever next
tries to change the driver.

# The loud half: the five patches

`0095`-`0099` edit files upstream will then already carry, so they stop
applying and `setup.sh --verify` reports `broken:<patch>`. That part is
visible. The trap is in the removal, and CLAUDE.md states it: **un-apply a
patch before deleting it.** `setup.sh` learns which files are patched from
the patches that are *there*, so deleting one first means it never clears
that file's `skip-worktree` bit -- `git reset --hard` then skips the file and
the deleted patch stays applied forever, with `--reset` unable to fix it
because fixing it is the thing it no longer knows how to do.

The order is therefore:

```bash
./scripts/setup.sh --reset          # with the patches still present
rm patches/aros/009[5-9]-*.patch
git -C external/aros ls-files -v | grep '^S'   # nothing of ours should remain
./scripts/setup.sh --verify
```

# What does not retire

The m68k copy in `aros/arch/m68k-emu68/soc/bluetooth/` stays. It is a
different driver for a machine upstream does not target: no `gpio.resource`,
its own `hardware/bcm2708.h`, and Emu68's peripheral window rather than a
fixed one. The two are siblings, not one file in two places, and the port
commit says which parts diverged.

# Check before doing any of this

Compare the merged upstream directory against ours before deleting, in case
review changed something on the way in. A `diff -r` between
`external/aros/arch/arm-native/soc/broadcom/2708/bluetooth` and our copy
answers it in one command, and anything it reports is a change we did not
make and may want to understand.

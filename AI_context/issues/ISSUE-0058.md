---
id: ISSUE-0058
title: "The AROS pin moved to upstream master, and what that lands on needs watching"
status: open
priority: medium
type: infra
owner: unassigned
created_at: 2026-08-25
updated_at: 2026-08-25
tags:
  - upstream
  - submodules
  - build
  - raspberry-pi-3
blockers:
  - "boot on real hardware at the new pin — the card has not been tested yet"
related_files:
  - patches/aros
  - aros/arch/m68k-emu68/include/hardware/bcm2708.h
  - aros/arch/m68k-emu68/soc/usb/usb2otg/usb2otg_device.c
  - docs/aros.md
  - AI_context/issues/ISSUE-0055.md
  - AI_context/issues/ISSUE-0046.md
  - AI_context/issues/ISSUE-0045.md
---

# Summary

2026-08-25, commit `7eb69aa`: `external/aros` moved from `8ffa393` to
`1986301` — 51 upstream commits, the newest three days old.

The bump itself was cheap. What it lands on is not, and this issue exists so
the next person does not have to re-derive it: four of those commits touch code
that open investigations are actively reasoning about, one of them changed a
file we compile in place, and two upstream fixes do **not** reach us because
the drivers they fix are forked here.

Verified at the new pin: the series applies clean, `setup.sh --verify` reports
`applied (53 patches)`, and a lean build links the kernel ELF.
**Not verified: anything that runs.** No boot, in QEMU or on hardware.

# How "is any patch redundant now?" was answered

Worth recording as a method, because guessing it is cheap and wrong.

The 53 patches were replayed as 53 commits onto the old pin, then
`git rebase --empty=drop --onto origin/master`. All 53 survived: no conflicts,
no commit went empty. A patch upstream has absorbed would come out empty and be
dropped; one upstream reshaped around would conflict. Neither happened, so the
series stays at 53 and nothing was removed.

That is the third bump in a row with the same answer. The reason is not that
the check is weak — it is that what remains in `patches/aros/` is either target
enablement for an architecture upstream does not have, or instruments, and
neither is something upstream will ever land on its own.

Do not check this by applying each patch to a pristine tree independently. They
are a series; a later patch can edit a region an earlier one reshaped, and that
test reports false redundancies (it did, on 2026-08-22).

# Points of attention

## 1. Our `bcm2708.h` shadows arm-native's, and drifts silently

**This already broke the build, and will again.**

`aros/arch/m68k-emu68/include/hardware/bcm2708.h` is arm-native's header
adopted whole, plus our own additions at the end. We compile
`arch/arm-native/soc/broadcom/2708/dma/dma_init.c` in place against *our* copy,
which comes first on the include path.

Upstream added the BCM2711 DMA4 register block on the 23rd and used it in
`dma_init.c`. Our copy did not have it, so the bump failed to compile with six
undeclared macros — in code that `BCM2708_DMA_IS_DMA4()` keeps away from a Pi 3
at runtime but that still has to compile. Re-synced in `7eb69aa`.

**On every future bump, diff the two files.** The only differences that belong
there are the include-guard name and the "Bellatrix additions" block at the end:

```bash
diff -u external/aros/arch/arm-native/soc/broadcom/2708/include/hardware/bcm2708.h \
        aros/arch/m68k-emu68/include/hardware/bcm2708.h
```

The header's own comment already says why the whole map lives here rather than
the subset that happens to be needed: carrying a subset is a queue of future
failures. This was one of them arriving.

## 2. `dma.resource` changed under the audio investigation

[ISSUE-0055](ISSUE-0055.md) is being debugged *through* this resource, and it
moved:

- the channel reset is now factored into a `dma_channel_quiesce()` that
  `DMAFreeChannel()` runs too, where it previously did its own reset inline;
- on a timeout, the report prints the **actual elapsed time and the caller's
  budget**, where it used to print the budget alone;
- DMA4-specific error handling was added, all of it behind
  `BCM2708_DMA_IS_DMA4()` and therefore dead on a Pi 3.

Legacy-engine behaviour should be unchanged. "Should be" is the point: audio
measurements taken before 2026-08-25 came from the old code, and a timeout line
that now carries two numbers instead of one will not compare with a log from
last week.

Patch `0025` still applies: the two `dmb sy` sites it replaces survived the
rewrite untouched.

## 3. `posixc` now initialises `fd_sem` for every base

`da83a4b7e` — the fd-table lock was only initialised on the owner path of
`__init_fd()`, so a pthread worker that resolved a descriptor through its own
base before the owner was wired obtained an uninitialised semaphore.
Intermittent, under threading load.

That is the same corner patch `0057` (m68k test-and-set) is in, and the same
corner [ISSUE-0045](ISSUE-0045.md) has been fighting through the GL demos. If a
"ObtainSemaphore called on a not initialized semaphore" alert stops appearing,
this is why — and it means the failure was never ours.

## 4. `bluetooth.library` grew a great deal

Three commits, ~2600 lines: LE pairing and HID over GATT, discoverable private
addresses, and device registrations plus bond keys persisted to `ENVARC:`. They
land on the stack that patch `0056` registers our controller with, which is
[ISSUE-0046](ISSUE-0046.md)'s subject.

Nothing here was written against this port, and the prefs application is Zune —
so it is also new Zune surface reaching a machine whose desktop performance is
the reason for the feature freeze. Re-verify the BT path on the new card before
reading anything into a change in behaviour.

## 5. Two upstream fixes do not reach us, because we forked those drivers

- **usb2otg**: upstream now accepts the `brcm,bcm2835-usb` device-tree
  compatible string and falls back to `brcm,bcm2708-usb`. Our fork at
  `aros/arch/m68k-emu68/soc/usb/usb2otg/usb2otg_device.c:24` still tests the
  old string alone. One line, worth carrying by hand — see
  [ISSUE-0057](ISSUE-0057.md), which has usb2otg among its suspects.
- **vcgfx**: thirteen more commits, for the second bump running. That was the
  stated cost of forking the driver on 2026-08-22 and it keeps coming due.

Neither is a defect today. Both are drift, and drift is only cheap while
somebody is counting it.

## 6. What arrives that we cannot use yet

A complete Mesa **v3d** driver for the BCM2711 — eight commits, an address
space, a submission pipeline, hang recovery and presentation through the HVS
overlay. Not for the Pi 3's VC4, and not built here.

It is why `galliumglue.py` learned to take several `--consumer` archives, which
is the other file both sides touch. Patch `0036` (m68k trampolines) is
unaffected: it adds an arm to `--arch`, not to `--consumer`.

## 7. The card carries modules, and they come from the distribution tree

A lean build proves the pin compiles and nothing else. Every library, Zune
class and `C:` command on the card comes from
`out/build/aros/bin/emu68-m68k/AROS`, so a card built from a stale tree tests
the old pin's modules against the new pin's kernel — silently. Rebuild with
`build-aros.sh full` before `make-sdcard.sh`.

# Series hygiene, noticed while doing this

Neither is urgent; both are cheap to fix and get more expensive to explain
later.

- **Two patches are numbered `0041`**: `0041-exec-say-when-a-library-init...`
  and `0041-mungwall-when-the-header-is-gone...`. They touch different files so
  the order between them does not matter, and `setup.sh` applies both — but the
  numbering is the series' only ordering contract, and `0042` and `0044` are
  free.
- **`docs/aros.md` is stale by a lot.** It states the pin as `8570536` and
  describes "eleven patches, numbered `0001`-`0011` with no gaps". That has been
  wrong since 2026-08-13, across three pin bumps, and predates this one. The
  three-kinds structure it introduces is still the right way to describe the
  series; the contents are not.

# Next steps

1. Boot the new card on the Pi 3 — the blocker on this issue.
2. Carry the usb2otg compatible-string fix into our fork (point 5).
3. Rewrite `docs/aros.md` against the 53-patch series, or say in it that it
   describes a state that is gone.

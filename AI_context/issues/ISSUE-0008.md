---
id: ISSUE-0008
title: "Uncommitted work sits outside both patch series and cannot be rebuilt"
status: doing
priority: critical
type: infra
owner: agent
created_at: 2026-08-05
updated_at: 2026-08-05
tags:
  - infra
  - patches
  - submodules
  - reproducibility
blockers:
related_files:
  - patches/emu68/0001-emulate-amiga-interrupt-registers.patch
  - patches/emu68/0002-offer-zorro3-rom-board.patch
  - external/aros/arch/m68k-all/exec/signal_fast.S
  - external/aros/arch/m68k-all/exec/wait.S
  - external/aros/arch/m68k-all/exec/switch.S
  - external/aros/arch/m68k-all/kernel/kernel_cpu.c
  - scripts/setup.sh
  - scripts/build-aros.sh
---

# Summary

`./scripts/setup.sh --verify` fails on both submodules, and
`./scripts/build-aros.sh` refuses to run because of it. Four files inside
`external/aros` carry edits that exist in no patch, and two Emu68 patch files
were hand-edited into a state `git apply` calls corrupt. The tree currently
builds only by invoking `make` directly in `out/build/aros`, which means the
thing being measured cannot be reconstructed from the repository.

# Problem

Two independent defects, both of which make the working state unrepresentable.

**Local edits inside `external/aros`.** First measured wrongly, and worth
recording why: the comparison was made only over the files the series touches,
which cannot find a file the series never knew about. Repeating it over the
whole tree on 2026-08-05 — pinned commit plus series in a scratch worktree,
`diff -rq` against `external/aros` — found **nineteen** drifted files, not four:

| area | files |
|---|---|
| scheduler | `arch/m68k-all/exec/{signal_fast,switch,wait}.S`, `arch/m68k-all/kernel/kernel_cpu.c` |
| FAT | `rom/filesys/fat/{date,direntry,ops}.c` |
| MUI | `workbench/libs/muimaster/{font,mui_redraw}.c`, `.../classes/{area,group,window}.c` |
| Wanderer | `workbench/system/Wanderer/{main,iconwindow,iconwindow_volumelist}.c`, `.../Classes/iconvolumelist.c` |
| tracing | `workbench/c/Shell/Shell.c`, `workbench/c/iprefs/{main,fontprefs}.c` |

The whole set is captured on branch `codex-2026-08-05`, as
`AI_context/codex-2026-08-05/aros-submodule-drift.patch`, with a README
recording what is known about each area. The FAT three are **required** — see
ISSUE-0007 — and are the first candidates to become a patch.

The four originally found, for the record:

| file | edit, present in no patch |
|---|---|
| `arch/m68k-all/exec/signal_fast.S` | `#ifndef __EMU68__` guards removed, so the chipset writes to `0xdff09a`/`0xdff09c` are compiled in |
| `arch/m68k-all/exec/wait.S` | same |
| `arch/m68k-all/exec/switch.S` | `movem.l` rewritten so the predecrement register is not in the register list |
| `arch/m68k-all/kernel/kernel_cpu.c` | task stack-frame bounds check plus `Alert(AT_DeadEnd \| AN_StackProbe)` |

`setup.sh` marks every series file `skip-worktree`, so none of this appears in
`git status` at either level — by design, because an applied series is the
normal working state. The cost is that a genuine local edit is equally
invisible. `--reset` clears the bit and then hard-resets, so running it would
have destroyed all four without a word.

The `kernel_cpu.c` check uses `M68K_TASK_FRAME_SIZE 66`. ISSUE-0007 established
that Emu68 produces a 68-byte frame, so the check under-measures by two bytes
and lets a small overrun pass.

**Corrupt Emu68 patch files.** `patches/emu68/0001` and `0002` were edited by
hand rather than regenerated: hunk bodies grew but the `@@` line counts were
adjusted separately and no longer agree. `git apply --check` reports
`corrupt patch at line 155` and `line 141`. `git apply --recount` recomputes the
counts and both then apply cleanly, which confirms the corruption is confined to
the headers. Against the pinned commit the recounted series reproduces the
working tree exactly, except that patch 0002 adds a second
`extern int zorro_disable;` the working tree does not have — harmless in C, but
it is drift.

# Goal

`./scripts/setup.sh --verify` reports `applied` for both submodules, and
`./scripts/build-aros.sh` builds without being bypassed.

# What was done

- Compared the working tree against a freshly applied series, file by file, for
  both submodules, and identified exactly which files drifted and how.
- Established that `patches/aros/0002` reproduces `/home/jaime/AROS`
  (`feature/m68k-emu68-baremetal`) byte for byte on every file it touches. The
  scheduler-safety code from that branch — `preserveall.S` and
  `preserveall_install.c` — is fully carried by the series. Nothing is missing;
  the divergence is entirely in the other direction.
- Widened that comparison on 2026-08-05 from "files the series touches" to the
  whole branch, which is the only form of the question that can find something
  the series never knew about. It found one: `d3baf6ed82`, the system-timer
  compare race, which had never existed in this repository. Imported; see
  ISSUE-0007. Nothing else was missing — the branch has no `arch/m68k-emu68`
  file this tree lacks, and both of its `rom/dos/newcliproc.c` fixes are in
  `patches/aros/0004`.
- Snapshotted everything uncommitted to
  `/home/jaime/bellatrix-codex-snapshot-2026-08-05` before touching anything:
  the tracked diff, a tarball of the untracked files, and copies of the four
  submodule files that no diff would have captured.

# What is left

- Regenerate `patches/emu68/0001` and `0002` from the working tree instead of
  repairing hunk headers, and drop the duplicate `zorro_disable` declaration.
- Decide where the four AROS edits belong. `switch.S` and `kernel_cpu.c` are
  target-neutral hardening and could extend `0002`; removing the
  `#ifndef __EMU68__` guards is a behavioural decision about whether this target
  writes to chipset addresses at all, and needs its own patch and rationale.
- Reconcile the frame size: 66 in `kernel_cpu.c` against 68 in
  `arch/m68k-emu68/kernel/context.c`.
- Restore `patches/aros/0006`, or record that dropping the DOS shell tracing was
  deliberate — it is currently deleted in the working tree with no note.

# Decisions taken

Hand-editing a generated patch is not an accepted way to change a series.
Regenerate from the submodule working tree, so the patch and the tree cannot
disagree.

# Acceptance criteria

- [ ] `./scripts/setup.sh --verify` exits 0
- [ ] `./scripts/build-aros.sh` runs without bypassing setup
- [ ] No file in either submodule differs from what its series produces
- [ ] The snapshot directory is redundant and can be deleted

# Notes

`setup.sh --verify` is the only reliable way to ask this question; `git status`
answers it wrongly by construction. Worth reading `patches/README.md` before
changing any of this.

# Execution log

- 2026-08-05 — found both defects while investigating ISSUE-0007; snapshotted
  the uncommitted state; confirmed `--recount` isolates the patch corruption to
  hunk headers.

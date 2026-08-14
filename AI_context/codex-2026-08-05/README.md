# Parked working state, 2026-08-05

This branch is a **capture, not a proposal**. It exists so that nothing is lost
while `main` is rebuilt deliberately, and so the work below can be reviewed
piece by piece later. Nothing here has been reviewed or accepted.

## Why it exists

The working tree had accumulated changes that were not representable by the
repository: files edited directly inside `external/aros` belonging to no patch,
and two Emu68 patch files hand-edited into a state `git apply` rejects. See
ISSUE-0008. Reverting to rebuild a baseline would have destroyed all of it
silently, because `skip-worktree` hides submodule edits from `git status`.

## What is here

**`aros-submodule-drift.patch`** — the complete difference between
`external/aros` as it stood on 2026-08-05 and the reference tree (pinned commit
`d0370bd757` with `patches/aros/` applied). Nineteen files, none of which is
covered by any patch series:

| area | files |
|---|---|
| scheduler | `arch/m68k-all/exec/{signal_fast,switch,wait}.S`, `arch/m68k-all/kernel/kernel_cpu.c` |
| FAT | `rom/filesys/fat/{date,direntry,ops}.c` |
| MUI | `workbench/libs/muimaster/{font,mui_redraw}.c`, `.../classes/{area,group,window}.c` |
| Wanderer | `workbench/system/Wanderer/{main,iconwindow,iconwindow_volumelist}.c`, `.../Classes/iconvolumelist.c` |
| tracing | `workbench/c/Shell/Shell.c`, `workbench/c/iprefs/{main,fontprefs}.c` |

The rest of the branch is the parent repository's working tree as it stood: the
`arch/m68k-emu68/exec/` Exec backend, `kernel/context.c`, the modified Emu68
patch files, and the investigation notes written on top.

## Authorship, so review can be targeted

Not all of this came from the same place. Roughly:

- **Codex, unreviewed** — `arch/m68k-emu68/exec/` (the 68-byte Exec backend),
  `kernel/context.c`, the four `arch/m68k-all` scheduler edits, the hand-edits
  to `patches/emu68/0001` and `0002`, the deletion of
  `patches/aros/0006`, and the diagnostic blocks in `interrupt_controller.c`
  and `platform.c`.
- **Earlier sessions** — the FAT endian fix, the MUI and Wanderer diagnostics.
- **2026-08-05 investigation** — the `intc_dispatch()` enable mask, the
  `systimer_arm()` import from `/home/jaime/AROS` `d3baf6ed82`, the restored
  `patches/aros/0006`, the IPrefs tracing, and the issue documents.

## What is known about it

- The FAT endian fix is **required**. Without it a boot writes byte-swapped
  directory entries for the font indexes it generates, and every subsequent boot
  hangs in `OpenDiskFont`. This is the regression that made the desktop
  unreachable; see ISSUE-0007.
- The 68-byte Exec backend is internally consistent: `exec/dispatch.S` lays the
  frame out as PC(0), SR(4), format(6), registers(8..67), and `context.c` reads
  the format word at offset 6 to match. Whether the format word is needed at all
  is **not** established — the port reached the desktop before this backend
  existed, using the 66-byte `m68k-all` path.
- `m68k_DispatchFrame` and `m68k_SwitchTail`, the 66-byte pair, are referenced by
  nothing in 1306 linked objects. They link only because `kernel_cpu.o` also
  defines `cpu_Exception`.
- Deleting `patches/aros/0006` removed the only tracing that shows where the
  boot is. It cost a day of investigation looking at a boot that appeared to
  stop after SD initialisation and in fact ran a thousand lines further.

## How to use it

Take one thing at a time, on top of a `main` that builds, and measure it.
`AI_context/consolidated/history/ISSUE-0007.md` records the measurement discipline this
problem has already cost twice: serial runs, idle machine, three per
configuration, `screendump` and dominant colour rather than a glance.

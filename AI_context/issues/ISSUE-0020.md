---
id: ISSUE-0020
title: "The full build compiles freetype 2.14.3 against 2.14.1's installed headers"
status: doing
priority: high
type: bug
owner: agent
created_at: 2026-08-14
updated_at: 2026-08-14
tags:
  - build
  - ports
  - mtime
  - freetype
blockers:
related_files:
  - external/aros/workbench/libs/freetype2/mmakefile.src
  - external/aros/config/make.tmpl
  - scripts/build-aros.sh
  - AI_context/issues/ISSUE-0019.md
---

# Summary

`./scripts/build-aros.sh full` fails in `workbench/libs/freetype2`:

```
.../Ports/freetype2/freetype-2.14.3/src/sfnt/pngshim.c:423:15:
    error: too few arguments to function 'ft_glyphslot_alloc_bitmap'
       error = ft_glyphslot_alloc_bitmap( slot );
.../AROS/Developer/include/freetype/internal/ftobjs.h:728: note: declared here
   ft_glyphslot_alloc_bitmap( FT_GlyphSlot  slot,
```

Three sites, all in freetype's own sources: `sfnt/pngshim.c:423`,
`sfnt/ttcolr.c:1762`, `sfnt/ttsbit.c:604`.

**It is not a source defect, in ours or upstream's.** The freetype 2.14.3
sources are being compiled against **freetype 2.14.1's headers**, left installed
in the sysroot by an earlier build, and the two disagree about this function's
signature.

Blocks ISSUE-0019 from step 3 on, and blocks any SD card built from a
distribution matching the current kernel.

# Root cause: mtime, not content

| | Signature | Date |
|---|---|---|
| `AROS/Developer/include/freetype/internal/ftobjs.h:728` (installed) | `(FT_GlyphSlot slot, FT_ULong size)` | **2026-08-03 20:12** |
| `Ports/freetype2/freetype-2.14.3/include/freetype/internal/ftobjs.h:719` (the port's own) | `(FT_GlyphSlot slot)` | **2026-03-11 13:35** |

The installed copy was put there on 3 August by a full build at the **old pin**,
when `FT2VERS` was `2.14.1`. Upstream then bumped it to `2.14.3` in
`2e41c4b6d6`, which we took with the pin bump.

The headers are installed by `%copy_includes`, which expands to a plain make
prerequisite (`config/make.tmpl:3527`):

```make
$(BD_INCL_FILES) : $(GENINCDIR)/%(path)/% : $(BD_INC_PATH)%
	$(Q)$(CP) $< $@
```

Make copies only when the source is **newer** than the target. A release tarball
preserves the upstream author's mtimes, so freetype 2.14.3's headers are dated
March 2026 — **five months older** than the 2.14.1 headers installed in August.
Make therefore considers the stale destination up to date and never overwrites
it. The newer version loses to the older one on a date neither of them chose.

## Why this is worth naming rather than just fixing

It is the **third** instance in this project of the same trap, and the first two
are already written down:

- `setup.sh --reset` rewrites every mtime in a submodule while the content stays
  pinned, so `configure` looks newer than `config.status` and the build refuses
  (`build-aros.sh` now guards this).
- The same reset makes every source newer than its object, so a post-reset build
  is never incremental.

All three are the same shape: **mtime is being used as a proxy for content, and
in this project mtime is routinely not what the content did.** A version bump
that goes backwards in time is simply the most confusing member of the family,
because the tree is more up to date than it was and the build gets *less*
correct.

It is also general, not ours: any AROS port whose version bump ships a tarball
with older mtimes than the previously installed headers hits this on an existing
build tree. A clean tree never sees it, which is why upstream would not have.

# The quick fix, applied 2026-08-14

Delete the installed headers so there is no destination for make to compare
against:

```sh
rm -rf out/build/aros/bin/emu68-m68k/AROS/Developer/include/freetype
./scripts/build-aros.sh full
```

Safe: everything under `out/` is a build artifact and regenerable. Reversible:
it deletes nothing that is not reproduced by the next build. And targeted — it
does not force the full rebuild that a `--reset` or a `clean` would.

This is a workaround, not a fix. It leaves the mechanism intact and the next
port to bump a version the same way will do it again, silently, in whatever way
that port's headers happen to disagree.

# Durable options, in the order I would consider them

1. **Extend the guard already in `build-aros.sh`.** It exists for exactly this
   family. Before building, drop any installed port headers whose source tree
   version differs from what is installed — or simply remove
   `$(AROS_INCLUDES)/freetype` when `FT2VERS` differs from a recorded marker.
   Cheap, local to us, no upstream negotiation.

2. **Make the copy rule order-independent upstream.** `%copy_includes` could
   depend on a stamp file keyed to the archive name rather than on the header
   mtimes, which is what `%fetch` already does with `.freetype-2.14.3-fetched`.
   The marker for the right version is *already sitting in `bin/Sources`* — the
   copy step just does not consult it. This is the correct fix and the one worth
   offering upstream.

3. **Touch the extracted port sources after fetch.** Blunt, would work, and
   would defeat incremental builds inside the port. Mentioned to be rejected.

Option 2 is the real answer; option 1 is what unblocks us without waiting for
anybody.

# Acceptance criteria

- [x] The failure reproduced at the current pin, and attributed with evidence
      rather than guessed.
- [ ] `./scripts/build-aros.sh full` completes.
- [ ] A card built from the resulting distribution boots to icons, so that the
      kernel and the modules on the card come from the same tree.
- [ ] A durable guard, so a future version bump does not reintroduce it.
- [ ] Offered upstream, if option 2 holds up.

# Notes

Worth stating plainly because it nearly went the other way: the first reading of
this — recorded in ISSUE-0019 before it was checked — was "an
AROS-against-freetype-2.14.3 mismatch", i.e. a source problem somebody else
should fix. The build log's `note: declared here` line, naming a path under
`AROS/Developer/include` rather than under `Ports/`, is what turned it from a
guess into a diagnosis. A two-minute look at *which* header the compiler chose
was worth more than any reasoning about which version was right.

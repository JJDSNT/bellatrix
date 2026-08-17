---
id: ISSUE-0025
title: "A port version bump can install headers that lose to older ones, and nothing notices"
status: backlog
priority: medium
type: bug
owner: unassigned
created_at: 2026-08-16
updated_at: 2026-08-16
tags:
  - build
  - ports
  - mtime
  - upstream
blockers:
related_files:
  - external/aros/config/make.tmpl
  - external/aros/workbench/libs/freetype2/mmakefile.src
  - scripts/build-aros.sh
  - AI_context/consolidated/history/ISSUE-0020.md
---

# Summary

`%copy_includes` installs a port's headers into the sysroot with a plain make
prerequisite (`config/make.tmpl:3533`):

```make
$(BD_INCL_FILES2) : %(includedir)/%(path)/% : $(BD_INC_PATH)%
	$(Q)$(CP) $< $@
```

Make copies only when the source is newer than the target. Release tarballs
preserve the upstream author's mtimes, so a **newer** port version can carry
**older** headers than the ones already installed by the previous version — and
make then declines to overwrite them. The build compiles the new sources against
the old headers and fails somewhere unrelated, or worse, does not.

`ISSUE-0020` is one instance, diagnosed in full: freetype 2.14.3's March-dated
headers lost to freetype 2.14.1's August-dated installed copies, and the failure
surfaced as three arity errors inside freetype's own sources. That instance is
fixed and closed. **The mechanism is not**, in our tree or upstream.

# Problem

The mechanism is general and belongs to AROS rather than to this port: any
version bump whose tarball is older than the last install hits it, on any
existing build tree. A clean tree never sees it, which is why it does not show
up in CI or for a first-time builder — and why it will keep being diagnosed from
scratch each time.

It is also the same shape as two traps already documented in `build-aros.sh`:
`setup.sh --reset` rewriting submodule mtimes so `configure` looks newer than
`config.status`, and the same reset defeating incremental builds. **In this
project mtime is routinely not what the content did**, and the build system
treats it as a proxy for content in several places.

# Goal

A version bump on an existing build tree either installs the right headers or
fails loudly, without anyone having to know this trap exists.

# What is left

1. **A local guard in `build-aros.sh`.** Cheapest signal available: two
   extracted trees under one `bin/<target>/Ports/<name>/` means this tree has
   crossed a version bump for that port. Our tree has exactly that today —
   `freetype-2.14.1` and `freetype-2.14.3` side by side — with nothing reading
   it. Detect it, and either drop the installed headers for that port or refuse
   with a message naming the port.

   The mapping from a port to its sysroot include subdirectory is the awkward
   part and the reason this is not a two-line change: it is per-port and not
   declared anywhere the script can read. Worth checking whether the port's own
   mmakefile can be made to say it, rather than keeping a table here.

   **A guard that deletes installed headers can break a working tree**, so this
   wants to start by refusing and reporting, not by deleting.

2. **The real fix, upstream.** Make the copy depend on a stamp keyed to the
   archive rather than on header mtimes. `%fetch` already writes exactly such a
   marker — `.freetype-2.14.3-fetched` — so the right version is already
   recorded in `bin/Sources`; the copy step simply does not consult it. This
   removes the mtime proxy instead of guarding against it.

3. **Offer it.** A patch plus a short statement of the failure, written so it
   does not require knowing anything about this port. `ISSUE-0024` took that
   route and is the worked example; it lives on the `kickstart-base-package`
   branch rather than here.

# Decisions taken

**Split out of `ISSUE-0020` rather than holding it open.** The bug it described
is fixed and cannot currently be reproduced; what was left is a build-system
change with its own risk. Keeping a fixed bug in `doing` for it made the issue
list less informative.

**Refuse before deleting.** The failure this guards against is silent, but the
guard's own failure mode — removing headers a good tree needs — is worse, and
the diagnostic value is in the naming, not in the cleanup.

# Acceptance criteria

- [ ] A version bump on an existing build tree is detected before it can compile
      new sources against old headers
- [ ] The report names the port and what to remove, so the fix does not need
      this issue to be read
- [ ] `./scripts/build-aros.sh full` still completes on a tree that has not
      crossed a bump
- [ ] The upstream fix drafted against `%copy_includes`, and offered

# Notes

**The diagnosis is already written and worth not redoing.**
`AI_context/consolidated/history/ISSUE-0020.md` has the evidence, the table of
the two signatures and their dates, and the reasoning that ruled out a source
defect. The one lesson recorded there that generalises: the build log's
`note: declared here` line, naming a path under `AROS/Developer/include` rather
than under `Ports/`, is what turned a guess into a diagnosis. A look at *which*
header the compiler chose was worth more than any argument about which version
was correct.

# Execution log

- 2026-08-16 — Opened, splitting the unfinished half of `ISSUE-0020` out so that
  issue could close against its own bug. Confirmed the upstream mechanism is
  unchanged at the current pin (`config/make.tmpl:3533` is still a bare mtime
  comparison) and that our tree still carries the two extracted freetype trees
  that the guard would key on.

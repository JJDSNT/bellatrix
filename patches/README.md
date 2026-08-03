# patches

Changes applied on top of the submodules in `external/`, which are pinned to
their upstreams and never edited in place.

One directory per submodule, mirroring `external/`. Each series is numbered
from `0001` independently, so adding a patch to one submodule never renumbers
another's.

```
patches/<submodule>/NNNN-<subject>.patch   ↔   external/<submodule>/
```

| Series | Submodule | Upstream | Documented in |
|---|---|---|---|
| [`aros/`](aros/) | `external/aros` | aros-development-team/AROS | [`docs/aros.md`](../docs/aros.md) |
| [`emu68/`](emu68/) | `external/emu68` | michalsc/Emu68 | [`docs/emu68.md`](../docs/emu68.md) |

A submodule may also receive whole directories of ours, symlinked in rather
than carried as patches — a patch is for changing someone else's code, not for
shipping our own. A top-level directory named after a submodule mirrors its
tree: `aros/arch/m68k-emu68/` is linked to `external/aros/arch/m68k-emu68`. The
injection point is the first level that does not already exist upstream, so
nothing has to be declared.

## Applying

```bash
./scripts/setup.sh            # apply every series (idempotent)
./scripts/setup.sh --verify   # report state, exit 1 if anything is not applied
./scripts/setup.sh --reset    # discard submodule changes and re-apply
```

The script discovers series from this directory's layout, so adding one means
adding a directory — there is nothing to register. It applies each series in
numeric order, since a patch may build on an earlier one, and checks the result
by tree hash derived from the patches themselves rather than recorded anywhere.

**An applied series does not show up in `git status`, at either level.** The
parent repository ignores submodule working-tree changes (`ignore = dirty` in
`.gitmodules`), and `setup.sh` marks the patched files `skip-worktree` inside
the submodule. Both are deliberate: the applied series is the normal working
state, not a pending change.

That hides genuine local edits too, so use `./scripts/setup.sh --verify` rather
than `git status` to ask whether a submodule is in the expected state — it
reads the working tree through a scratch index and reports `dirty` for anything
that is neither pristine nor exactly the series. `--reset` clears the
`skip-worktree` bits before resetting, because `git reset --hard` silently
skips entries that carry it.

Each submodule's document under `docs/` states what its patches change and
where.

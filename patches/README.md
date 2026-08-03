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
| [`emu68/`](emu68/) | `external/emu68` | michalsc/Emu68 | [`docs/emu68.md`](../docs/emu68.md) |

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

Each submodule's document under `docs/` states what its patches change and
where.

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

Per submodule, in numeric order, one at a time. `git apply` given a whole
series at once checks every patch against the original tree, so patches that
build on an earlier one fail:

```bash
cd external/emu68
for p in ../../patches/emu68/0*.patch; do git apply "$p" || break; done
```

Each submodule's document states what its patches change and where, why the
cuts fall as they do, and how to regenerate and verify the series.

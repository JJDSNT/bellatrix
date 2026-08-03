# patches

Changes applied on top of the `external/emu68` submodule, which is pinned to
upstream michalsc/Emu68 and never edited in place.

| # | Patch | What it does |
|---|---|---|
| 0001 | `emu68-emulate-amiga-interrupt-registers` | Serves INTENA/INTREQ/INTENAR/INTREQR from `INT_shadow` for a guest with no Paula. |
| 0002 | `emu68-offer-zorro3-rom-board` | Emulates Zorro autoconfig against Emu68's own board list for a guest with no expansion bus. |
| 0003 | `emu68-trim-standalone-module-list` | Offers a chipset-less guest only the modules it can use. |

Apply in numeric order, one at a time — 0002 depends on 0001, and `git apply`
given the whole series at once checks every patch against the original tree:

```bash
cd external/emu68
for p in ../../patches/0*.patch; do git apply "$p" || break; done
```

See **[`docs/emu68.md`](../docs/emu68.md)** for what each patch changes and
where, why the cuts fall this way, and how to regenerate and verify the series.

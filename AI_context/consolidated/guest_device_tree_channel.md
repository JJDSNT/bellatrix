# What reaches the guest's device tree, and what does not

Emu68 parses the device tree it was booted with, and Bellatrix and Emu68 both
add to that parsed tree. **None of it reaches the guest.** The guest receives

```c
void *fdt = (void*)(top_of_ram - ((dt_total_size() + 4095) & ~4095));
memcpy(fdt, dt_fdt_base(), dt_total_size());
```

-- a byte copy of the *original blob*, taken in `M68K_StartEmu`'s initramfs
path (`src/aarch64/start.c`). `dt_add_property()` edits the parsed tree, which
is a different object and is never re-flattened. A property added there is
invisible to the guest, and so is the whole `/emu68` node, which on most boots
exists only in the parsed tree because Emu68 made it.

This has now cost four attempts.

# Motivation

The mistake is attractive because the parsed tree looks like the tree: it is
what `dt_find_node("/emu68")` returns, it is what the boot log dumps, and
adding to it succeeds. Nothing fails. The property simply is not there when the
guest looks, and a guest that reads zero from a missing property behaves
exactly like a guest reading a legitimately zero value -- so the attempt does
not announce itself as failed either.

Two separate pieces of work walked into it:

- **`host-mem`.** `patches/emu68/0007` records three attempts to publish where
  Emu68's own pool ends, so the guest heap would stop overlapping it. None
  arrived.
- **the chipset selection.** `AI_context/issues/ISSUE-0068.md`, 2026-08-31: the
  fourth attempt, to publish whether the machine has a chipset so the guest
  would not have to read the command line itself.

`aros/arch/m68k-emu68/boot/boot.c` still contains a reader for
`/emu68/host-mem`. It has never once been taken. It is kept because it costs
nothing and is right if a host ever does publish the property, and its comment
now says so outright -- it read as the working mechanism, which is how the
fourth attempt got made.

# Solution adopted

**Correct the copy in place.** The copy exists, the guest has not read it, and
its layout is the original blob's -- so a property's address in Emu68's parsed
tree maps into the copy by a fixed offset:

```c
uint32_t *range = (uint32_t *)((uintptr_t)fdt +
                  ((uintptr_t)p->op_value - (uintptr_t)dt_fdt_base()));
```

Two things are done this way, and the constraint on both is that an FDT is
packed: an edit may not change any property's length, or everything after it
moves.

- **`patches/emu68/0007`** rewrites the cells of `/memory`'s `reg` to exclude
  Emu68's own range. Same cell count, different values.
- **`patches/emu68/0026`** blanks a word out of `/chosen/bootargs` when the
  machine did not implement it -- `rigel` on a command line handed to an image
  built with `CONFIG_RIGEL=0`. Blanking is length-preserving; the word becomes
  spaces, which the token scanners on both sides skip.

The shape both share: **the guest is handed what the machine implemented, not
what was asked for**, and then a guest that simply reads its own device tree is
right without needing to know any of this.

# Files changed

- `external/emu68/src/aarch64/start.c` via `patches/emu68/0007`, `0026`
- `src/machine/options.c` -- `bellatrix_correct_guest_cmdline()`
- `aros/arch/m68k-emu68/boot/boot.c` -- the `/emu68/host-mem` reader's comment

# Architectural impact

There is no channel from the host to the guest through added device-tree
nodes or properties. Anything the machine has to tell the guest goes through
one of:

1. an in-place correction of a property the original blob already carries
   (`/memory`, `/chosen/bootargs`) -- length-preserving only;
2. the magic-address MMIO the port already uses in the other direction
   (`0xdeadbeef` for the console, `aros/arch/m68k-emu68/boot/console.c`);
3. a MOVEC control register, for anything the CPU must read cheaply
   (JITCTRL2 bit 29 is the host-interrupt latch).

Adding a re-flatten so `dt_add_property()` did reach the guest is possible and
has not been done. It would be the general fix, and it would also let a
property be *added* rather than only corrected -- which is the limitation the
in-place approach cannot lift.

# Documentation updated

- `CLAUDE.md` -- "A property on `/emu68` is not a channel to the guest"
- `AI_context/issues/ISSUE-0068.md` -- "Closing the one direction the two
  halves could disagree in"

# Next steps

- If a third thing ever needs announcing to the guest, do the re-flatten
  instead of a third in-place special case.

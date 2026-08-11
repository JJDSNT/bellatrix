Current decision

- Bellatrix does not expose classic Expansion/Autoconfig.
- AROS m68k-emu68 uses the standard generic no-Expansion stub.
- No Zorro, Autoconfig, Super Buster or $E80000 provider
  is required by the baseline.
- No m68k-emu68-specific Expansion implementation is allowed.

Future compatibility

If classic Expansion becomes necessary:

- first evaluate reuse/refactoring of the existing m68k-amiga
  implementation;
- if that is impractical, an explicitly derived target copy may
  temporarily be used;
- runtime probing should then determine whether the classic
  hardware domain is actually present;
- Bellatrix-specific Autoconfig semantics must still not be
  introduced into AROS.

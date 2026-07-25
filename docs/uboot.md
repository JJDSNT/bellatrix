# Bellatrix U-Boot flow

Bellatrix uses U-Boot `v2026.01` as a first-stage selector on Raspberry Pi 3B.
The dependency is pinned as `external/u-boot`.

## Build

```sh
./scripts/setup.sh
./scripts/build-all.sh
```

The aggregate build produces:

```text
out/
├── boot/
│   ├── boot.cmd
│   ├── boot.scr
│   └── roms/
├── build/
├── firmware/
│   ├── config.txt
│   ├── u-boot.bin
│   └── bcm2710-rpi-3-b.dtb
└── images/
    ├── bellatrix_emu68.img
    ├── bellatrix_emu68_multicore.img
    ├── bellatrix_musashi_68000.img
    ├── bellatrix_musashi_68000_multicore.img
    ├── bellatrix_musashi_68040.img
    └── bellatrix_musashi_68040_multicore.img
```

`scripts/generate-uboot-menu.sh` scans `src/roms/` (or
`BELLATRIX_ROMS_DIR`) for `.rom` and `.bin` files. ROMs remain ignored by Git
unless explicitly allow-listed; generated copies under `out/boot/` are also
ignored.

## Boot sequence

1. Raspberry Pi firmware loads `u-boot.bin`, selected by `config.txt`.
2. U-Boot loads and executes `boot.scr`.
3. The first `bootmenu` presents the six CPU/topology variants.
4. The selected entry opens a second `bootmenu` containing the available ROMs.
5. U-Boot loads the kernel at `0x00080000`, DTB at `0x07000000`, and ROM at
   `0x08000000`.
6. `booti` updates `/chosen/linux,initrd-start` and
   `/chosen/linux,initrd-end`, then transfers control to Bellatrix.

Bellatrix images already carry a valid AArch64 Linux Image header, so `booti`
can start them directly. Patch `0024-emu68-64bit-initrd-address.patch` makes
the Emu68 boot path accept both the 32-bit initrd properties emitted by the
Pi firmware and the 64-bit properties emitted by U-Boot.

The menu implementation follows U-Boot's documented
[`bootmenu`](https://docs.u-boot.org/en/latest/usage/cmd/bootmenu.html) and
[`booti`](https://docs.u-boot.org/en/latest/usage/cmd/booti.html) interfaces.

## Installation

For a mounted FAT32 boot partition:

```sh
./scripts/flash.sh /media/user/BOOT
```

To create a complete FAT32 image:

```sh
./scripts/make_sdcard_image.sh
```

The generated flow has been build-validated. Display, USB-keyboard menu input,
and the final handoff still require Raspberry Pi 3B hardware validation.

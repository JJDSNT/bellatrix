# SD card, RDB and controller ownership

*Status: design under evaluation as of 2026-07-25.*

Bellatrix needs one physical SD card to carry both the Raspberry Pi boot files
and Amiga hard-disk volumes. The intended on-disk layout is independent of the
Amiga-side driver:

```text
SD card (MBR, not GPT)
├── partition 1: FAT32
│   └── Raspberry Pi firmware, U-Boot, Bellatrix images and ROMs
└── partition 2: type 0x76
    └── RDB
        ├── DH0:
        ├── DH1:
        └── optional filesystem LSEG blocks
```

The `0x76` partition is one container disk. `DH0:`, `DH1:` and other Amiga
volumes are RDB partitions inside it; they are not separate MBR entries.

## Three distinct storage paths

Bellatrix currently contains three mechanisms with different ownership:

| Path | Owner | Guest interface | Purpose |
|---|---|---|---|
| `bcm_emmc` + FAT32 | ARM/Bellatrix | none | Old ADF launcher path and current Bluetooth persistence |
| Emu68 SD card board | m68k driver | `brcm-sdhc.device`, Z3 | Direct access to physical SD and MBR `0x76` units |
| Bellatrix RIPPLE board | ARM/Bellatrix | `lide.device`, Z2 ATA | HDF/ISO today; can serve raw SD partition sectors |

The old FAT32 ADF path is not the Amiga hard disk. U-Boot now selects and loads
Bellatrix images and ROMs. Some FAT access remains because Bluetooth reads and
writes pairing/key/diagnostic files.

## Option A: native `brcm-sdhc.device`

Emu68 supplies a Zorro III ROM board containing an entirely m68k SDHC driver.
The driver accesses the Raspberry Pi peripheral aperture directly and exposes
MBR partitions of type `0x76` as disk units.

This is the shortest and fastest datapath:

```text
Amiga filesystem -> brcm-sdhc.device -> Raspberry Pi SDHC -> 0x76/RDB
```

The board is conceptually independent of the CPU implementation. If the
machine has Z3 and the platform supplies the Raspberry Pi SDHC peripheral, it
should work with either Emu68 or Musashi and in either runtime topology.
Platform-specific boards must not be inserted into the POSIX harness.

An audit found integration work still to validate for Musashi:

- the board object is linked into Musashi firmware but currently registers in
  Emu68's `.boards.z3` table, while the Musashi profile walks
  `bellatrix_boards`;
- its `map()` calls `mmu_map()` directly rather than the shared
  `cpu_backend_map_direct()` contract;
- Emu68 maps the Pi peripheral aperture into m68k space from `0xF2000000`.
  Musashi resolves m68k addresses in memory callbacks, so it needs an explicit
  DEVICE-region path that preserves MMIO width, volatility and byte order.

These are integration gaps, not a reason to create a second SDHC board.

The difficult ownership property is that the m68k driver owns the physical
controller after it starts. ARM-side FAT access for Bluetooth cannot safely
continue without a handoff or a cross-domain arbitration protocol.

## Option B: `lide.device` backed by the whole SD card

As an initial implementation, Bellatrix can retain ownership of the controller
and expose the whole card through its existing emulated ATA channel:

```text
Amiga filesystem
  -> lide.device
  -> Z2 RIPPLE ATA registers
  -> Bellatrix ATA emulation
  -> bcm_emmc sector backend
  -> whole SD card
  -> MBR
  -> partition 0x76 containing RDB
```

The ATA unit represents one physical disk. The LIDE mounter discovers the
primary MBR entry of type `0x76`, scans the RDB relative to that container, and
adds the container offset to each mounted partition's `DosEnvec`. The offset
must be aligned to the RDB partition geometry because classic trackdisk
interfaces express partition bounds in cylinders. The FAT32 partition remains
visible in the disk layout but is not mounted as an Amiga RDB volume.

### Advantages

- Reuses the existing `lide.device`, ATA emulation and raw-sector interface.
- Uses the shared Z2 board path with either CPU backend.
- Keeps Bluetooth FAT access and Amiga disk access under one ARM-side storage
  owner.
- Allows locking and transaction serialization in one `bcm_emmc` service.
- Can be tested with HDF in the harness and with the same RDB bytes on SD.
- Preserves the disk layout expected by a later switch to
  `brcm-sdhc.device`.

### Trade-offs

- Z2 plus emulated ATA is slower than the native Z3 SDHC driver.
- Register and data transfers traverse the EXTERNAL-board path.
- The current ATA implementation is limited to LBA28.
- Bluetooth and ATA still require arbitration. A single ARM owner makes that
  arbitration implementable; it does not make concurrent controller access
  safe automatically.
- This option does not validate the native Emu68 SDHC board.

## Provisional direction

The initial implementation uses `lide.device` with a writable whole-card
backend and an MBR-aware LIDE mounter. This preserves normal disk semantics:
one unit is exposed and the partition scanner publishes however many RDB
volumes it finds.

The native Z3 board remains the preferred direct path to investigate later.
Both approaches use the same card layout and RDB contents, so choosing the Z2
path first does not create a storage-format migration.

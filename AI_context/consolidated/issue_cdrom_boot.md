# Issue: ATAPI CD-ROM Boot Chain (OAHR RIPPLE lide.cdrom)

## Context

The OAHR RIPPLE board (`MFR=0x144A, PROD=7`) is a Zorro 2 IDE/ATAPI expansion emulated at
`$E90000`, 64 KB window. lide.device (with `CDBOOT=1`) is the AmigaOS driver responsible for
detecting and mounting CD-ROMs. This document traces why CD boot never succeeds in our
current emulation and what is required to fix it.

## CD Boot Chain

```
lide.device init()
  └─ FindCDFS()          -- search FileSystem.resource for DosType 'CD01' or 'CDVD'
       └─ cdBoot = (FindCDFS() != NULL)
MountDrive(&ms)
  └─ (only if ms->cdBoot == true)
       └─ ScanCDROM(md)
            ├─ UnitIsReady()   -- TEST UNIT READY
            ├─ isDataCD()      -- READ TOC
            ├─ CheckPVD()      -- CMD_READ LBA 16, verify "CD001" + "AMIGA BOOT"/"CDTV"
            ├─ find_filesystem('CD01','CDVD',…)  -- second FSR search
            └─ AddBootNode(bootPri, ADNF_STARTPROC, node, configDev)
```

**lide.device runs at `DEVICE_PRIORITY=10`** — it initialises early in Exec's resident
scan, before any filesystem handler from the boot floppy or CD can register.

## Why READ_TOC Is Never Issued

`FindCDFS()` (device.c line 187, compiled in when `CDBOOT=1`) scans
`fsr->fsr_FileSysEntries` for `fse->fse_DosType == 'CD01'` (0x43443031).

If nothing has registered 'CD01' in FileSystem.resource before lide.device runs,
`FindCDFS()` returns NULL → `cdBoot = false` → `MountDrive` skips `ScanCDROM` entirely.
**No `READ_TOC`, no `AddBootNode`, no CD visible to DOS.**

### AROS Official ROM Does Not Provide 'CD01'

`external/aros/rom/filesys/cdfs/` and `external/aros/rom/filesys/CDVDFS/` both have
`modtype=handler` in their mmakefiles. They are DOS filesystem handlers loaded by
mountlists/tooltypes at runtime — they do **not** register themselves into
FileSystem.resource at startup with a DosType code. `FindCDFS()` therefore returns NULL
even when booting the official AROS ISO.

**User insight (confirmed):** putting CDFS on the CD itself would not help, because
lide.device's `init()` runs and calls `FindCDFS()` long before any CD content is readable.
The driver that would populate FSR with 'CD01' must already be resident in ROM when
the Exec resident scan runs.

### 64 KB Board — No Room for BootCDFileSystem

The real OAHR RIPPLE board can be configured as **128 KB**. The second 64 KB bank is
designed to hold `BootCDFileSystem` (from AmigaOS OS4), which registers 'CD01' in FSR
during its own resident init, satisfying `FindCDFS()`.

Our emulated board is declared as `AC_SIZE_64KB` (`lide_cdrom.c`). There is no second bank
and no mechanism to deliver BootCDFileSystem. Expanding to 128 KB and embedding the
binary is the real-hardware solution documented in the RIPPLE README.

lide.device README quote:
> "lide.device supports booting from CD-ROM but requires a CD Filesystem to be loaded
> for this to work. Configure as a 128K board to permit loading CDFS from ROM (Optional)"

## The ADF/ISO Mutual Exclusion Bug (Fixed)

### Root Cause

`tools/harness/main.c` lines 846–858 (before fix): when both `--adf` and `--iso` were
passed on the command line, the ISO was silently discarded with a warning:

```c
/* OLD — buggy */
if (iso_path) {
    if (adf_path) {
        fprintf(stderr, "[HARNESS] WARNING: both --adf and --iso specified; ISO ignored\n");
    } else {
        iso_data = load_file_limited(...);
        ...
    }
}
```

`lide_cdrom_insert_iso` was never called. `AtapiCdromState.media_present` stayed `false`.
Every ATAPI command logged `media=0`.

### Fix Applied

ADF and ISO are independent devices (ADF → DF0 floppy, ISO → lide_cdrom ATAPI). The
exclusion guard was removed:

```c
/* NEW — fixed */
/* ADF → DF0, ISO → lide_cdrom. Both can coexist. */
if (iso_path) {
    iso_data = load_file_limited(iso_path, &iso_size, 800u * 1024u * 1024u, "ISO");
    if (!iso_data) {
        free(rom_data);
        if (adf_data) free(adf_data);
        return 1;
    }
    printf("[HARNESS] ISO: %s  size=%u bytes  (%u sectors)\n",
           iso_path, iso_size, iso_size / 2048u);
}
```

No other ADF/ISO exclusion exists elsewhere in `src/` or `tools/`.

## ATAPI Presence and Slave Detection

### How Presence Works

- `lide_cdrom_insert_iso` → `atapi_cdrom_insert` → sets `media_present = 1` → calls
  `ata_ide_channel_reset` (IDE state only; media_present survives).
- First TUR after insertion triggers UNIT_ATTENTION (SK=0x06, ASC=0x28).
  lide.device retries up to 4 times; on SK=0x06/ASC=0x28 it treats `ret=0` (success).
- `atapi_update_presence` then calls `atapi_get_capacity` (READ_CAPACITY command).
- `atapi_read_toc` would be called by `ScanCDROM` — but only if `cdBoot == true`.

### Slave Device Responds to Both Master and Slave Addresses (Known Issue)

`detectChannels` returns 2 for OAHR RIPPLE → lide.device probes master (i=0) and
slave (i=1) on each channel. Our ATAPI emulation currently ignores bit 4 of the
`dev_head` register (DEV bit), so it responds to both master and slave `DEVICE_RESET`
and `IDENTIFY` commands. Real hardware would only have a device on master (unit 0).

**Symptom:** two CD-ROM units appear in lide.device's unit list instead of one.
**Fix needed:** check `dev_head & (1<<4)` in `ata_ide_reg_read`/`ata_ide_reg_write`
and return 0x7F / ignore writes when the addressed device doesn't match the channel's
configured presence.

## RIPPLE IDECS Offsets (Hardware Reference)

From lide.device README, actual hardware register map:
- Primary channel IDECS1 asserted at offset `$1000` from board base
- Secondary channel IDECS1 at offset `$2000`

Our emulation maps both channels. This is consistent with `detectChannels` returning 2.

## Current Status

| Condition | Status |
|-----------|--------|
| ADF + ISO coexist in harness | ✅ Fixed |
| media_present set when ISO loaded | ✅ Works |
| UNIT_ATTENTION cleared on first TUR | ✅ Works |
| READ_CAPACITY issued after presence update | ✅ Works |
| FindCDFS() finds 'CD01' in FSR | ❌ AROS has no FSR entry |
| cdBoot == true | ❌ Blocked by above |
| ScanCDROM / READ_TOC issued | ❌ Blocked by above |
| AddBootNode called | ❌ Blocked by above |
| CD visible to AmigaDOS | ❌ Blocked by above |
| Slave responds only to slave address | ❌ dev_head not checked |

## Path to Fix CD Boot

**Option A — Embed BootCDFileSystem (real hardware path):**
1. Expand board to 128 KB (`AC_SIZE_128KB`)
2. Embed the BootCDFileSystem binary in the second 64 KB ROM bank
3. Machine maps second bank; Exec resident scan finds and runs it
4. It registers 'CD01' in FSR; `FindCDFS()` succeeds; CD boot proceeds

**Option B — Inject 'CD01' into FSR via DiagArea:**
1. Our DiagArea ROM (already present for serial debug) runs at expansion init time
2. Add code to allocate a `FileSysEntry` with `fse_DosType = 'CD01'` and a minimal
   `fse_SegList` pointing to a stub handler
3. Add it to `FileSystem.resource`
4. lide.device will find it; `cdBoot = true`; `ScanCDROM` runs
5. `find_filesystem` inside ScanCDROM also needs 'CD01' present — same mechanism covers it

Option B is lower overhead and stays within the current 64 KB window. Option A matches
real hardware exactly. Neither is implemented yet.

## Files Referenced

| File | Role |
|------|------|
| `tools/harness/main.c` | Harness entry; ADF/ISO fix applied here |
| `src/machine/expansions/lide_cdrom/lide_cdrom.c` | Board init, media insert, reset |
| `src/machine/expansions/lide_cdrom/ata_ide.c` | IDE register dispatch, ATAPI packet exec |
| `src/machine/expansions/lide_cdrom/atapi_cdrom.c` | ATAPI command handlers |
| `src/machine/machine.c` | `bellatrix_machine_insert_iso` wiring |
| `external/lide.device/device.c` | `FindCDFS`, `init`, `detectChannels` |
| `external/lide.device/mounter/mounter.c` | `ScanCDROM`, `MountDrive` |
| `external/lide.device/ata.c` | `ata_init_unit`, ATAPI identify/TUR |
| `external/lide.device/atapi.c` | `atapi_test_unit_ready`, `atapi_update_presence` |
| `external/aros/rom/filesys/cdfs/` | AROS CDFS — handler, NOT FSR entry |
| `external/aros/rom/filesys/CDVDFS/` | AROS CDVDFS — handler, NOT FSR entry |
| `external/lide.device/README.md` | Board config, CDFileSystem requirements |

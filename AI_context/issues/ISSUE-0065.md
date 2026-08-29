---
id: ISSUE-0065
title: "Hardware boot hangs while first-boot WiFi and dwc2emu68 are both active"
status: open
priority: high
type: defect
owner: unassigned
created_at: 2026-08-28
updated_at: 2026-08-29
tags:
  - wifi
  - bwfm
  - sdio
  - usb
  - dwc2
  - raspberry-pi-3
blockers:
  - needs controlled A/B boots on real hardware
related_files:
  - patches/aros/0087-wirelessmanager-wake-the-event-loop-from-the-gui.patch
  - aros/arch/m68k-emu68/soc/wifi/sdio
  - aros/arch/m68k-emu68/soc/wifi/bwfm
  - aros/arch/m68k-emu68/soc/usb/dwc2emu68
  - aros/arch/m68k-emu68/soc/wifi/arostcp
  - patches/aros/0082-arostcp-start-stack-before-network-managers.patch
  - AI_context/issues/ISSUE-0054.md
  - AI_context/issues/ISSUE-0064.md
---

# Summary

On real Raspberry Pi 3 hardware, the first image that both starts the onboard
WiFi automatically and uses `dwc2emu68.device` reaches `STARTING WANDERER` but
then appears to hang. The Wireless Manager AppIcon does not appear. The current
evidence cannot assign the hang to WiFi: the USB driver is simultaneously in a
known interrupt-transfer watchdog storm, and both paths are active during the
same part of startup.

Do not close this as a WiFi-driver failure from `InitResident bwfm.device`.
That line proves only that the SANA-II module was loaded. Its SDIO probe,
firmware load and WirelessManager interface creation are all silent because
the three WiFi modules are compiled with `DEBUG 0` and Package-Startup sends
the manager's output to `NIL:`.

# Change that exposed the symptom

The release tree previously shipped `Wireless.prefs` but no persistent
AROSTCP interface. The current working tree adds a first-boot configuration:

```text
AROSTCP/AutoRun             True
AROSTCP/WirelessAutoRun     True
AROSTCP/WirelessDevice      DEVS:Networks/bwfm.device UNIT 0
AROSTCP/db/interfaces       wlan0 DEV=DEVS:Networks/bwfm.device UNIT=0 IP=DHCP UP
```

It also adds patch 0082, moving `C:Execute S/startnet` before
`WirelessManager`. That ordering is necessary because WirelessManager's
network autoinit opens `bsdsocket.library`; when the manager ran first the log
showed that open returning NULL and no AppIcon could survive.

`fd.library` was missing independently because the lean build installed only
its headers. It is now explicitly built. Its absence had a fallback and is not
an explanation for this hang.

# Hardware evidence, 2026-08-28

The relevant order in the serial log is:

```text
[DWC2/Emu68:WD] 1, 2, 4, 8, 16 recoveries (all stage=4)
[LDDemon] OpenLibrary("bsdsocket.library", 0) opened but returned NULL
[InitResident] fd.library
[InitResident] bwfm.device
[LDDemon] OpenLibrary("LIBS:MUI.MiamiPanel", 0) opened but returned NULL
[AROSTCP] Opening log file 'T:Log/Syslog' failed
[DWC2/Emu68:WD] 32, 64 recoveries (stage=4)
[BootUI] STARTING WANDERER...
```

There is no WiFi/SDIO success or failure line. There is, however, an increasing
USB recovery counter on interrupt transfers before and after `bwfm.device`
loads. This matches the active investigation in ISSUE-0064: dwc2emu68 can lose
or delay channel completion handling and repeatedly recover stage-4 transfers.
It is therefore at least as plausible a cause of the apparent freeze as WiFi.

The `bsdsocket.library` failure in this particular trace is not yet safely
attributed to WirelessManager. It precedes `bwfm.device` and may come from a
different concurrently loaded network component. The later 238 KB ELF is the
size range of WirelessManager, but asynchronous loader output is not process
identity. Capture the manager's own output before making another ordering
inference.

`MUI.MiamiPanel` and `T:Log/Syslog` are separate packaging/configuration
problems. Neither proves that the AROSTCP task failed to start.

# Controlled isolation matrix

Change one variable per boot and retain the complete serial log:

| boot | USB | WiFi autorun | question |
|---|---|---|---|
| A | `dwc2emu68` | off | does the same hang occur with no WiFi open/probe? |
| B | `dwc2emu68` | on | current failing combination |
| C | `usb2otg` | off | baseline without either new path |
| D | `usb2otg` | on | does WiFi alone reproduce the hang? |

For A/C, set only `Prefs/Env-Archive/AROSTCP/WirelessAutoRun` to `False`;
leave the driver, firmware and `Wireless.prefs` present. This prevents an
OpenDevice/probe without changing the ELF or card contents. For C/D, build
with `BELLATRIX_USB_DRIVER=usb2otg`; do not swap drivers by copying one file
over another because Startup-Sequence selects by filename.

First repeat A and B with external USB devices disconnected. The onboard
LAN9514 hub still exists, but this removes mouse/keyboard interrupt endpoints
from the stage-4 workload.

After the four-way USB/WiFi-autorun matrix establishes whether WiFi is needed
to reproduce the hang, add `WiFiPi.device` as a second, exclusively selected
WiFi implementation as specified in ISSUE-0054. Do not use it as the first
test: changing both the USB and WiFi implementations together would preserve
the current ambiguity. With USB held constant, `bwfm` versus `wifipi` becomes
the fifth controlled axis and independently exercises the shared BCM43438.

# Required diagnostics

## Implemented in the 2026-08-28 diagnostic image

Bounded unconditional milestones now use three searchable prefixes without
enabling the existing per-command `DEBUG` flood:

- `[WIFI:SDIO]`: resource init, probe start, exact failing CMD5/CMD3/CMD7
  stage, OCR readiness and successful RCA/function discovery;
- `[WIFI:BWFM]`: resource availability, attach, function 1/buscore setup,
  chip ID, RAM/core enumeration and firmware-ready result;
- `[WIFI:DEV]`: SANA-II open, firmware and NVRAM filenames/sizes, firmware
  start and whether the device opened online or in its current offline
  fallback state.

The messages are present in the linked kernel ELF and `bwfm.device` in the
regenerated `out/aros/sd.img`. The first missing terminal milestone in the
hardware serial log is now the failure boundary.

1. Stop discarding WirelessManager output during the investigation. Run it
   after Wanderer with:

   ```text
   Run >SDCARD0P0:wireless-manager.log C:WirelessManager DEVS:Networks/bwfm.device UNIT 0 VERBOSE
   ```

   Record both the file and whether `Status` lists `C:WirelessManager` and
   `WirelessManager GUI`.

2. Add bounded, unconditional milestone diagnostics around:
   `SDIOProbe`, chip ID, firmware filename/open/result, `BWFMInit`, and the
   SANA-II device open result. Do not enable every existing `D()` call: the
   serial volume would perturb this already timing-sensitive boot.

3. Record a terminal marker after AROSTCP has created the port and another
   immediately before and after the WirelessManager launch. Loader-size
   inference is not enough with asynchronous package startup.

4. For every A/B boot, record the final DWC2 recovery count and whether it
   continues increasing after `STARTING WANDERER`. A stopped serial log and a
   live recovery storm are different failures.

# Decision rule

- A hangs like B: investigate/fix dwc2emu68 first; WiFi is not required.
- D hangs and C does not: instrument SDIO/bwfm; WiFi is sufficient.
- Only B hangs: investigate shared timing/resource interaction rather than
  either driver in isolation.
- None except the current card hangs: compare exact copied files and verify
  that Package-Startup and the kernel ELF came from the same build.

# Current conclusion

## Hardware result, 2026-08-29

The bounded milestones isolated the failure:

```text
[WIFI:SDIO] probe OK: OCR 0xa0ffff00, 2 function(s), RCA 0x0001
[WIFI:BWFM] chip ID 0x1541a9a6: id 43430 rev 1 type 1
[WIFI:BWFM] attach OK: 7 cores, RAM 0x000000/512 KB
[WIFI:DEV] blobs ready: firmware 400447, nvram text 1121, binary 684 bytes
[WIFI:BWFM] firmware ready timeout
```

The controller reaches the card, identifies the expected BCM43430, enumerates
its cores and RAM, and reads both blobs from disk. It fails only after upload
and CPU activation while polling the firmware-ready mailbox. The device then
opens through its deliberate offline fallback, which explains why AROSTCP can
continue but WirelessManager cannot scan.

This boot reached Wanderer and reported `dwc2emu68` heartbeats with
`recoveries=0`. USB is not required for this WiFi failure and is no longer the
leading explanation for the missing AppIcon in this capture. The broader USB
defect remains tracked separately in ISSUE-0064.

The next diagnostic image makes the existing timeout-state dump unconditional
only on this failure path: CHIPCLKCSR, mailbox, interrupt status, CR4
reset/ioctl state, and the first two firmware words read back from chip RAM
against the source image. That separates clock/reset failure from a corrupt or
misaddressed upload.

The first such capture reports `CHIPCLKCSR=0x48` (ALP requested and
available), zero mailbox/interrupt status, and exact agreement between the
first two RAM words and the source image (`00000000 c5220000`). Upload reaches
chip RAM intact; the firmware produces no observable signal. BCM43430 uses
CM3/SOCRAM, so the diagnostic has been extended to print those two cores'
IOCTL/reset/isup state instead of only CR4.

There is also a controlled firmware alternative already vendored with
WiFiPi: `cyfmac43430-sdio.bin`, Cypress 7.45.98, 399344 bytes. The current
release blob is Broadcom 7.45.98.94 from the Buster firmware tree, 400447
bytes. Both use the exact same 1121-byte NVRAM. If CM3 and SOCRAM report up,
renaming the Cypress blob to `brcmfmac43430-sdio.bin` on the card is the next
one-variable experiment before changing the upload/reset implementation.

The follow-up capture confirms both relevant cores are running by the host's
coarse criteria:

```text
CM3    ioctl=0x00000001 resetctl=0x00000000 isup=1
SOCRAM ioctl=0x00000001 resetctl=0x00000000 isup=1
```

Both have clock, are out of reset, and satisfy `bwfm_ai_isup()`. Together with
the matching RAM words and available ALP clock, every coarse prerequisite for
execution is present while mailbox and interrupt status remain zero. The
Cypress-blob substitution is therefore the next test. If it also times out,
compare the finer activation and memory-remap sequence against WiFiPi;
notably, WiFiPi compiles out the BCM43430 bank-3 remap write that the adopted
AROS path currently executes.

The Cypress 7.45.98 blob was then tested and produced the same timeout with
the same healthy CM3/SOCRAM/ALP state and zero mailbox. Firmware choice is no
longer the leading cause. The next image changes only the bank-remap
difference: the BCM43430 `BANKIDX=3/BANKPDA=0` writes are removed to match
WiFiPi's active code path, leaving the firmware, NVRAM, upload and core-reset
sequence unchanged.

That test also timed out. A fuller comparison then found a more consequential
ordering difference. The AROS ARM-native `bwfm` path, inherited by this port,
waits for `FWREADY` immediately after releasing the CPU and only afterwards
advertises the SDPCM version and enables SDIO function 2. WiFiPi performs those
bus-start operations before expecting firmware traffic. The next image now
matches that working order: release CM3, force HT, write the SDPCM version,
enable and wait for function 2, install the host interrupt mask/watermark, then
poll `FWREADY`. `SDIOEnableFunction()` was also corrected to return failure if
its ready-bit polling count expires; previously that timeout returned success.

## Root cause, 2026-08-29: the PIO data path reverses every four bytes

That image reported `HT clock timeout: CHIPCLKCSR=0x50` -- ALP available, HT
requested, HT never granted -- and stopped before function 2. Reading the
transport underneath both symptoms found the actual defect, in
`sdio.resource` rather than in `bwfm`:

```c
static inline ULONG sdio_fifo_r(struct SDIOBase *SDIOBase)
{
    return AROS_LE2LONG(*(volatile ULONG *)(SDIOBase->sdio_iobase + SDHCI_BUFFER));
}
```

`SDHCI_BUFFER` is not a numeric register. It is a window onto the wire: the
byte at its lowest address is the byte transferred first. A big-endian 32-bit
load already delivers the four wire bytes in stream order, so the
`AROS_LE2LONG()` turns them into the little-endian *value* they spell, and
`CopyMem()` then lays that value down big-endian -- reversing each group of
four. The write side reverses symmetrically.

This is inherited, not introduced: on `arch/arm-native`, where this code comes
from, `AROS_LE2LONG()` is the identity and the accessor is correct. The
conversion only becomes wrong on a big-endian host.

Every observation in this issue follows from it:

- **Chip ID, EROM, core and RAM sizing were right.** `bwfm_read_4()` passed a
  4-byte buffer through the same reversal and then read it back as a big-endian
  word, reversing it a second time. Two errors that cancel, so the whole
  register layer looked healthy.
- **The upload was corrupt.** The firmware image is a byte stream with no
  second reversal to save it, so BCM43430 SOCRAM received the image in
  4-byte-reversed groups.
- **The readback verification could not see it.** Reading back through the
  4-byte window reverses the groups again, so RAM and image compared equal word
  for word while the CM3 was executing garbage. This is why "RAM 0x000000 =
  00000000 c5220000, image = 00000000 c5220000" was consistent with a broken
  upload.
- **CM3 and SOCRAM were genuinely up.** `isup=1`, out of reset, clocked --
  running a scrambled image, which signals nothing.
- **Neither firmware blob mattered**, because both were corrupted the same way.
- **HT never arrived**, because on a SOCRAM part the PMU grants HT only after
  the firmware has programmed it.

### Fix

1. `sdio_fifo_r()` / `sdio_fifo_w()` no longer convert. They are the one pair
   of accessors in that file that must stay raw, and they now say why.
2. `bwfm_read_4()` / `bwfm_write_4()` convert explicitly with
   `AROS_LE2LONG()` / `AROS_LONG2LE()`: the backplane register *is* a number,
   and now that the transport is honest, its own conversion has to be too.
   `rstvec` and the image-comparison diagnostics follow the same rule.
3. The HT step is now a request, not a precondition -- matching
   `brcmf_sdio_bus_init()`, which ORs `HT_AVAIL_REQ` for a chip with an
   INTERNAL_MEM core (and `FORCE_HT` otherwise) and proceeds straight to
   function 2 without waiting. It logs whether HT arrived, which still
   separates a clock failure from a firmware failure, but no longer deadlocks
   the chip whose firmware is waiting on function 2.

### The check that can actually prove it

Word-granular verification is structurally incapable of detecting a
group-reversing transport. The new diagnostic reads the first eight image
bytes back with CMD52, one byte per command, and prints them beside the image:

```text
[WIFI:BWFM] RAM bytes   xx xx xx xx xx xx xx xx
[WIFI:BWFM] image bytes xx xx xx xx xx xx xx xx
```

Equal in order confirms the transport. Equal in reversed groups of four would
mean the fix did not take. This is the objective record the earlier captures
were missing.

### Status

Built and staged into `out/aros/sd.img`; not yet run on hardware.

## Hardware result, 2026-08-29: the firmware starts

```text
[WIFI:BWFM] RAM bytes 00 00 00 00 c5 22 00 00
[WIFI:BWFM] image bytes 00 00 00 00 c5 22 00 00
[WIFI:BWFM] CM3 released: ioctl=0x00000001 reset=0x00000000
[WIFI:BWFM] clock 0x48 | 0x10 -> CHIPCLKCSR=0xd8 (HT available)
[WIFI:SDIO] function 2 ready: IOEN=0x06 IORDY=0x06
[WIFI:BWFM] bus start OK: SDPCM 4, function 2 ready
[WIFI:BWFM] firmware ready
[WIFI:DEV] firmware start OK
[WIFI:DEV] open: firmware online
```

The byte-order proof matches in order, so the transport is correct. HT was
granted (`0xd8` = HT_AVAIL | ALP_AVAIL | both requests) once the firmware ran,
which is exactly the dependency that made waiting for it a deadlock. Every
symptom in this issue is accounted for by the one defect.

Association was not exercised: the test machine had no USB input attached.

## Status

The bring-up defect is closed. What remains under this issue's title -- whether
first-boot WiFi and `dwc2emu68` interact -- is now testable for the first time,
because WiFi reaches an online device instead of failing early.

## Hardware results, 2026-08-29 (later): scan and join both work

Three defects were found in sequence, each hidden behind the previous one.

### 1. The scan was never requested

With bounded milestones on `BWFMScan()`, the log showed no `scan begin` at all:
nothing ever called it. Every diagnostic on that path had been behind `D()`
with `DEBUG 0`, so "nothing in the log" had been read as "the scan failed"
when it meant "the scan never ran" -- the trap CLAUDE.md records.

### 2. The Scan button could not reach the event loop

Root cause, fixed by `patches/aros/0087`: WirelessManager runs its MUI window
in a separate process (`main_amiga.c` `start_gui()` spawns "WirelessManager
GUI") while `wpa_supplicant_run()` -> `eloop_run()` blocks in the main task.
`ScanFunc()` calls `wpa_supplicant_req_scan()`, which registers an eloop
timeout from the GUI process -- and nothing woke the task that would run it.

`eloop_run()` builds its signal mask once per iteration, before `Wait()`, from
the timeouts and read sockets existing at that moment. With no networks
configured there is neither: the mask is zero, and `Wait(0)` can never return.
The loop was parked for ever.

The patch gives the loop its own signal, always in the mask, signals it from
`eloop_register_timeout()` when the caller is another task, re-arms the timer
request when the new timeout is nearer than the one in flight, and puts the
list splice under `Forbid()` since it has two writers.

### 3. Results

```text
[WIFI:BWFM] scan begin: C_UP err 0, CLM -1 bytes
[WIFI:BWFM] scan complete: 21 network(s), 34 escan event(s), 1200 ms
[WIFI:BWFM] join begin: "Megamaster" WPA, host handshake (ie 22, pass 0; ...)
[WIFI:BWFM] join event 3 status 0 flags 0x0000     E_AUTH
[WIFI:BWFM] join event 7 status 0 flags 0x0000     E_ASSOC
[WIFI:BWFM] join event 16 status 0 flags 0x0001    E_LINK up
[WIFI:BWFM] join event 25 status 0 flags 0x0000    E_EAPOL_MSG
[WIFI:BWFM] join attempt 1 OK (result 0)
[WIFI:BWFM] join "Megamaster": CONNECTED
```

Scans return 14-21 networks in 1.2-1.4 s. The join authenticates, associates,
brings the link up, and an EAPOL frame arrives -- so the four-way handshake at
least starts.

`CLM -1` means no `.clm_blob` is on the card. **The CLM is no longer a
suspect**: a radio returning twenty-one networks has valid channels. The
Cypress-pair experiment proposed earlier is not needed.

### What CONNECTED does and does not say

It is a link-layer statement: authenticated (open), associated, link up. A WPA
network accepts anyone's association and only then demands the four-way
handshake, so this milestone cannot distinguish "connected" from "associated,
handshake pending" from "about to be deauthenticated". The wording is
deliberately link-layer, and ISSUE-0066 records that the UI must not repeat it
as a user-facing claim.

`pass 0` beside `ie 22` is correct, not a defect: with an RSN IE the driver is
on the host-handshake path, where `wpa_supplicant` performs the four-way with
its own PSK and the driver never needs the passphrase.

## The real symptom, 2026-08-29: an association loop

A longer capture shows the join is not a one-off success. It repeats, roughly
every few seconds, for as long as the machine is up:

```text
scan complete: 19 network(s) -> join "Megamaster" -> CONNECTED
scan complete: 13 network(s) -> join "Megamaster" -> CONNECTED
scan complete: 17 network(s) -> join "Megamaster" -> CONNECTED
scan complete: 21 network(s) -> join "Megamaster" -> CONNECTED
scan complete: 15 network(s) -> join "Megamaster" -> ...
[WIFI:BWFM] join event 3 status 5 flags 0x0000      E_AUTH, status 5
[WIFI:BWFM] join event 19 status 1 flags 0x0000     E_ROAM, status 1 (FAIL)
```

The `dwc2emu68` heartbeat runs 35, 36, 37, 38, 39, 40 through all of it, so the
machine is alive and this is a loop, not a hang.

This is the signature of a four-way handshake that never completes:
`wpa_supplicant` associates, waits for the handshake, gives up, disconnects,
re-scans and associates again. The last attempt in the capture degrades
further -- `E_AUTH` status 5 is NO_ACK and `E_ROAM` status 1 is FAIL, which is
what an AP does to a client that keeps failing authentication.

So the driver's link layer is doing its job and the WPA authentication above it
is not. Two candidates, and they are cheap to tell apart:

1. **No usable PSK.** `ConnectFunc()` derives one from whatever is in the
   window's passphrase field, including the empty string, without prompting or
   validating (ISSUE-0066). The user reports the window never asked for a
   password.
2. **EAPOL not reaching the supplicant.** `driver_sana2.c` does not use
   `l2_packet` at all; it queues its own SANA-II read with
   `ios2_PacketType = ETH_P_EAPOL` (line 1019) and feeds `drv_event_eapol_rx()`.
   If our datapath does not deliver 0x888E frames to that reader, the handshake
   cannot complete however correct the PSK is. Note `rx_deliver()` in
   `bwfm_dev.c` already carries a fix for exactly this class of bug -- its
   `packet_type` is a `UWORD` because 0x888E sign-extended as a `WORD` and
   matched no reader.

`E_EAPOL_MSG` (event 25) in every successful join says the *firmware* saw an
EAPOL frame. It says nothing about whether the frame reached `wpa_supplicant`.

### Discriminating test, no rebuild

A `network={}` block with a known-good `psk=` in
`Prefs/Env-Archive/SYS/Wireless.prefs` removes candidate 1 entirely. If the
loop continues with a correct PSK configured, the defect is candidate 2 and
belongs in the driver's EAPOL delivery. If it stops, the defect is the window.

# Still open under this issue's original title

Whether first-boot WiFi and `dwc2emu68` interact was never answered -- WiFi
was failing too early to ask. It is now testable.

Two new threads split off:

- the four-way handshake. `wpa_supplicant`'s output goes to `NIL:` under
  Package-Startup, so a failed handshake is invisible. The test that needs no
  code is a `network={}` block in `Prefs/Env-Archive/SYS/Wireless.prefs`, which
  removes the GUI's passphrase field from the question entirely (ISSUE-0066).
- `ifconfig -a` failing with `ENOBUFS` (ISSUE-0067). Not a WiFi defect.

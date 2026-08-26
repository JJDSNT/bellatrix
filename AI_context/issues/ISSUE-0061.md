---
id: ISSUE-0061
title: "The Bluetooth stack reconnects by luck: no scan policy, no retry, no controller-capability layer"
status: backlog
priority: medium
type: refactor
owner: unassigned
created_at: 2026-08-26
updated_at: 2026-08-26
tags:
  - bluetooth
  - low-energy
  - reconnection
  - design
  - upstream
blockers: []
related_files:
  - external/aros/rom/bluetooth/bluetooth/hwtask.c
  - external/aros/rom/bluetooth/bluetooth/hwconn.c
  - external/aros/rom/bluetooth/bluetooth/bluetooth_intern.h
  - external/aros/rom/bluetooth/stack/core/device/device_registry.c
  - AI_context/issues/ISSUE-0059.md
  - AI_context/issues/ISSUE-0062.md
---

# Summary

Four gaps in how the stack reconnects a bonded LE device. None of them is
specific to the Raspberry Pi 3; they would bite any host with any radio. They
were found while chasing a concrete failure on a Pi 3 — a multi-channel mouse
that does not come back — which is [ISSUE-0059](ISSUE-0059.md). That issue
carries the fix for *this* board; this one carries what the stack should
eventually look like.

Worth taking upstream: the stack lives in `rom/bluetooth` in the AROS tree, and
gap 3 in particular is about hardware this port will never see.

**Revised 2026-08-26 on two counts.** Gap 3 originally said there were two
correct implementations split at Bluetooth 4.2. That is wrong: the Filter
Accept List is **4.0**, so this board's BCM43438 can already do
controller-driven reconnection — for a peer that advertises with an identity
address. The ladder has three rungs, not two, and which one this board lands on
is decided by one byte nobody has looked at yet. Gap 4 overstated the
observability problem: a per-address scan journal already exists and already
prints the classification that decides the question.

**Further corrected the same day.** The concrete failure this issue was
extracted from — a mouse that "does not come back" — is not a reconnection
defect at all: the machine hangs, [ISSUE-0062](ISSUE-0062.md). The four gaps
below remain real design gaps in the stack and would bite any host with any
radio, which is why this issue survives. But none of them explains that
board's symptom, and the plan at the end is not the route to fixing it.

# 1. One shot, and no second

A returning device gets exactly one opportunity: the background scan sees an
advert, `bConnAdvertising()` (`hwconn.c:854`) acts on it. If that is missed —
because the scan was between windows, or because the connection object was in a
state the function does not handle — nothing else ever happens.

There is no retry, no backoff, and no recovery path.

The shape that is missing is not hypothetical; the pre-2026-08-03 tree had it
(`~/bellatrix-legacy/src/io/bluetooth/bt_host.c:1339`), over BTstack and
BR/EDR:

```
PASSIVE      -> once a deadline passes, try connecting to the known pairs
CONNECTING   -> with a timeout; on expiry fall through to DISCOVERING
DISCOVERING  -> start an *active* discovery for the known device, 30 s
                found   -> back to PASSIVE, which connects
                expired -> BACKOFF 60 s, then round again
```

Periodic retry, an active-discovery recovery, and a backoff. The current stack
has none of the three. This is knowledge the project already had and lost in
the crossing to the new stack.

# 2. There is no notion of a scan *scenario*

`bBgScanUpdate()` (`hwtask.c:755`) writes two constants by hand:

```c
bt_buf_writer_write_le16(&w, 0x0800); /* interval 1.28 s */
bt_buf_writer_write_le16(&w, 0x0030); /* window 30 ms */
```

Linux keeps five sets and picks between them
(`include/net/bluetooth/hci_core.h`, assigned in `hci_alloc_dev_priv()`):

| scenario | interval | window | duty |
|---|---|---|---|
| discovery | 0x0012 (11.25 ms) | 0x0012 (11.25 ms) | 100% |
| background, awake | 0x0060 (60 ms) | 0x0030 (30 ms) | 50% |
| establishing a connection | 0x0060 (60 ms) | 0x0060 (60 ms) | 100% |
| suspended | 0x0800 (1.28 s) | 0x0012 (11.25 ms) | 0.9% |
| advertisement monitor | 0x0060 (60 ms) | 0x0030 (30 ms) | 50% |

BlueZ exposes them as `ScanInterval*` / `ScanWindow*` pairs in `main.conf` for
the same reason.

What is missing here is not the numbers — those are one edit — but the layer
that knows there is a choice to make and what it depends on. As written, an
awake mains-powered desktop scans at the duty cycle Linux reserves for a
suspended laptop, and nothing in the code can express that this is wrong.

The stack in fact already has a second, much more aggressive set — it is just
applied at the wrong moment. See gap 3.

# 3. No controller-capability layer, and the ladder has three rungs

The stack never asks the radio what it can do, so it always does the least
capable thing — and the least capable thing is further down the ladder than
this radio actually requires.

## What the code does today

`bStartConnect()` (`hwconn.c:703`):

```c
bBgScanStop(hc);                          /* initiating is refused while a scan runs */
bt_buf_writer_write_le16(&w, 0x0060);     /* scan interval  60 ms */
bt_buf_writer_write_le16(&w, 0x0030);     /* scan window    30 ms  => 50% duty */
bt_buf_writer_write_u8(&w, 0x00);         /* filter policy: peer address */
```

with the peer taken from the address it was last *heard* using:

```c
const UBYTE *peer = bd->bd_CurAddrValid ? bd->bd_CurAddr : bd->bd_Address.bd_Addr;
```

Three things follow.

- The initiator filter policy is hardcoded **0x00**, "connect to this one
  address". The Filter Accept List is never used — `grep` finds no accept-list
  or resolving-list opcode anywhere in `rom/bluetooth/`.
- Because the target is the last-seen address, the stack **structurally cannot
  reconnect without having heard an advert first**. Hearing is not an
  optimisation in this design, it is the only entry point.
- There are already two duty cycles in the tree, and they are applied the wrong
  way round: **50%** while establishing a connection we have already decided to
  make, **2.3%** while trying to notice a device that needs to be rediscovered.

## The ladder

`LE_Create_Connection` with **`Initiator_Filter_Policy = 0x01`** tells the
controller to scan on its own and connect to whatever it hears from the Filter
Accept List — the Link Layer's Auto Connection Establishment procedure. The
host submits one command and sleeps until `LE Connection Complete`. No
advertising reports climb to the host, no window to miss, no polling. That is
Zephyr's `bt_conn_le_create_auto()` and BlueZ's kernel auto-connect.

The Filter Accept List is **Bluetooth 4.0**. What Bluetooth **4.2** adds is the
**Resolving List** (`LE_Add_Device_To_Resolving_List`,
`LE_Set_Address_Resolution_Enable`), needed only when the *peer* advertises
with a resolvable private address — because then the accept list is matching
against an address that changes.

| rung | needs | who scans | works with a peer using |
|---|---|---|---|
| **2** | 4.2 — Resolving List | controller | any address, RPA included |
| **1** | **4.0** — Accept List | controller | public or static-random identity |
| **0** (today) | nothing | host | anything, at the cost of waking for every advert |

`LE_Set_Privacy_Mode` — device- vs network-privacy, which is what the Linux
kernel kept mis-programming — is **5.0**, and falls outside this boundary
entirely. That is the reason not to encode the ladder as a version comparison:
the pieces do not land on one version.

## Which rung this board lands on is one unmeasured byte

If the mouse advertises with a public or static-random identity address, rung 1
solves ISSUE-0059 **on this 4.1 radio**, with no host scanning, no duty cycle
to tune, and no new hardware. If it advertises with an RPA, this board is stuck
on rung 0 until there is a 4.2 controller.

The discriminator is the `Address_Type` of the advertising report — 0x00
public, 0x01 random — and, when random, the top two bits of the most
significant octet: `0b11` static, `0b01` resolvable, `0b00` non-resolvable.
`bt_le_addr_is_stable()` (`stack/core/device/device_registry.c:101`) already
implements exactly that test.

See the Plan: this costs no code to measure.

## The one feature bit the stack does read, it reads wrong

Evidence that the capability layer is needed rather than merely tidy.
`bluetooth_intern.h:297`:

```c
UBYTE bth_LEFeatures[8];  /* LE controller features (bit 6 of byte 0 = LE Secure Connections) */
```

and, acting on that comment, `hwconn.c:2154`:

```c
cfg.features.auth_req = BT_SMP_AUTHREQ_BONDING | BT_SMP_AUTHREQ_MITM |
                        ((bth->bth_LEFeatures[0] & 0x40) ? BT_SMP_AUTHREQ_SC : 0);
```

`bth_LEFeatures` is filled from `LE_Read_Local_Supported_Features`
(`hwtask.c:1434`, `HCB_LE_READ_LOCAL_FEATURES`). In that bitmap bit 6 of byte 0
is **LL Privacy**, not LE Secure Connections; LESC is not in the LE feature set
at all — controller support for it is discovered from
`HCI_Read_Local_Supported_Commands` (the P-256 and DHKey commands), which the
stack never issues. `HC_OP_LE_READ_LOCAL_P256` is defined (`hwtask.h:81`) and
submitted (`hwconn.c:1929`), but with `bIgnoreCompletion`, so the answer is
discarded.

On a BCM43438 both bits read 0 and the outcome — legacy pairing — is correct,
which is why it has never shown. It is right by coincidence, on a 4.1 radio,
and would misjudge any 4.2+ controller that has one feature without the other.
Worth fixing on its own terms regardless of the rest of this issue. To confirm
before changing anything: Core spec Vol 6 Part B §4.6 (the LE feature set).

## What does not get better by climbing

- **There is one initiator.** `bBgScanStop(hc); /* initiating is refused while
  a scan runs */` is not an artefact of rung 0. Waiting on the accept list is
  still initiating, and still excludes discovering new devices. BlueZ handles
  it by cancelling and reissuing. The scan-scenario state machine of gap 2 is
  therefore required at every rung — it is imposed by the controller, not a
  nicety.
- **The timeout policy inverts.** `bConnTick()` expires connection attempts
  (`cn_LastActivity`, `hwconn.c:711`). On rungs 1 and 2 the wanted behaviour is
  the opposite: the initiator stays pending indefinitely until
  `LE_Create_Connection_Cancel`, which is precisely the semantics of "come back
  whenever you like".
- **The accept list is finite.** `LE_Read_White_List_Size` (OGF 0x08, OCF
  0x000F) is not even defined in `hwtask.h`; real controllers hold roughly 8 to
  25 entries. Per-device `bpc_AutoConnect` (`BDA_AutoConnect`, shown by
  `prefs/DevWinClass.c:77`) today gates host logic only; on rungs 1 and 2 it
  acquires a direct hardware meaning — *this device occupies a slot in the
  controller's accept list* — and so needs an over-subscription policy and a
  prefs page that shows the budget instead of failing quietly.

# 4. It cannot say why it did not reconnect

More of the machinery exists than this issue first claimed. `bScanDiagNote()`
(`hwtask.c:394`) journals one entry per address for every advertising report,
on both the discovery and the background path, and `bScanDiagDump()`
(`hwtask.c:463`) writes `SYS:BluetoothScan.log` with the address already
classified:

```c
const char *kind = (sd->sd_AddrType == 0) ? "public"
                 : (sd->sd_AddrType >= 2) ? "identity"
                 : sd->sd_Stable ? "static-random" : "RPA";
```

Two things keep it from answering the question at hand:

- it is dumped **only when a discovery finishes** (`hwtask.c:652`, gated on
  `BTF_KLOG` / `btdebug`), and
- the table is **reset when the next discovery starts** (`hwtask.c:867`).

So reports gathered by the background scan are recorded and then silently
discarded. There is no way to ask "what has the background scan been hearing?"

What is genuinely missing is on the decision side. `bConnAdvertising()` has
four exits and three are silent:

```c
if(hc->hc_Connecting || (bth->bth_Flags & BTHF_DISCOVERING))   return;  /* silent */
if(cn) {
    if((cn->cn_State == HCNS_CONNECTING) && cn->cn_WaitAdv) { ...logs... }
    return;                                                             /* silent */
}
if(!bonded || !bpc_AutoConnect || !bgc_AutoConnect)            return;  /* silent */
```

Whether the background scan is running is `KPRINTF` only, so invisible in any
normal capture.

**Corrected 2026-08-26, later the same day.** This paragraph originally read
that a device which does not come back produces an empty log, and that the
empty log could not distinguish "the advert never arrived" from "it arrived
and was discarded". That was wrong about the case it cited: the log was empty
because the machine had hung — [ISSUE-0062](ISSUE-0062.md). Hours did go into
the ambiguity, but the ambiguity was between a silent decision and a dead
machine, and it was resolved by a heartbeat (`tests/gl/btwatch`), not by
logging inside the stack.

The gap itself stands. Three of four exits from `bConnAdvertising()` still say
nothing, and the day a device really is discarded rather than never heard,
there will be no way to tell. What changes is the priority: a heartbeat that
proves the machine is alive is worth more than any amount of logging inside a
subsystem, and it comes first.

Any logging added here needs throttling — one line per state change, never one
per advert, or it floods and buries what it was meant to show.

# Plan

Ordered so that the cheapest step is the one that decides the architecture.
Steps 1 and 2 are specific to this board and this radio; steps 3 onward are the
generic shape, which step 1 tells us how much of we need.

**1. Measure the mouse's advertising address type. No code.** Boot with
`btdebug`, switch the mouse back to this channel, and immediately start a
discovery from Prefs. `SYS:BluetoothScan.log` will name the address `public`,
`identity`, `static-random` or `RPA`, and that decides the rung.

Two things to expect, so the run is not misread: a discovery sets
`BTHF_DISCOVERING`, which makes `bConnAdvertising()` return early, so the mouse
deliberately will *not* reconnect during the scan — the observation is the
point, not the connection. And `RPA` in that log lumps together resolvable and
non-resolvable private addresses, because `bt_le_addr_is_stable()` only tests
for static-random; if the answer comes back `RPA`, check the top two bits by
hand before concluding.

**2. Make the journal readable outside a discovery.** Dump `hc_ScanDiag[]` on a
trigger other than "discovery finished", and stop clearing it on the next
discovery start. Without this, nothing can ever be learned from the background
scan — which is the scan that matters here.

**3. Then choose the rung.**

- `public` or `static-random` → implement rung 1 for this board: add the peer
  to the Filter Accept List, issue `LE_Create_Connection` with filter policy
  0x01, and let the controller wait. Fixes ISSUE-0059 outright, on 4.1, with
  no duty cycle to tune.
- `RPA` → this board stays on rung 0, and ISSUE-0059 is fixed by the duty-cycle
  and retry work (gaps 1 and 2) instead. Rung 2 becomes a design item for
  hardware we do not have.

**4. Regardless of the rung**: gap 1 (retry and backoff), gap 2 (scan
scenarios, needed at every rung because there is one initiator), gap 4's
decision-side logging, and the `bth_LEFeatures` bit fix, which is independent
of all of it.

**5. The capability layer proper** — a small set of named booleans derived from
`LE_Read_Local_Supported_Features` and `HCI_Read_Local_Supported_Commands`, not
from `bth_HCIVersion`, with room for a per-controller override. The override is
not defensive programming: Linux gated LL Privacy on exactly this bit
(`le_features[0] & HCI_LE_LL_PRIVACY`, i.e. the same `0x40`) and still had to
force it off for controllers that advertise the feature and implement it
wrongly. This is the piece worth proposing upstream.

# Appendix: prior art

**Linux, kernel 5.9 through ~5.18.** Turning on controller-side LL Privacy
broke reconnection for LE-only multi-host mice and keyboards — the exact device
class and the exact symptom of ISSUE-0059, with a different cause. The
circulated workaround was to force host-side resolution back on:

```c
/* include/net/bluetooth/hci_core.h */
#define use_ll_privacy(dev) (0)          /* was: (dev)->le_features[0] & HCI_LE_LL_PRIVACY */
```

Two things transfer. First, a 4.1 radio is *immune* to this whole class of
failure, because it has no resolving list to mis-programme — a point ISSUE-0059
records as a limitation and which is, on this front, protection. Second, the
Linux failure and ours produce the same empty log for opposite reasons: an
advert that arrived and was not matched looks exactly like an advert that never
arrived. That is the argument for gap 4 preceding gap 2.

Sources: [Arch BBS 259954](https://bbs.archlinux.org/viewtopic.php?id=259954),
[Arch BBS 260517](https://bbs.archlinux.org/viewtopic.php?id=260517),
[linux-bluetooth, privacy mode on a BCM43438](https://www.spinics.net/lists/linux-bluetooth/msg67334.html).

**Bluetooth SIG, Reconnection Configuration Service 1.0.1.** Not applicable —
it is a GATT service implemented by the *peripheral*, for non-connected phases
"in the area of minutes or hours", written by the Medical Devices Working Group
with a glucose meter as the worked example. No HID device implements it. It is
however the best available vocabulary for what gap 2 and gap 5 are missing, and
four clauses are worth stealing:

- Advertising is modelled as a **burst**, not a flat interval: *Advertisement
  Interval*, *Advertisement Count* and *Advertisement Repetition Time*. Our
  duty-cycle arithmetic assumes flat on both sides and may be computing the
  wrong encounter probability.
- A **Reconnection Timeout** with an explicit fallback rule: on expiry the
  device "shall change all parameters modified back to the stored values of
  Parameter-Set 0x00 ... This enables a device to be again in a connectable
  condition." That is gap 1's backoff invariant, specified. Sentinels worth
  copying: `0` = applies to the current connection only, `0xFFFE` = disabled.
- **The accept list is governed by a timer**, "to enable connectivity to other
  devices if the devices in the Filter Accept List are not available any more"
  — the safety valve against exactly the Linux failure above. Cross-check
  against the finite-list constraint in gap 3.
- Capabilities are a **feature bitmap with an extension bit** (RC Feature,
  24 bits, bit 23 chaining another octet), not a version number — the model
  step 5 should follow.

The specification is SIG-proprietary and must not be committed to this
repository.

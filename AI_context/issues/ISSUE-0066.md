---
id: ISSUE-0066
title: "WirelessManager's window cannot be used to join a network without knowing how it works"
status: backlog
priority: medium
type: enhancement
owner: unassigned
created_at: 2026-08-29
updated_at: 2026-08-29
tags:
  - wifi
  - wirelessmanager
  - ui
  - zune
  - bluetooth
blockers:
  - feature freeze - nothing new is built until the machine is fast and stable
related_files:
  - external/aros/workbench/network/WirelessManager/wpa_supplicant/wpa_gui_amiga.c
  - external/aros/rom/bluetooth/prefs/ScanWinClass.c
  - external/aros/rom/bluetooth/prefs/ActionClass.c
  - external/aros/rom/bluetooth/prefs/DevWinClass.c
  - AI_context/issues/ISSUE-0065.md
---

# Summary

The WiFi bring-up works: the firmware starts, a scan returns twenty-odd
networks, and a join reaches `E_AUTH` / `E_ASSOC` / `E_LINK up`. What is not
usable is the window in front of it.

Reported from hardware, 2026-08-29: *"ele em nenhum momento pediu a senha. o
prefs do bluetooth add device funciona muito melhor"*. The complaint is exact,
and this repository already contains the better design.

# What the window does today

`wpa_gui_amiga.c` is about 230 lines: a list of SSIDs with an encryption
column, one `StringObject` labelled "Passphrase", and two buttons, Scan and
Connect. `ConnectFunc()` reads the string gadget and builds a network from it:

```c
passphrase = (STRPTR) XGET(str1, MUIA_String_Contents);
ssid = wpa_config_add_network(wpa_sup->conf);
...
ssid->passphrase = os_strdup(passphrase);
wpa_config_update_psk(ssid);
```

Nothing prompts for the passphrase, nothing rejects an empty one, and nothing
reports what happened next. An empty field derives a PSK from the empty string
and produces a four-way handshake that fails several seconds after the
association succeeded -- and because `wpa_supplicant`'s own output goes to
`NIL:` under Package-Startup, no message reaches the user or the serial log.
The window looks like it worked.

That last part matters beyond the UI: association and authentication are
separate steps, and a WPA network accepts the association of anyone. A user
watching this window has no way to tell "associated, handshake pending" from
"connected" from "the AP threw us off two seconds ago".

# The design to copy

`rom/bluetooth/prefs/` solves the same problem for the same kind of device and
is already in the tree.

- **`ScanWinClass.c`** -- an "Add Bluetooth Device" window. A discovered-device
  list with real columns (Address, Name, RSSI, Status), Refresh and Connect,
  and double-click as a synonym for Connect. The WiFi equivalent of the columns
  is SSID, BSSID, signal, security, and whether it is already configured.
- **`ActionClass.c`** -- a persistent status line, `SetStatus()`, that narrates
  every action *and every refusal*: `"Adding radio..."`, `"Could not add that
  radio."`, `"Select a device first."`, `"pairing - enter the passkey"`. This is
  the single largest difference. Every failure path says something.
- Per-entry status icons (`ICON_LED_GREEN` / `ICON_LED_RED`) so the state of a
  device is visible in the list rather than inferred.
- **`DevWinClass.c`** -- a per-device window with Name, Address, Type, Status,
  Bound class and Stored keys, a custom-name field, and checkmarks for
  auto-reconnect and trusted. The WiFi equivalent is a per-network window with
  the security type, the stored PSK state, and an auto-join checkmark.

# What this issue asks for

1. **Ask for the passphrase**, in a requester or a focused field, when the
   selected network is encrypted and no configuration for it exists. Refuse an
   empty one rather than deriving a PSK from it.
2. **A status line** carrying the association state through to the four-way
   handshake: scanning, associating, authenticating, connected, failed and why.
   `wpa_supplicant` already emits these transitions; today they go to `NIL:`.
3. **Per-network state in the list** -- configured, in range, connected -- with
   the same LED convention the Bluetooth window uses.
4. **Do not report "connected" on association alone.** The driver's own
   `[WIFI:BWFM] join ... CONNECTED` milestone has the same defect and is
   deliberately worded as a link-layer statement; the UI must not repeat it as
   a user-facing claim.

# Not now

This is new functionality and falls on the wrong side of the standing freeze
(CLAUDE.md, 2026-08-17). It is recorded because the design decision is worth
capturing while the comparison is fresh, not because it is queued.

The workaround needs no code and no UI: a `network={}` block in
`Prefs/Env-Archive/SYS/Wireless.prefs` on the card makes `wpa_supplicant`
associate on its own at boot, with the right PSK, and never involves the
window at all.

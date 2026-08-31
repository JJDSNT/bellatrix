---
id: ISSUE-0067
title: "AROSTCP cannot create a socket: ifconfig fails with ENOBUFS"
status: open
priority: high
type: defect
owner: unassigned
created_at: 2026-08-29
updated_at: 2026-08-29
tags:
  - arostcp
  - network
  - memory
related_files:
  - external/aros/workbench/network/common/C/ifconfig.c
  - external/aros/workbench/network/stacks/AROSTCP/bsdsocket/kern/uipc_socket.c
  - external/aros/workbench/network/stacks/AROSTCP/bsdsocket/kern/kern_malloc.c
  - external/aros/workbench/network/stacks/AROSTCP/bsdsocket/conf.h
  - AI_context/issues/ISSUE-0037.md
blockers:
  - needs an Avail reading from the failing machine
---

# Summary

On hardware, with the WiFi driver associated and the link up, `ifconfig -a`
fails:

```text
No buffer space available
```

This is not a WiFi defect. The driver authenticates, associates and brings the
link up (ISSUE-0065); the failure is above it, in AROSTCP.

# Where the error comes from

`ifconfig -a` calls `printall()`, which calls `getsock()` and then
`err(1, "socket")` if the descriptor is negative (`ifconfig.c:507`). So the
failure is in **socket creation**, not in the `SIOCGIFCONF` ioctl that follows.

`socreate()` returns `ENOBUFS` in exactly one place
(`uipc_socket.c:141`):

```c
MALLOC(so, struct socket *, sizeof(*so), M_SOCKET, M_WAIT);
if(so == NULL)
    return (ENOBUFS);
```

The stack answered -- `bsdsocket.library` opened for `ifconfig`'s task, or the
failure would have come earlier -- but its allocator had nothing to give.

That allocator is an exec pool created once at stack start
(`kern_malloc.c:41`):

```c
mem_pool = CreatePool(MEMF_PUBLIC, __MALLOC_POOLSIZE, __MALLOC_POOLSIZE_THRESHOLD);
```

with `__MALLOC_POOLSIZE` = 128 KB and the threshold 16 KB (`conf.h:335`).
128 KB puddles of `MEMF_PUBLIC`: if no contiguous block that size can be had --
low memory, or fragmentation -- every `socket()` fails this way.

# Why nothing says so

Two independent silences hide the cause:

- `S:startnet` runs the stack as `Run >NIL: QUIET AROSTCP`, so its startup
  output goes nowhere;
- AROSTCP logs to `T:Log/Syslog` and that open fails at every boot
  (`[AROSTCP] Opening log file 'T:Log/Syslog' failed`). `T:` exists -- the
  Startup-Sequence assigns it to `RAM:T` and creates the directory -- but
  **nothing creates `T:Log/`**, so the stack has no log at all.

The missing directory is a one-line fix and is worth making regardless of this
defect: it is the stack's only channel for saying what went wrong.

# Next measurements

1. `Avail` on the failing machine, specifically the largest free public block.
   Under 128 KB explains the failure outright and moves this to the heap
   (ISSUE-0037), not to the network stack.
2. `MakeDir T:Log`, stop the stack, restart it with output captured:

   ```text
   MakeDir T:Log
   C:Execute S:stopnet
   Run >T:arostcp.log AROSTCP
   WaitForPort AROSTCP
   Type T:arostcp.log
   Type T:Log/Syslog
   ```

3. `Status`, to confirm an `AROSTCP` process exists at all. If it does not, the
   `WaitForPort` in `startnet` failed silently and the boot log's
   `OpenLibrary("bsdsocket.library", 0) opened but returned NULL; task=AROSTCP`
   stops being noise and becomes the first symptom.

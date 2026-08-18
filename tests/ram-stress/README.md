# RAM: stress scripts — reproducing ISSUE-0037 on demand

The heap corruption in ISSUE-0037 is reached once per boot, and shows up in
roughly one boot in four. The resolved backtrace named the path exactly:

```
ram_Handler -> RAMMain -> CmdDeleteObject -> AttemptDeleteObject
            -> DeleteObject -> SetString -> FreePooled -> tlsf_freevec
```

— a file being **deleted from `RAM:`**, freeing its name string. The user
confirmed it independently by deleting the `ENV:SYS/Packages` block from the
Startup-Sequence, after which the corruption stopped.

These scripts turn "once per boot, sometimes" into a loop.

## What the boot actually does, and why there are three scripts

The block that was removed runs **two** create/write/delete cycles in `RAM:`,
not one, and that matters because they are not the same shape:

```
:152  List "ENV:SYS/Packages" ... TO "T:P" ...     (1) short name, one write
:153  Execute "T:P"                                (2) see below
:154  Delete "T:P" QUIET                           (1) deleted here
```

`Execute` is the non-obvious one. It copies the whole script into a temporary
file of its own before running it (`workbench/c/shellcommands/Execute.c:126-170`):

```c
__sprintf(tmpname, "%sTmp%lu%lu%lu%lu%lu", tmpdir,
          proc->pr_TaskNum, ds.ds_Days, ds.ds_Minute, ds.ds_Tick, count);
tmpfile = Open(tmpname, MODE_NEWFILE);
while((c = FGetC(from)) != -1 && FPutC(tmpfile, c) != -1);
...
DeleteFile(tmpname);
```

So it creates a second `T:` file whose **name is long and varies in length**
run to run, fills it, and deletes it. Since `muddy_pool` contains only two
kinds of allocation — file data blocks and name/comment strings — a
variable-length generated name is exactly the sort of thing worth isolating.

| script | what it exercises |
|---|---|
| `ram-stress-a` | `List ... TO "T:P"` + `Delete` — short fixed name, no `Execute` |
| `ram-stress-b` | `Execute` only, on a payload already on the card — the temp file path, long generated names |
| `ram-stress-c` | the boot's sequence verbatim, both cycles |

Run `c` first: if it reproduces, `a` and `b` split it in one more run each.

## Using them

Copy into `S:` on a card and call one from the Startup-Sequence, before the
desktop comes up so the serial log is uncluttered:

```
Execute "S:ram-stress-c"
```

Each iteration prints a marker, so the serial log says how many it survived.
They loop forever: the corruption is what stops them. If a script runs for
minutes without a word from `[Kernel:TLSF]`, that is a result too — see the
caveat below.

Under QEMU (`./run.sh --headless`) an iteration costs milliseconds, which is
the whole point: it replaces a card write and a boot with a scroll of output.

## The caveat, stated up front

**A quiet run does not prove the defect is absent.** If the corruption depends
on how the pool is fragmented after a full boot — or on state left by USB or
the card driver — a tight loop over the same two files may settle into a
steady state and never trigger it. That is why ISSUE-0037's plan pairs these
with guard bytes inside `rom/filesys/ram`, which do not depend on reproducing
anything.

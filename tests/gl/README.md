# GL probes

`gl-probe` reports what the MesaGL chain looks like from inside a running
system, with every line going to `DEBUG:` so that it survives a machine that
stops responding. That distinction is the point: for ISSUE-0045 a hang with no
serial output and a hang whose output stops at a known line are different
findings, and only the second one narrows anything.

    Execute "S:gl-probe"

`DEBUG:` reaches the serial line, and getting there took fixing two things
that had left every probe here writing into nothing:

- `DEVS:DOSDrivers/DEBUG` names `L:debug-handler`, which was never built. The
  target is `workbench-fs-debug` and `build-aros.sh` invokes it now.
- boot tests were inserted at line 30 of `S:Startup-Sequence`, and
  `DEVS:DOSDrivers` is mounted on line 53 -- so a probe ran before the device
  it wrote to existed. `make-sdcard.sh` anchors after the `Mount` now.

If a probe ever goes silent again, check those two before concluding anything
about what it was probing. A run that prints nothing and a run that never got
there look identical, and that is exactly the confusion these scripts exist to
prevent.

`make-sdcard.sh` copies these onto every card for the same reason the
ram-stress scripts are copied: a diagnostic that has to be re-injected after
every build quietly stops being there.

To run one automatically at boot, build the card with

    BELLATRIX_BOOT_TEST=gl-probe ./scripts/make-sdcard.sh

which appends an `Execute` of it to `S:Startup-Sequence` just before Wanderer
is started -- late enough that DOS, the display and the assigns exist, early
enough that a hang is not confused with a desktop problem.

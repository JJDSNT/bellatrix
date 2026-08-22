# GL probes

`gl-probe` reports what the MesaGL chain looks like from inside a running
system, with every line going to `DEBUG:` so that it survives a machine that
stops responding. That distinction is the point: for ISSUE-0045 a hang with no
serial output and a hang whose output stops at a known line are different
findings, and only the second one narrows anything.

    Execute "S:gl-probe"

`make-sdcard.sh` copies these onto every card for the same reason the
ram-stress scripts are copied: a diagnostic that has to be re-injected after
every build quietly stops being there.

To run one automatically at boot, build the card with

    BELLATRIX_BOOT_TEST=gl-probe ./scripts/make-sdcard.sh

which appends an `Execute` of it to `S:Startup-Sequence` just before Wanderer
is started -- late enough that DOS, the display and the assigns exist, early
enough that a hang is not confused with a desktop problem.

# Repository Guidelines

## Project Structure & Module Organization

Bellatrix combines two pinned upstream submodules: `external/emu68/` and
`external/aros/`. Do not edit either submodule in place. Upstream changes belong
in numbered patches under `patches/emu68/` or `patches/aros/`; project-owned AROS
port code lives in `aros/arch/m68k-emu68/` and is symlinked into the AROS tree by
the setup script. Build and image tooling is in `scripts/`, cross-compilation
configuration is in `cmake/`, and technical notes are in `docs/`. Generated
artifacts go under the ignored `out/` directory. Use `AI_context/` for issue,
specification, and consolidated investigation records.

## Build, Test, and Development Commands

- `./scripts/setup.sh`: initialize submodules, apply patch series, and install
  project-owned symlinks.
- `./scripts/setup.sh --verify`: confirm submodules are pristine and correctly
  patched; use this instead of relying on `git status` inside submodules.
- `./scripts/build.sh`: build Emu68 incrementally into `out/images/Emu68.img`.
- `./scripts/build-aros.sh`: build the AROS ELF. Keep this build serial; its
  generated headers and cross-toolchain stages are not safe under `make -j`.
- `./scripts/make-sdcard.sh`: create `out/aros/sd.img`.
- `./run.sh --headless`: build as needed and boot under QEMU using serial I/O.

Pass `clean` to either build script for a clean build. See `README.md` for host
dependencies and additional run options.

## Incremental Patch and Build Workflow

Do not use `./scripts/setup.sh --reset` for ordinary development. It resets
both submodules, can discard local work, rewrites mtimes, and causes unrelated
AROS targets to regenerate. Reserve it for an explicitly requested recovery
after inspecting and reporting exactly what would be lost.

Do not revise a patch that is already applied to a submodule during an
iteration. Add a numbered follow-up patch, validate it with `git apply
--check`, and apply only that patch to the relevant submodule. The numbered
patch remains the authoritative source; never make an unrecorded submodule
edit. Use `./scripts/setup.sh --verify` afterwards. Inspect the submodule
itself before applying anything, because the parent repository may hide dirty
submodule state.

Build the smallest affected target and keep AROS builds serial. For example,
use `make kernel-usb-usb2otg-emu68` for that disk driver and
`make kernel-link-emu68-m68k` for ELF/resident changes. Do not run the full
`AROS-emu68-m68k` distribution target unless disk-distribution contents
genuinely require it. Reuse the existing distribution tree and regenerate the
SD image only after updating the disk files that actually changed; relinking
the ELF alone does not require rebuilding the distribution.

Record whether an interrupted build completed before using its artifacts. If
an explicitly authorized reset was unavoidable, use the timestamp repair in
`scripts/build-aros.sh` rather than treating the resulting global regeneration
as a reason to perform a full build.

## Coding Style & Naming Conventions

Write code, comments, documentation, and commits in English. Follow nearby AROS
C and assembly conventions: four-space indentation, braces on their own line
for functions, descriptive `snake_case` functions, and uppercase constants.
Shell scripts use Bash, `set -euo pipefail`, quoted expansions, and uppercase
configuration variables. Keep patches narrowly scoped and numbered with a
four-digit prefix, for example `0004-fix-description.patch`.

## Testing Guidelines

There is no repository-wide unit-test harness or coverage target. At minimum,
run `./scripts/setup.sh --verify`, rebuild the affected image, and smoke-test it
with `./run.sh --headless`. For boot or graphics changes, capture the relevant
serial output and also verify the GUI path when applicable. Document hardware-
only validation or known gaps in the pull request.

## Commit & Pull Request Guidelines

History follows short, imperative Conventional Commit subjects such as
`build: add SD image` and `fix(run): show the framebuffer`. Keep each commit to
one logical change. Pull requests should explain the motivation, identify the
affected patch or port area, list exact validation commands, link relevant
issues, and include logs or screenshots when runtime behavior changes.

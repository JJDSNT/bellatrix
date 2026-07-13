#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/bellatrix-test-emu68-machine"

cc -std=c11 -Wall -Wextra -Werror \
   -I"$ROOT/src" \
   "$ROOT/tests/unit/test_emu68_machine.c" \
   "$ROOT/src/cpu/emu68/emu68_machine.c" \
   -o "$OUT"
"$OUT"

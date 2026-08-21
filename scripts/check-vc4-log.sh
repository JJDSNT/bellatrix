#!/usr/bin/env bash

set -euo pipefail

usage()
{
    echo "usage: $0 qemu|pi SERIAL_LOG" >&2
    exit 2
}

[[ $# -eq 2 ]] || usage
MODE="$1"
LOG="$2"
[[ -f "$LOG" ]] || { echo "ERROR: log not found: $LOG" >&2; exit 2; }
command -v rg >/dev/null || { echo "ERROR: rg is required" >&2; exit 2; }

require()
{
    local pattern="$1"
    local description="$2"

    if ! rg -q -- "$pattern" "$LOG"; then
        echo "FAIL: missing $description" >&2
        return 1
    fi
    echo "PASS: $description"
}

reject()
{
    local pattern="$1"
    local description="$2"

    if rg -q -- "$pattern" "$LOG"; then
        echo "FAIL: found $description" >&2
        rg -n -- "$pattern" "$LOG" >&2
        return 1
    fi
    echo "PASS: no $description"
}

require 'BootUI.*retargeted to RGB32 framebuffer' \
    'BootUI retargeted to the vc4gfx framebuffer'
require 'BootUI: STARTING WANDERER' 'boot reached Wanderer'
require 'BootUI.*display takeover: direct scanout' \
    'BootUI stopped writing over direct scanout'
reject 'FBALLOC refused' 'firmware framebuffer allocation refusal'
reject 'flip: SETVOFFSET refused' 'disabled framebuffer flipping'
reject '\[VideoCoreGfx\] DMA .* failed' 'vc4gfx DMA failure'

case "$MODE" in
    qemu)
        require 'no HVS found \(ID=0x00000000\).*QEMU or unmapped' \
            'QEMU selected the mailbox framebuffer fallback'
        reject 'takeover: ACTIVE' 'unexpected HVS takeover under QEMU'
        ;;
    pi)
        reject 'no HVS found' 'missing or unmapped HVS on real hardware'
        require 'takeover: ACTIVE - list [0-9]+, out [0-9]+x[0-9]+, fb ' \
            'native HVS display-list takeover'
        require 'vsync: bit [0-9]+ ticks per frame, using it' \
            'calibrated PixelValve vsync interrupt'
        require 'vsync: alive, [1-9][0-9]* ticks during check' \
            'live vsync interrupts after takeover'
        ;;
    *)
        usage
        ;;
esac

echo "VC4 $MODE validation passed: $LOG"

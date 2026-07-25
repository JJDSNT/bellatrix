#!/usr/bin/env bash

# Shared naming contract for local builds and GitHub release images.
bellatrix_image_name() {
    local backend="${1:-emu68}"
    local musashi_cpu="${2:-68040}"
    local multicore="${3:-0}"
    local name

    case "$backend" in
        emu68|"")
            name="bellatrix_emu68"
            ;;
        musashi)
            case "$musashi_cpu" in
                68000|68040)
                    name="bellatrix_musashi_${musashi_cpu}"
                    ;;
                *)
                    # Non-release CPU models remain usable without pretending
                    # to be one of the six standard U-Boot/release choices.
                    name="bellatrix_musashi_${musashi_cpu}"
                    ;;
            esac
            ;;
        *)
            echo "ERROR: invalid Bellatrix CPU backend: $backend" >&2
            return 1
            ;;
    esac

    if [ "$multicore" = "1" ]; then
        name="${name}_multicore"
    elif [ "$multicore" != "0" ]; then
        echo "ERROR: invalid Bellatrix multicore value: $multicore" >&2
        return 1
    fi

    printf '%s.img\n' "$name"
}

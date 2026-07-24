// src/cpu/cpu_bus_policy.h
//
// CpuBusPolicy — the institutionalised catalog of the points where the CPU
// model (and its integration) changes what the chipset observes. It exists
// because the same title renders differently across CPUs: Battle Squadron shows
// graphical divergence on 68000 vs 68040, and some OCS demos (eonA) only run on
// 68000 at all. Those differences come from a handful of CPU-model-dependent
// behaviours — how fast the CPU runs relative to the beam, whether it stalls on
// the chip bus, when it recognises interrupts. This struct is the single place
// those behaviours are named, so they stop being implicit and scattered.
//
// What this is NOT:
//   - It is NOT a runtime CPU selector. The model is already chosen at build
//     time (scripts/build.sh musashi_68000 / musashi_68040) or via HARNESS_CPU.
//     A backend looks up the policy *for the model it was given*; it does not
//     switch models at runtime.
//
// How it is used:
//   Musashi-68000 target  -> cpu_bus_policy_68000     (the easiest reference)
//   Musashi-68040 target  -> cpu_bus_policy_68040
//   Emu68 target          -> cpu_bus_policy_68040     (Emu68 is a ~68040 class)
//   The adapter applies the policy's points; Rigel stays CPU-agnostic.
//
// The plan (ISSUE-0072): institutionalise the points starting from the 68000
// (the most constrained, easiest-to-match reference), then 68040, then the
// multicore path, so that in the end a multicore Emu68 serves as a faithful
// generic Amiga. Each point is turned on and validated (timing-test rows +
// Battle Squadron) before it changes default behaviour.
//
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum CpuModel {
    CPU_MODEL_68000 = 0,
    CPU_MODEL_68010,
    CPU_MODEL_68020,
    CPU_MODEL_68EC020,
    CPU_MODEL_68030,
    CPU_MODEL_68040,
} CpuModel;

typedef struct CpuBusPolicy {
    const char *name;   /* "68000", "68040" */
    CpuModel    model;

    /* Speed: the CPU clock that converts a backend's per-instruction cycle
     * count into Rigel colour-clocks. This is the dominant difference behind
     * Battle-Squadron-style divergence — a faster CPU issues the same chip
     * writes at a different beam position. Musashi already produces model-correct
     * cycle counts, so the clock (not a per-op table) is what the policy owns.
     * 0 = leave the current fixed ratio untouched. */
    uint32_t    cpu_clock_hz;

    /* Bus contention: when true, a chip-bus access consults Rigel's
     * cpu_would_stall and the CPU waits for a free slot (classic 68000
     * behaviour). Default false = today's behaviour (no stall). */
    bool        stalls_on_chip_access;

    /* Interrupt timing: CPU cycles between an IPL raise and the CPU recognising
     * it. 0 = current immediate recognition. */
    uint16_t    irq_recognition_latency;
} CpuBusPolicy;

/* Institutionalised profiles. Values are conservative where not yet calibrated
 * against a config-matched reference; each point is validated before it is
 * turned on (ISSUE-0072). */
extern const CpuBusPolicy cpu_bus_policy_68000;
extern const CpuBusPolicy cpu_bus_policy_68040;

/* Look up the policy for a built model. `name` is the build/HARNESS_CPU string
 * ("68000", "68040"); the enum form is for backends that hold a CpuModel.
 * Unknown -> the 68040 profile (Emu68's class, and the historical default). */
const CpuBusPolicy *cpu_bus_policy_by_name(const char *name);
const CpuBusPolicy *cpu_bus_policy_for_model(CpuModel model);

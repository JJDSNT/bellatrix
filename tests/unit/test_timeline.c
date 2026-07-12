#include <stdint.h>

#include "runtime/timeline.h"

int main(void)
{
    RuntimeTimeline t;

    runtime_timeline_init(&t, 10000u, 1000u, 20u, 5000u, 40u);
    if (runtime_timeline_update(&t, 6000u, 77u, 77u) != 77u)
        return 1;

    /* Pure realtime with the chipset keeping up: wall clock drives. */
    runtime_timeline_set_mode(&t, RUNTIME_TIMELINE_REALTIME, 6000u, 77u);
    if (runtime_timeline_update(&t, 6500u, 0u, 1000u) != 127u ||
        runtime_timeline_update(&t, 7000u, 0u, 1000u) != 177u)
        return 2;

    runtime_timeline_set_mode(&t, RUNTIME_TIMELINE_HYBRID, 7000u, 177u);
    if (runtime_timeline_update(&t, 8000u, 180u, 500u) != 200u)
        return 3;
    if (runtime_timeline_update(&t, 9000u, 400u, 500u) != 377u)
        return 4;

    runtime_timeline_set_paused(&t, true, 9000u, 377u);
    if (runtime_timeline_update(&t, 19000u, 900u, 377u) != 377u)
        return 5;
    runtime_timeline_set_paused(&t, false, 19000u, 377u);
    if (runtime_timeline_update(&t, 19500u, 400u, 800u) != 420u)
        return 6;

    /* Resuming rebases at the resume counter: the ten seconds spent paused
     * must never become a catch-up burst. */
    if (t.realtime_target_cck != 427u || t.generation != 5u)
        return 9;

    /* Clamp released with a small wall deficit (157 cck < 250 ms worth):
     * the deficit is replayed so the machine stays honest to realtime. */
    if (runtime_timeline_update(&t, 21000u, 900u, 800u) != 577u ||
        runtime_timeline_update(&t, 21200u, 900u, 800u) != 597u)
        return 7;

    /* Clamp released after a long CPU stall (>250 ms of wall deficit):
     * rebase at the release point — the stalled interval must not be
     * fast-forwarded through the machine as a catch-up burst. */
    if (runtime_timeline_update(&t, 22000u, 100u, 800u) != 597u)
        return 10;
    if (!t.clamp_active)
        return 11;
    if (runtime_timeline_update(&t, 30000u, 10000u, 10000u) != 597u)
        return 12;
    if (t.clamp_active || t.realtime_target_cck != 597u)
        return 13;
    if (runtime_timeline_update(&t, 31000u, 10000u, 10000u) != 697u)
        return 14;

    /* Chipset unable to keep realtime (the Pi 3 case): even in pure
     * REALTIME mode the horizon follows chipset+backlog, so the wall-clock
     * debt cannot accumulate into a later fast-forward burst. */
    runtime_timeline_init(&t, 10000u, 1000u, 20u, 0u, 0u);
    runtime_timeline_set_mode(&t, RUNTIME_TIMELINE_REALTIME, 0u, 0u);
    if (runtime_timeline_update(&t, 2000u, 0u, 50u) != 70u)
        return 15;
    if (!t.clamp_active)
        return 16;
    if (runtime_timeline_update(&t, 4000u, 0u, 50u) != 70u)
        return 20;
    /* Chipset catches up; the >250 ms wall debt is slipped, not replayed. */
    if (runtime_timeline_update(&t, 4400u, 0u, 5000u) != 70u)
        return 17;
    if (t.clamp_active)
        return 18;
    if (runtime_timeline_update(&t, 4500u, 0u, 5000u) != 80u)
        return 19;

    /* Long counter deltas use quotient/remainder without overflowing the
     * intermediate elapsed*rate product. */
    runtime_timeline_init(&t, 1u, 100u, 20u, 0u, 0u);
    runtime_timeline_set_mode(&t, RUNTIME_TIMELINE_REALTIME, 0u, 0u);
    if (runtime_timeline_update(&t, 1000000000000ull, 0u,
                                100000000000000ull) != 100000000000000ull)
        return 8;

    return 0;
}

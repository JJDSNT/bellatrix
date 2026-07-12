#include "runtime/timeline.h"

#include <stddef.h>

static uint64_t timeline_elapsed_cck(const RuntimeTimeline *timeline,
                                     uint64_t host_counter)
{
    uint64_t elapsed;
    uint64_t seconds;
    uint64_t remainder;

    if (host_counter <= timeline->base_counter ||
        timeline->host_frequency == 0u) {
        return 0u;
    }
    elapsed = host_counter - timeline->base_counter;
    seconds = elapsed / timeline->host_frequency;
    remainder = elapsed % timeline->host_frequency;

    /* Split quotient/remainder so long uptimes do not multiply the complete
     * counter delta by the chipset clock and overflow prematurely. */
    return seconds * timeline->cck_per_second +
           (remainder * timeline->cck_per_second) /
               timeline->host_frequency;
}

static void timeline_rebase(RuntimeTimeline *timeline,
                            uint64_t host_counter,
                            uint64_t rigel_cck)
{
    timeline->base_counter = host_counter;
    timeline->last_counter = host_counter;
    timeline->base_cck = rigel_cck;
    timeline->horizon_cck = rigel_cck;
    timeline->realtime_target_cck = rigel_cck;
    timeline->clamp_active = false;
    timeline->generation++;
}

void runtime_timeline_init(RuntimeTimeline *timeline,
                           uint64_t host_frequency,
                           uint64_t cck_per_second,
                           uint64_t max_cpu_backlog_cck,
                           uint64_t host_counter,
                           uint64_t rigel_cck)
{
    if (timeline == NULL)
        return;
    *timeline = (RuntimeTimeline){
        .mode = RUNTIME_TIMELINE_CPU_DRIVEN,
        .host_frequency = host_frequency,
        .cck_per_second = cck_per_second,
        .max_cpu_backlog_cck = max_cpu_backlog_cck,
    };
    timeline_rebase(timeline, host_counter, rigel_cck);
}

void runtime_timeline_set_mode(RuntimeTimeline *timeline,
                               RuntimeTimelineMode mode,
                               uint64_t host_counter,
                               uint64_t rigel_cck)
{
    if (timeline == NULL || mode > RUNTIME_TIMELINE_HYBRID)
        return;
    timeline->mode = mode;
    timeline_rebase(timeline, host_counter, rigel_cck);
}

void runtime_timeline_set_paused(RuntimeTimeline *timeline,
                                 bool paused,
                                 uint64_t host_counter,
                                 uint64_t rigel_cck)
{
    if (timeline == NULL || timeline->paused == paused)
        return;
    timeline->paused = paused;
    timeline_rebase(timeline, host_counter, rigel_cck);
}

uint64_t runtime_timeline_update(RuntimeTimeline *timeline,
                                 uint64_t host_counter,
                                 uint64_t cpu_published_cck,
                                 uint64_t chipset_cck)
{
    uint64_t target;

    if (timeline == NULL || timeline->paused)
        return timeline == NULL ? 0u : timeline->horizon_cck;

    if (timeline->mode != RUNTIME_TIMELINE_CPU_DRIVEN &&
        (host_counter < timeline->last_counter ||
         (timeline->host_frequency >= 4u &&
          host_counter - timeline->last_counter >
              timeline->host_frequency / 4u))) {
        /* A counter discontinuity or >250 ms host gap is a pause/rebase, not
         * emulated time that Core 2 must replay in a catch-up burst. */
        timeline_rebase(timeline, host_counter, timeline->horizon_cck);
        return timeline->horizon_cck;
    }
    timeline->last_counter = host_counter;

    if (timeline->mode == RUNTIME_TIMELINE_CPU_DRIVEN) {
        target = cpu_published_cck;
        timeline->realtime_target_cck = target;
        timeline->clamp_active = false;
    } else {
        bool was_clamped = timeline->clamp_active;
        uint64_t follow;
        uint64_t clamp;

        /* The horizon is permission the executors must be able to honor.
         * It follows the wall clock but never runs more than the backlog
         * bound ahead of the CHIPSET (both modes — an unbounded horizon
         * debt would replay as a fast-forward burst once load lightens)
         * nor of the CPU (hybrid only). Realtime is best-effort: time the
         * executors could not keep up with is slipped, never replayed. */
        follow = chipset_cck;
        if (timeline->mode == RUNTIME_TIMELINE_HYBRID &&
            cpu_published_cck < follow)
            follow = cpu_published_cck;

        target = timeline->base_cck +
                 timeline_elapsed_cck(timeline, host_counter);
        timeline->realtime_target_cck = target;
        timeline->clamp_active = false;
        clamp = follow + timeline->max_cpu_backlog_cck;
        if (clamp < follow) /* saturate overflow */
            clamp = UINT64_MAX;
        if (target > clamp) {
            target = clamp;
            timeline->clamp_active = true;
        }

        /* Clamp release policy: a small wall deficit accumulated while the
         * CPU lagged is replayed (the machine stays honest to realtime),
         * but more than 250 ms means the CPU was effectively stalled —
         * rebase like a host gap instead of fast-forwarding the machine
         * through the stalled interval. Same threshold philosophy as the
         * host-counter gap rule above. */
        if (was_clamped && !timeline->clamp_active &&
            timeline->cck_per_second >= 4u &&
            target > timeline->horizon_cck &&
            target - timeline->horizon_cck > timeline->cck_per_second / 4u) {
            timeline->base_counter = host_counter;
            timeline->base_cck = timeline->horizon_cck;
            timeline->realtime_target_cck = timeline->horizon_cck;
            target = timeline->horizon_cck;
        }
    }

    if (target > timeline->horizon_cck)
        timeline->horizon_cck = target;
    return timeline->horizon_cck;
}

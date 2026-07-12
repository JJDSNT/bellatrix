#ifndef BELLATRIX_RUNTIME_TIMELINE_H
#define BELLATRIX_RUNTIME_TIMELINE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum RuntimeTimelineMode {
    RUNTIME_TIMELINE_CPU_DRIVEN = 0,
    RUNTIME_TIMELINE_REALTIME = 1,
    RUNTIME_TIMELINE_HYBRID = 2,
} RuntimeTimelineMode;

typedef struct RuntimeTimeline {
    RuntimeTimelineMode mode;
    uint64_t host_frequency;
    uint64_t cck_per_second;
    uint64_t base_counter;
    uint64_t last_counter;
    uint64_t base_cck;
    uint64_t horizon_cck;
    uint64_t realtime_target_cck;
    uint64_t max_cpu_backlog_cck;
    uint64_t generation;
    bool paused;
    bool clamp_active;
} RuntimeTimeline;

void runtime_timeline_init(RuntimeTimeline *timeline,
                           uint64_t host_frequency,
                           uint64_t cck_per_second,
                           uint64_t max_cpu_backlog_cck,
                           uint64_t host_counter,
                           uint64_t rigel_cck);
void runtime_timeline_set_mode(RuntimeTimeline *timeline,
                               RuntimeTimelineMode mode,
                               uint64_t host_counter,
                               uint64_t rigel_cck);
void runtime_timeline_set_paused(RuntimeTimeline *timeline,
                                 bool paused,
                                 uint64_t host_counter,
                                 uint64_t rigel_cck);
uint64_t runtime_timeline_update(RuntimeTimeline *timeline,
                                 uint64_t host_counter,
                                 uint64_t cpu_published_cck);

#endif

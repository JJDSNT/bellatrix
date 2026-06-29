#ifndef BELLATRIX_DEBUG_OS_DEBUG_H
#define BELLATRIX_DEBUG_OS_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

struct BellatrixMachine;

void os_debug_dump(struct BellatrixMachine *m);
int os_debug_rescue_waiting_signaled_tasks(struct BellatrixMachine *m);
int os_debug_suppress_process_requesters(struct BellatrixMachine *m);

#ifdef __cplusplus
}
#endif

#endif

#include "cpu/cpu_bus_policy.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                  \
                    __FILE__, __LINE__, #condition);                         \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main(void)
{
    const CpuBusPolicy *m68000 = cpu_bus_policy_by_name("68000");
    const CpuBusPolicy *m68040 = cpu_bus_policy_by_name("68040");

    CHECK(m68000 == &cpu_bus_policy_68000);
    CHECK(m68000->model == CPU_MODEL_68000);
    CHECK(m68000->cpu_clock_hz == 7093790u);
    CHECK(m68000->stalls_on_chip_access);

    CHECK(m68040 == &cpu_bus_policy_68040);
    CHECK(m68040->model == CPU_MODEL_68040);
    CHECK(!m68040->stalls_on_chip_access);
    CHECK(cpu_bus_policy_by_name("unknown") == m68040);
    CHECK(cpu_bus_policy_for_model(CPU_MODEL_68000) == m68000);
    CHECK(cpu_bus_policy_for_model(CPU_MODEL_68040) == m68040);
    CHECK(strcmp(m68000->name, "68000") == 0);
    return 0;
}

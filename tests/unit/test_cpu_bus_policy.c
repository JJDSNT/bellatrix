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
    const CpuBusPolicy *m68ec020 = cpu_bus_policy_by_name("68ec020");
    const CpuBusPolicy *m68040 = cpu_bus_policy_by_name("68040");

    CHECK(m68000 == &cpu_bus_policy_68000);
    CHECK(m68000->model == CPU_MODEL_68000);
    CHECK(m68000->cpu_clock_hz == 7093790u);
    CHECK(m68000->accounts_chip_access);
    CHECK(m68000->stalls_on_chip_access);
    CHECK(!m68000->chip_access_program_only);
    CHECK(m68000->chip_transfer_cpu_cycles == 4u);

    CHECK(m68ec020 == &cpu_bus_policy_68ec020);
    CHECK(m68ec020->model == CPU_MODEL_68EC020);
    CHECK(m68ec020->cpu_clock_hz == 7093790u);
    CHECK(m68ec020->accounts_chip_access);
    CHECK(!m68ec020->stalls_on_chip_access);
    CHECK(m68ec020->chip_access_program_only);
    CHECK(m68ec020->chip_transfer_cpu_cycles == 3u);

    CHECK(m68040 == &cpu_bus_policy_68040);
    CHECK(m68040->model == CPU_MODEL_68040);
    CHECK(!m68040->accounts_chip_access);
    CHECK(!m68040->stalls_on_chip_access);
    CHECK(m68040->chip_transfer_cpu_cycles == 0u);
    CHECK(cpu_bus_policy_by_name("unknown") == m68040);
    CHECK(cpu_bus_policy_for_model(CPU_MODEL_68000) == m68000);
    CHECK(cpu_bus_policy_for_model(CPU_MODEL_68EC020) == m68ec020);
    CHECK(cpu_bus_policy_for_model(CPU_MODEL_68040) == m68040);
    CHECK(strcmp(m68000->name, "68000") == 0);
    CHECK(strcmp(m68ec020->name, "68ec020") == 0);
    return 0;
}

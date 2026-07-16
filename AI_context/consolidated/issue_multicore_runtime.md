# Issue: Multicore Runtime — RPi3 Bare-Metal

> **DOCUMENTO HISTÓRICO.** A topologia canônica vigente está em
> `AI_context/consolidated/multicore_topology.md` e `src/runtime/topology.h`.
> Core0=Host Reactor e
> Core1=Emu68 não é mais baseline. O placement conservador devolve Emu68 ao
> Core 0 até que startup, IRQ física, vectors, timers e contexto por core sejam
> entendidos e reproduzidos com prova de equivalência.

> **DOCUMENTO HISTÓRICO — ABSORVIDO em 2026-07-12.** O runtime multicore atual
> e seus próximos gates são definidos pelas ISSUE-0051/0052. Planos antigos de
> epoch/worker não devem ser reintroduzidos por este texto.

## Status: SUPERADO (updated 2026-07-15)

Arquitetura ativa: Core0=Host Reactor, Core1=CPU, Core2=Rigel e Core3 reservado.
Musashi multicore, launcher, USB HID e HDMI foram validados em Raspberry Pi 3B.
O snapshot antigo Core3=IO abaixo é histórico; a descrição canônica atual está
em `docs/host_reactor.md` e `ISSUE-0045`.

## Contexto

Bellatrix usa os 4 cores do RPi3 com responsabilidades dedicadas. A arquitetura
foi iterada nos Sprints 27-29 (GFX/Paula/IO split sobre Agnus/Denise/Paula
internos) e depois substituída pela migração para o Rigel (chipset unificado,
externo). Este documento descreve a arquitetura **atual** (pós-migração Rigel
+ relabeling de cores conforme `multicore.md`); ver `project_refactoring_sprint11.md`
e `project_refactoring_sprint12.md` para o histórico da migração Rigel, e
`multicore.md` para a proposta original e a matriz de aderência.

## Arquitetura Atual

| Core | Domínio | Loop | Implementação |
|------|---------|------|--------------|
| 0 | Control Plane — boot, supervisor e Host Reactor/I/O físico | `bellatrix_core0_supervise()` → `bellatrix_runtime_io_step()` | `src/cpu/emu68/bellatrix.c`, `src/runtime/core_io.c` |
| 1 | CPU — Emu68 JIT (`M68K_StartEmu`/`MainLoop`) ou Musashi | `bellatrix_run_selected_cpu_backend()` / `MainLoop()` | `src/cpu/emu68/bellatrix.c`, `emu68/src/ExecutionLoop.c` |
| 2 | Chipset (Rigel) — Agnus+Denise+Paula+CIA via `rigel_step()` | `chipset_core_loop()` → `bellatrix_runtime_host_step()` | `src/runtime/core_chipset.c` |
| 3 | Acceleration Plane — reservado/estacionado | — | futuro RTG/AHI job worker |

Core 0 nunca executa CPU nem chipset: depois de `bellatrix_init()` lançar
Core 1/2, ele supervisiona e atende o Host Reactor a ~1 kHz. Em single-core
(`BELLATRIX_ENABLE_MULTICORE` off), nenhum core secundário é lançado — o
backend de CPU roda inline no boot core exatamente como antes desta mudança,
e o chipset avança de forma síncrona via `bellatrix_machine_advance()`.

## Fluxo de Ciclos

### Core 1 (CPU) → Core 2 (Chipset)
```c
// Emu68: bellatrix_emu68_report_jit_progress() em src/cpu/emu68/bellatrix.c
// Musashi: bellatrix_run_selected_cpu_backend() — mesmo padrão
if (PAL_Core_IsMulticoreEnabled())
    bellatrix_bridge_publish_cpu_cycles(cycles)
        → bellatrix_runtime_publish_cpu_cycles(m68k_cycles)   // core_chipset.c
           → s_cpu_cck_target += (m68k_cycles / 2)             // M68K → CCK
           → PAL_Runtime_WakeupChipset()                       // sev — acorda Core 2
else
    bellatrix_singlecore_advance_cpu_cycles(cycles)            // síncrono, sem cross-core
```

### Core 2 (Chipset)
```c
bellatrix_runtime_host_step():                    // src/runtime/core_chipset.c
  target = atomic_load(&s_cpu_cck_target)
  while (s_chipset_cck < target):
    rigel_step(rigel, min(remaining, CHIPSET_QUANTUM))
    on RIGEL_EVENT_IRQ_CHANGED   → bellatrix_machine_on_ipl_changed(ipl)  // IPL → Core 1
    on RIGEL_EVENT_FRAME_READY   → bellatrix_machine_on_frame_ready()
```

### Core 0 (Host Reactor)
```c
bellatrix_runtime_io_step():                       // src/runtime/core_io.c
  bt_host_step(&g_runtime.bluetooth)
  usb_host_step(&core->usb_host)
```
Não depende do contador de ciclos da CPU. Launcher e runtime usam o mesmo
service point; Core 3 não é lançado.

## Sincronização

### Lock de acesso ao chipset
`core_chipset_lock_acquire()` / `core_chipset_lock_release()` —
`atomic_flag` (TAS spinlock + WFE/SEV), definido em `src/runtime/core_chipset.c`.

**Holders**: `bellatrix_bridge_cpu_read()`/`bellatrix_bridge_cpu_write()` em
`src/cpu/cpu_bridge.c` — chamado tanto pelo caminho de fault do Emu68
(`bellatrix_bus_access()`) quanto pelas chamadas diretas do Musashi
(`musashi_read()`/`musashi_write()`). Cobre os dois backends uniformemente
desde esta mudança; antes, só o caminho Emu68 tinha o lock.

**No-op** quando `BELLATRIX_ENABLE_MULTICORE` não está definido (single-core:
não há core de chipset concorrente).

### Cycle target atômico
`s_cpu_cck_target` (`_Atomic uint64_t`, em `core_chipset.c`) — Core 1 produz
(soma), Core 2 drena (compara e avança `rigel_step()` em blocos de
`CHIPSET_QUANTUM=128` CCK).

### WFE/SEV
- Core 2 dorme em WFE quando idle; Core 0 dirige supervisão e Host Reactor
- Core 1 emite SEV ao publicar ciclos (`bellatrix_runtime_publish_cpu_cycles`)
- Cada core emite SEV ao liberar o lock de acesso ao chipset

## Secondary Boot (start.c — BELLATRIX)

```c
secondary_boot(cpu_id):
  if BELLATRIX:
    if cpu_id == 1: bellatrix_core1_entry()  → espera s_cpu_entry (CPU backend)
    if cpu_id == 2: bellatrix_core2_entry()  → espera s_chipset_entry → chipset_core_loop()
    if cpu_id == 3: bellatrix_core3_entry()  → espera s_io_entry → chipset_io_loop()
    else: while(1) wfe
```

```c
// emu68/src/aarch64/start.c, cauda de boot() — roda no boot core (Core 0)
bellatrix_init();
bellatrix_launch_cpu_and_park(
    bellatrix_cpu_backend_owns_execution_loop()
        ? bellatrix_run_selected_cpu_backend   // Musashi
        : bellatrix_emu68_cpu_entry);          // Emu68: wraps M68K_StartEmu(0, NULL)
```

`bellatrix_launch_cpu_and_park()` (em `src/cpu/emu68/bellatrix.c`): single-core
chama `entry()` direto no boot core (nunca retorna); multicore chama
`PAL_Core_LaunchCpu(entry)` e estaciona o boot core em `wfe` para sempre.

`PAL_Core_LaunchCpu()`     → `s_cpu_entry = entry; dsb+sev`      (Core 1)
`PAL_Core_LaunchChipset()` → `s_chipset_entry = chipset_core_loop; dsb+sev` (Core 2)
`PAL_Core_LaunchIO()` permanece dormente para uma migração futura; não é chamado.

`PAL_Core_LaunchChipset()` chamado de dentro de
`bellatrix_init()` quando `BELLATRIX_ENABLE_MULTICORE` está definido;
`PAL_Core_LaunchCpu()` chamado depois, da cauda de `boot()` em start.c.

## Modo Single-Core (Harness / Debug)

`pal_posix.c`: `PAL_Core_IsMulticoreEnabled() = 0`; `PAL_Core_LaunchCpu/Chipset/IO`
são no-ops. `bellatrix_runtime_publish_cpu_cycles()` cai no stub fraco de
`pal_core.c`/`pal_posix.c` quando não há override forte — mas o harness não
passa por `bellatrix_init()`/`start.c` (driver próprio em `tools/harness/main.c`),
então essa cadeia de boot não se aplica a ele.

## Histórico: migração Sprint 27-29 → Rigel (obsoleto)

Antes da migração para o Rigel, o chipset era dividido em módulos internos
(Agnus/Denise/Paula/CIA) espalhados por dois cores: "Core 1 GFX"
(`core_gfx.c`: Agnus+Denise+DMA) e "Core 2 Paula"
(`core_audio.c`: áudio+disk+serial+IRQ). Esses arquivos não existem mais — o
Rigel unificou todo o domínio do chipset em `rigel_step()`, rodando
single-thread, e o esqueleto de "Core 2 Audio" ficou morto (nunca lançado)
até ser reaproveitado nesta mudança para se tornar o Core 2 = Chipset real.

## Pendentes / Riscos

### Barreira temporal de MMIO crítico (lacuna da proposta original)
`bellatrix_bus_access()`/`bellatrix_bridge_cpu_read/write()` não forçam o
chipset (Core 2) a alcançar o tempo da CPU (`rigel_step_until(cpu_time)`)
antes de aplicar leitura/escrita crítica (`DMACON`, `INTENA/INTREQ`,
`COPJMP`, `BLTSIZE`, etc.) — ver `multicore.md`, seção "MMIO crítico". Isso é
uma lacuna funcional aberta, não resolvida por este relabeling de cores.

### Scheduler por deadline (lacuna da proposta original)
O scheduler ainda é um acumulador de ciclos com quantum fixo
(`CHIPSET_QUANTUM=128`), não um `T = min(próximo evento...)` deadline-oriented.
**Decisão e plano registrados** em `issue_core0_arbiter_scheduler.md`: árbitro no
Core 0 (hoje parado), rendezvous de epoch substitui o lock por acesso, pré-requisito
= `emu68_run_until` + `rigel_next_event_tick`.

### RuntimeMailbox / Event Queue
`src/runtime/event.c/h` e `src/runtime/mailbox.c/h` existem mas não estão
integrados — sincronização hoje é só atomics + lock + WFE/SEV.

### Interrupções ARM de periférico não são roteadas (por decisão)
USB/Bluetooth/UART são servidos pelo Host Reactor no Core 0, nunca por IRQ ARM na
vector table do Emu68 — que é hardcoded para o modelo PiStorm (`ARM IRQ ≡ Amiga
INT6→IPL6`). Ver `issue_emu68_pistorm_interrupt_contract.md` para o contrato
portátil e por que um "gateway de IRQ no Core 0" foi rejeitado.

## Arquivos Relevantes
- `src/cpu/emu68/bellatrix.c` — `bellatrix_launch_cpu_and_park`,
  `bellatrix_run_selected_cpu_backend`, `bellatrix_bus_access`, `bellatrix_init`
- `src/cpu/cpu_bridge.c` — `core_chipset_lock_acquire/release` aplicado a
  `bellatrix_bridge_cpu_read/write` (cobre Emu68 e Musashi)
- `src/host/raspi3/pal_core.c` — `PAL_Core_LaunchCpu/LaunchChipset/LaunchIO`,
  `bellatrix_core1/2/3_entry`, `chipset_core_loop`, `chipset_io_loop`
- `src/host/posix/pal_posix.c` — stubs single-core
- `src/runtime/core_chipset.h/.c` — Core 2: Rigel step + lock de acesso
- `src/runtime/core_io.h/.c` — Core 0: Host Reactor, USB/BT/UART/console
- `emu68/src/aarch64/start.c` — `secondary_boot()` BELLATRIX block,
  `bellatrix_emu68_cpu_entry()` wrapper, cauda de `boot()`

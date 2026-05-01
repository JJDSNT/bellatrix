src/chipset/agnus/
├── copper.c              # lógica principal do Copper
├── copper.h              # API pública do Copper
├── copper_service.c      # agendamento/execução temporal do Copper
├── copper_service.h      # interface do serviço
├── copper_regs.c         # writes/reads em COP1LC, COP2LC, COPJMP, etc.
├── copper_regs.h

O papel ficaria assim:

copper.c

Executa instruções: MOVE, WAIT, SKIP, JMP.

copper_regs.c

Recebe writes nos registradores custom:

COP1LCH/COP1LCL
COP2LCH/COP2LCL
COPJMP1
COPJMP2
copper_service.c

Decide quando o Copper roda:

copper_service_begin_frame();
copper_service_step_until_beam();
copper_service_wake_after_wait();

Ou seja: o Copper deixa de ser chamado como “1 instrução por agnus_step” e passa a ser um serviço temporal de Agnus, sincronizado com o beam.

A ideia central:

Agnus owns time.
Copper is a scheduled service inside Agnus.
Bitplanes must not snapshot the line before Copper had a chance to run
all pending effects for that beam position.

beam_step(...);

copper_service_poll(...);   // wake de WAIT (CRÍTICO)

copper_service_step(...);   // execução normal

bitplanes_step(...);        // snapshot depois do Copper
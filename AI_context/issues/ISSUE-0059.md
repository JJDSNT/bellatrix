# ISSUE-0059 — Assimilação dos avanços Bluetooth no rebaseline Core0

Status: implementação integrada; revisão estática concluída; build e hardware
deliberadamente não executados nesta etapa.

## Objetivo

Assimilar os ganhos funcionais de `issue-0054-bt-physical-irq` sem restaurar
as premissas arquiteturais superadas daquela branch. A autoridade permanece:

- Emu68/JIT e seus vetores no Core 0;
- MMIO por Data Abort/fault handler como baseline;
- IRQ ARM normal para PL011/Bluetooth, nunca FIQ;
- Core 3 como owner do reactor USB/BT e dos modais em runtime;
- mini-UART como log independente desde o primeiro boot;
- PL011 pertencendo exclusivamente ao Bluetooth desde o primeiro boot;
- nenhuma IRQ física de host convertida implicitamente em IRQ/IPL Amiga.

## Funcionalidade assimilada

- `launcher.c` foi reduzido a coordenador e as telas foram separadas em
  `btscan`, `media_selection` e `launcher_ui`.
- Um roteador HID único recebe teclado/mouse USB e Bluetooth, mantém ownership
  por dispositivo e impede releases duplicados ou vazamento de eventos.
- F11 publica a ação host `BTSCAN`; F12 publica `MEDIA`. Press e release são
  consumidos no host e nunca alcançam o Amiga.
- O dispatcher roda depois do passo normal do reactor Core 3. Callback HID
  apenas publica intenção; framebuffer, FAT e BTStack não rodam na exceção.
- `Esc` cancela/fecha; `Enter` confirma/fecha. Um modal ativo exclui o outro.
- F12 lista ADF, faz staging em buffer privado e efetua somente o commit curto
  sob `core_chipset_lock`; ISO/HDF hot-swap continuam fora do escopo.
- A flag atômica de modal é publicada antes do primeiro desenho, evitando que
  a apresentação de frame pelo Core 2 sobrescreva a abertura da tela.
- F11 abre scan/pairing sem bloquear o reactor; conexão e persistência da nova
  link key continuam em background depois do fechamento da tela.
- Ao identificar um mouse HID durante um scan de pareamento, o fluxo padrão
  inicia a conexão imediatamente, que é o único comportamento comprovado pela
  evidência anterior. No F11 o pedido ocorre no passo seguinte do Core 3 para
  manter HCI fora do callback, sem espera artificial e sem aguardar `Enter`.
- Reconexão HID tem tentativas assíncronas e limitadas. Pares conhecidos podem
  autenticar fora da janela; dispositivos desconhecidos continuam negados.
- Relatórios HID de erro `0x01..0x03` são ignorados em USB e BT.
- Mouse Classic HID com Report ID 1 e deslocamentos signed-16 little-endian é
  decodificado sem confundir outros report IDs com movimento.
- Silêncio total do controlador é distinguido de falha no drain: FIFO PL011
  não vazio denuncia o caminho IRQ/host e suprime power-cycle mascarador.
- Hardware Error recebido dentro de callback BTstack apenas publica recovery;
  a mutação de HCI/transporte ocorre depois, no reactor Core 3.
- O antigo atraso de dois bilhões de `nop` após BTSCAN foi removido; mini-UART
  permanece a fonte primária de diagnóstico e BTSCAN.TXT segue opcional.

## O que não foi trazido

- FIQ para Bluetooth, seu trampoline, ABI ou gates.
- Substituição de `vectors.c`/fault handler por API pública de máquina.
- Topologias em que Emu68 sai do Core 0.
- Conversão de IRQ ARM em `INT.ARM`, EXTER ou IPL Amiga.
- Documentação antiga que tratava FIQ e acesso fault-independent como alvo.

## Verificação desta etapa

- [x] Conflitos de merge removidos e `git diff --check` limpo.
- [x] Comentários/ownership revisados para Core0=Emu68, Core2=Rigel,
  Core3=reactor/modal.
- [x] Caminho F11/F12 não chama guest keyboard/mouse nem `INT.ARM`.
- [x] Caminho de ADF usa staging e lock apenas no commit.
- [x] PL011 permanece BT e mini-UART permanece log/Paula.
- [ ] Build: não solicitado nesta etapa.
- [ ] QEMU: não repetido nesta etapa.
- [ ] Hardware: proibido até nova autorização do usuário.

## Próximos gates

1. Revisão dirigida do recovery/reconnect e budgets do Core 3.
2. Build nas variantes somente quando voltar a ser solicitado.
3. Teste funcional F11/F12 em hardware pelo usuário, quando conveniente.
4. Depois da evidência, promover o contrato estabilizado aos documentos
   canônicos e a `main`.

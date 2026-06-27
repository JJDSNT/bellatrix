# AI_context — Bellatrix

Memória operacional do projeto. Lida por agentes antes de cada sessão de trabalho.

## Fluxo

```text
Issue (backlog/doing)
 ↓
Implementação
 ↓
Review
 ↓
Consolidation (conhecimento estabilizado)
 ↓
Documentation Update (docs/)
```

## Estrutura

- `issues/` — trabalho ativo e pendente. Arquivos vivos, modificáveis pelos agentes.
- `consolidated/` — conhecimento estabilizado (implementações concluídas + investigações históricas).
  Ver `consolidated/README.md` para a regra de promoção.
- `templates/` — modelos para novas issues, specs e entradas consolidadas.
- `specs/` — especificações formais. Ver `specs/README.md`.
- `metadata/` — reservado para view derivada futura (não populado ainda).

## Status permitidos

`backlog`, `ready`, `doing`, `review`, `done`, `consolidated`, `blocked`

## Prioridades

`low`, `medium`, `high`, `critical`

## Tipos

`feature`, `bug`, `refactor`, `research`, `docs`, `infra`

## Visão rápida do estado

```bash
# Issues por status
for f in AI_context/issues/ISSUE-*.md; do head -8 "$f" | grep "^status:"; done | sort | uniq -c

# Tabela id/título/status/prioridade
for f in AI_context/issues/ISSUE-*.md; do
  head -12 "$f" | grep -E "^(id|status|priority|title):"
  echo
done
```

## Contexto do projeto

Bellatrix é um emulador de chipset Amiga que roda sobre Emu68 (JIT m68k→AArch64)
em Raspberry Pi 3, sem hardware Amiga. Ver `CLAUDE.md` e `docs/` para arquitetura.

Fases ativas:
- **Fase 6**: integração Emu68 JIT (MMIO performance, bus API, quantum window)
- **Fase 7**: AROS desktop (trackdisk, CD-ROM, autoconfig)

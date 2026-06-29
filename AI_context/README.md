# AI_context — Bellatrix

## Objetivo

Esta pasta representa a memória operacional do projeto.

Ela é utilizada por agentes para:

- planejamento
- execução
- rastreamento
- consolidação de conhecimento

## Fluxo

```text
Issue
 ↓
Implementação
 ↓
Review
 ↓
Consolidation
 ↓
Documentation Update
```

## Estrutura

- `issues/` — trabalho ativo. Documentos vivos, modificáveis pelos agentes.
- `specs/` — especificações formais. Ver `specs/README.md`.
- `consolidated/` — conhecimento estabilizado. Ver `consolidated/README.md`
  para a regra de promoção.
- `templates/` — modelos para novas issues, specs e entradas consolidadas.
- `metadata/` — reservado para uma futura view derivada/cacheada (não
  populado nesta fase; leitura é feita diretamente do frontmatter dos
  arquivos markdown).

## Status permitidos

`backlog`, `ready`, `doing`, `review`, `done`, `consolidated`, `blocked`

## Prioridades

`low`, `medium`, `high`, `critical`

## Tipos

`feature`, `bug`, `refactor`, `research`, `docs`, `infra`

## Visão rápida do estado

Sem precisar de UI, API ou MCP — `grep` direto nos arquivos já responde
"o que está em aberto?". `head` antes do `grep` limita a busca ao bloco de
frontmatter (sempre as primeiras linhas), evitando casar texto solto no
corpo da issue que por acaso comece com `status:`/`priority:`.

Quantas issues por status:

```bash
for f in AI_context/issues/ISSUE-*.md; do head -8 "$f" | grep "^status:"; done | sort | uniq -c
```

Tabela rápida (id, título, status, prioridade) de todas as issues:

```bash
for f in AI_context/issues/ISSUE-*.md; do
  head -12 "$f" | grep -E "^(id|status|priority|title):"
  echo
done
```

Mesma ideia vale para `AI_context/specs/SPEC-*.md` (campos `id`/`status`/
`title`, sem `priority`).

---
id: ISSUE-0028
title: "Avaliar/adicionar suporte SFS na ferramenta HDF (tools/hdf)"
status: open
priority: low
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - hdf
  - sfs
  - tooling
related_files:
  - tools/hdf/hdf.py
  - external/amitools
---

# Contexto

`tools/hdf/hdf.py` (amitools) hoje cobre apenas OFS/FFS (DOS0–DOS5).
Muitos HDFs prontos (AROS/OS3.x pré-configurados) usam SFS (dostype
`SFS\0`/0x53465300, SFS2 `SFS\2`), que o xdftool **não** lê nem escreve.

# Objetivo

Se for o caso (i.e., se precisarmos manipular conteúdo de HDFs SFS, não só
bootá-los), adicionar à ferramenta:

- Detecção/report de partições SFS no `analyze` (dostype já aparece via
  rdbtool; falta ao menos identificar e não falhar no FS check)
- Leitura de arquivos de partições SFS
- (Opcional) escrita/criação SFS

# Abordagens candidatas

- Backend SFS próprio em Python dentro de tools/hdf (formato documentado;
  fonte do SFS é aberta)
- Usar um HDF FFS como intermediário e deixar o próprio AmigaOS/AROS copiar
  para SFS em runtime (zero código host-side)
- Verificar se amitools upstream aceita contribuição de backend SFS

# Nota

Para *boot* de partição SFS, o RDB precisa carregar o driver SFS via LSEG
(`rdbtool fsadd`), independente do suporte host-side.

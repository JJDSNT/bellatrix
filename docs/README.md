# docs — Bellatrix

## Purpose

Reference documentation for the project: what the system **is** today —
architecture, contracts between components, decisions in force.

## Relationship to AI_context/

| | `docs/` | `AI_context/` |
|---|---|---|
| Describes | the current state | work in progress |
| Tense | present | past and future |
| Lives while | the decision holds | the issue is not consolidated |

An issue consolidated into `AI_context/consolidated/` normally results in a
document here being created or updated. No file in `docs/` should narrate
investigation or chronology — that belongs to the issue it came from.

## Organization

One file per subject, named in `snake_case.md`, with no numbering and no
folder hierarchy until the volume justifies it. A document describing
something that no longer holds is not deleted: it gets a correction header
stating what changed and why, and stays for as long as any part of its design
is still relevant.

## Language

All project content — documentation, issues, specs, code comments and commit
messages — is written in English.

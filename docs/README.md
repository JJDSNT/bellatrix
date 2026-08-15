# docs — Bellatrix

## Purpose

Reference documentation for the project: what the system **is** today —
architecture, contracts between components, decisions in force.

This folder is published with GitHub Pages at
<https://jjdsnt.github.io/bellatrix/>.

## Viewer

[`viewer/`](viewer/) is a single page that reads `AI_context/` live from the
GitHub API: the issue board, blockers, specs and the consolidated history, with
nothing to clone, build or install. Published alongside these documents at
<https://jjdsnt.github.io/bellatrix/viewer/>, and it works just as well opened
from disk. `?repo=owner/repo` points it at any other repository following the
same convention.

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

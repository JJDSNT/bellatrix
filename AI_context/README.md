# AI_context — Bellatrix

## Purpose

This folder is the project's operational memory.

Agents use it for:

- planning
- execution
- tracking
- knowledge consolidation

## Flow

```text
Issue
 ↓
Implementation
 ↓
Review
 ↓
Consolidation
 ↓
Documentation Update
```

## Structure

- `issues/` — active work. Living documents, editable by agents.
- `specs/` — formal specifications. See `specs/README.md`.
- `consolidated/` — stabilized knowledge. See `consolidated/README.md`
  for the promotion rule.
- `templates/` — models for new issues, specs and consolidated entries.
- `metadata/` — reserved for a future derived/cached view (not populated
  at this stage; reads go straight to the frontmatter of the markdown
  files).

## Allowed statuses

`backlog`, `ready`, `doing`, `review`, `done`, `consolidated`, `blocked`

## Priorities

`low`, `medium`, `high`, `critical`

## Types

`feature`, `bug`, `refactor`, `research`, `docs`, `infra`

## Blockers (`blockers`)

Optional frontmatter field, same format as `tags` (a plain list of
strings):

```yaml
blockers:
  - "validation on real hardware (Pi 3B) — awaiting user authorization"
```

`status: blocked` is too coarse for the most common pattern in this
project: an issue stays `doing` (real software work is moving forward)
while one or more specific checklist items are held up waiting on
something narrow — typically authorization to test on real hardware.
`blockers` exists for that. It does not replace `status`; it is a short,
readable list of what is held up and why, visible regardless of the
issue's status (a `doing` issue with a non-empty `blockers` is still
active work, it just has one stuck end). Use short free text, not an
enum — the reason matters more than the category.

## Quick view of the current state

No UI, API or MCP needed — a plain `grep` over the files already answers
"what is open?". `head` before `grep` limits the search to the
frontmatter block (always the first lines), avoiding matches on loose
body text that happens to start with `status:`/`priority:`.

Issue count per status:

```bash
for f in AI_context/issues/ISSUE-*.md; do head -8 "$f" | grep "^status:"; done | sort | uniq -c
```

Quick table (id, title, status, priority) of every issue:

```bash
for f in AI_context/issues/ISSUE-*.md; do
  head -12 "$f" | grep -E "^(id|status|priority|title):"
  echo
done
```

The same applies to `AI_context/specs/SPEC-*.md` (fields `id`/`status`/
`title`, no `priority`).

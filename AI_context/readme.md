Use the `AI_context/` folder to document the progress of this session and plan the next one.

## Goals
- Track what was done in the current session
- Define what should be done in the next session
- Maintain alignment with the project roadmap
- Ensure that changes do not break the build or runtime behavior

## Instructions

- Organize the work into **issues**, grouped in a **sprint-like format**:
  - Each issue should represent a unit of delivered value
  - Include:
    - Description
    - What was implemented
    - Current status
    - Next steps

- Always check existing files inside `AI_context/` to understand:
  - What has already been done
  - What is still pending

- Use `docs/` as the source of truth for:
  - Project goals
  - Requirements
  - Roadmap

- If needed, use `referencias/` for additional context (e.g., FS-UAE related files)

## Validation (MANDATORY)

- After any significant change (new feature, refactor, structural change):
  - You MUST run at least one of the following:
    - `./scripts/build.sh`
    - `./run.sh qemu`

- The goal is to ensure that:
  - The project still compiles
  - No integration point with Emu68 was broken

- If tests are available:
  - Prefer running tests instead of (or in addition to) build
  - Example:
    - `./run.sh qemu` (smoke test)
    - or any test under `tests/`

- If the build or run fails:
  - The issue must NOT be marked as complete
  - The failure must be documented in `AI_context/`

## File conventions

- All files must be written in **English**
- Add a header comment at the top of each file with its full path, for example:

```
// AI_context/context.md
```

## Behavior

- If something is unclear, ask questions before proceeding
- If you identify better approaches, suggest improvements proactively
- Focus on clarity, traceability, and continuity between sessions
- Prefer safe, incremental changes over large unvalidated modifications

## Output expectation

- Update or create files inside `AI_context/`
- Keep entries concise but informative
- Ensure the context is sufficient for the next session to continue seamlessly
- Always reflect whether the last change was validated (build/test/run)
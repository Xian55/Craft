---
name: craft-docs
description: Keep the project's documentation current after a code change — the root CLAUDE.md and the memory/ dir. Use at the end of any feature or structural change (it's the final step of craft-feature), or whenever commands, architecture, conventions, gotchas, or machine/deploy facts shift. Keep everything lean — CLAUDE.md loads into context every session.
---

# craft-docs — keep CLAUDE.md and memory current

Docs rot fast. After a change, update what the change touched, and keep it **lean** — the root
`CLAUDE.md` and the `MEMORY.md` index load into context every session, so they must stay short and
scannable. craft has a **single** root `CLAUDE.md` (no per-subsystem CLAUDE.md files); detailed,
per-topic facts live as individual files in `memory/`.

## What to update, and when

- **Root `CLAUDE.md`** — a command/script changed, a new file or subsystem was added to `src/`, a
  convention or a gotcha changed, a new skill was added, a build/test/deploy flag moved, or a
  documented number (RD_MAX, memory sizes, tick rates) changed. Sections to keep honest: Quick
  start, Build, Tests, Architecture (the `src/` map), Debug hooks, Gotchas learned the hard way.
- **`memory/` dir** — anything that isn't derivable from the code and matters across sessions:
  - `user` — who the user is / preferences.
  - `feedback` — guidance on how to work (with the why + how-to-apply).
  - `project` — ongoing goals/constraints not in the code or git history (convert relative dates to
    absolute).
  - `reference` — machine paths, tool versions, deploy hosts, URLs, dashboards.
  Each memory is one file with frontmatter; add a one-line pointer in `MEMORY.md`
  (`- [Title](file.md) — hook`). Update the existing file rather than duplicating; delete a memory
  that turns out wrong. Don't record what the repo already captures (code structure, past fixes,
  git history).

## Rules
- Lean and current beats complete and stale. Delete outdated lines; don't append forever.
- CLAUDE.md is a **map**, not a manual: what each file owns, the gotchas, how to extend — not a code
  dump. Deep per-topic detail goes in a `memory/` file, linked with `[[name]]`.
- If you changed a documented command, path, or number, update the **exact** figure.
- ASCII only in `assets/lang/*.lang` values (raylib default font has no accents) — a doc reminder
  worth keeping if you touch localization.

## Quick check
```bash
git diff --name-only        # which files/subsystems changed -> what in CLAUDE.md / memory to revisit
git status
```
Verify any file path, function, or flag a memory names still exists before leaving it in — memories
are point-in-time and go stale.

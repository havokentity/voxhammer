# ADR-001: cvar schema versioning

- **Status:** Accepted
- **Date:** 2026-05-20

## Context

`cvars.toml` is user-editable and persists across engine versions. As the
engine evolves, cvars get added, renamed, or removed. A user's customized file
must keep working — we cannot silently drop their settings, and we cannot let a
stale file from an old build break a new one (or vice versa).

## Decision

- `cvars.toml` carries a `[meta]` table with `schema_version = N`.
- The engine ships **additive** migrations from version K to K+1 — they never
  delete user customizations silently. Removals happen only via an explicit,
  documented migration step.
- On load: read `schema_version`, apply all pending migrations in sequence
  (`ApplyMigrations` in `engine/cvar/Console.cpp`), then apply the values.
  On the next save, the file is written at the current `kSchemaVersion`.
- **Unknown cvars are preserved**, not discarded, with a
  `warn: unrecognized cvar X` log line. They round-trip through save so a file
  shared between builds (or hand-edited ahead of a feature) survives.
- Each migration is documented in [`docs/cvar-migrations.md`](../cvar-migrations.md).

`Console::kSchemaVersion` is the single source of truth for the current
version (currently `1`).

## Consequences

- Forward/backward compatible config files; no data loss on upgrade.
- A small amount of migration bookkeeping per schema bump.
- Tooling can inspect `[meta].schema_version` to reason about a file's vintage.

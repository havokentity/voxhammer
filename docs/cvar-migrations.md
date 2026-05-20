# cvar schema migrations

Log of `cvars.toml` schema migrations. Policy: [ADR-001](adr/adr-001-cvar-versioning.md).
The current version is `Console::kSchemaVersion` in `engine/cvar/Console.cpp`.

Migrations are **additive**: a step transforms the parsed key/value map from
version K to K+1. Never silently delete a user's customization; removals require
an explicit, documented step. Unknown cvars are preserved and warned, not
dropped.

## v1 — baseline (2026-05-20)

Initial schema. No transforms. Files written by the first release carry
`schema_version = 1`. A file with a missing or `0` version is treated as v0 and
adopted into v1 with no key changes.

## Adding a migration (template)

When bumping to v`N+1`:

1. Increment `Console::kSchemaVersion` to `N+1`.
2. Add a `from_version == N` branch in `Console::ApplyMigrations` that mutates
   the `kv` map (rename keys, remap enum values, drop deprecated keys, etc.).
3. Document the step here:

   ### v`N+1` — <summary> (YYYY-MM-DD)
   - `old.cvar.name` → `new.cvar.name`
   - dropped `deprecated.cvar` (reason)

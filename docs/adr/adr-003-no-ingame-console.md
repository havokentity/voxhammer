# ADR-003: no in-game console

- **Status:** Accepted
- **Date:** 2026-05-20

## Context

Quake/Source/idTech-lineage engines shipped a `~` in-game console. Voxhammer
deliberately does not — not in v0.1, not later. The control surface is the
**web console** (`https://localhost:27960/`) plus the **keybindings** system.

## Decision

No in-game console. Every scenario one might want it for has a better answer:

| Scenario | Voxhammer answer |
|---|---|
| Forgot the web console password | `--remote-password=NEW` launch flag (re-hashes, rewrites `remote.password_hash`, logs the change). |
| Cert broken / browser won't connect | `console.rotate_cert`, `--reset-cert`, or delete `%APPDATA%/Voxhammer/cert/`. |
| `cvars.toml` corrupted | Engine refuses to start, names the broken file; edit or delete to regenerate. |
| Quick quit in fullscreen | Esc bound to quit in keybindings. |
| Toggle debug states during play | **Keybindings** map a key to a cvar toggle or command (e.g. F3 → `debug.show_brick_grid`). |
| Live Lua REPL | The web console's multi-line editor beats typing in-engine. |
| Bring-up before the web console is alive | ImGui telemetry overlay — display-only, no input capture, no parser. |

**Maintenance argument:** an in-game console would duplicate the schema,
autocomplete, and history that already exist in the web UI. The two would drift,
and the in-game one would always be the worse experience. Skipping it removes a
whole duplicate surface and keeps engine input handling to game inputs only.

## Consequences

- Simpler input handling (no console capture mode).
- Better streaming/OBS workflow (console on a second monitor, never obscuring
  gameplay).
- A hard dependency on the web console + keybindings for all live control.

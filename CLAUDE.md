# Coop Workbench — guide for AI coding assistants

This file orients an AI agent (Claude Code, Cursor, etc.) working in this repo. Humans: see
[README.md](README.md), [ARCHITECTURE.md](ARCHITECTURE.md), and [CONTRIBUTING.md](CONTRIBUTING.md).

## What this is

An **in-memory mod** that adds real-time co-op building to Stormworks: it hooks the game's own
editor commands and calls them, so edits made on one machine render live on a partner's, synced
peer-to-peer over Steam. It **modifies no game files.**

## The one thing to know about testing

**You (the AI) cannot load the mod into the game — a human does that step.** Injecting a DLL into a
running process is blocked for AI agents by design, and that's expected. The workflow is a
partnership:

- **The AI** reads code, **builds**, edits, fixes, and reasons about the architecture.
- **The human** runs the injector and drives the **in-game two-machine test**, then reports results.

So: make your change, build it, and hand the human a clear test instruction. Don't try to inject or
launch the game yourself.

## Build

Requires **MSVC x64 build tools** (VS2022 Build Tools) with `ml64` (MASM), on **Windows**.

```bat
cd sw-coop-build
build-coop.cmd        & rem  builds coopworkbench.dll (co-op sync + the in-world overlay, one DLL)
```

Build scripts are script-relative (`%~dp0`), so they run from wherever the repo lives.

## Dev loop

1. Edit `sw-coop-build/hook_dll/coop.cpp` (or a `detour_*.asm`, or `wsdraw.cpp`).
2. `sw-coop-build/reload-build.ps1` — unloads the live mod and rebuilds (the human keeps the game
   running).
3. The **human** re-injects with `inject.ps1` and runs the test in-game.
4. Verify against `sw-coop-build/coop-package/TESTING.txt` (the two-machine checklist).

## How it fits together

Read [ARCHITECTURE.md](ARCHITECTURE.md) first. In short: **detect** local edits via inline hooks →
**send** over Steam P2P → **apply** on the peer by forging the same editor command (echo-suppressed,
on the main thread). Hooked addresses are found by signature scan at startup, so they survive game
patches.

## Conventions & guardrails

- **Match the surrounding code style** in whatever file you touch.
- **Never modify game files or ship game assets** — this stays a clean in-memory mod.
- **Never commit**: `coop-peer.txt`, `*-log.txt`, built `*.dll`, release `*.zip` (all gitignored),
  or any real **SteamID64** (they identify a person's Steam account).
- Keep PRs small and focused; say what you tested (gamemode / workbench / one or two machines).

## Testing status (don't assume more than this)

Confirmed only in **Custom gamemode, starter vehicle workbench, both players empty from the same
origin block.** Other workbenches, career/survival, and different world seeds are **unverified** —
coordinates may not line up there yet. Treat broader coverage as an open question, not a given.

## Where things live

| Path | What |
|---|---|
| `sw-coop-build/hook_dll/coop.cpp` | the mod (detect + Steam P2P + forge-apply) |
| `sw-coop-build/hook_dll/detour_*.asm` | inline-hook detours |
| `sw-coop-build/hook_dll/wsdraw.cpp` | the in-world partner-camera overlay |
| `sw-coop-build/coop-package/` | shippable package + setup/test docs |
| `ARCHITECTURE.md` / `CONTRIBUTING.md` | how it works / how to contribute |

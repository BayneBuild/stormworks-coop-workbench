# Architecture

A tour of how Coop Workbench works, for anyone who wants to hack on it. This is the conceptual map;
the exact hooked addresses and struct layouts live in comments next to the code that uses them
(`sw-coop-build/hook_dll/`).

## The core idea

Stormworks builds vehicles from a voxel grid. The naive way to add a block from code would be to
write into that grid directly — but the game only re-renders when its **own** editor commands run, so
a direct write produces an invisible, half-broken block.

So instead of touching the grid, the mod **calls the game's own commands**. It finds the editor's
place / delete / paint / connect functions in memory and, when a remote edit arrives, **calls them
exactly as a mouse click would.** Every synced action runs through the real code path and renders
identically to something you did by hand. This is the single most important design decision in the
project.

## The loop

```
   YOUR GAME                          STEAM P2P                    PARTNER'S GAME
 ┌───────────┐   detect edit      ┌───────────────┐   apply       ┌───────────┐
 │  editor   │ ─────────────────► │  relay / SDR  │ ────────────► │  editor   │
 │  (hooks)  │   {type,pos,rot,   │  (no server)  │   forge same  │  (hooks)  │
 └───────────┘    color, ...}     └───────────────┘   command     └───────────┘
```

### 1. Detect (local edits)

Inline hooks sit on the editor functions that run when **you** place, drag, delete, paint, or connect
something. The hook records the edit (part type by its definition name, voxel position, rotation,
color, connection endpoints) into a lock-free ring buffer and lets the original function continue
untouched. A worker drains the ring and sends the edit.

Drags and area-fills are bursty, so captures are **ring-buffered** rather than single-slot — a fast
eraser drag or a 50-block fill won't drop edits.

### 2. Send / receive (Steam P2P)

Transport is Steam's `ISteamNetworkingMessages` over the relay/SDR network: **no server, no port
forwarding, no IP addresses**, and you don't need to be in the same Stormworks multiplayer session.
The peer is identified purely by **SteamID64** (read from `coop-peer.txt`). Messages are small,
fixed-layout records, one per edit, plus a keepalive.

### 3. Apply (remote edits)

On the receiving side, the mod resolves the part's definition name back to a template, sets the
rotation/color, and **forges the matching editor command** — so the block/paint/wire renders live.
Two details keep this safe:

- **Echo suppression.** A `g_suppress` flag gates the detect hooks while the mod is applying its own
  forged command, so an applied edit is never re-broadcast in a loop.
- **Main-thread apply.** Forged commands run on the game's main thread (pumped from a hook on the
  Steam `RunCallbacks` path), not from the network worker — so we never mutate editor state from the
  wrong thread.

## Surviving game updates

Hardcoded addresses shift every time the game patches. At startup the mod **scans the loaded module
for a unique byte-pattern from each target function's compiled prologue** and hooks whatever address
it finds, falling back to a known offset only if a scan fails. This is why a game update is less
likely to break it.

## The partner overlay

A second, self-contained piece (`wsdraw.cpp`) draws **custom UI inside the game's 3D scene** — the
"where is my partner looking" marker. It hooks the frame present (`SwapBuffers`) and draws with the
game's own OpenGL, and it captures the workbench camera so it can project any world point to the
screen. It has its own small Steam channel and shares state with the co-op mod through a named
memory block ([`coop_hud_state.h`](sw-coop-build/hook_dll/coop_hud_state.h)).

## File map

| Path | What |
|---|---|
| [`sw-coop-build/hook_dll/coop.cpp`](sw-coop-build/hook_dll/coop.cpp) | the mod: detect + Steam P2P + forge-apply |
| `sw-coop-build/hook_dll/detour_*.asm` | the inline-hook detours (place, add, delete, arm, factory, connect) |
| [`sw-coop-build/hook_dll/wsdraw.cpp`](sw-coop-build/hook_dll/wsdraw.cpp) | the in-world partner-camera overlay |
| [`sw-coop-build/hook_dll/coop_hud_state.h`](sw-coop-build/hook_dll/coop_hud_state.h) | shared-memory bridge between the two |
| `sw-coop-build/build-coop.cmd` | build the DLL(s) (MSVC x64 + `ml64`) |
| `sw-coop-build/inject.ps1` / `reload-build.ps1` | load the mod / dev hot-reload |
| [`sw-coop-build/coop-package/`](sw-coop-build/coop-package) | the shippable package (inject scripts, setup + test docs) |
| [`sw-coop-build/SHAPE-COVERAGE.md`](sw-coop-build/SHAPE-COVERAGE.md) | which part shapes are handled |
| [`sw-coop-build/TEST-RESULTS.md`](sw-coop-build/TEST-RESULTS.md) | two-machine test results + known gaps |

## Current limits (and why)

- **Only edits after load sync.** There's no full-craft snapshot yet, so pre-existing blocks aren't
  known to the other side until touched. A snapshot/resync system is the roadmap item that fixes this
  *and* enables late-join.
- **Coordinate frames.** Both players must share the same voxel origin. Confirmed at the starter
  workbench; other benches/gamemodes are unverified (see the README).
- **Not all edits yet.** Electric wires sync; other connection types, component properties, and
  microcontroller resizing don't yet.

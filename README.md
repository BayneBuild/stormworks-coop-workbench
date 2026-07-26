<!-- Banner -->
![The Stormworks workbench prompt showing "Create Vehicle — [E] CO-OP / [Q] SOLO", added by the mod](docs/workbench-coop-prompt.png)

*The mod running in-game: the workbench itself offers co-op.*

# Coop Workbench

**Real-time multiplayer building for [Stormworks](https://store.steampowered.com/app/573090/).**
Place, delete, or paint a block on one machine and it appears **live** on your partner's — synced
peer-to-peer over Steam. No server, no port-forwarding, no IP addresses.

![status](https://img.shields.io/badge/status-experimental-orange)
![platform](https://img.shields.io/badge/platform-Windows-blue)
![license](https://img.shields.io/badge/license-MIT-green)

> ⚠️ **Early and experimental.** This is a hobby mod, tested in a narrow set of conditions (see
> [Tested / not tested](#tested--not-tested)). It runs entirely **in-memory on your own game and
> modifies no game files** — but back up your creations and use it at your own risk.

## Why this exists

Co-op in Stormworks never really felt like *co-op* to me. You spend **hours** building your own
separate thing, then meet up and go *"hey, look at what I made."* But the game is **more than half
building** — and that half, the best half, you do alone.

Coop Workbench is the missing piece: **actually build the same vehicle together, at the same time.**
Think Google Docs or Figma, but for Stormworks vehicles — you place a block, your friend sees it
appear instantly, and you build as a team instead of in parallel.

> 👋 Heads up: this is my **first mod** and my **first public repository**. I'm learning as I go, so
> feedback, corrections, and help are genuinely welcome — see [Contributing](#contributing).

## Hasn't someone already made this?

No — and that's the surprising part. Real-time co-op building has been one of Stormworks' **most-requested
features since within months of its 2018 launch.** The developers' feedback site has a long-running
[*Coop Build Mode* request](https://geometa.co.uk/support/stormworks/2620) with dozens of votes and
**~69 separate duplicate requests merged into it**, and players were still filing new ones in 2026. As
one put it, they'd been *"waiting for years."*

It hasn't happened because it's genuinely hard, and the developers have
[said since 2018](https://steamcommunity.com/app/573090/discussions/0/3397295779078820665/) they'd
*"like to do this eventually"* but have no near-term plans. Two reasons it's tough:

- **The editor is single-player by design.** Stormworks multiplayer syncs *spawned* vehicles, not the
  workbench — normally the only way to share a build is to spawn it and have your friend load a copy.
- **There's no modding API for the build menu.** Stormworks' Lua APIs cover missions and in-vehicle
  logic, never the editor — so nobody could build this as a conventional mod.

Coop Workbench takes the one door that's open: it runs as an **in-memory mod** that drives the game's
own editor. It's an experiment, not a finished product — but it's a real, working attempt at the thing
the community has wanted for years.

## What works today

| Feature | Details |
|---|---|
| Placement | single-click, click-drag, and big area fills, with type, position, rotation and colour |
| Delete | eraser tool including fast drag-erase, remeshed the same frame |
| Paint | whole-block and per-face repaints |
| Angled parts | wedges, pyramids, inverse pyramids etc. keep their correct auto-filled shape |
| Connections | electric and on/off-logic wires, add *and* disconnect |
| Battery charge | charge level syncs live |
| Whole-craft sync | pull your partner's entire craft over Steam — blocks, colours, wires, microcontrollers, part settings — in one go. Used for joining, catching up, or fixing a desync |
| Co-op or solo entry | the workbench prompt becomes `[E] START CO-OP` / `[Q] SOLO`, and changes to `[E] JOIN PARTNER` once your partner is building. Whoever gets in first is the source of truth; joining pulls their craft |
| Partner overlay | their camera, and the exact cell they're hovering, drawn in the world — only while they're actually in the bench |
| Bench matching | build volumes are compared and a mismatch is refused up front, rather than half-syncing and leaving you both confused |
| Networking | peer-to-peer by SteamID over Steam's relay. No server, no ports, no IPs, and you don't need to be in the same Stormworks session |
| Auto-arm | opening the workbench is enough; there's no separate step |
| Patch resilience | hooked functions are found by signature scan at startup, so game updates are less likely to break it |

## Tested / not tested

What's actually been tested, and what hasn't:

**✅ Tested and working** (on two real machines over Steam):
- **Custom gamemode**, at the **starter vehicle workbench**, with **both players starting from an
  empty craft and placing their first block at the same origin spot.**
- The full feature table above held up in a live two-machine session.

**✅ Also confirmed since:**
- **Multiple different workbenches**, not just the starter one.
- **While both players are in a shared Stormworks multiplayer session.** (It also works when you're in
  completely separate saves — the sync doesn't go through the game's networking at all.)
- **Partner camera and cursor markers**, seen across two machines.
- **Workbench coordinates are bench-independent.** Measured at four benches: the first block always lands
  at voxel `(0,0,0)`, and the origin is the **centre** of the build volume. The earlier worry about world
  seeds affecting placement was unfounded.
- **Bench build volumes differ, and that does matter** — starter is 30×30×60 voxels (±13/±13/±28 reach),
  larger benches 146×40×140. The mod compares them and refuses to sync a mismatch.
- **Large crafts** — a 552 KB craft (4,783 render chunks) transfers and rebuilds correctly.

**❓ Not yet verified — help wanted:**
- **Career / survival gamemodes** — everything so far has been Custom.
- **Different world seeds and starting islands** — expected to be irrelevant now that coordinates are known
  to be bench-independent, but not directly tested.
- **More than two players.**

If you try it somewhere new, [**opening an issue**](../../issues) with your result — and, if something
broke, **both players' `coopworkbench-log.txt`** (with SteamIDs removed) — is the single most useful
thing you can contribute right now.

## Known limitations — what doesn't work yet

It's **alpha**. Plenty is still missing or in progress:

### ⚠️ The big one: component settings and microcontrollers need a **resync**

Live sync covers **geometry** — placing, deleting, painting, rotating, wiring. It does **not** cover most
**per-component internal settings**:

- **Microcontrollers** — their internal logic and custom size
- **Part settings** — sliders, logic-constant values, names, seat/display configuration
- (Battery **charge level** is the exception — that one does sync live.)

Change a microcontroller's internals and **your partner will not see it happen.** They carry across only
when someone does a **full-craft resync** — press **F7**, or re-enter the bench as `JOIN PARTNER` — which
pulls the partner's whole craft, settings and all.

So the working pattern today is: **build together live, and resync after doing internals work.** Making
those changes stream live is the single biggest item on the roadmap.

### Other known limitations

- **Both players must use the same workbench type.** Bench build volumes differ (the starter bench is
  30×30×60 voxels; some are 146×40×140), so a block that fits one won't fit the other. Mismatches are
  detected and sync is **blocked on purpose**, with a warning in the overlay.
- **No auto-connect yet.** You still exchange SteamID64s by hand (`coop-peer.txt`). Automatic partner
  discovery is in progress.
- **Stuck warning markers.** Yellow warning icons can pile up on screen during a session, pinned to the
  screen rather than the world. They're **cosmetic** — quitting to the menu clears them — and it isn't yet
  confirmed whether they're caused by the mod at all.
- **One crash seen when copying a part** on a freshly pulled craft. A fix is in, but it hasn't been
  reproduced or confirmed since — [report it](../../issues) if you hit it.
- **Not all wire types verified.** Electric *and* on/off-logic wires sync (including **disconnects**);
  number, composite, video, audio and fluid are untested, and rope is a separate system.
- **No undo/redo sync**, and it's **two players** only for now. (Symmetry-mode placement does work.)
- **Concurrent edits to the same voxel** resolve last-writer-wins — there's no smart merge yet.
- **Missing parts are skipped.** If your partner places a modded part you don't have installed, it's
  logged and skipped, not placed.
- **Windows only**, and a **game update can break it** until offsets are re-checked (a signature scan
  at startup makes that less likely).

All of this is on the [roadmap](ROADMAP.md) — help is very welcome.

## Quick start

1. Both players: install Steam + Stormworks and launch the game.
2. Grab the [latest release](../../releases), unzip it, and both run **`inject.bat`**. It prints *your*
   SteamID64 and asks for your *partner's* — paste it in.
3. Both walk up to a workbench — **the same type on both machines** (easiest: the starter bench) — and
   open it with **`E`** for co-op. `Q` opens it solo, syncing nothing.
4. Start building. Whoever entered first is the source of truth; if your partner is already in there, your
   prompt says **`JOIN PARTNER`** and entering pulls their craft across automatically.
5. After anyone edits **microcontroller internals or part settings**, press **F7** to resync — those don't
   stream live yet (see [limitations](#️-the-big-one-component-settings-and-microcontrollers-need-a-resync)).

Full step-by-step + a feature-by-feature checklist:
[`coop-package/README.txt`](sw-coop-build/coop-package/README.txt) and
[`coop-package/TESTING.txt`](sw-coop-build/coop-package/TESTING.txt).

> Windows Defender or your antivirus may warn about the injector — it uses `LoadLibrary` to load the
> mod DLLs into the running game. The source for everything it loads is in this repo.

## Build from source

Requires the **MSVC x64 build tools** (VS2022 Build Tools) with `ml64` (MASM). From `sw-coop-build/`:

```bat
build-coop.cmd
```

Then load it into the running game with `inject.ps1`. During development, `reload-build.ps1` unloads
the live mod and rebuilds so you can re-load without restarting the game. Architecture and hook map:
[`ARCHITECTURE.md`](ARCHITECTURE.md).

## How it works (one line)

Rather than writing the voxel grid directly (which never triggers a render), the mod **hooks the
game's own place / delete / paint / connect commands and calls them**, so every synced action runs
through the game's real code path and looks identical to a manual click. More in
[`ARCHITECTURE.md`](ARCHITECTURE.md).

## Roadmap — the north star

The goal isn't just "syncs blocks" — it's the **best co-op building experience in Stormworks**:

- **Never silently desync** — a periodic hash detects drift, and a full-craft snapshot can heal any
  divergence (also enabling **late-join**).
- **Full edit coverage** — all connection types, component properties, undo/redo, symmetry,
  multi-body, and more than two players.
- **Presence & polish** — live partner cursors in each player's color, incoming-edit highlights, and
  a sync-status HUD on the in-world overlay.

**Concrete, prioritized tasks — from no-code test reports to bigger features — live in
[`ROADMAP.md`](ROADMAP.md).** That's the best place to see where to jump in.

## Contributing

Contributions are very welcome — this is exactly the kind of project the Stormworks community can
push forward together. Bug reports (especially "I tested it in *X* and it did *Y*"), feature ideas,
and pull requests all help. Start with [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Disclaimer

Coop Workbench is an **unofficial, fan-made mod**. It is **not affiliated with, endorsed by, or
supported by** the developers or publisher of Stormworks. It runs entirely in your own game's memory,
**modifies no game files, and ships no game assets**. It's provided as-is, with no warranty — see
[Tested / not tested](#tested--not-tested). Back up your creations.

## License

[MIT](LICENSE) © 2026 BayneBuild

# Roadmap & where to help

This is an early experiment, and there's a lot of approachable work — you don't need to understand the
whole thing to help. Tasks are grouped roughly from "great first thing to try" to "bigger project."

If you want to take something on, **open an issue** so we don't double up, and say how you'd approach it.
(Once the repo has some traction I'll tag concrete `good first issue` / `help wanted` tickets too.)

## 🧪 Start here — test reports (no code needed)

Confirmed in **Custom gamemode**, across **multiple workbench types**, with either player joining an
existing craft — workbench coordinates turned out to be bench-independent, and a 552 KB craft has moved
between machines intact. **Career and Survival are still unverified.** The single most useful thing you can do is try it somewhere else and report back:

- Career / survival benches, larger workbenches — does it still line up?
- Different **world seeds** and workbench positions — do coordinates still match?
- Anything that crashes, desyncs, or lands blocks in the wrong place.

A short "I tried it in *X* and got *Y*" issue is genuinely valuable — it's how we map what actually works.

## 🟢 Good first contributions

- **Non-electric wires.** Electric and on/off-logic wires are confirmed working; the connection path
  looks type-agnostic, so number / composite / video / audio / fluid **very likely work too** — **verify
  each and flag any that don't**. A great test-plus-small-fix task. (Rope is a separate system.)
- **Docs & setup polish** — clearer steps, screenshots, a short setup clip, fixing anything confusing.
- **Surface missing parts better** — when your partner is missing a mod/part, make it obvious in-game
  instead of only in the log.

## 🟡 Medium

- ~~**Component properties**~~ — **SHIPPED in v0.5.0-alpha**, solo-tested only. Sliders, names, logic
  constants and microcontroller internals stream live.
- **Two severe open bugs**, both new and both documented in the README: a partner's microcontroller arrives
  carrying *yours*, and an `F7` pull can land the craft offset (root cause found, fix unconfirmed).
- ~~**Zero-friction install**~~ — **SHIPPED.** `install.bat` or two files by hand; the mod loads with the
  game. Automatic partner pairing over Steam friends, no SteamIDs to exchange.
- **Microcontroller resize** — sync custom microcontroller footprints.
- **Overlay polish** — live peer cursors in each player's color, and a small sync-status HUD.

## 🔴 Bigger pieces

- ~~**Full-craft snapshot / late-join**~~ — **SHIPPED.** `F7`, and automatic on `JOIN PARTNER`. Also the
  universal "heal any desync" primitive.
- **Consistency core** — a periodic hash to detect drift plus automatic re-sync, and per-voxel
  last-writer-wins so two people editing at once never corrupt the craft.
- **More than two players.**

## Working on the internals?

Some features need lower-level detail about how the game's editor behaves. If you're tackling one of the
bigger pieces, open an issue with your plan — happy to point you in the right direction.

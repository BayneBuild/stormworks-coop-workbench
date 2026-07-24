# Roadmap & where to help

This is an early experiment, and there's a lot of approachable work — you don't need to understand the
whole thing to help. Tasks are grouped roughly from "great first thing to try" to "bigger project."

If you want to take something on, **open an issue** so we don't double up, and say how you'd approach it.
(Once the repo has some traction I'll tag concrete `good first issue` / `help wanted` tickets too.)

## 🧪 Start here — test reports (no code needed)

The mod is only confirmed in **Custom gamemode, starter workbench, both players empty from the same
origin block.** The single most useful thing you can do is try it somewhere else and report back:

- Career / survival benches, larger workbenches — does it still line up?
- Different **world seeds** and workbench positions — do coordinates still match?
- Anything that crashes, desyncs, or lands blocks in the wrong place.

A short "I tried it in *X* and got *Y*" issue is genuinely valuable — it's how we map what actually works.

## 🟢 Good first contributions

- **Non-electric wires.** Electric power wires sync today; logic / composite / fluid / etc. very likely
  flow through the same path already — **verify which types work** and flag any that don't. A great
  test-plus-small-fix task.
- **Docs & setup polish** — clearer steps, screenshots, a short setup clip, fixing anything confusing.
- **Surface missing parts better** — when your partner is missing a mod/part, make it obvious in-game
  instead of only in the log.

## 🟡 Medium

- **Disconnect sync** — removing a wire should sync, not just adding one.
- **Component properties** — battery charge % / display name, logic-constant values, and other per-part
  electrical/mechanical settings.
- **Microcontroller resize** — sync custom microcontroller footprints.
- **Overlay polish** — live peer cursors in each player's color, and a small sync-status HUD.

## 🔴 Bigger pieces

- **Full-craft snapshot / late-join** — serialize the whole craft and load it on the other side, so a
  player can join mid-build and pre-existing blocks sync. This is also the universal "heal any desync"
  primitive.
- **Consistency core** — a periodic hash to detect drift plus automatic re-sync, and per-voxel
  last-writer-wins so two people editing at once never corrupt the craft.
- **More than two players.**

## Working on the internals?

Some features need lower-level detail about how the game's editor behaves. If you're tackling one of the
bigger pieces, open an issue with your plan — happy to point you in the right direction.

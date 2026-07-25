# Two-Machine Test Results & Known Gaps

Live results from real two-machine Steam sessions (machines A and B, peer-to-peer over Steam relay),
plus the running list of things that are known NOT to sync yet. This is the canonical, git-tracked
record — update it after each cross-machine test.

---

## 2026-07-24 — connections: energize + logic + disconnect; overlay live (merged DLL)

Single merged `coopworkbench.dll` on both machines. New this session:

- **Electric wires ENERGIZE.** A placed the wires, B spawned the craft — the light **powered**. So a
  forged wire (flat-store write) genuinely carries power; **no logic-net graph hookup needed** (drops the
  "energize" TODO). Logs: `type=4 (ELECTRIC)` → `>>> SEND conn EResult=1` → `<<< APPLIED CONN`.
- **Logic / on-off connections sync AND function.** A toggle button → light worked on B's spawned craft.
  **Type enum learned: electric = `4`, on/off logic = `0`** (the earlier `1` guess was wrong). So the
  connection apply/forge path is confirmed for BOTH electric and logic — not electric-only anymore.
- **Disconnect sync works (electric), echo-free.** connect↔disconnect ×3 on one wire produced **exactly
  3** `[conn] DEL` → `>>> SEND disconn EResult=1`, one per action — no re-broadcast bounce, so the
  `conn_prev_remove` echo-suppression is correct. Apply confirmed visually (wire vanished on B). *(The
  partner-side `<<< APPLIED DISCONN` line wasn't captured — his log was stale — but the visual confirms it.)*
- **Merged overlay validated live** — `wsdraw-log`: `peer camera link LIVE` on both sides in the same
  session. The wsdraw→`coopworkbench.dll` merge holds up in a real 2-machine run.

### Coordinate insight (overlay)
Exiting the workbench into the world with the mod running, the **partner's camera marker keeps tracking in
the real world** at the starter build area. So the **workbench is the real world at the craft's spawn
location, just with the world geometry unrendered** — the workbench camera is TRUE world coordinates, and
**body origin = the spawn-platform world position.** See [`WORLD-SPACE-OVERLAY.md`](WORLD-SPACE-OVERLAY.md).

---

## 2026-07-23 — combined test: overlay + connections

Both machines ran the combined portable package (two DLLs at the time — `coopworkbench.dll` + `wsdraw.dll`;
since **merged into a single `coopworkbench.dll`**, same features, one inject). Everything expected worked.

### Proven end-to-end on two real machines

- **Peer-camera overlay over Steam** — `wsdraw-log`: `net: *** peer camera link LIVE ***`. The partner's
  camera rendered as the world-space **"PARTNER"** frustum from live network data (not the delayed
  self-ghost). The session-sharing safeguard engaged (`coopworkbench.dll=present (will NOT close sessions)`), and
  the mandatory-join hot-unload was clean (`unloaded cleanly`). *Feature "see your partner's camera" is
  done across machines.*
- **Electric-power connections** — `type=4 (ELECTRIC)` detected → sent → `<<< APPLIED CONN` on the peer,
  both directions.
- **Everything from prior tests held up in the same session** — single/drag/area-fill placement, whole-
  block and per-face paint, pyramid + inverse-pyramid sub-shape variants, deletes, rotation, color.

---

## Known gaps — not synced yet (as of 2026-07-24)

Ordered roughly by effort. Items 2–5 are the "property edits / component internal state" scope.

1. **Non-electric connections — partly closed (2026-07-24).** Electric (`type=4`) AND on/off logic
   (`type=0`) are now confirmed **applied + functioning** on the peer (add *and* disconnect). Remaining
   untested wire types: number, composite/data, video, audio, fluid (rope is a separate system). The
   apply/forge and the disconnect-diff are both type-agnostic, so these are expected to work — they just
   need a confirming test.

2. **Microcontroller custom SIZE.** A microcontroller's dimensions are a configurable property. The
   place-forge currently ships only `{defName, pos, rotation, color, cat}`, so a **resized microcontroller
   lands at DEFAULT size on the peer.** This is a desync *amplifier*: the wrong footprint throws off every
   voxel and connection placed against the microcontroller's faces, so one machine lines up and the other
   doesn't. Fix: read the size field at detect, write it before the forge on apply.

3. **Microcontroller internal logic graph.** The block syncs; its internal logic (a separate editor
   context) does not.

4. **Displays.** Content / configuration not synced.

5. **Part properties generally.** Battery charge % / name, logic-gate constant values
   (`gate_bool_constant`, etc.), seat/part settings, and the many other per-part electrical/mechanical
   settings. This is the roadmap's "property edits" item — the test above makes the concrete targets clear.

---

## Observation for `coopworkbench.dll` (not the overlay)

`coop-log` on hot-unload: `UNLOAD: worker wait -> 258 (0x102=timeout)` then `freeing library now`. It
frees the DLL after a join **timeout** — the same use-after-free-risk pattern the overlay's adversarial
review caught and fixed (mandatory join: loop the wait until the worker has provably exited, never free on
timeout). Worth hardening `coopworkbench.dll`'s unload the same way.

---

## How to re-run

Both machines: extract `stormworks-coop.zip`, open the workbench from the **same base** with one block at
the **same origin spot**, run `inject-both.bat`, exchange SteamID64s. Full checklist:
[`coop-package/TESTING.txt`](coop-package/TESTING.txt). Overlay details:
[`WORLD-SPACE-OVERLAY.md`](WORLD-SPACE-OVERLAY.md).

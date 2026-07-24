# Two-Machine Test Results & Known Gaps

Live results from real two-machine Steam sessions (machines A and B, peer-to-peer over Steam relay),
plus the running list of things that are known NOT to sync yet. This is the canonical, git-tracked
record — update it after each cross-machine test.

---

## 2026-07-23 — combined test: overlay + connections

Both machines ran the combined portable package (`inject-both.bat` → `coop.dll` + `wsdraw.dll`).
Everything expected worked.

### Proven end-to-end on two real machines

- **Peer-camera overlay over Steam** — `wsdraw-log`: `net: *** peer camera link LIVE ***`. The partner's
  camera rendered as the world-space **"PARTNER"** frustum from live network data (not the delayed
  self-ghost). The session-sharing safeguard engaged (`coop.dll=present (will NOT close sessions)`), and
  the mandatory-join hot-unload was clean (`unloaded cleanly`). *Feature "see your partner's camera" is
  done across machines.*
- **Electric-power connections** — `type=4 (ELECTRIC)` detected → sent → `<<< APPLIED CONN` on the peer,
  both directions.
- **Everything from prior tests held up in the same session** — single/drag/area-fill placement, whole-
  block and per-face paint, pyramid + inverse-pyramid sub-shape variants, deletes, rotation, color.

---

## Known gaps — not synced yet (as of 2026-07-23)

Ordered roughly by effort. Items 1 is the smallest next win (detect+send already work — only the apply
side is missing); items 2–5 are the "property edits / component internal state" scope.

1. **Non-electric connections.** Logic (`type=1 default/logic`), and `type=0` / `type=5` wires are
   **detected and sent**, but only electric (`type=4`) is confirmed **applied** on the peer. The
   connection apply/forge path is validated for electric only; the other wire types (logic, data,
   composite, video, audio, fluid, rope) need their apply path finished and tested.

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

## Observation for `coop.dll` (not the overlay)

`coop-log` on hot-unload: `UNLOAD: worker wait -> 258 (0x102=timeout)` then `freeing library now`. It
frees the DLL after a join **timeout** — the same use-after-free-risk pattern the overlay's adversarial
review caught and fixed (mandatory join: loop the wait until the worker has provably exited, never free on
timeout). Worth hardening `coop.dll`'s unload the same way.

---

## How to re-run

Both machines: extract `stormworks-coop.zip`, open the workbench from the **same base** with one block at
the **same origin spot**, run `inject-both.bat`, exchange SteamID64s. Full checklist:
[`coop-package/TESTING.txt`](coop-package/TESTING.txt). Overlay details:
[`WORLD-SPACE-OVERLAY.md`](WORLD-SPACE-OVERLAY.md).

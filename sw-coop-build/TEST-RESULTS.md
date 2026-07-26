# Two-Machine Test Results & Known Gaps

Live results from real two-machine Steam sessions (machines A and B, peer-to-peer over Steam relay),
plus the running list of things that are known NOT to sync yet. This is the canonical, git-tracked
record — update it after each cross-machine test.

---

## 2026-07-25 — 2-MACHINE: full-craft PULL over Steam (with a community tester)

First cross-machine run of the F7 pull, plus auto-arm, with an outside tester (not the usual partner).

**PASSED**
- **Auto-arm** — neither side pressed anything to start syncing; opening the bench was enough.
- **Live edit sync from a blank craft** — both directions, "I didn't push any buttons".
- **Leave + re-enter the bench mid-session** — one player exited, the other kept building, the first re-entered
  and was **still in sync**. (This is what motivated the presence model in FINDINGS §16 — it worked because
  edits were being applied through a stale editor while out of the bench.)
- **F7 whole-craft pull, both directions, on big crafts** — 24 KB / 300 nodes, then **182 KB / 1325 nodes**,
  reassembled and rendered. A wired torpedo example "went through the load intact" — power and data
  connections preserved. This is the feature working: an entire configured craft moved over Steam.

**FAILED / open**
- **Pulled craft lands in the WRONG PLACE.** Same craft appeared centred for the sender and at the
  front-bottom of the build area for the receiver — sometimes partly outside it. Cause: the blob carries the
  SENDER's world anchor. **Fix built (FINDINGS §13.1), 2-machine confirm still pending.**
- **Stuck out-of-bounds warning markers** — yellow triangles pinned to the screen (screen-space, not
  world-anchored) after pulling a craft that landed out of bounds. Partly downstream of the anchor bug, and
  partly the bench-size mismatch found later (§15.4) — a craft built at a big bench does not fit a small one.
- Reminder: the shared test zip predates the position fix; rebuild before redistributing.

---

## 2026-07-25 — SOLO: workbench coordinate survey (2 benches)

Method: place one block at the bench origin, then drag a "plus" out to the boundary in all 6 directions; read
the extents from the `<detect> add` log lines. Both runs were pure plus shapes (arm counts matched placed
counts exactly), so the numbers are exact. Full analysis in FINDINGS §15.

- **Origin voxel is `(0,0,0)` at BOTH benches** → the voxel frame is bench-independent → **block sync works at
  any workbench.** This was the open "only tested at the starter bench" question.
- **The origin is the CENTRE of the build volume**, not a corner (all axes symmetric, all dimensions odd).
  This corrected a wrong assumption.
- Volumes: **starter 27×27×57 voxels** (6.75×6.75×14.25 m) · **larger base 143×37×137** (35.75×9.25×34.25 m).
- **New hazard found:** benches differ hugely in size, so a block legal at a large bench is out-of-bounds at
  the starter bench → warning markers for a partner on a smaller bench. Guidance for now: **both players
  should use the same bench type.** Bounds-guard on the apply path is the code fix.
- Still to do: new saves at different starting islands (world position / seed as the remaining variable).

---

## 2026-07-25 — full-craft LOAD works end-to-end (solo, F4)

Solo validation of the pull-model's load half before the 2-machine test. **PASS.**

- **F4 load = deserialize + build + render, instant, no hang, no duplicate.** F5 saved the pyramid to a 3412-byte
  blob; F4 loaded it back onto the live editor vehicle and it rebuilt **in place and rendered the same frame.**
- **The fix took several iterations (all in-game):**
  - `0x4C6160` deserialize loads DATA only (eraser saw the blocks) → craft **invisible**; the game re-meshed it
    on its own only minutes later (lazy pass).
  - Replicating the game's load tail (`0x4C3410`) didn't help — it only dirty-marks, never meshes.
  - Calling the edit-commit mesh pair `0x4C0870` (node create) then `0x4A3740` (whole-body mesh + GPU upload)
    **HUNG the game** — both deadlock when driven from the RunCallbacks context (`0x4A3740` ends in GPU-upload
    `0x471D60`). Two hard lockups localized this precisely via split logging.
  - **Winning recipe:** `0x4C98C0` (body build → creates render nodes in `body+0x3F0` + spatial hash) then
    `force_remesh` (`0x4A2E40`+`0x4A31E0`) on each node — the proven-safe pair paint/delete already use from
    `my_runcb`. Renders instantly, no hang. In-game dump confirmed 1 body / 40 render nodes for the pyramid.
- **Implication:** the blob is the game's save format, so this is the **universal state-transfer primitive** — a
  pull carries MC internals/size, battery charge/name, displays, part properties, and modded parts (still to be
  empirically checked with an MC+battery craft). See FINDINGS §12.5.
- **Next:** wire the 2-machine pull ("load peer's craft" → request → peer serializes → chunk over Steam → load),
  and the passive auto-arm (no "place a block first").

### 2026-07-25 — PARTNER CURSOR WORKING + floating origin SOLVED (solo)  [v0.2.0]
- **Partner cursor marker renders correctly, on the right cell, at a bench 19 km from the world origin.**
  **PASS.** With the F8 self-test the marker trails your own cursor by ~1 s, cell-accurate.
- Took **three** independent fixes, each of which alone looked like "the feature doesn't work":
  1. **Wrong source** — `editor+0x12F8` is not a cursor at all; it is the translation row of a matrix the
     editor ctor pins to 0.0 (FINDINGS §17/hover RE). Real live hover = `editor+0x1440` (ghost cell, int3)
     gated by the raycast-hit byte `editor+0x1568`.
  2. **FLOATING ORIGIN** (FINDINGS §18) — the renderer draws in a space rebased to a whole-km tile chosen
     from the camera, so markers drawn at absolute-world coords landed ~19 km off screen. Solved:
     `render = world - R`, `R = 1000*round(cam_world/1000)`, with `cam_world` read from `editor+0xE0`
     (translation row of the camera→world 4x4 at `editor+0x80`, validated as a rigid transform). Derived by
     SUBTRACTION against the MVP-decoded camera so it assumes nothing about tile size or rounding.
  3. **Half-voxel offset** — blocks are CENTRED on their voxel, so the cell centre is `v*0.25 + origin`
     exactly; an extra `+0.5` voxel put the marker half a block off.
- **The arrow-key overlay calibration is now obsolete** — it had been silently compensating for the floating
  origin all along. The overlay self-calibrates at any bench.
- Partner camera + cursor now share one orange colour constant so "orange = your partner" is learnable.

### 2026-07-25 — E/Q co-op entry + sync banner CONFIRMED (solo)  [build v0.2.0]
- **E/Q mode selection WORKS.** `[key] interact: action=0x14 state=1` → `[mode] opened with E (CO-OP) -> sync
  ON | STARTING (our craft is the source)`, and `action=0x13` → `Q (SOLO) -> sync OFF`. **PASS.**
  (First attempt silently did nothing: the ids are 32-bit in 8-byte stack slots, so reading the full qword
  gave `0xB2_00000014` and matched neither constant — see FINDINGS §17.2.)
- **Sync banner WORKS** — F7 with no partner shows a red centre-screen `SYNC FAILED / no partner connected`.
  **PASS.** (First attempt showed nothing: `pull_request()` returned on the no-peer path BEFORE setting the
  banner flag — the exact silent failure the banner exists to prevent. Every refusal now surfaces.)
- **Partner cursor still not visible — root cause found and it is NOT the cursor.** The hover data is correct
  (`voxel=(-1,0,3) src=ghost hit=1`, after fixing the source per §17/hover RE) and the marker draws fine; the
  overlay is simply drawing in ABSOLUTE WORLD while the renderer uses a REBASED space — ~19 km apart at this
  bench. See **FINDINGS §18 (floating origin)**. Auto-calibration is now guarded so it cannot regress.
- Overlay now shows `COOP WORKBENCH by BAYNEBUILD  v0.2.0  EXPERIMENTAL`; startup logs version + build time
  so a tester's report always identifies the build (we twice tested a build before re-injecting it today).

### 2026-07-25 — workbench prompt text CONFIRMED in-game (solo)
Injected at the **main menu**, walked up to a workbench: the hover prompt reads
`Create Vehicle    [E] CO-OP    [Q] SOLO`. **PASS.** First in-game UI change the mod makes, and it needed no
game files, no code patching and no `VirtualProtect` — the localisation table at `base+0xD04E10` is already
writable .data, so it is a pointer+length swap (FINDINGS §17), restored on unload. Injecting at the main menu
works, so the patch does not depend on game state. Currently **cosmetic**: the game sends the same
"use workbench" request for either key, so both still just open the bench. Key detection is the follow-up.

### 2026-07-25 (later) — passive AUTO-ARM confirmed solo
- **Auto-arm works.** New build injected → on entering the workbench the log emitted
  `AUTO-ARMED (passive, bench open): editor=0x1688F0015E0` (the new per-frame resolver's line, distinct from
  the old `ARMED (single-click)`), then **F5 serialize + F4 load+render both worked** — no local edit needed to
  bootstrap the editor, **no crash** from the new `0x847EE0` capture hook. See FINDINGS §13.2.
- The passive path reads the app-state pointer (independent of placements), so it arms the instant the bench
  is the active build-mode state. Pull (F7) + delete/paint/conn/prop/snapshot now need zero local edits; only
  remote block-placement still needs one placement (forge-template capture, gated by `g_have_struct`).
- **Ready for the community 2-machine test** (this one build carries auto-arm + the F7 pull).

---

## 2026-07-24 (later) — properties, disconnect, overlay menu, full-craft serialize

- **Disconnect sync** — connect↔disconnect ×3 on an electric wire: exactly 3 `[conn] DEL` → `SEND disconn`,
  echo-free, wire vanished on the peer. **PASS.**
- **Battery charge %** — syncs (float at `comp+0x2A8`, gated to battery-type parts). **PASS.**
- **Generic property sync FAILED (crashed both machines).** A "sync every changed dword" diff flooded Steam
  (EResult=35) and byte-copied strings/pointers into the peer → crash (reproduced in **vanilla**, so it was
  ours). Reverted to a SAFE per-type whitelist (charge only, value-validated). Full coverage now moves to the
  game's own component codec (FINDINGS §12.2), which also covers **modded** parts.
- **Overlay status menu** — always-on top-left panel (link / partner / cam + key legend); F6 toggle. Working.
- **Full-craft SERIALIZE confirmed** — F5 → `0x4C5FE0` serialized the whole craft to a **3412-byte portable
  binary blob** (def names, rotations, colors), no crash; GStr `{data, len@+8}`. The pull-model snapshot's
  serialize half is proven; the load/deserialize half is next (solo-validated first). See FINDINGS §12.3.
- **Known gap** — the manually-placed origin block (placed BEFORE injection) doesn't sync paint; fixed by
  neighbour-seeding placements into the paint cache.

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

## 2026-07-25 (night) — 2-MACHINE: session model works; two transfer bugs found + fixed

With ThePwnageKitty. First cross-machine run of the v0.2.x session model.

**PASSED**
- **START CO-OP / JOIN PARTNER prompt** — partner saw "join partner and solo" exactly as designed.
- **JOIN auto-resync worked, twice** — including loading a completely different craft while the other was
  out of the bench, then resyncing on re-entry.
- **Electrical carried through** the pull.
- **182 KB craft** transferred and loaded perfectly (1326 nodes).

**BUG 1 — large-craft transfer (FIXED + CONFIRMED)**
552644-byte craft stalled at 400000. Two causes:
- SEND: 400 KB chunk fits Steam's per-message cap but FILLS the connection send buffer, so chunk 2 came back
  `rc=25` (k_EResultLimitExceeded) and was silently lost.
- RECEIVE (worse): `got` was a running SUM, so a retried chunk 0 double-counted to 800000, passed the
  `>= total` test, and loaded a buffer whose last 152 KB were NEVER WRITTEN. Those zeros became malformed
  components — the "holes", and a **game crash** when the partner edited a railing on it.
Fixed with 64 KB chunks + a per-frame pump that retries on `rc=25`, contiguous byte counting, exact-equality
completion, and a hard refusal to load an incomplete buffer. **CONFIRMED in his log:** all 9 chunks in order
to an exact `552644/552644`, 4783 nodes loaded, sender logging `send buffer full at 524288 - pacing`.

**BUG 2 — missing component<->node linkage (fixed, UNTESTED)**
The craft then loaded COMPLETELY and the game still crashed when he **copied a railing**. `0x4C3410` had been
dropped from the post-load sequence (it never meshes) — but it also establishes the component<->render-node
links (`comp+0x148` -> node, `node+0x150` -> comp) and prunes unlinked ones. So a complete transfer still
produced a STRUCTURALLY incomplete craft. Restored between the body build and the remesh pass.
- Brayden's note: this was probably the **first ever use of COPY** in the project. Place/delete/paint never
  traverse those links, so the defect was **latent**, not a regression — and it is the most plausible single
  cause of the stuck warning markers too (half-linked components are what a validator flags).
- **NEXT TEST:** pull the big craft, then COPY a railing. And check whether fresh warnings still appear.

**Auto-connect:** memory probe found the partner's SteamID64 in-process (x3, first at a stack-like
`0x000000F5578F6428`) among ~40 unrelated ids — possible but fragile, so discovery was switched to the Steam
friends API (`GetFriendGamePlayed`, AppID 573090). Untested.

## OPEN ISSUE — stuck screen-pinned warning indicators (observed 3x)

Yellow warning markers get stuck on screen. They are **screen-space pinned, not world-tracking** — they do
not move with the camera — so they are a stuck UI/validation state, not out-of-bounds world markers.

Sightings:
1. 2026-07-24 — after pulling a craft that landed out of bounds (bench-size mismatch + wrong anchor).
2. 2026-07-25 — after loading a craft from an incomplete transfer (the 400000/552644 corrupt load).
3. **2026-07-25 — after simply hosting, LEAVING the craft, and rejoining. NO pull involved.**

Sighting 3 is the important one: it breaks the "caused by a bad craft load" theory, since no sync/pull
happened. Possible that this is a vanilla Stormworks behaviour (or a bench enter/exit interaction) that the
mod merely coincides with.

4. 2026-07-25 (late) — after an F4 load on a build that ALREADY had the `0x4C3410` linkage fix. So the
   missing component<->node linkage was **not** the cause (that fix was aimed at the copy crash and remains
   unverified for that).

**KEY FINDING — they clear on a quit to menu.** Quitting to the menu, loading back in and reloading the craft
leaves NO warnings. So this is **accumulated session UI state, not craft corruption**: if the vehicle data
were bad the markers would return with the reloaded craft, and they do not.
- Severity is therefore COSMETIC, with a workaround (quit to menu).
- They accumulate during a session and are torn down with the world.
- Do NOT spend RE effort on a "clear the warning state" path until the ownership question below is settled.

**Still-unrun test (cheap, settles ownership):** do a load / leave / rejoin cycle with the mod NOT injected.
Sighting 3 involved no pull at all, so this may simply be vanilla Stormworks behaviour that the mod coincides
with. Worth 30 seconds before any further investigation.

**Withdrawn lead:** node counts appearing to grow across loads (4797 -> 4804 -> 4818) and the body count going
1 -> 4 were initially flagged as a possible leaky clear. They are almost certainly just the railing being
copied during that session - copied parts add nodes, and a detached copy becomes its own body.

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

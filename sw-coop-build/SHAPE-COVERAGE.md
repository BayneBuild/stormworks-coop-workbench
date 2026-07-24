# Shape-Variant Coverage Audit — Sub-Shape Orientation Replication

_Audit of all 758 part definitions (2026-07-23): 292 shape-variant parts, 466 plain cubes._

Grounded against `apply_place` (hook_dll/coop.cpp) and FINDINGS.md §11.5. The fix resolves each placed
cell's real category byte (`template+0x40`) and indexes the canonical template's variant array
`variant = *(*(canon+0)+cat*8)`, using it **only if `variant+0x40 == cat`**, else falling back to
canonical. The guard makes a wrong-variant forge impossible — worst case is a silent fallback to
canonical (i.e. the *old* sawtooth reappears), never a bad forge or crash.

---

## 1. Shape-Variant Part Families

### A. Proven "smart-drag auto-fill" geometry (category 0 — the sawtooth class)
Primitives where a drag auto-fills complementary orientation variants under **one shared definition
name** (the exact `02_wedge` cat=0/cat=7 case).

| Family | Count | Parts |
|---|---|---|
| **Wedges** | 3 | Wedge (`02_wedge`, blockType 1), Wedge 1x2 (`05_wedge_2`), Wedge 1x4 (`08_wedge_4`) |
| **Pyramids** | 6 | Pyramid (`03_pyramid`), 1x2, 1x4, 2x2, 2x4, 4x4 |
| **Inverse Pyramids** | 6 | Inverse Pyramid (`04_invpyramid`), 1x2, 1x4, 2x2, 2x4, 4x4 |
| **Static/Weight cubes** | 2 | Static Block (`01_block_static`), Weight Block |
| **Stairs** | 2 | Stair Step (`stair_segment`), Stair Top (`stair_top`) |

Long shape arrays (Wedge 1x4 = 13 shapes; Pyramid 4x4 = 14) confirm dense sub-shape packing under one name.

### B. Glass geometry — windows (category 15) — 22 parts
Same wedge/pyramid/diamond/corner geometry as class A, rendered as glass.
- **Angles** (7): `window_1x1_wedge`, `window_angle_s_1x2`, `window_angle_m_1x2x2`, `window_angle_m_2x2x2`, `window_angle_l_2x3x3`, `window_angle_xl_1x4x4`, `window_narrow_angle`, `window_large_angle`
- **Pyramids / inv-pyramids** (4): `window_1x1_pyramid`, `window_1x1_inv_pyramid`, `window_2x2_inv_pyramid`, `window_corner_2`
- **Diamonds** (6): `window_diamond_s_1x1`, `window_diamond_m_1x2x3`, `window_diamond_m_3x2x3`, `window_diamond_l_2x3x4`, `window_diamond_l_3x3x4`, `window_diamond_xl_3x4x5`
- **Corners** (5): `window_corner`, `window_corner_full_1x1`, `window_corner_full_small`, `window_corner_full_medium`, `window_corner_2`

### C. Buoyancy / float shapes (category 4) — 3 parts
`buoyancy_float_pyramid`, `buoyancy_float_wedge`, `landing_float` — wedge/pyramid geometry.

### D. Decorative / structural angled parts
- **Railings** (2, cat 8): `railing_segment_angle`, `railing_segment_angle_end`
- **Wings** (3, cat 1): Wing Section Large / XL / XXL

### E. Pipe network (`trans_*`) — ~16 parts
straight/angle/corner/T/cross/omni, open + enclosed. Orientation is rotational, single shape (`[3]`/`[1,3]`).

### F. Functional rotationally-oriented parts (the bulk)
Engines, motors, rotors, pumps, tanks, valves, wheels, winches, connectors, pistons, hinges, radars,
weapons, rockets, heat exchangers, etc. — blockType 0, functional categories, shapes `[1,3]`/`[0,3]`/`[3]`.
Placed individually (no drag auto-fill); the "shapes" are mount/rotation states, not name-shared variants.

---

## 2. Prioritized Drag-Test Matrix (one representative per family)

Drag a **beam** of each and confirm the echo renders a clean solid run (no sawtooth / flipped top cells).

| # | Priority | Family | Test part | Why |
|---|---|---|---|---|
| 1 | **P0** | Wedges | **Wedge 1x4** (`08_wedge_4`) | Longest variant array (13 shapes); stresses the proven path |
| 2 | **P0** | Pyramids | **Pyramid 4x4** (`13_pyramid_4x4`) | 14 shapes; verifies pyramids share the wedge layout |
| 3 | **P0** | Inverse Pyramids | **Inverse Pyramid 4x4** (`16_invpyramid_4x4`) | Confirms inverse-family cat indexing |
| 4 | **P1** | Windows | **Window Corner Full 3x3** (`window_corner_full_medium`) | Most sub-shapes of any window; category 15 — top unproven suspect |
| 5 | **P1** | Windows (angle) | **Window Angle 1x1x1** (`window_1x1_wedge`) | Simplest glass-wedge analog to proven `02_wedge` |
| 6 | **P1** | Buoyancy floats | **Buoyancy Float Pyramid** (`buoyancy_float_pyramid`) | Geometric variant under functional category 4 |
| 7 | **P2** | Stairs | **Stair Top** (`stair_top`) | Cat 0 step geometry |
| 8 | **P2** | Pipes | **Pipe Angle Corner (Enclosed)** (`trans_block_corner`) | Confirm auto-orient pipe drags don't false-trigger |
| 9 | **P3** | Wings | **Wing Section (Large)** (`wing_large`) | Rotational-only, fallback sanity check |
| 10 | **P3** | Functional | **Small Wheel** (`wheel_small`) | Representative `[1,3]` functional part; guard falls back to canonical |

---

## 3. Risk Assessment

**GREEN — should work via cat-indexed `+0x40` resolution (high confidence):** Wedges, Pyramids, Inverse
Pyramids (family A). Exact class the fix was proven against — one shared name, cat=0 canonical + higher-cat
variants in the `*(canon+0)` array. Wedge confirmed in-game. Static/Weight cubes and Stairs are low-shape-count
and either resolve or fall back cleanly.

**YELLOW — probably work, MUST be tested (different declared category):**
- **Windows (family B, category 15).** Geometrically identical to the primitives and likely to auto-fill on a
  strip drag — but a distinct category and a distinct render subsystem (glass). If their variant's `+0x40`
  doesn't read back the drag cat, the guard fails and they silently sawtooth. **Highest-value thing to verify.**
- **Buoyancy floats (family C).** Same open question on a smaller scale.

**LOW RISK — different encoding, safe by design:** Pipes (E) and functional parts (F). Orientation is the
rotation matrix (editor+0x14a0, already proven) plus the aux channel (component+0x24). Their `+0x40` holds a
functional category that never equals a drag-fill cat, so the guard falls straight through to canonical.
Wings and railings ride the rotation path too.

**The one thing the guard does NOT save you from:** it prevents forging a *wrong* template, but can't *invent*
a variant that lives elsewhere. Any family that (a) auto-fills complementary variants on drag AND (b) stores
the flip somewhere other than `*(canon+0)+cat*8` falls back to canonical and reproduces the old sawtooth —
no crash, but visibly wrong top cells. **Windows are the prime candidate** — they lead the test matrix.

---

## 4. Confidence

**High** for wedges/pyramids/inverse-pyramids — proven mechanism, same primitives. **Medium** for windows and
buoyancy floats — geometrically identical, non-crashing fallback, but the category-15 glass path is unverified
and is the realistic remaining sawtooth risk. **High** that everything rotational (pipes, engines, wheels,
wings — the bulk of the 758) is unaffected: they never exercise the variant array and fail safe to canonical.
Net: the fix is correct where proven and safe everywhere else; **the only untested failure surface worth a
real drag-test pass is the window family (P1).**

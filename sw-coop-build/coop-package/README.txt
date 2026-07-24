======================================================================
  STORMWORKS  -  Real-time Co-op Editor
======================================================================

WHAT IT DOES
  Build a vehicle together, live. What you do in the workbench appears on
  your partner's screen and theirs on yours - in real time:
    - place blocks (single-click AND click-drag / area fill)
    - angled parts (wedges, pyramids, etc.) keep their correct shape
    - delete (eraser tool)
    - paint - whole block AND individual faces
    - rotation + color carried across
  It connects directly over Steam's network (relay) - no server, no
  ports, no IP addresses, and you do NOT need to be in the same
  Stormworks multiplayer session. It's peer-to-peer by SteamID.

PLUS - see your partner in the world
  A second mod, wsdraw.dll (the "overlay"), draws your partner's CAMERA as a
  marker floating in the workbench, so you can see where they're working. Use
  inject-both.bat to load the co-op sync AND the overlay together in one step.

WHAT'S IN THIS FOLDER
  coop.dll          the co-op sync mod (blocks/paint/delete/wires)
  wsdraw.dll        the world-space overlay (see your partner's camera)
  inject-both.bat   *** RECOMMENDED *** connect + load BOTH mods in one step
  inject-both.ps1   (what inject-both.bat runs)
  unload-both.bat   cleanly unload BOTH mods without closing the game
  inject-coop.bat   load ONLY the co-op sync mod (no overlay)
  inject-coop.ps1   (what inject-coop.bat runs)
  unload-coop.bat   cleanly unload only the co-op mod
  coop-peer.txt     auto-written with your partner's SteamID (both mods read it)
  coop-log.txt      co-op status/activity log (created after first run)
  wsdraw-log.txt    overlay status log (created after first run)
  TESTING.txt       step-by-step two-machine test checklist

SETUP  (do this on BOTH computers)
  1. Start Steam (logged in) and launch Stormworks.
  2. Open the vehicle workbench. IMPORTANT: both of you start from the
     SAME base so block positions line up. Easiest: both start empty and
     each place ONE block at the very same spot (e.g. the origin) first.
  3. Double-click  inject-both.bat
       - it shows YOUR SteamID64  (send it to your partner)
       - it asks for your PARTNER's SteamID64  (paste it, press Enter)
       - it loads BOTH mods (co-op sync + overlay) and connects.
  4. In the workbench, just start building - your first edit (a click OR a
     drag) automatically arms the sync. You'll see each other's edits live,
     and your partner's camera as a cyan "PARTNER" marker in the world.

  You each need the OTHER person's SteamID64. The tool prints yours;
  or find it at steamid.io, or read 'our=...' in coop-log.txt.

NOTES / LIMITS
  - TESTED SETUP (important): so far this is only confirmed in CUSTOM gamemode
    at the STARTER vehicle workbench, with both players starting from an empty
    craft and placing their first block at the SAME origin spot. Other
    workbenches, career/survival, and different world seeds are UNTESTED - the
    voxel coordinates may not line up there yet. If blocks appear in the wrong
    place, that mismatch is the most likely cause.
  - Arming is automatic: your first placement each session (single-click OR
    drag) arms the sync. No special step needed.
  - Both machines need the SAME block set (base game + same mods). An
    unknown part from your partner is skipped (logged, not placed).
  - Start from a common base: only edits made AFTER the mod loads sync.
    A block that already existed before injecting won't repaint/delete-sync
    until it's touched. (Full late-join craft sync is coming.)
  - Re-launch Stormworks + re-run the .bat each play session, OR use
    unload-both.bat to swap builds without restarting the game.
  - Electric power connections (wires) now sync; other connection types and
    property edits are still coming.
  - The overlay's partner-camera marker is new - if it doesn't appear, the
    co-op sync still works; check wsdraw-log.txt.
  - Windows Defender / antivirus may warn about the injector (it uses
    LoadLibrary injection). It only loads coop.dll + wsdraw.dll into Stormworks.

TROUBLESHOOTING
  - "Stormworks is not running": launch the game & open the workbench first.
  - Nothing appears: check coop-log.txt on both sides. You want to see
    'our=' (your id), the partner id, '*** SESSION ACCEPTED ***' or
    '*** LINK LIVE ***', and '>>> SEND' when you edit.
  - "NOT ARMED" in the log: rare now (arming is automatic) - it just means an
    edit arrived a split second before the first placement armed; place again.
  - Blocks land in the wrong spot: you didn't both start from the same
    base - line up your origin block and try again.
  - See TESTING.txt for a full feature-by-feature checklist.
======================================================================

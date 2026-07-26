======================================================================
  COOP WORKBENCH  -  Real-time multiplayer building for Stormworks
======================================================================

WHAT IT DOES
  Build a vehicle together, live. What you do in the workbench appears on
  your partner's screen and theirs on yours - in real time:
    - place blocks (single-click AND click-drag / area fill)
    - angled parts (wedges, pyramids, etc.) keep their correct shape
    - delete (eraser tool)
    - paint - whole block AND individual faces
    - connect AND disconnect wires (electric power + logic on/off)
    - battery charge level
    - rotation + colour carried across
  Plus "LOAD PARTNER'S CRAFT" (press F7): instantly pull your partner's WHOLE
  build onto your screen - blocks, colours, wires, microcontrollers and part
  settings, the entire craft in one shot. Only your side changes.
  It also draws your partner's CAMERA as a marker floating in the workbench,
  so you can see where they're working.

  It connects directly over Steam's network (relay) - no server, no
  ports, no IP addresses, and you do NOT need to be in the same
  Stormworks multiplayer session. It's peer-to-peer by SteamID.

  >> This is ALPHA. Back up your creations. See "TESTED SETUP / LIMITS" below.

WHAT'S IN THIS FOLDER
  coopworkbench.dll   the mod (block/paint/delete/wire sync + partner overlay)
  inject.bat          *** RUN THIS *** connect + load the mod
  inject.ps1          (what inject.bat runs)
  unload.bat          cleanly unload the mod without closing the game
  coop-peer.txt       auto-written with your partner's SteamID
  coopworkbench-log.txt  status/activity log (created after first run)
  TESTING.txt         step-by-step two-machine test checklist

SETUP  (do this on BOTH computers)
  1. Start Steam (logged in) and launch Stormworks.
  2. Open the vehicle workbench. IMPORTANT: both of you start from the
     SAME base so block positions line up. Easiest: both start empty and
     each place ONE block at the very same spot (e.g. the origin) first.
  3. Double-click  inject.bat
       - it shows YOUR SteamID64  (send it to your partner)
       - it asks for your PARTNER's SteamID64  (paste it, press Enter)
       - it loads the mod and connects.
  4. That's it - just being in the workbench arms the sync automatically (no
     "first edit" needed). You'll see each other's edits live, your partner's
     camera as a cyan "PARTNER" marker, and you can press F7 any time to pull
     their whole craft onto your screen.

  You each need the OTHER person's SteamID64. The tool prints yours;
  or find it at steamid.io, or read 'our=...' in coopworkbench-log.txt.

IN-GAME KEYS
  F7   LOAD PARTNER'S CRAFT (pull their whole build onto your screen)
  F6   show / hide the top-left status menu
  F9   show / hide the overlay (the partner-camera marker)
  F10  show / hide the calibration readouts
  F5 / F4  save / load your craft to a local file (dev/test)

TESTED SETUP / LIMITS
  - TESTED SETUP (important): so far this is only confirmed in CUSTOM
    gamemode at the STARTER vehicle workbench, with both players starting
    from an empty craft and placing their first block at the SAME origin
    spot. Other workbenches, career/survival, and different world seeds are
    UNTESTED - the voxel coordinates may not line up there yet. If blocks
    appear in the wrong place, that mismatch is the most likely cause.
  - Arming is automatic: just opening the workbench arms the sync (no first
    placement needed). One exception: to have your BLOCK PLACEMENTS sync, make
    one local placement first (it captures the block template) - delete, paint,
    wires, battery %, and the F7 pull all work with no placement at all.
  - Both machines need the SAME block set (base game + same mods). An
    unknown part from your partner is skipped (logged, not placed).
  - Live edits sync only AFTER the mod loads. To grab a build that already
    existed, or to catch up after joining late, press F7 to pull your
    partner's whole craft.
  - Electric power AND logic on/off wires sync, including DISCONNECTS, plus
    battery charge level. Other wire types, more part properties, and
    microcontroller INTERNAL logic over the live path are still coming - but
    the F7 pull already transfers all of those at once.
  - Windows Defender / antivirus may warn about the injector (it uses
    LoadLibrary injection). It only loads coopworkbench.dll into Stormworks.

TROUBLESHOOTING
  - "Stormworks is not running": launch the game & open the workbench first.
  - Nothing appears: check coopworkbench-log.txt on both sides. You want to
    see 'our=' (your id), the partner id, '*** SESSION ACCEPTED ***' or
    '*** LINK LIVE ***', and '>>> SEND' when you edit.
  - Blocks land in the wrong spot: you didn't both start from the same
    base - line up your origin block and try again.
  - Re-run inject.bat each play session, OR use unload.bat to swap builds
    without restarting the game.
  - Still stuck? Grab coopworkbench-log.txt from BOTH machines (remove the
    SteamID64s first) and open an issue on the GitHub repo.
  - See TESTING.txt for a full feature-by-feature checklist.
======================================================================

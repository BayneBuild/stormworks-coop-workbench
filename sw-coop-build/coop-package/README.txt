======================================================================
  COOP WORKBENCH  -  Real-time multiplayer building for Stormworks
  v0.5.0-alpha   EXPERIMENTAL
======================================================================

WHAT IT DOES
  Build a vehicle together, live. What you do in the workbench appears on
  your partner's screen and theirs on yours, as it happens:
    - place blocks (single-click, click-drag, and big area fills)
    - angled parts (wedges, pyramids, inverse) keep their correct shape
    - delete, including drag-erase
    - paint - whole block AND individual faces
    - connect AND disconnect wires (electric power + logic on/off)
    - rotation, colour and symmetry-mode placement
    - battery charge level
    - component settings (sliders, names, logic constants)   <- NEW, see LIMITS

  Press F7 to LOAD PARTNER'S CRAFT: pulls their whole build onto your
  screen in one shot - blocks, colours, wires, microcontroller internals
  and part settings. Only your side changes. It is also the fix for any
  desync: when in doubt, press F7.

  It also draws your partner's CAMERA and their CURSOR in the workbench,
  so you can see where they are working.

  It connects directly over Steam's network - no server, no ports, no IP
  addresses, and you do NOT need to be in the same Stormworks multiplayer
  session. It is peer-to-peer by Steam friendship.

INSTALL
  1. Run  install.bat
     It finds your Stormworks folder, shows you exactly the two files it
     will copy, and asks before writing anything.
  2. Launch Stormworks normally.

  That is the whole thing. The mod loads with the game every time - there
  is no injector to run and nothing to start each session. You will see it
  start up on the loading screen.

  If you and your partner are Steam friends and you both have this
  installed, you pair AUTOMATICALLY. There is nothing to configure and no
  SteamIDs to exchange.

  To remove it: run  uninstall.bat  (or delete the two files it names).

MANUAL INSTALL  (if you would rather not run a script)
  The installer only copies two files. You can do it yourself in ten seconds,
  and some people reasonably prefer that to running a .bat that writes into
  Program Files:

  1. In Steam, right-click Stormworks > Manage > Browse local files.
     That folder contains stormworks64.exe.
  2. Drag  dinput8.dll  into that folder, next to stormworks64.exe.
  3. Make a folder called  plugins  there, if it does not exist.
  4. Drag  coopworkbench.asi  into  plugins.

  Result:
     <Stormworks>\dinput8.dll
     <Stormworks>\plugins\coopworkbench.asi

  That is exactly, and only, what install.bat does. To uninstall, delete those
  two files. Nothing else is touched and no game file is modified.

  (dinput8.dll is Ultimate-ASI-Loader, a widely used open-source (MIT) plugin
  loader. If another mod already put one there, keep theirs - any version loads
  our plugin.)

WHAT'S IN THIS FOLDER
  install.bat / install.ps1       *** RUN THIS *** installs the mod
  uninstall.bat / uninstall.ps1   removes it, and nothing else
  coopworkbench.asi               the mod itself
  LICENSE-Ultimate-ASI-Loader.txt licence for the bundled loader (MIT)
  dinput8.dll                     Ultimate-ASI-Loader (MIT, third party) -
                                  the standard loader that starts .asi plugins

  Nothing is patched. Both files are additive; uninstalling leaves the game
  exactly as it was.

KEYS
  F7   load your partner's craft (press twice if you have local work to lose)
  F6   overlay / status panel
  F8   in-game log - PgUp/PgDn scroll, Home/End jump, F2 filter
  F9   world overlay on/off
  F10  calibration HUD (and only then do the arrow keys nudge the overlay)
  F4/F5  developer save/load a craft to file

FILES THE MOD WRITES  (beside the mod, in <Stormworks>\plugins\)
  coopworkbench-log.txt     what the mod is doing - the useful bug report
  wsdraw-log.txt            overlay log
  coopworkbench-CRASH.txt   only if it crashes; please attach it to a report

FILES YOU CAN CREATE TO CHANGE BEHAVIOUR  (same folder)
  coop-peer.txt             your partner's SteamID64, to pair manually
                            instead of automatically
  coop-autoconnect-off.txt  turn automatic pairing off
  coop-noprops.txt          turn component-settings sync off

TESTED SETUP / LIMITS - please read
  This is alpha software that modifies a running game in memory. BACK UP
  YOUR CREATIONS.

  Confirmed working across two machines:
    place / delete / paint / rotate / sub-shapes / symmetry, wires
    (electric + logic, connect and disconnect), battery charge, the F7
    whole-craft pull in both directions, the partner camera marker, and
    rejoining mid-session.

  Works but has only been tested on ONE machine so far:
    - COMPONENT SETTINGS. Sliders, names and logic constants now sync
      live. This is new and has not yet been run with a real partner.
      If a setting does not appear, press F7.
    - Automatic pairing, the installer, and the move tool.

  Known limitations:
    - TWO PLAYERS only, and Windows only.
    - MICROCONTROLLERS placed by your partner arrive WRONG. Not merely
      empty: they arrive carrying whatever microcontroller YOU currently
      have selected, at that one's size. Press F7 to pull the real craft
      after either of you places one, and do not build against an MC's
      faces until you have, or everything placed there will be misaligned.
      This is a known bug with a known cause, not a mystery.
    - Both players need the SAME WORKBENCH TYPE. Bench sizes differ, and
      a block that fits one does not fit the other. The mod detects a
      mismatch and blocks sync on purpose rather than corrupting a craft.
    - Both players need the same workshop PART mods, if any. A part your
      partner does not have is skipped, not placed.
    - Career and survival modes are not verified; testing has been in
      Custom.
    - A craft pulled with F7 can land offset from where it should be.
      This is a known open bug.

REPORTING A PROBLEM
  Send coopworkbench-log.txt from BOTH machines - and
  coopworkbench-CRASH.txt if there is one. Remove the 17-digit SteamID64s
  before sharing. Say what you both did and what you each saw; "it didn't
  sync" is usually two different stories on the two screens.

  You can read the log in-game with F8, which is often faster than
  finding the file.

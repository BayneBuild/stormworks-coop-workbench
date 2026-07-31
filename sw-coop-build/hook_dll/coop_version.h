#pragma once
// Single source of truth for the mod version. Shown in the in-game overlay menu and written to the log on
// startup, so a tester's bug report always identifies exactly which build they were running.
// Bump this when handing a new build to testers.
// Keep this in step with the shipped zip name - they drifted once (zips said v0.2.1/v0.2.2 while the overlay
// still reported v0.2.0), which is precisely the confusion this single definition exists to prevent.
// Keep the -alpha suffix: the first public release was v0.1.0-alpha and every package since should carry the
// same signal. The overlay also says EXPERIMENTAL, which is deliberate belt-and-braces for a DLL that people
// inject into their own game.
#define COOP_VERSION "v0.5.0-alpha"

// The version alone cannot tell two builds apart when several are made in one session - which is exactly the
// confusion that had a tester pressing keys against a DLL that had already been replaced, and me diagnosing
// a "silent no-op" that was really an older copy still answering. COOP_BUILD stamps the compile time into
// the OVERLAY as well as the log, so the on-screen text differs on every single rebuild with nothing to
// remember to bump. __TIME__ is "HH:MM:SS"; we show HH:MM.
#define COOP_BUILD_HHMM  { __TIME__[0], __TIME__[1], __TIME__[3], __TIME__[4], 0 }

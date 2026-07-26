#pragma once
// Single source of truth for the mod version. Shown in the in-game overlay menu and written to the log on
// startup, so a tester's bug report always identifies exactly which build they were running.
// Bump this when handing a new build to testers.
// Keep this in step with the shipped zip name - they drifted once (zips said v0.2.1/v0.2.2 while the overlay
// still reported v0.2.0), which is precisely the confusion this single definition exists to prevent.
// Keep the -alpha suffix: the first public release was v0.1.0-alpha and every package since should carry the
// same signal. The overlay also says EXPERIMENTAL, which is deliberate belt-and-braces for a DLL that people
// inject into their own game.
#define COOP_VERSION "v0.2.2-alpha"

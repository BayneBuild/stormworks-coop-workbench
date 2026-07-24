# Contributing to Coop Workbench

Thanks for wanting to help! This started as a solo hobby project, and it's exactly the kind of thing
the Stormworks community can push forward together. Every kind of contribution is welcome — you don't
have to write code to be useful.

## The most useful thing right now: test reports

The mod has only been confirmed in a **narrow** setup (Custom gamemode, starter workbench, both
players empty from the same origin block). The single most valuable contribution today is telling us
**what happens when you try it somewhere else.**

Open an issue with:
- **Where** you tried it (gamemode + which workbench).
- **What you did** and **what happened** (worked / blocks landed in the wrong spot / crash / nothing).
- Both players' `coopworkbench-log.txt` if something went wrong (see below about scrubbing it first).

"I tried it in career mode and blocks appeared 3 tiles off" is a genuinely great bug report.

## Other ways to help

- **Bug reports** — use the 🐞 issue template.
- **Feature ideas** — use the ✨ issue template. See [`ROADMAP.md`](ROADMAP.md) for what's planned and where help is wanted.
- **Code** — pull requests welcome (see below).
- **Docs** — clarifying setup steps or fixing mistakes is real help.

## Before you post logs or files

`coopworkbench-log.txt` and `coop-peer.txt` can contain **SteamID64s** (yours and your partner's). Please
**remove or redact them** before pasting logs into a public issue. Never commit `coop-peer.txt`,
`*-log.txt`, built `.dll`s, or release `.zip`s — they're already in [`.gitignore`](.gitignore).

## Development setup

You'll need:
- **Windows** + **Stormworks** (owned, on Steam).
- **MSVC x64 build tools** (VS2022 Build Tools) including `ml64` (MASM).

Build and load:
```bat
cd sw-coop-build
build-coop.cmd            & rem  builds the mod DLL(s)
```
Then load into the running game with `inject.ps1`, and use `reload-build.ps1` during development to
unload + rebuild without restarting the game.

**You load the mod into your own game yourself** — the injector is user-run by design. See
[`ARCHITECTURE.md`](ARCHITECTURE.md) for how the hooks, networking, and apply path fit together, and
for the note that hooked addresses are resolved by signature scan (so they survive game patches).

## Pull requests

- **Branch off `main`**, keep PRs **small and focused** on one change.
- In the PR description, say **what you tested and how** (which gamemode/bench, one machine or two).
- **Match the surrounding code style** — look at the file you're editing and follow its conventions.
- Don't include personal data (SteamIDs, absolute paths from your machine, logs with IDs).
- It's fine to open a **draft PR** or an issue first to discuss a bigger change before building it.

## Scope & good-citizen rules

- Everything runs **in-memory on your own game**. Contributions must **not modify game files** or
  **ship game assets** — keep it a clean, in-memory mod.
- Be respectful of the game and its developers. This is a fan project that adds a feature on top of a
  game we like; keep it that way.

## Be kind

Assume good faith, be patient with newcomers, and keep discussion friendly. That's the whole culture
we're going for.

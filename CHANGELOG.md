# Changelog

All notable changes to EoW Recomp are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org). See `CONTRIBUTING.md` for the release steps.

## [Unreleased]

### Added
- Unlocked aspect ratio: the camera fills wide windows.
- Logging of shader records the shipped list lacks, once per run.
- "Remove tilt-shift blur" setting (General tab, Off by default), as in LAS Recomp. It switches live.

### Changed
- The app is named EoW Recomp: binaries `eowrecomp` and `eowgame.dylib`, logs `eowrecomp.log`, and the name in
  menus, window titles and docs. Env vars stay `LARECOMP_*`.
- A new app icon of EoW Recomp's own: a staff with a glowing orb under a rift in a dusk sky, in plum and bronze
  (macOS, Windows, iOS and Android).
- Indirect draws run on the GPU by default (no CPU waits, shadows kept).
- LA's 30 fps hold is off for EoW.
- The start screen has EoW's own look: the title "TriCompiled", the game's title movie playing behind it (a still
  of it on Vulkan; a plum and bronze background before the game files are set up), and text in the settings menu's
  colours.

### Fixed
- The start screen accepts EoW's game files (it said "Wrong version of the game").
- The Shadows setting changes EoW's shadows (it only knew LA's shadow map).
- The 3D image is sharp at Match display: EoW draws its 3D at 792 lines, not LA's 1080, and the resolution settings now scale by that (Match display: your screen's pixels; 1x to 4x: 1080 to 4320 lines).
- The app shows its version as v0.1.0 and looks for updates on EoW Recomp's own releases, not LAS Recomp's.
- A cold first start is much faster on Mac (shader preparation 19.5 min -> about 1 min on an M3 Max).
- The underwater area is no longer black (shader builds run at the default priority).

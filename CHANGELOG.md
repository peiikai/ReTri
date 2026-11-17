# Changelog

All notable changes to ReTri are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org). See `CONTRIBUTING.md` for the release steps.

## [Unreleased]

### Added
- Unlocked aspect ratio: the camera fills wide windows.
- Logging of shader records the shipped list lacks, once per run.
- "Remove tilt-shift blur" setting (General tab, Off by default), as in LAS Recomp. It switches live.
- Android (arm64): the app ReTri (`com.retri.android`), which installs next to LAS Recomp. The game code
  is built on the device from your own game files, as in LAS Recomp's Android port.
- Touch controls for EoW: the whole Switch pad on screen (stick, D-pad, A B X Y, L R ZL ZR, + and -), each
  control movable and resizable in the layout editor. A controller hides them, a touch brings them back.
- Button icons follow the controller, as in LAS Recomp: PlayStation pads show Cross, Circle, Square and Triangle,
  Xbox pads their own letters, in the HUD, menu prompts and text. "Button icons" setting (Controls tab).

### Changed
- Button layout "Xbox / PlayStation" swaps the bottom and right buttons on every controller, Nintendo-style ones too.
- The start screen title is in Hylia Serif, a free fan font by Omni Jacala (credited in the README).
- The window and error dialogs are titled "ReTri", the project's new name (a play on recomp and Tri).
- All app text says ReTri: menus, the start screen, error and crash messages, in every language. The app,
  disk image, Windows zip, AppImage and iOS files are named ReTri too (`ReTri.app`, `ReTri-<version>-...`).
  Binaries, the data folder and `LARECOMP_*` env vars keep their names, so saves and settings stay where they are.
- The app is named EoW Recomp: binaries `eowrecomp` and `eowgame.dylib`, logs `eowrecomp.log`, and the name in
  menus, window titles and docs. Env vars stay `LARECOMP_*`.
- A new app icon of EoW Recomp's own: a staff with a glowing orb on a hill at dusk, a small rift on a far hill,
  in plum and bronze (macOS, Windows, iOS and Android).
- Indirect draws run on the GPU by default (no CPU waits, shadows kept).
- LA's 30 fps hold is off for EoW.
- The start screen has EoW's own look: the title "ReTri", the game's title movie playing behind it (a still
  of it on Vulkan; a plum and bronze background before the game files are set up), and text in the settings menu's
  colours.

### Removed
- LAS Recomp's Link's Awakening randomizer (seed generator, tracker, spoiler log and its game features). The start
  screen's Randomizer row is hidden until EoW has a randomizer of its own.

### Fixed
- Retroid Pocket: the face buttons match their labels (A and B, X and Y were swapped).
- Vulkan (Windows, Linux, Android): the hearts show as red hearts, not dark hearts in a red box.
- Building the game code no longer stalls for half an hour or more on two very large functions the game barely
  runs: they are built without optimisation (every platform).
- Vulkan: draws that read what a compute pass just wrote get the GPU's own copy of it (EoW's interiors were darker
  than on Mac), without stopping the CPU to wait.
- The start screen's game folder help names Echoes of Wisdom and ReTri, not Link's Awakening (all languages).
- The start screen accepts EoW's game files (it said "Wrong version of the game").
- The Shadows setting changes EoW's shadows (it only knew LA's shadow map).
- The 3D image is sharp at Match display: EoW draws its 3D at 792 lines, not LA's 1080, and the resolution settings now scale by that (Match display: your screen's pixels; 1x to 4x: 1080 to 4320 lines).
- The app shows its version as v0.1.0 and looks for updates on EoW Recomp's own releases, not LAS Recomp's.
- A cold first start is much faster on Mac (shader preparation 19.5 min -> about 1 min on an M3 Max).
- The underwater area is no longer black (shader builds run at the default priority).
- The start screen builds EoW's game code with EoW's own layout (it used LA's).
- The start screen's shader preparation no longer fails EoW's compute shaders with surface stores.
- The echo list and the pause screen fill windows of any shape with Aspect ratio Unlocked; the echo list's band behind
  the echoes no longer shows the world squeezed (it showed the window's whole width in the 16:9 middle).

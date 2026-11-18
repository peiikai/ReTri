# Changelog

All notable changes to ReTri are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org). See `CONTRIBUTING.md` for the release steps.

## [Unreleased]

### Added
- Controls > Configure inputs > ReTri Menu: choose the controller buttons for Confirm, Back, Option and Confirm text
  in ReTri's own menus. Tabs (L / R) and the D-pad stay fixed. Until you change one, the menus follow the Confirm
  button setting.
- Unlocked aspect ratio: the camera fills wide windows.
- Logging of shader records the shipped list lacks, once per run.
- "Remove tilt-shift blur" setting (General tab, Off by default), as in LAS Recomp. It switches live.
- iPhone and iPad (ReTri for iOS, bundle ID com.peiikai.retri.ios): the app holds the game code built on a Mac from
  your own game files.
- Touch controls for EoW: the whole Switch pad on screen (stick, D-pad, A B X Y, L R ZL ZR, + and -), each
  control movable and resizable in the layout editor. A controller hides them, a touch brings them back.
- Button icons follow the controller, as in LAS Recomp: PlayStation pads show Cross, Circle, Square and Triangle,
  Xbox pads their own letters, in the HUD, menu prompts and text. "Button icons" setting (Controls tab).
- Windows (x86-64, Vulkan): plays Echoes of Wisdom at 60 fps, with the same picture as on Mac. The zip sets the game
  up from your own extracted files and builds the game code on your PC (about an hour the first time).
- The EoW touch controls look like the inventory screen's background (its felt and colour, the item slots'
  edges, see-through and lit when pressed as in LAS), laid out as in LAS. Y shows the echo or item on the HUD's Y slot.
- Phones and tablets, EoW: the hearts go to the screen's upper left corner, the mini map to the upper right with the
  Y / X buttons beside it. "Hide X/Y item HUD" works for EoW, and the touch X shows the HUD's X item.
- Phones and tablets, EoW: a drag on the game's screen moves the camera, as the right stick.

### Changed
- With Dynamic resolution on, the FPS target sets EoW's frame rate (60, 30, or 30 outdoors and 60 indoors) and the
  resolution does the rest, as in LAS Recomp; the game's own 60 / 30 switching is off.
- "Button layout" is now "Confirm button" (Right or Bottom). It applies the same way to every controller, in the
  game and in ReTri's menus. Before, ReTri's menus could confirm with the other button when "Button icons" was set
  to a different controller style than the one plugged in.
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
- EoW's final image and HUD are drawn at the window's size, not larger and then scaled down: less GPU work at
  every resolution setting (on an 11" iPad, 4 million pixels instead of 7.5).
- The start screen has EoW's own look: the title "ReTri", the game's title movie playing behind it (a still
  of it on Vulkan; a plum and bronze background before the game files are set up), and text in the settings menu's
  colours.

### Removed
- LAS Recomp's Link's Awakening randomizer (seed generator, tracker, spoiler log and its game features). The start
  screen's Randomizer row is hidden until EoW has a randomizer of its own.

### Fixed
- Mac: trees, bushes and grass cast shadows again.
- Shadow edges no longer show steps at Shadows 2x and 4x.
- Mac: chests and other metal keep their shine in shadow.
- Vulkan (Windows, Linux, Android): the hearts show as red hearts, not dark hearts in a red box.
- Vulkan: draws that read what a compute pass just wrote get it, as on Mac (EoW's interiors were darker).
- Vulkan: the rifts read the scene's stencil and line their map up, as on Mac.
- Rift edges are clean lines instead of steps: the rift map now uses the game's final, closer view, as on the Switch.
- On iPhone, iPad and Android the game no longer stays at 30 fps after a brief dip: it tries 60 again after a
  second (the GPU slows its clock at 30, and the game read that as too slow for 60).
- Settings tabs keep their box under the mouse or the controller cursor (it was see-through).
- Memory no longer grows during play: the extra command memory the game runs out of is reused instead of leaked.
- The start screen's game folder help names Echoes of Wisdom and ReTri, not Link's Awakening (all languages).
- The start screen accepts EoW's game files (it said "Wrong version of the game").
- Before any game files are chosen, the start screen has EoW's look and names Echoes of Wisdom (it showed LA's).
- The Shadows setting changes EoW's shadows (it only knew LA's shadow map).
- The 3D image is sharp at Match display: EoW draws its 3D at 792 lines, not LA's 1080, and the resolution settings now scale by that (Match display: your screen's pixels; 1x to 4x: 1080 to 4320 lines).
- The app shows its version as v0.1.0 and looks for updates on EoW Recomp's own releases, not LAS Recomp's.
- A cold first start is much faster on Mac (shader preparation 19.5 min -> about 1 min on an M3 Max).
- The underwater area is no longer black (shader builds run at the default priority).
- The start screen builds EoW's game code with EoW's own layout (it used LA's).
- The start screen's game code generator knows Echoes of Wisdom (the packaged one said "unknown game eow").
- The start screen's shader preparation no longer fails EoW's compute shaders with surface stores.
- Building the game code no longer spends half an hour on one file (a function the game rarely runs is built
  without optimisation).
- The echo list and the pause screen fill windows of any shape with Aspect ratio Unlocked; the echo list's band behind
  the echoes no longer shows the world squeezed (it showed the window's whole width in the 16:9 middle).

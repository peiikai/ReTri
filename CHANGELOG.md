# Changelog

All notable changes to ReTri are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org). See `CONTRIBUTING.md` for the release steps.

## [Unreleased]

### Added
- Updates from the start screen, as in LAS Recomp: when a newer release is out, its link opens what's new, with
  Download. Windows, Linux (AppImage) and macOS can update in place, checked against a signed update file; this
  stays off for releases until it has been tested on every platform.
- Controls > Configure inputs > ReTri Menu: choose the controller buttons for Confirm, Back, Option and Confirm text
  in ReTri's own menus. Tabs (L / R) and the D-pad stay fixed. Until you change one, the menus follow the Confirm
  button setting.
- Unlocked aspect ratio: the camera fills wide windows.
- Logging of shader records the shipped list lacks, once per run.
- Command line for game launchers (`eowrecomp --help`): set up without a window (`--prepare`), start straight in,
  pick the data folder, change settings for one run. A RecompLauncher profile ships with the Windows zip and the
  Linux files. Windows zips are named `windows-x64` / `windows-arm64`.
- The RecompLauncher profile names what the PC needs (Windows 10, glibc 2.35, Vulkan 1.3), so the launcher can say
  when a PC is too old.
- "Remove tilt-shift blur" setting (General tab, Off by default), as in LAS Recomp. It switches live.
- FPS setting under Resolution: 30 or 60 (default 60), used while Dynamic resolution is off. It switches live.
- Android (arm64): the app ReTri (`com.retri.android`), which installs next to LAS Recomp. The game code
  is built on the device from your own game files, as in LAS Recomp's Android port.
- iPhone and iPad (ReTri for iOS, bundle ID com.peiikai.retri.ios): the app holds the game code built on a Mac from
  your own game files.
- Touch controls for EoW: the whole Switch pad on screen (stick, D-pad, A B X Y, L R ZL ZR, + and -), each
  control movable and resizable in the layout editor. A controller hides them, a touch brings them back.
- Button icons follow the controller, as in LAS Recomp: PlayStation pads show Cross, Circle, Square and Triangle,
  Xbox pads their own letters, in the HUD, menu prompts and text. "Button icons" setting (Controls tab).
- Windows (x86-64, Vulkan): plays Echoes of Wisdom at 60 fps, with the same picture as on Mac. The zip sets the game
  up from your own extracted files and builds the game code on your PC (about an hour the first time).
- Linux (x64 and arm64, Vulkan): an AppImage, `ReTri-<version>-linux-<x64|arm64>.AppImage`, and the same files as a
  tarball. Like the Windows zip, it sets the game up from your own extracted files and builds the game code on your PC.
- The EoW touch controls look like the inventory screen's background (its felt and colour, the item slots'
  edges, see-through and lit when pressed as in LAS), laid out as in LAS. Y shows the echo or item on the HUD's Y slot.
- Phones and tablets, EoW: the hearts go to the screen's upper left corner, the mini map to the upper right with the
  Y / X buttons beside it. "Hide X/Y item HUD" works for EoW, and the touch X shows the HUD's X item.
- Phones and tablets, EoW: a drag on the game's screen moves the camera, as the right stick.
- Game versions 1.0.0 and 1.0.1 work too, besides 1.0.2. The start screen shows which version the game files are.

### Changed
- The start screen prepares 836 more shader variants (from a long play test), so fewer stutters the first time a
  scene or effect shows.
- Language: Portuguese (Brazil) now changes the game's own text too, not just the menus (it stayed English). New
  options Spanish (Latin America) and French (Canada) use the game's own text for them; Auto picks them from your
  computer's region.
- Menus use the EoW colours without LAS Recomp's gold: buttons such as Show in Finder, Import fonts, Return and
  Apply are plain, lit only under the mouse or the controller cursor; headings are bronze, arrows cream. The start
  screen's title and menu are cream and bronze too. The System tab no longer mentions seeds.
- The game always runs at 60 fps: its own switch to 30 fps when frames run long is off (it could get stuck at 30).
  On slower devices, set FPS (under Resolution) to 30, or turn on Dynamic resolution and pick its FPS target.
- The game code generator is part of the app (C++, from LAS Recomp): no release ships Python any more, and setup
  generates the game code in seconds instead of a minute or more.
- Setup no longer spends 10 minutes or more compiling one large function the game runs once at start: it is built
  without optimisation (every platform).
- Menus (from LAS Recomp): the left stick steps one row per push, and a held direction scrolls slower. In Configure
  inputs, the right column is empty while Device is selected, and the Open menu shortcuts are under ReTri Menu.
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
- A new app icon of ReTri's own: a staff with a glowing orb on a hill at dusk, a small rift on a far hill,
  in plum and bronze (macOS, Windows, iOS and Android).
- Indirect draws run on the GPU by default (no CPU waits, shadows kept).
- LAS Recomp's 30 fps hold is off (it was made for Link's Awakening).
- EoW's final image and HUD are drawn at the window's size, not larger and then scaled down: less GPU work at
  every resolution setting (on an 11" iPad, 4 million pixels instead of 7.5).
- The start screen has EoW's own look: the title "ReTri", the game's title movie playing behind it (a still
  of it on Vulkan; a plum and bronze background before the game files are set up), and text in the settings menu's
  colours.

### Removed
- LAS Recomp's Link's Awakening randomizer (seed generator, tracker, spoiler log and its game features). The start
  screen's Randomizer row is hidden until EoW has a randomizer of its own.

### Fixed
- The iOS app's licence texts: only SDL3's and FFmpeg's from the build, no leftover ones from before Python was
  removed.
- Unlocked aspect ratio: the map no longer shows unexplored areas beside the 16:9 part on wide windows.
- Mac, Aspect ratio Unlocked: rifts no longer flash in wide windows, and their edges are no longer a staircase.
  The outline jumped every few frames while standing still and turned to big steps for a few frames about once a
  second; the rift map now has more pixels in wide windows, so the edges look as they do at 16:9.
- Mac: the frame rate no longer slides from 60 to under 30 after the window loses focus (seen at 5120x1440). The
  GPU slowed its clock because each frame's present deadline was set from when it was sent, which a late frame
  still met. Deadlines now follow the 60 fps schedule, so a late frame asks for a faster clock.
- Ambient occlusion Off removes the soft shadows in corners and around objects (it changed nothing), and High
  draws them sharper.
- Retroid Pocket: the face buttons match their labels (A and B, X and Y were swapped).
- With Aspect ratio Unlocked, the lock-on reticle sits on its target in wide windows (it was pulled toward the
  centre), and so do action prompts and other markers placed over things in the world.
- Mac: trees, bushes and grass cast shadows again.
- Shadow edges no longer show steps at Shadows 2x and 4x.
- Mac: chests and other metal keep their shine in shadow.
- The rift no longer disappears while talking to someone next to it.
- Short frame-time spikes in play (30-47 ms every few seconds): the audio renderer made its 5 ms frames in bursts of
  the audio device's period (21 ms on Mac), so the game updated its audio every 21 ms and its main thread waited
  that long whenever it freed an audio buffer. The frames now keep their own 5 ms clock, as on the Switch.
- A crash at start in a wide window with MetalFX on (MetalFX was asked for an image of no width).
- Wide windows: the first frames at start no longer show an image of no width (the 16:9 box opened from nothing).
- No more drops to 35-55 fps in busy areas (seen on Mac): the game's own dynamic resolution is off, as in LAS Recomp. It
  raised the 3D size on a fast GPU (up to 1.9x the window's pixels at Match display) until frames ran late.
- Vulkan (Windows, Linux, Android): the hearts show as red hearts, not dark hearts in a red box.
- Vulkan: draws that read what a compute pass just wrote get it, as on Mac (EoW's interiors were darker).
- Vulkan: the rifts read the scene's stencil and line their map up, as on Mac.
- Vulkan: chests and other metal keep their shine in shadow (the environment map was black).
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
- The app shows its version as v0.1.0 and looks for updates on ReTri's own releases, not LAS Recomp's.
- A cold first start is much faster on Mac (shader preparation 19.5 min -> about 1 min on an M3 Max).
- The underwater area is no longer black (shader builds run at the default priority).
- The start screen builds EoW's game code with EoW's own layout (it used LA's).
- The start screen's game code generator knows Echoes of Wisdom (the packaged one said "unknown game eow").
- The start screen's shader preparation no longer fails EoW's compute shaders with surface stores.
- Building the game code no longer spends half an hour on one file (a function the game rarely runs is built
  without optimisation).
- The echo list and the pause screen fill windows of any shape with Aspect ratio Unlocked; the echo list's band behind
  the echoes no longer shows the world squeezed (it showed the window's whole width in the 16:9 middle).

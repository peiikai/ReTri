# Contributing to ReTri

ReTri is a private project for now. This file sets out how work is organised, so that every change follows the
same path from the start.

## The one hard rule: no Nintendo files

Nothing owned by Nintendo is ever committed or attached to an issue or pull request. That means:
- no game files, extracted or otherwise
- no generated code (`generated*/`)
- no fonts
- no textures, models, music, text or level files

Screenshots and short clips in bug reports are fine.

## Versions

ReTri uses [Semantic Versioning](https://semver.org): `MAJOR.MINOR.PATCH`.

- The version lives in `VERSION`. On `develop` it is the next version being worked on.
- Before 1.0, a MINOR release (0.2.0) carries new features and may change behaviour or settings. A PATCH release
  (0.1.1) carries fixes only.
- Releases are tagged `vX.Y.Z` on `main`. Only release tags go on `main`.
- Every user-facing change gets a line under `Unreleased` in `CHANGELOG.md` in the same PR.

## Branches

| Branch | What it holds | Branches from | Merges into |
| --- | --- | --- | --- |
| `main` | released code only, one tag per release | | |
| `develop` | the next version | | `release/X.Y` |
| `release/X.Y` | one release line: its last bug fixes, then patch releases | `develop` | `main` (tagged), then back into `develop` |
| `feature/<name>` | new features | `develop` | `develop` |
| `fix/<issue>-<name>` | bug fixes | `develop`, or `release/X.Y` for a fix that release needs | the branch it came from (`release/X.Y`, then back into `develop`) |
| `docs/<name>`, `ci/<name>`, `chore/<name>` | docs, CI, upkeep | `develop` | `develop` |

Rules:
- Branch names are lower case with dashes, for example `fix/12-text-box-flicker`. Never `claude/...`.
- A fix targets `release/X.Y` when that release needs it, otherwise `develop`. The maintainer decides which version
  ships it. A release branch is merged back into `develop`, so its fixes reach the next version too.
- Features never target `release/X.Y`: they go into `develop` and ship in the next version.
- `main`, `develop` and `release/*` only change through pull requests. A local pre-push hook refuses direct pushes
  to them (see below).
- Merge pull requests with a merge commit, so each branch's history stays readable. Delete the branch after it is
  merged.

## Making a release

1. Cut `release/X.Y` from `develop` when the version's features are in.
2. On the release branch, set `VERSION` to `X.Y.0` and move the `Unreleased` lines in `CHANGELOG.md` under
   `## [X.Y.0] - <date>`. Only fixes land on the release branch from here on.
3. Open a PR from `release/X.Y` into `main`. Once it's merged, tag the merge commit `vX.Y.0`.
4. Merge `release/X.Y` back into `develop`, then set `VERSION` on `develop` to the next version.
5. For a patch release, merge its fixes into `release/X.Y`, then repeat steps 2 to 4 with `X.Y.Z`.

Tags are plain `vX.Y.Z`: the update check ignores anything after the version (`v0.1.0-beta.1` is never offered).
During the beta, `v0.N.M` shows as "Beta N.M".

### Update files

The start screen offers newer releases (runtime/ui/launcher.mm), and Windows, Linux and macOS can update in place
(runtime/update.h; macOS through Sparkle). Each platform's release file needs its signed update file next to it, made
with the signing key in `UPDATE_SIGNING_KEY` (base64; never in the repo, a log or a note):

- Windows: `tools/update_manifest.sh vX.Y.Z windows-x64 build/dist/ReTri-vX.Y.Z-windows-x64.zip build/dist/ReTri`
  (and `windows-arm64`), after `make dist`.
- Linux: `tools/update_manifest.sh vX.Y.Z linux-x64 build/dist/ReTri-vX.Y.Z-linux-x64.AppImage` (and `linux-arm64`).
- macOS: `tools/sparkle_appcast.sh vX.Y.Z build/dist/ReTri-vX.Y.Z-macOS.dmg`; once the release is published,
  `tools/sparkle_notes.sh --publish vX.Y.Z` puts its notes in the feed.

Upload `build/dist/update-<platform>.txt` and `appcast-macOS.xml` with the files. Their names never change: every
build out there fetches them by name. Without the key the scripts only warn, and players use Download.

## Commits and pull requests

- Commit messages are plain English: a short first line saying what changed and why.
- Each PR does one thing. It names its issue (`Fixes #12`), says how it was tested and updates `CHANGELOG.md`.
- PRs target `develop`. A fix may target `release/X.Y` instead; features never do.
- The game is play-tested before any PR touching rendering, timing or audio is merged.

## Pre-push guard

`git config core.hooksPath ~/.config/eowrecomp-hooks` installs the hooks. They refuse:
- pushes to `main`, `develop` and `release/*` (they change only through PRs)
- any commit not authored as Peiikai

To bootstrap a release branch, the maintainer can allow a single push with `EOW_ALLOW_PROTECTED_PUSH=1`.

## Building

You need your own extracted game files of Echoes of Wisdom (version 1.0.0, 1.0.1 or 1.0.2), with `exefs/` and `romfs/` in a folder
named `extracted/` in the checkout. Then:

    make -j GEN=generated-eow                 # the app, build/eowrecomp
    make gen GEN=generated-eow                # once: the game code, from extracted/exefs, into generated-eow/
    make -j GEN=generated-eow                 # the game code library, build/eowgame.*
    ./build/eowrecomp extracted               # the start screen

`GEN=generated-eow` matters: the Makefile still picks LA's layout when `GEN` doesn't name EoW. Without `make gen`,
run `tools/fetch_zig.sh` once and set the game files up on the start screen: it builds the game code itself, as
for players.

Each platform has its own page: macOS needs Apple Silicon, Homebrew `sdl3` and `ffmpeg` (`make app` and `make dmg`
package it, after `make deps` and `tools/fetch_zig.sh`); Windows: [docs/BUILD-windows.md](docs/BUILD-windows.md);
Linux: [docs/linux.md](docs/linux.md); iPhone and iPad: [docs/ios.md](docs/ios.md); Android:
[docs/android.md](docs/android.md).

## Project layout

- `runtime/gen/`: the game code generator (C++, also built into the app). It turns `exefs/main` and `exefs/sdk` into
  C. `patches.cpp` changes or hooks game instructions while generating; EoW's are in `build_other_patches`.
- `runtime/game_addresses/`: `games.h` lists the supported EoW builds (1.0.0, 1.0.1, 1.0.2) by the main program's
  build ID, each with its own address table `eow_v1_0_x.h` (read through `OTHER(NAME)`). The other tables in that
  folder (`versions.h`, `patches_v1_0_*.h`, `runtime_v1_0_*.h`, read through `GA(NAME)`) are LA's, from LAS Recomp.
- `runtime/`: the host.
  - `kernel.c`, `loader.c`, `services/`: the Switch OS stand-in (threads, memory, files and saves, input, audio).
  - `hle/`: SDK replacements and game features; EoW's are the `*_other.c` files.
  - `nvn/`: graphics. Recording (`cmdbuf.c`), Metal (`renderer.m`, `metal.m`), Vulkan (`vulkan/`), the shader
    translator (`maxwell/translate.c`).
  - `ui/`: the start screen and the overlay menu (Dear ImGui), with the translation tables in `ui/lang/`.
  - `platform/`: one file per OS.
- `android/`: the Android app (Gradle) and its dependency scripts.
- `tools/`: tests, packaging scripts, and the Python reference generator (`gen.py`, `patches.py`).
- `docs/`: [architecture](docs/architecture.md), [debug hooks](docs/debug-hooks.md), saves, per-platform pages.

## Testing your change

- Build cleanly on your platform. If you touch rendering, say which backend you tested: a Metal change usually needs
  a Vulkan counterpart, and the other way round.
- Test in game and look at the result: a fix isn't verified until you've seen it work.
- `tools/shot.sh <frame> [seconds]` runs the game with `LARECOMP_*` switches (scripted input, warps) and saves a
  screenshot. Give test runs a scratch save folder (`LARECOMP_SAVE_DIR`, `tools/fresh_save.sh`) so your own saves
  stay untouched. The switches are listed in [docs/debug-hooks.md](docs/debug-hooks.md).
- `LARECOMP_VK_VALIDATION_STOP=1` stops a Vulkan run at the first validation error (needs the Khronos validation
  layer). `LARECOMP_VK_VALIDATION=1` only logs them.

## Tests

None of these need game files unless they say so.

- `python3 tools/menu_strings.py check`: what each menu language is missing (untranslated text shows in English).
- `python3 tools/test_save.py`: the save handlers commit and reload, and a failed save keeps the last one.
- `python3 tools/test_build_cache.py` and `python3 tools/test_gamebuild_resume.py`: the game code build's cache and
  resume.
- `python3 tools/test_cli.py`: the command line for launchers (needs `build/eowrecomp`).
- `python3 tools/test_data_move.py`: moving the data folder.
- `python3 tools/test_gen_cpp.py build/lasgen <extracted folder> --game eow`: the C++ generator against the Python
  one (needs game files).
- `python3 tools/test_game_addresses.py`: the address tables of every game version and the patch output (with
  `--game KEY=FOLDER`, also against your game files).
- The updater: `tools/test_update.cpp` (manifest, signature, the swap and its rollback; build line at its top),
  `tools/test_update_e2e.sh` (download, check, swap and restart against a local web server),
  `tools/test_update_tags.cpp` (tags, labels, the update dialog's changelog) and `python3 tools/test_update_paths.py`
  (what each shipped build offers).
- Windows: `python tools/test_windows_processes.py` (helper processes and crash exit codes).
- Shaders: `tools/test_translate.sh` and `tools/test_vk_pipelines.sh` ([docs/BUILD-windows.md](docs/BUILD-windows.md)).

## The game code generator

`runtime/gen/` turns the game's executables into C. The app runs it during setup, and `make lasgen` builds it as a
command-line tool (`make gen GEN=generated-eow` uses it). `tools/gen.py` and its modules are the Python reference it
was ported from, kept until the port has shipped: a generator, patch or chunk-cut change goes into both, and
`tools/test_gen_cpp.py build/lasgen <extracted folder> --game eow` must report the C identical (gen.py needs a Python
with unicorn, capstone and lz4).

# Notes for agents

- **What this is.** ReTri statically recompiles the Switch release of *Echoes of Wisdom* (EoW) into native apps for
  macOS, Windows, Linux, iPhone / iPad and Android. It is built on LAS Recomp, the *Link's Awakening* (LA)
  recompiler. The runtime still holds some of LA's code (its address tables, patches and hooks); ReTri doesn't use
  it, and EoW's own code paths are named `*_other` / `eow_*`.
  - The generator (`runtime/gen/`, C++, part of the app) turns the game's executables into C, which is built on the
    player's machine from their own extracted game files. `tools/gen.py` is the Python reference it was ported from.
  - A host stands in for the Switch OS and services (`runtime/kernel.c`, `loader.c`, `services/`).
  - The game's NVN graphics are drawn with Metal on macOS, iPhone and iPad, and Vulkan on Windows, Linux and
    Android (`runtime/nvn/`).
  - Most features live in `runtime/hle/` and `runtime/ui/`. There is no randomizer: LA's was removed, and the
    Randomizer entry stays hidden (`RANDO_ON` in `runtime/rando.h`).
  - [README.md](README.md) is the player-facing page. [CONTRIBUTING.md](CONTRIBUTING.md) has the layout, building,
    tests and pull request rules.

- **Required reading.** Before you change anything, read all of [CONTRIBUTING.md](CONTRIBUTING.md) and
  [docs/architecture.md](docs/architecture.md). Then read the doc for the area you are working in from the table
  below ([debug-hooks](docs/debug-hooks.md), [vulkan-gpu-defaults](docs/vulkan-gpu-defaults.md),
  [save-format](docs/save-format.md), the build guides). Don't skim them or work from guesses about how the project
  is set up. If what you find in the code disagrees with a doc, say so in your pull request.

- **Where to start:**

  | Task | Read first |
  |---|---|
  | How the parts fit together | [docs/architecture.md](docs/architecture.md) |
  | Building | CONTRIBUTING "Building", [docs/BUILD-windows.md](docs/BUILD-windows.md), [docs/linux.md](docs/linux.md), [docs/ios.md](docs/ios.md), [docs/android.md](docs/android.md) |
  | A crash or hang | the log (`eowrecomp.log` in the data folder): the crash line names the guest address (`main+0x...`). Map it with `tools/sym.py` and read the code there before changing anything. Debug switches: [docs/debug-hooks.md](docs/debug-hooks.md) |
  | Rendering | `runtime/nvn/cmdbuf.c` (recording), `renderer.m` / `metal.m` (Metal), `vulkan/exec.c` (Vulkan), `maxwell/translate.c` (shaders); per-GPU Vulkan defaults: [docs/vulkan-gpu-defaults.md](docs/vulkan-gpu-defaults.md) |
  | Game behaviour or a new feature | `runtime/hle/` (EoW's: `*_other.c`) and EoW's patches in `runtime/gen/patches.cpp` (`build_other_patches`; `tools/patches.py` is the reference) |
  | Saves | [docs/save-format.md](docs/save-format.md), `runtime/services/fs.c` |
  | Game addresses | CONTRIBUTING "Project layout" (`runtime/game_addresses/games.h`, `eow_v1_0_x.h`, `OTHER(NAME)`) |
  | Menu text | `python3 tools/menu_strings.py check` |

- **No Nintendo files, ever.**
  - Never commit, attach or paste game files, extracted data, generated code (`generated*/`), fonts or assets.
  - Game files stay outside the repository.
  - Refer to them only as the player's own extracted game files. Don't write about how they are obtained, in code,
    comments, docs, commits or pull requests.

- **Branch.** `main` holds the last release and is not where work happens. Base your work on `develop` and open the
  pull request against it; the maintainer decides which version ships a fix. Name the branch
  `type/short-name`: `fix/<issue>-<name>`, `feature/<name>`, `docs/<name>`, `ci/<name>` or `chore/<name>`. Check the
  base branch first: the change you have in mind may already be there. Releases are tagged `v<major>.<minor>.<patch>`
  (`v0.1.0` is Beta 1).

- **Fix the cause, not the symptom.**
  - Before changing code, find why it happens: the faulting instruction, the value that went wrong and who wrote
    it, and the NVN / Metal / Vulkan rule the host gets wrong. Fix that place.
  - A stub, a skipped call, a clamp, a retry or a longer timeout is a workaround. Use one only when the cause is
    known and can't be fixed where it lies (usually the game's own code). Say so in a comment.
  - Patch the game's own code minimally rather than replacing whole systems.

- **Addresses and patches.**
  - EoW ships as 1.0.0, 1.0.1 and 1.0.2. Each build is a row of `runtime/game_addresses/games.h`, found by its main
    program's build ID, with its own table (`eow_v1_0_x.h`, the same names in each). Unknown build IDs are refused.
  - Every EoW address goes through those tables: `OTHER(NAME)` in the runtime, `T("NAME")` in `runtime/gen/patches.cpp`
    (`build_other_patches`) and `T['NAME']` in `tools/gen.py`. The `GA(NAME)` tables (`versions.h`,
    `patches_v1_0_*.h`, `runtime_v1_0_*.h`) are LA's.
  - Run `python3 tools/test_game_addresses.py` after touching addresses.
  - Every patch asserts the original instruction word, so a wrong address fails loudly. Keep it that way.
  - A generator or patch change goes into both `runtime/gen/` and `tools/gen.py` / `tools/patches.py`, and
    `tools/test_gen_cpp.py` must report the C identical (CONTRIBUTING "The game code generator").

- **Both graphics backends.**
  - A rendering fix on Metal usually needs the Vulkan counterpart, and the other way round. Say in the pull
    request which backend you tested.
  - Workarounds for one GPU or driver go in the per-GPU defaults in `runtime/nvn/vulkan/exec.c`
    (`vk_exec_init`), keyed by vendor ID. Each one has a `LARECOMP_VK_*` switch to turn it off, a comment saying
    which driver it was tested on, and a row in [docs/vulkan-gpu-defaults.md](docs/vulkan-gpu-defaults.md).
  - macOS is Metal only. Don't add a MoltenVK path.
  - Minimums: macOS 15 on Apple Silicon; Windows and Linux with Vulkan 1.3; x86 CPUs with AVX2 and FMA; iOS 16.4;
    Android 11 (arm64).

- **Testing.**
  - A fix is verified only when you've seen it work in game. A clean build or a quiet log is not enough.
  - `tools/shot.sh <frame> [seconds]` runs the game with scripted input (`LARECOMP_INPUT`) and warps
    (`LARECOMP_WARP_AT`), and saves a screenshot. Many more debug hooks are
    documented where they are defined (grep `getenv("LARECOMP_`).
  - For test runs, use a scratch save folder (`LARECOMP_SAVE_DIR`, `tools/fresh_save.sh`) so the user's saves
    and settings stay untouched. Mute them with `LARECOMP_NO_AUDIO=1` and `LARECOMP_NO_MOVIE_AUDIO=1` unless the
    test is about sound.
  - Never close or restart a copy of the game the user is playing. Run one test window at a time (`shot.sh` takes
    a lock).
  - The unit tests are listed in CONTRIBUTING "Tests".

- **Menus and text.**
  - Settings get short labels and vanilla defaults: no presets, no paragraphs.
  - New text is written in English only. Don't edit the translation tables (`runtime/ui/lang/`): we fill them for
    every language before each release, and untranslated text shows in English until then. A fix to an existing
    translation from a native speaker is welcome as its own pull request.
  - Player-facing text (menus, README, release notes, issue replies) is plain and short.

- **Code.**
  - Match the surrounding code: naming, structure, comment density.
  - Comments explain why (what the game does, which bug or save showed it), not what the next line does.
  - Code copied from other projects must be MIT or CC0, and credited. GPL or unlicensed code can be read for
    ideas but not copied.

- **Pull requests.**
  - One change per pull request.
  - Say what changed, why, and how it was tested (platform, GPU, where in the game, screenshots).
  - No personal paths, names or other personal information in code, comments, commits or pull requests.

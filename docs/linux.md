# ReTri on Linux (x64 and arm64)

ReTri runs on Linux with Vulkan, the same renderer as Windows. Releases come as an AppImage and a tarball for x64 and
arm64; arm64 is less tested. `make appimage` packages the AppImage.

## Running the AppImage
    chmod +x ReTri-<version>-linux-x64.AppImage
    ./ReTri-<version>-linux-x64.AppImage

Needs: a distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch, ...), a Vulkan 1.3 driver
(Mesa RADV / ANV / NVK / Turnip / Asahi Honeykrisp, or NVIDIA's), and on x86-64 a CPU with AVX2 (2013 or newer).
On Linux 6.7 or newer the renderer also gets page write tracking (userfaultfd + PAGEMAP_SCAN, as Windows' write watch):
its vertex cache, and textures re-checked only where the game wrote. Older kernels run without it (the log says
`[plat] no page write tracking: ...`); `LARECOMP_WRITE_TRACKING=0` turns it off.
The start screen asks for the folder with your own game files and sets them up.

Everything the app writes lives in `~/.local/share/retri` (`$XDG_DATA_HOME/retri`): game files (`extracted/`),
saves, settings (`larecomp.ini`), shader caches, imported fonts. Started without a terminal the log is
`eowrecomp.log` in that folder, always the latest run; up to five recent runs are kept as `eowrecomp-backup-*.log`
(from a terminal it prints there instead). `--data-dir <folder>` (or `LARECOMP_DATA_ROOT`) uses another folder for
one run; Settings > System > Data folder moves it for good.

To keep everything next to the AppImage instead, make the AppImage's portable folder once:

    ./ReTri-<version>-linux-x64.AppImage --appimage-portable-home

That creates `ReTri-<version>-linux-x64.AppImage.home` next to it (making that folder by hand works too), and
the data goes in its `.local/share/retri`.

## The tarball (no FUSE)

`ReTri-<version>-linux-<x64|arm64>.tar.gz` holds the same files as the AppImage in one `ReTri` folder. Use it
where an AppImage can't mount itself, like inside a Flatpak sandbox:

    tar -xzf ReTri-<version>-linux-x64.tar.gz
    "ReTri/eowrecomp"

It keeps its data in its own folder, like the Windows zip, because of the `portable.txt` file in it. Delete that file
to use `~/.local/share/retri` instead. An AppImage that a launcher unpacked works the same way: `portable.txt`
next to its `AppRun` keeps the data there.

## Launchers

Command-line options for launchers (`--play`, `--data-dir`, `--game-files`, `--prepare`, `--set`, `--fullscreen`,
`--no-update-check` and more) are in [debug-hooks.md](debug-hooks.md#command-line), or run `eowrecomp --help`.

Controllers work as on the other systems (SDL3's gamepad support, bundled): Xbox, PlayStation (DualShock 4,
DualSense), Switch Pro / Joy-Con, 8BitDo and other pads through the kernel's drivers, plugged in or paired while the
game runs; the button prompts follow the controller. For SDL's own drivers (rumble, LEDs and gyro on PlayStation /
Switch pads) the user needs access to `/dev/hidraw*`, which most distributions give with Steam's udev rules
(`steam-devices`, or Steam installed); without them the controller still works through the kernel driver. Started from
Steam with Steam Input on, the game sees Steam's virtual Xbox controller (Xbox prompts).

If the AppImage won't start with a FUSE error, install your distribution's `fuse3` package, or run it with
`APPIMAGE_EXTRACT_AND_RUN=1`. On a Steam Deck, put `APPIMAGE_EXTRACT_AND_RUN=1 %command%` in the game's launch options.

## Building from source
Ubuntu / Debian packages (other distributions: the same libraries under their names). SDL3 must be 3.4 or newer
(ReTri loads and saves PNGs through it); where the distribution ships an older one (Debian 13 has 3.2), build SDL3
3.4 from source first - `tools/linux/build_deps.sh` builds the pinned version the AppImage uses (into
`build/linux/<arch>/deps`).

    sudo apt install clang lld make pkg-config python3 libsdl3-dev libavformat-dev libavcodec-dev \
        libavutil-dev libswscale-dev libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev glslc vulkan-tools

For Chinese or Korean menus, also install a font for them (`fonts-noto-cjk`). ReTri bundles fonts for Russian
and Japanese only; without one, Chinese and Korean menu text shows as boxes.

shaderc is linked the way its pkg-config file says (`libshaderc_shared` on Arch, Fedora, AOSC...); to choose
yourself: `make SHADERC=-lshaderc_shared` (or `-lshaderc`).

Then from the repo root:

    make -j GEN=generated-eow    # the app (no game code yet)
    make gen GEN=generated-eow   # once, with your game files in extracted/ (exefs/, romfs/): recompile the game's code
    make -j GEN=generated-eow    # -> build/eowrecomp and the game code library build/eowgame.so

(`GEN=generated-eow` picks EoW's layout; without it the Makefile still uses LA's.)

Without `extracted/` here, run `tools/fetch_zig.sh` once and set up your game files on the start screen: it builds the
game code itself, as the AppImage does. The generator is part of the app (`runtime/gen/`; `make lasgen` builds it as a
command-line tool). Python 3 is only needed for the tests.

Run `./build/eowrecomp` from the repo root: with `extracted/` in the starting folder it uses that folder for
everything (dev mode) instead of `~/.local/share/retri`.

`ARCH` picks the target, separately from the OS: the generated code differs per arch (aarch64 runs the guest's
FP/SIMD as inline asm, like Apple Silicon; x86-64 lowers it to C with `-march=x86-64-v3`, like Windows). `make gen`
passes the matching `--simd`. A cross build (`ARCH=aarch64` on an x86-64 machine, `SYSROOT=<arm64 root filesystem>`)
keeps its own `build/aarch64/`; give it its own generated code folder too (`GEN=generated-eow-aarch64`). Distributions that name shaderc's library
`libshaderc_shared.so` need `SHADERC=-lshaderc_shared`.

## AppImages
Needs podman, and for the other arch's AppImage QEMU user emulation with binfmt (`sudo apt install podman qemu-user
qemu-user-binfmt`).

    make appimage                # this machine's arch -> build/dist/ReTri-<version>-linux-<x64|arm64>.AppImage
    make appimage ARCH=aarch64   # on x86-64: cross-compiled
    make appimages               # both

Everything is built in Ubuntu 22.04 containers (`tools/linux/Containerfile`), so the result runs on glibc 2.35+:
SDL3, an LGPL-only FFmpeg (only the movie codecs, as on Windows / macOS), and shaderc from pinned sources
(`tools/linux/build_deps.sh`), then the game against them (`tools/linux/package.sh`). This machine's arch is built
natively in one container of that arch (the release does both archs that way, on an arm64 runner for aarch64).
aarch64 on an x86-64 machine is cross-compiled in the amd64 container against the arm64 image's root filesystem
(`build/linux/aarch64/sysroot`). The AppImage holds no game code: the start screen builds it on the player's machine.

Release check of the x86-64 AppImage (`make smoketest`, tools/linux/smoketest.sh; `tools/linux/smoketest.sh <AppImage>`
for another one): the start screen's Set up from `extracted/` in the checkout (game files copied into
`build/smoke/root`, the game code built with the bundled tools), the start screen, then a new game that plays its
opening into the first room. Each step is checked in the log and by a screenshot. `build/smoke/report.txt` ends with
READY FOR RELEASE or NOT READY; `build/smoke/sheet.png` shows every screenshot. Delete `build/smoke` afterwards: it
holds a copy of the game files. It needs Python's Pillow; on a slow machine raise the setup's limit with
`SMOKE_SETUP_SECONDS` (default 3600) and the game's with `SMOKE_GAME_SECONDS` (default 600).

The AppImage holds `eowrecomp`, `zig/`, the start screen font and the bundled libraries (SDL3,
FFmpeg, shaderc). Not bundled: glibc, libstdc++, the Vulkan loader and the GPU drivers (always the system's). No game
files or fonts.

`make tarball` (after `make appimage`, same `ARCH`) packs that AppDir as the tarball, with `portable.txt` and a
`eowrecomp` link to `AppRun` (`tools/linux/tarball.sh`). `tools/linux/check_no_game_code.sh` checks both before
they ship.

## Differences from macOS / Windows
- Platform layer `runtime/platform/linux.c`: like macOS, the guest window (0x70_0000_0000..0x73_2000_0000) is one
  read/write mapping (`MAP_NORESERVE`), crashes are caught with `sigaction` on an alternate stack.
- The HLE / service / reset tables are ELF sections found through the linker's `__start_` / `__stop_` symbols.
- Renderer: the Windows Vulkan backend unchanged.
- Without the user's Switch fonts the game text uses the bundled BIZ UDPGothic (`third_party/fonts/BIZUDPGothic`),
  sized like the console's font, where macOS / Windows use Hiragino / Yu Gothic; the in-game menu and button letters
  use DejaVu Sans, the system's or the bundled copy (`third_party/fonts/DejaVu`). TrueType is required; the game's font
  engine can't use CFF fonts such as Noto Sans CJK.
- Pages: 4K and 16K kernels (Asahi) work; 64K-page kernels (some ARM servers) are not supported.

## Testing on ARM (aarch64): what to report
arm64 is less tested than x64, so reports from ARM machines are welcome. Please include:

1. **Distribution and kernel**: `cat /etc/os-release | head -3; uname -r; getconf PAGESIZE`
2. **GPU and Vulkan driver**: `vulkaninfo --summary` (package `vulkan-tools`); at least the devices section with
   `deviceName`, `driverName` and `driverInfo`.
3. **What happened**: start screen? extraction? in game? where it stopped, any visual problems (screenshots help).
   With a controller: its model, and whether it works (the log names it: `[input] gamepad: ...`).
4. **The log**: `~/.local/share/retri/eowrecomp.log` (or the terminal output when started from a terminal).
   A crash writes `*** signal ...` and a backtrace there; attach the whole file (or if the problem was in
   an earlier run, that run's `eowrecomp-backup-*.log`, by date modified).
5. **A crash backtrace with gdb** when the log's isn't enough:

       ./ReTri-<version>-linux-arm64.AppImage --appimage-extract    # -> squashfs-root/
       cd ~/.local/share/retri
       gdb -batch -ex 'handle SIGSEGV nostop noprint pass' -ex run -ex 'thread apply all bt 20' \
           --args ~/squashfs-root/usr/bin/eowrecomp > gdb.txt 2>&1

   (Adjust the `squashfs-root` path to where you extracted it. The game itself uses SIGSEGV for memory watches, hence
   the `handle` line; a real crash still ends in the backtrace. Attach `gdb.txt`.)

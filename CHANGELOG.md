# Changelog

All notable changes to EoW Recomp are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org). See `CONTRIBUTING.md` for the release steps.

## [Unreleased]

### Added
- Unlocked aspect ratio: the camera fills wide windows.
- Logging of shader records the shipped list lacks, once per run.

### Changed
- The app is named EoW Recomp: binaries `eowrecomp` and `eowgame.dylib`, logs `eowrecomp.log`, and the name in
  menus, window titles and docs. Env vars stay `LARECOMP_*`.
- Indirect draws run on the GPU by default (no CPU waits, shadows kept).
- LA's 30 fps hold is off for EoW.

### Fixed
- The underwater area is no longer black (shader builds run at the default priority).

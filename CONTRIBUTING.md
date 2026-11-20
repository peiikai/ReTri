# Contributing to ReTri

ReTri is a private project for now. This file sets out how work is organised, so that every change follows the
same path from the start.

## The one hard rule: no Nintendo files

Nothing owned by Nintendo is ever committed or attached to an issue or pull request. That means:
- no game files, extracted or otherwise
- no generated code (`generated-eow/`)
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
| `develop` | the next minor version | | `release/X.Y` |
| `release/X.Y` | one release line: its last bug fixes, then patch releases | `develop` | `main` (tagged), then back into `develop` |
| `feature/<name>` | new features | `develop` | `develop` |
| `fix/<issue>-<name>` | bug fixes for the next version | `develop` | `develop` |
| `hotfix/<issue>-<name>` | fixes for a released version | `release/X.Y` | `release/X.Y`, then into `develop` |
| `docs/<name>`, `ci/<name>`, `chore/<name>` | docs, CI, upkeep | `develop` | `develop` |

Rules:
- Branch names are lower case with dashes, for example `fix/12-text-box-flicker`. Never `claude/...`.
- `main`, `develop` and `release/*` only change through pull requests. A local pre-push hook refuses direct pushes
  to them (see below).
- Merge pull requests with a merge commit, so each branch's history stays readable. Delete the branch after it is
  merged.

## Making a release

1. Cut `release/X.Y` from `develop` when the version's features are in.
2. On the release branch, set `VERSION` to `X.Y.0` and move the `Unreleased` lines in `CHANGELOG.md` under
   `## [X.Y.0] - <date>`. Only fixes land on the release branch from here on.
3. Open a PR from `release/X.Y` into `main`. Once it's merged, tag the merge commit `vX.Y.0`.
4. Merge `release/X.Y` back into `develop`, then set `VERSION` on `develop` to the next minor version.
5. For a patch release, merge hotfixes into `release/X.Y`, then repeat steps 2 to 4 with `X.Y.Z`.

## Commits and pull requests

- Commit messages are plain English: a short first line saying what changed and why.
- Each PR does one thing. It names its issue (`Fixes #12`), says how it was tested and updates `CHANGELOG.md`.
- PRs target `develop`, except hotfixes, which target their `release/X.Y` branch.
- The game is play-tested before any PR touching rendering, timing or audio is merged.

## Pre-push guard

`git config core.hooksPath ~/.config/eowrecomp-hooks` installs the hooks. They refuse:
- pushes to `main`, `develop` and `release/*` (they change only through PRs)
- any commit not authored as Peiikai

To bootstrap a release branch, the maintainer can allow a single push with `EOW_ALLOW_PROTECTED_PUSH=1`.

## Building

See `CLAUDE.md` for the build and run commands.

## The game code generator

`runtime/gen/` turns the game's executables into C. The app runs it during setup, and `make lasgen` builds it as a
command-line tool (`make gen GEN=generated-eow` uses it). `tools/gen.py` and its modules are the Python reference it
was ported from, kept until the port has shipped: a generator, patch or chunk-cut change goes into both, and
`tools/test_gen_cpp.py build/lasgen <extracted folder> --game eow` must report the C identical (gen.py needs a Python
with unicorn, capstone and lz4).

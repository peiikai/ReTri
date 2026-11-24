# CI, nightlies and releases

The GitHub Actions workflows in `.github/workflows/`, ported from LAS Recomp's. None of them uses game files, and
nothing they build or publish holds game code: `tools/check_no_game_files.sh` checks every file before it goes up.

| Workflow | Runs on | Does |
|---|---|---|
| `ci.yml` | pull requests into `develop` / `release/*`, pushes there, by hand | builds the app on Linux x64 and arm64, Windows x64 and arm64 and Android, and runs the tests that need no game files. macOS and iOS only on a run by hand. |
| `nightly.yml` | by hand (schedule off) | builds every platform and puts the files on the rolling [`nightly`](https://github.com/peiikai/ReTri/releases/tag/nightly) pre-release |
| `release.yml` | a pushed `vX.Y.Z` tag, by hand | builds the release's files into a draft release, for a person to write the notes and publish |
| `appcast_notes.yml` | a release published or edited | puts the release notes into the macOS update feed |
| `log_triage.yml` | an issue or comment added or edited | one bot comment summing up the attached `eowrecomp.log` files, and a `log: ...` label |

## By hand

    gh workflow run ci.yml --ref <branch>                       # every job, macOS and iOS too
    gh workflow run nightly.yml [-f ref=develop] [-f new_round=false]
    gh workflow run release.yml --ref <branch> -f tag=ci-test-<name>   # dry run: a pre-release draft

`new_round=false` builds only the platforms the current nightly round is missing (a failed job).

GitHub starts a workflow by hand only if a file of that name is on `main`. Until the next release brings these
there, `ci.yml` and `release.yml` work (`main` has older files with those names) and `nightly.yml` doesn't.

## Files

Release files are `ReTri-<tag>-<platform>.<ext>`: `macOS.dmg`, `windows-x64.zip`, `windows-arm64.zip`,
`linux-x64.AppImage` / `.tar.gz`, `linux-arm64.AppImage` / `.tar.gz`, `android.apk`, `iOS.laskit`. Nightly files are
the same with the tag `nightly-<YYYYMMDDHHMM>-<commit>`. Next to them: `update-<platform>.txt` (Windows, Linux) and
`appcast-macOS.xml`, signed with the `UPDATE_SIGNING_KEY` secret. Those names never change: every build out there
fetches them by name. Without the key the scripts only warn, and players use Download.

A release ships macOS, Windows x64 and Linux x64. Linux arm64, Android, iOS and Windows on ARM are on
the nightly page only; to ship one with releases, add it to `RELEASE` in `release.yml`.

## Secrets and settings

- `UPDATE_SIGNING_KEY`: signs the update files. Never printed or uploaded.
- `ANDROID_KEYSTORE_B64`, `ANDROID_KEYSTORE_PASSWORD`: the APK signing key ([android.md](android.md)). Without them
  the nightly APK is built and checked but not put up.
- `MAC_RUNNER` (a repository variable, optional): the labels of a Mac runner of our own as a JSON list, e.g.
  `["self-hosted", "macos-vm"]`. Once set, `ci.yml` runs the macOS and iOS jobs on every run, there.

## Cost

While the repository is private, Actions minutes come out of the plan's monthly allowance, and GitHub counts a Windows
minute as two and a macOS minute as ten (each job rounded up to a whole minute). Measured in November 2026, caches
warm (a job's first run builds its dependencies and takes longer):

| Run | Takes | Minutes counted |
|---|---|---|
| `ci.yml`, push or pull request (no macOS) | about 12 min | about 60 |
| `ci.yml` by hand (with macOS and iOS) | about 12 min | about 130 |
| `nightly.yml`, every platform | about 15 min | about 110 |
| `release.yml`, the Beta 1 set | about 10 min | about 50 (estimate) |

So the nightly schedule stays off until the repository is public (public repositories' minutes are free). To turn it
on, uncomment the two `cron` lines at the top of `nightly.yml`.

## Tests left out

`tools/test_save.py`, `test_dynres.py`, `test_pace.py`, `test_shadow_map.py` and `tools/test_active_pad.c` don't
link at the moment (their stubs miss runtime symbols added since LAS Recomp), so `ci.yml` leaves them out until they
are fixed. `tools/test_gen_cpp.py` needs game files.

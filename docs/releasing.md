# Making a release

For the maintainer. Contributors never cut releases: see `CONTRIBUTING.md` for which branch a change targets.

## Steps

1. Cut `release/X.Y` from `develop` when the version's features are in.
2. On the release branch, set `VERSION` to `X.Y.0` and move the `Unreleased` lines in `CHANGELOG.md` under
   `## [X.Y.0] - <date>`. Only fixes land on the release branch from here on.
3. Open a PR from `release/X.Y` into `main`. Once it's merged, tag the merge commit `vX.Y.0`.
4. Merge `release/X.Y` back into `develop`, then set `VERSION` on `develop` to the next version.
5. For a patch release, merge its fixes into `release/X.Y`, then repeat steps 2 to 4 with `X.Y.Z`.

Tags are plain `vX.Y.Z`: the update check ignores anything after the version (`v0.1.0-beta.1` is never offered).
During the beta, `v0.N.M` shows as "Beta N.M".

## Update files

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

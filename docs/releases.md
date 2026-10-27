<!-- Copyright (c) OpenIPC  https://openipc.org  MIT License -->

# Versions and releases

The change history of every version is in [CHANGELOG.md](../CHANGELOG.md); a version
bump adds its section there in the same PR.

## Single source of truth

The **only** place to change the software version is `src/version.h`:

```c
#define CAMEX_VERSION "X.Y.Z"
```

Every build takes it from there: the app, the kernel module (`MODULE_VERSION`,
not git), the name of `make apk`'s file, and the Android `versionName` and
`versionCode`, which `android/app/build.gradle.kts` reads from `version.h` at
build time (a version it cannot read fails the build). Nothing else holds a
version to keep in step; CI checks that the APK's version matches `version.h`.

## When to bump

`CAMEX_VERSION` is `MAJOR.MINOR.PATCH`, numbers without leading zeros:

- **PATCH** — fixes and small changes; **MINOR** — a new feature (option, config field,
  protocol, platform); **MAJOR** — an incompatible change (arguments, config format,
  protocol, default behaviour), only when the maintainer decides.
- A pull request that changes the code or the build (`src/`, `android/`, `Makefile`,
  `ci/`, `scripts/`) bumps the version as its last commit before the merge and adds
  its `## X.Y.Z — YYYY-MM-DD` section to `CHANGELOG.md`. Pull requests with only docs,
  tests, screenshots or workflows in `.github/` do not.
- CI checks this on every pull request (`.github/check-version.sh`, job "Version
  bump"): the new version must be well-formed, greater than the one on `master` and
  have its new `CHANGELOG.md` section. Until the bump commit is pushed, the check is
  red by design.

## Release process

There are no version tags and no versioned releases: one release, `latest`, is
replaced by CI after every push to `master`, once all build and test jobs pass.

1. **In a PR**: bump `CAMEX_VERSION` and add the version's section to `CHANGELOG.md`.
2. **Merge into `master`**. CI deletes the old `latest` release and its tag and creates
   it again on the built commit: title `Camex X.Y.Z`, the files from
   [Permanent links](../README.md#permanent-links).
3. **Telegram**: when the version differs from the title of the previous `latest`, CI
   posts the APK to the OpenIPC Telegram channel. A push without a version bump only
   replaces the files.

## Version numbering rules

- `versionName` (Android) = `CAMEX_VERSION` (e.g., `"3.0.2"`)
- `versionCode` (Android) = `MAJOR * 10000 + MINOR * 100 + PATCH` (e.g., `30002`)
- GitHub Release title = `Camex X.Y.Z`, tag `latest`

## Manual CI trigger

To rebuild and republish `latest` without a new commit, run the workflow with
`workflow_dispatch` on `master` in the Actions tab.

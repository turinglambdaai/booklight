# Changelog

All notable changes to PDFGist are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is semver.
The 1.x release era was retired in the version epoch reset below; its
releases and tags were deleted and the history lives in git on `main`.

## [Unreleased]

### Added
- **The settings pane closes with the support surface** (the taskly
  About facts): the version footer now carries a Website link
  (pdfgist.jrtx.site), a Report-an-Issue link (the GitHub issue
  tracker) and the AGPL-3.0 license line. A bug report starts one click
  away from the failure it describes.
- **GitHub issue templates** (bug report + feature request) so feedback
  from the About link lands structured.

### Changed
- **Update error text is selectable** in the macOS update sheet, so a
  failure can travel into a bug report unchanged.
- **The release feed is published under both names.** The signed
  manifest ships as `update-stable.json` (what installed clients pull,
  unchanged) and, with identical bytes, as the family-standard
  `update-manifest.json` — family probes of the standard name stop
  404ing. Existing installs are unaffected; the host switching to the
  standard name is a later change.
## [0.1.1] - 2026-10-14

Alignment, honesty, and a manual. No new features.

### Added

- **User guide.** The site now carries a bilingual user manual
  (`docs/guide.html`, linked from the nav): a five-minute start, the
  bring-your-own-model key setup (where the key field is, what format the
  key has, how to fetch models and test the connection), the reading /
  summary / annotation workflow, and an FAQ covering storage locations,
  privacy, platform status, updates and uninstall.
- **Updater test suite.** The updater is pinned by an offline test suite:
  the Ed25519 trust chain (in-process signing), the manifest rejection
  matrix, the sticky rollout bucket, and the loopback download path.

### Changed

- **Updater moved onto rivet/distribution end to end.** The half-rolled
  custom fetch is gone; the updater now uses rivet's fetch-update-manifest
  (verify-before-parse, built-in redirects), select-update policy and
  download-update with the signed size cap, SHA-256 verification and
  `.partial` cleanup on every failure, plus a stall watchdog. Rollout
  bookkeeping (sticky bucket, last-update timestamp) moved out of
  `settings.json` — that document stays byte-compatible with v1 installs —
  into a sibling `updater.json`.
- **i18n fix.** Fallback translation catalogs used `hasheq`, so lookups
  silently returned raw keys unless compilation coalesced the string
  literals (CI green, local build broken). The tables are now equal-based,
  so every host locale resolves the same way everywhere.
- **Upstream Rivet alignment (v0.11.0).** All three hosts consume Rivet as
  a package instead of compiling its sources: the macOS host depends on the
  root rivet package (`Package.swift`), the Linux host links
  `rivet::platform` via `add_subdirectory` (CMakeLists.txt), and the
  Windows host references `rivet-host.vcxproj` via ProjectReference with
  the CRT runtime library forwarded. CI and the release workflow install
  Rivet from the pinned tag (`RIVET_PIN: v0.11.0`) instead of the floating
  catalog package.
- **Real screenshots.** The site hero images were renders of the retired
  v1 (Tauri/PDF.js) app; they are replaced with captures of the current
  native macOS build, light and dark.
- **Site and README.** Download buttons point at the latest release page
  instead of hardcoded versioned asset URLs (the numbers drifted every
  release); the nav links to the guide; the README gains release and CI
  badges and keeps its header free of URLs. The site ships its own
  self-hosted Inter subset (no third-party font CDN).

## [0.1.0] - 2026-10-10

Version epoch reset. The technical rewrite on Rivet is settled and the
project enters the 0.x feature-validation phase: the 1.x release line
(1.0.0 – 1.2.0) and its tags were removed, and 0.1.0 is the first release
of the new epoch. Functionally identical to the retired 1.2.0 — its entry
is summarized here so this section stands alone as release notes.

### Added

- **Intel Mac support.** macOS ships both architectures —
  `pdfgist-<version>-macos-arm64.dmg` / `.zip` (Apple silicon) and
  `pdfgist-<version>-macos-x64.dmg` / `.zip` (Intel, built on Intel
  runners). The signed update feed carries both portable zips, and each
  client picks its own architecture at check time (rivet/distribution
  selects by platform *and* architecture).
- **Portable zips.** Both macOS architectures ship a
  `pdfgist-<version>-macos-<arch>.zip` alongside the drag-to-install
  DMG; the in-app update feed serves the zip.

### Changed

- **Release artifact naming** follows the family convention: lowercase
  with an explicit architecture — `pdfgist-<version>-macos-<arch>.dmg`
  replaces `PDFGist-v<version>-macos.dmg`.
- The in-app updater downloads the portable zip and the native installer
  unpacks it with `ditto` (DMG artifacts in a manifest remain supported).
- **Upstream Rivet alignment.** Rivet main scopes generated Swift records
  and enums under `RivetTypes`; the macOS host's call sites were migrated
  accordingly (`RecentEntry` → `RivetTypes.RecentEntry` and so on).
- Release metadata is gated by `scripts/check-release-version.sh`
  (VERSION == rivet.rktd == updater constants == tag) in CI and in every
  release job.

## [1.2.0] - 2026-10-09

Family packaging standard (the taskly v1.3.0 benchmark) applied to PDFGist.

### Added

- **Intel Mac support.** macOS now ships both architectures —
  `pdfgist-<version>-macos-arm64.dmg` / `.zip` (Apple silicon) and
  `pdfgist-<version>-macos-x64.dmg` / `.zip` (Intel, built on Intel
  runners). The signed update feed carries both portable zips, and each
  client picks its own architecture at check time (rivet/distribution
  selects by platform *and* architecture).
- **Portable zips.** Both macOS architectures ship a
  `pdfgist-<version>-macos-<arch>.zip` alongside the drag-to-install
  DMG; the in-app update feed now serves the zip.

### Changed

- **Release artifact naming** follows the family convention: lowercase
  with an explicit architecture — `pdfgist-<version>-macos-<arch>.dmg`
  replaces `PDFGist-v<version>-macos.dmg`.
- The in-app updater downloads the portable zip and the native installer
  unpacks it with `ditto` (DMG artifacts in a manifest remain supported).
- **Upstream Rivet alignment.** Rivet main now scopes generated Swift
  records and enums under `RivetTypes`; the macOS host's call sites were
  migrated accordingly (`RecentEntry` → `RivetTypes.RecentEntry` and so
  on). Cold builds of the host fail against the new codegen without this.
- Release metadata is gated by `scripts/check-release-version.sh`
  (VERSION == rivet.rktd == updater constants == tag) in CI and in every
  release job.

### Note for 1.1.0 installations

The 1.1.0 updater can verify and download this release but its installer
only mounts DMGs, so it cannot install the new zip feed — update once
manually: download `pdfgist-1.2.0-macos-arm64.dmg` from this release and
drag it to Applications. From 1.2.0 on, in-app updates work again.

## [1.1.0] - 2026-10-09

Online updates. The app can now check for new releases and update itself.

### Added

- Signature-verified online updates on macOS (File → 检查更新… / Check for
  Updates, ⌘U): the backend fetches the release channel manifest
  (`update-stable.json`), verifies its Ed25519 signature and key id
  (`pdfgist-2026-10`) before parsing, applies channel/version/rollout
  policy, then downloads the DMG on a background thread with progress
  reporting and SHA-256 verification against the signed manifest. The host
  owns installation: mount, replace `/Applications/PDFGist.app` (keeping
  the previous bundle beside it as a rollback copy), relaunch. A silent
  launch-time check runs at most once a day; nothing is installed without
  the user clicking through.
- Release pipeline now runs `raco rivet release --development` on macOS and
  publishes the signed channel manifest `update-stable.json` alongside the
  DMG (SHA256SUMS and build-provenance attestation unchanged).

## [1.0.0] - 2026-10-08

First release of the Rivet line. macOS (Apple silicon, macOS 14+);
the Windows and Linux hosts are under construction.

### Added

- Full rebuild on Rivet: one Racket domain core (`racket/`, `app/backend.rkt`)
  driving first-party native hosts over typed RPC (RVT1). The Tauri 2 + PDF.js
  stack has been removed; v1 remains in git history and v1.x tags for reference.
  Parity carried over: AI streaming (translate/summarize/chat), annotations &
  bookmarks, recents, settings, AcroForm filling, printing, TTS, split view,
  and page-level editing (delete / rotate / insert blank / extract / merge /
  watermark & text-box bake) backed by a self-contained Racket PDF reader &
  writer. EPUB reading is ported too: a domain-core EPUB reader
  (container/OPF/spine/TOC, sanitized chapter HTML with inlined images,
  plain-text extraction) rendered by WKWebView in the host with the v1
  reading typography, TOC navigation, whole-book search, font-size
  control and the chapter-scoped AI workflow.
- Drag-to-Applications DMG installer with SHA256SUMS and build-provenance
  attestation.

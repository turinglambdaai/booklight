# Changelog

All notable changes to Booklight are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is semver.

## [0.1.0] - 2026-10-16

The first release of **Booklight** — the reading-first rebuild of what
briefly existed as PDFGist (its release history was retired with the old
identity; the shelf imports those books on first launch).

### Added

- **The shelf** — launch straight into your bookshelf: cover grid with
  reading progress, collections, search, and import that references your
  files in place (books are never copied; the shelf file is a single
  documented, stable `library.json` you can keep in your sync drive —
  see [DATA-FORMAT](shared/spec/DATA-FORMAT.md)).
- **Reading** — tabbed PDF (continuous scroll, two-page book view, zoom /
  fit-width, thumbnails, outline, full-text search) and EPUB (reflowable
  serif typography, chapter TOC, whole-book search) on macOS, Windows and
  Linux; continue-reading everywhere.
- **AI at your elbow** — selection translation, page / document summaries,
  and document chat, all streamed, all pointed at the provider you choose
  (GLM, DeepSeek, Kimi, Qwen, Doubao, SiliconFlow, Ollama, or any
  OpenAI-compatible endpoint). Your key stays on your machine.
- **Annotations** — three-color highlights, underline, strikeout, notes
  and bookmarks stored beside each book (sidecar JSON) or in the central
  store, ready for cloud-drive sync; byte-compatible across all three
  hosts.
- **Light editing** — a dedicated Tools menu: rotate, insert blank page,
  delete, extract, merge (in-memory; source files untouched) and AcroForm
  filling with export of the filled copy.
- **Comforts** — text-to-speech, printing, split view, password-protected
  PDFs, drag-and-drop import, interface language (follow system / 中文 /
  English), localized system chrome.
- **Signed self-updates on all three platforms** — Ed25519-verified
  manifest, SHA-256-checked download, installs only on your click.

### Local-first data

Books stay in your folders and sync drives; progress and annotations ride
beside each book in sidecars; the shelf is one documented file. No
account, no telemetry — see [DATA-FORMAT](shared/spec/DATA-FORMAT.md).

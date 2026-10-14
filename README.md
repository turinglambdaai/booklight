# PDFGist

> A local-first AI PDF reader — bring your own model. One Racket domain core driving first-party native hosts over typed RPC.

**English** · [中文](README.zh-CN.md)

[![release](https://img.shields.io/github/v/release/turinglambdaai/pdfgist)](https://github.com/turinglambdaai/pdfgist/releases/latest) ![platform](https://img.shields.io/badge/platform-macOS%2014%2B%20%28Apple%20silicon%20%26%20Intel%29-lightgrey) ![built with](https://img.shields.io/badge/built%20with-Rivet-blue) [![CI](https://github.com/turinglambdaai/pdfgist/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/pdfgist/actions/workflows/ci.yml) [![License](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)

Most AI PDF apps lock you into their subscription. PDFGist takes the opposite approach: the app is a thin, fast reader, and the intelligence is whatever model *you* point it at. Your key, your model, your cost — stored locally, sent directly to the provider you choose. Nothing leaves your machine except the requests you make to your own provider.

## Features

- **PDF reading** — tabbed documents, continuous scroll, two-page book view, zoom / fit-width, thumbnails, outline, full-text search, dark mode, printing, split view, text-to-speech, keyboard navigation
- **EPUB reading** — reflowable serif typography, chapter table of contents, whole-book search, font-size control, dark mode, and the same AI workflow on chapters
- **Page editing** — delete, rotate, insert blank pages, extract a selection into a new file, merge documents, bake watermarks and text boxes (in-memory; your source files are never modified)
- **Continue where you left off** — recent files keep your page and scroll position; files stay in your folders and sync drives, never copied into an app library
- **Selection translation** — select any text, one click to translate, streaming result
- **Bilingual page reading** — paragraph-aligned translation of the current page: original and translation in pairs, streamed in parallel
- **Page & document summaries** — one-click structured summaries of the current page, a selection, or the whole document
- **Chat with the document** — ask questions grounded in the current page, your selection, or the first pages of the document
- **AcroForm filling** — fill text / checkbox / choice fields and export the filled PDF
- **Annotations & bookmarks** — highlights (three colors), notes, bookmarks stored in a sidecar JSON next to your PDF, ready for cloud-drive sync
- **Bring your own model** — presets for Zhipu GLM (incl. the GLM Coding plan endpoint), DeepSeek, Moonshot Kimi, Alibaba Qwen, Doubao (Volcano Ark), SiliconFlow and Ollama, or any OpenAI-compatible base URL; model lists fetched on demand
- **Online updates** — signature-verified self-update (Ed25519-signed release manifest, SHA-256-checked download); checks at most once a day, installs only when you click through
- **Local-first** — settings and API keys live in your user config directory; no account, no telemetry

## Architecture

One Racket domain core (`racket/` + `app/backend.rkt`) owns all business logic — AI streaming, PDF page surgery, annotation storage, settings — and exposes it over [Rivet](https://github.com/turinglambdaai/rivet)'s typed RPC (RVT1). First-party hosts render and interact only:

| Host | Stack | Directory |
|---|---|---|
| macOS | SwiftUI + generated client | `macos-host/` |
| Windows | C++/WinRT + generated client | `windows/` (under construction) |
| Linux | GTK4 + generated client | `linux/` (under construction) |

The macOS host ships first; the Windows and Linux hosts are being brought to
parity and are not part of this release.

PDF page-level operations run on the domain core's own minimal PDF reader/writer (`racket/pdfgist/pdfdoc.rkt`) — no external PDF libraries.

## Building from source

Prerequisites: [Racket](https://racket-lang.org) CS 9.x with [Rivet](https://github.com/turinglambdaai/rivet) linked, plus the Swift / WinRT / GTK toolchain for your platform.

```bash
raco rivet doctor --json   # toolchain check
raco rivet build           # generate clients, build backend + host
raco rivet dev             # develop (rebuild on change)
raco test racket/          # domain-core tests
```

## Roadmap

- [ ] Windows & Linux host builds
- [ ] Mobile — iOS / iPadOS host (Rivet's portable Swift layer) and Android
      host (Rivet's Kotlin platform), matching the family's full-platform
      scope; a watchOS progress companion trails the phone host

## License

[AGPL-3.0](LICENSE)

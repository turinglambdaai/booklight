# Booklight

> A calm cross-platform bookshelf — PDF and EPUB reading with AI at your elbow.

**English** · [中文](README.zh-CN.md)

[![release](https://img.shields.io/github/v/release/turinglambdaai/booklight)](https://github.com/turinglambdaai/booklight/releases/latest) ![platform](https://img.shields.io/badge/platform-macOS%2014%2B%20%7C%20Windows%2010%2B%20%7C%20Linux-lightgrey) ![built with](https://img.shields.io/badge/built%20with-Rivet-blue) [![CI](https://github.com/turinglambdaai/booklight/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/booklight/actions/workflows/ci.yml) [![License](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)

Apple built the best reading experience and then locked it to its devices.
Booklight is that calm, for every desktop: a shelf you own, reading that
stays out of the way, and AI when you want it — your files, your model,
your cost. Nothing is copied into an app library and nothing leaves your
machine except the requests you make to your own provider.

## The shelf

- Launch straight into your bookshelf — covers, collections, search, reading progress
- Books are cataloged **in place**: import references your files, it never
  copies them; move a file and the shelf marks it missing until you relink it
- Reading progress joins the shelf automatically; closing a book returns to it
- The shelf file (`library.json`) is a single documented, stable file you can
  drop into your sync drive — see [DATA-FORMAT](shared/spec/DATA-FORMAT.md)

## Reading

- **PDF** — continuous scroll, two-page book view, zoom / fit-width,
  thumbnails, outline, full-text search, split view, keyboard navigation
- **EPUB** — reflowable serif typography, chapter TOC, whole-book search
- Annotations — three-color highlights, underline, strikeout, notes and
  bookmarks stored beside your book, ready for cloud-drive sync
- Text-to-speech, printing, dark mode, continue-reading everywhere

## AI at your elbow

- Selection translation, page / document summaries, chat grounded in the
  page, your selection, or the document
- Streaming everywhere; nothing arrives pre-chewed
- Bring your own model — Zhipu GLM (incl. the GLM Coding plan endpoint),
  DeepSeek, Moonshot Kimi, Alibaba Qwen, Doubao (Volcano Ark), SiliconFlow,
  Ollama, or any OpenAI-compatible base URL; model lists fetched on demand
- Your key lives in your config directory and goes straight to your provider

## Light editing (deliberately small)

- Page surgery in a dedicated Tools menu: rotate, insert blank, delete,
  extract, merge — in memory, your source files untouched
- AcroForm filling with export of the filled copy

## Online updates

Ed25519-signature-verified self-update (signed release manifest,
SHA-256-checked download); checks at most once a day, installs only when
you click through — on all three platforms. Contract:
[UPDATE](shared/spec/UPDATE.md)

## Architecture

One Racket domain core (`racket/booklight/` + `app/backend.rkt`) owns all
business logic — the shelf, AI streaming, PDF page surgery, annotation
storage, settings — and exposes it over [Rivet](https://github.com/turinglambdaai/rivet)'s
typed RPC (RVT1). First-party hosts render and interact only:

| Host | Stack | Directory |
|---|---|---|
| macOS | SwiftUI + generated client | `macos-host/` |
| Windows | WinUI 3 (C++/WinRT) + PDFium + generated client | `windows/` |
| Linux | GTK4 + PDFium + WebKitGTK + generated client | `linux/` |

PDF page-level operations run on the domain core's own minimal PDF
reader/writer (`racket/booklight/pdfdoc.rkt` + `pdfedit.rkt`) — no external
PDF libraries. Where your data lives and how it syncs:
[DATA-FORMAT](shared/spec/DATA-FORMAT.md).

## Building from source

Prerequisites: [Racket](https://racket-lang.org) CS 9.x with [Rivet](https://github.com/turinglambdaai/rivet) linked,
plus the Swift / WinRT / GTK toolchain for your platform. The Windows and
Linux hosts render PDF through [PDFium](https://pdfium.googlesource.com/pdfium/)
(BSD-3): the build fetches pinned, SHA-256-verified binaries at build time —
nothing is committed to the repository.

```bash
raco rivet doctor --json   # toolchain check
raco rivet build           # generate clients, build backend + host
raco rivet dev             # develop (rebuild on change)
raco test racket/          # domain-core tests
```

## Roadmap

- [x] Shelf — collections, covers, search, reading progress, missing-file
      handling (all three platforms)
- [x] Reading — PDF/EPUB, annotations, AI workflow, light page editing,
      TTS, printing, split view, AcroForm (all three platforms)
- [x] Reading themes — paper / night page surfaces, typography controls
      (all three platforms)
- [ ] Mobile — iOS / iPadOS host (Rivet's portable Swift layer) and Android
      host (Rivet's Kotlin platform), matching the family's full-platform
      scope

## License

[AGPL-3.0](LICENSE)

## Delivery priorities (2026-10-23)

The current product review issue linked from AGENTS owns the next bounded slice and its acceptance scenarios. Follow the existing product specification and the [family delivery standard](https://github.com/turinglambdaai/turinglambdaai.github.io/blob/master/docs/PRODUCT-STANDARD.md). Hardware, native-platform and live-payment gates remain explicit; this documentation update does not claim they passed.

## Feedback and diagnostics

Report reproducible bugs and feature requests through [GitHub Issues](https://github.com/turinglambdaai/booklight/issues). Include the release, OS/architecture and steps; inspect and redact diagnostics before sharing. Do not post API keys, license files, vault contents or private documents. For sensitive security reports, follow SECURITY.md when present; otherwise request a private contact route without disclosing sensitive details. Nothing is uploaded automatically.

## Product & pricing

免费阅读器 + 研究工作台 Pro 买断. Launch design, not a live offer: see [PRICING.md](PRICING.md) for prices, boundaries and release gates. Current development builds do not gain payment restrictions from this documentation change.

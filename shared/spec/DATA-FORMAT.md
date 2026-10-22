# DATA-FORMAT — where booklight keeps your data

booklight is local-first: every byte of your data lives in files you control,
and books themselves are **never copied into the app** — the shelf references
your files in place. This document specifies every file the app writes, so
your data stays escapeable and your sync setup stays yours.

## The three layers

| Layer | Where it lives | How it syncs |
|---|---|---|
| The books | Your own folders (anywhere) | Your own sync drive (iCloud / OneDrive / Dropbox / 坚果云 …) |
| Reading progress + annotations | Sidecar JSON beside each book | Rides the book through your sync drive |
| The shelf (collections, covers, metadata) | One `library.json` + a regenerable cover cache | Put `library.json` in your sync drive (Taskly-style single file) |

None of these involve an account or a server. If you sync your book folder,
progress and annotations follow automatically; if you also sync the shelf
file, your collections follow too.

## 1. The books — your files, in place

Importing never copies or moves a file. The shelf stores the absolute path;
the book stays where you put it, readable by every other app you own. The
optional “Move into shelf folder” command performs an ordinary file **move**
(not a copy) into a folder you choose.

## 2. Reading state — sidecar beside each book

Two storage shapes, both documented and stable:

- `<book>.pdfgist.json` — annotations and bookmarks (next to the book,
  opt-in via Settings; default keeps them in the central config dir).
- Central `<config-dir>/` recents — last-read page and timestamp per book
  (see `settings.json` → `recent_files`), powering “continue reading”.

Format of the sidecar:

```json
{
  "annotations": [
    { "id": "a-…", "page": 3,
      "rects": [{ "x": 10.0, "y": 20.0, "width": 100.0, "height": 12.0 }],
      "excerpt": "…", "color": "yellow", "kind": "highlight",
      "note": "", "created": 1792100000 },
    { "id": "e-…", "page": 2, "rects": [],
      "excerpt": "…", "color": "green", "kind": "highlight",
      "note": "", "created": 1792200000,
      "anchor": { "chapter": 2, "start": 120, "end": 136 } }
  ],
  "bookmarks": [ { "page": 12, "label": "第 12 页", "created": 1792100000 } ]
}
```

`rects` are in PDF page space: origin bottom-left, y up, in PDF units.
(Reader apps that render top-left-origin must flip at paint time.)

**EPUB anchors.** EPUB annotations carry an optional `anchor` instead of
rects: `chapter` (1-based, same ordinal as `page`) plus `start`/`end`
character offsets inside the chapter's plain text — the exact text the
`epub-chapter-text` RPC serves and the whole-book search uses. The
reader renders marks by mapping offsets onto the chapter's DOM text
nodes; an annotation may wrap several segments (one mark per segment,
all sharing `id` — the PDF multi-rect model). Older readers that don't
know `anchor` simply drop it on re-save, which only loses the highlight
paint, never the file. PDF annotations carry no `anchor` key at all, so
pre-0.8 PDF files stay byte-identical.

## 3. The shelf — `<config-dir>/library.json`

```json
{
  "format": 1,
  "books": [
    { "path": "/Users/you/Books/alpha.pdf",
      "title": "Alpha", "author": "", "kind": "pdf",
      "pages": 42, "added_at": 1792100000,
      "cover": "c1d2….png",
      "collections": ["论文"],
      "status": "ok" }
  ],
  "collections": ["论文", "闲书"]
}
```

- `path` is the book's identity. The app never rewrites or relocates it.
- `pages` is the page count (PDF) or chapter count (EPUB).
- `cover` names a file in `<config-dir>/covers/`. The cover cache is
  regenerable and not part of this documented format.
- `status` is `"missing"` once the path stops resolving (the file was
  moved/renamed/deleted outside the app); rescanning or relocating the
  file clears it. The shelf never silently drops an entry.

## Stability promise

Format `1` is stable: newer app versions read every format-1 file; fields
may be added, none removed or repurposed without a format bump and a
migration. Deleting `library.json` is always safe — the shelf can be
rebuilt by re-importing (books, sidecars, and progress are untouched);
you only lose collections and cover cache.

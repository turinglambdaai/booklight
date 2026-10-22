// Annotation storage schema — the JSON sidecar/central format of
// racket/pdfgist/annotations.rkt (mac CoreTypes.swift is the spec).
//
// Coordinate space contract (cross-app sync compat): stored rects are in
// PDF PAGE space, origin BOTTOM-LEFT, y-up — exactly what PDFKit hands the
// mac host and what FPDFText_GetRect reports. The view layer flips into
// top-left origin at paint time (see pdfview.cpp).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pdfgist {

struct AnnoRect {
  double x = 0;
  double y = 0;  // bottom edge in page space (y-up)
  double width = 0;
  double height = 0;
};

// EPUB annotations anchor to chapter + character offsets inside the
// chapter's plain text; PDF annotations use page-space rects and carry
// no anchor.
struct StoredAnchor {
  int chapter = 1;
  std::int64_t start = 0;
  std::int64_t end = 0;
};

struct StoredAnnotation {
  std::string id;
  int page = 0;  // 1-based (schema keeps mac/v1 semantics)
  std::vector<AnnoRect> rects;
  std::string excerpt;
  std::string color;  // "yellow" | "green" | "blue"
  std::string kind;   // "highlight" | "underline" | "strike"
  std::string note;
  std::int64_t created = 0;  // unix seconds
  std::optional<StoredAnchor> anchor;
};

struct StoredBookmark {
  int page = 0;  // 1-based
  std::string label;
  std::int64_t created = 0;
};

struct DocData {
  std::vector<StoredAnnotation> annotations;
  std::vector<StoredBookmark> bookmarks;
};

// Tolerant decode: missing members become zero values; garbage returns an
// empty DocData rather than throwing.
DocData parse_doc_data(std::vector<std::uint8_t> const& bytes);

// Field names byte-for-byte with the mac host's JSONEncoder output:
// annotations[{id,page,rects[{x,y,width,height}],excerpt,color,kind,note,
// created}], bookmarks[{page,label,created}].
std::vector<std::uint8_t> encode_doc_data(DocData const& data);

// Page-space rect helper: a pdfium TextRect (user space, y-up) as stored.
AnnoRect anno_rect_from_text_rect(double left, double top, double right,
                                  double bottom);

// RGBA for a stored color name — mac Theme.annotationColor values
// (yellow 0xFFD500 a0.9, green 0x4CC383 a0.85, blue 0x569CFF a0.85;
// anything else reads as yellow).
void annotation_rgba(std::string const& name, double* r, double* g, double* b,
                     double* a);

// "a-<epoch>-<4 digits>" like the mac host.
std::string make_annotation_id();

}  // namespace pdfgist

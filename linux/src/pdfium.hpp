// PDFium binding for the Linux host.
//
// PDFium ships as a C API only (bblanchon/pdfium-binaries, pinned to the
// same chromium/8100 revision as the Windows host — per-OS packages of one
// build, each SHA-256-verified). The library is loaded with dlopen instead of
// link-time DT_NEEDED so the executable always starts: a missing libpdfium.so
// surfaces as a readable message in the host UI instead of a loader abort,
// and release layouts never need an embedded rpath (CMAKE_SKIP_RPATH).
//
// The .so is searched beside the executable (every rivet stage/package
// layout), then RIVET_PDFIUM_PATH, then the normal loader search.
#pragma once

#include <cairo/cairo.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pdfium {

// Loads libpdfium.so once per process. Thread-safe; false means
// load_error() explains what failed.
bool load();
std::string const& load_error();

// ---------------------------------------------------------------- types --
// Minimal self-contained surface of the PDFium C API (the vendored headers
// declare the same symbols as imports, which cannot be used without linking).

using FPDF_DOCUMENT = void*;
using FPDF_TEXTPAGE = void*;

constexpr int kBitmapFormatBgra = 2;  // FPDFBitmap_BGRA

// PDFium error codes (FPDF_GetLastError), the ones the host distinguishes.
constexpr unsigned long kPdfErrorPassword = 4;  // FPDF_ERR_PASSWORD

// ------------------------------------------------------------ conversion --

std::string utf16_to_utf8(unsigned short const* text, std::size_t length);
std::vector<unsigned short> utf8_to_utf16(std::string const& text);

// ----------------------------------------------------------------- docs --

class Page;
class TextPage;

// One AcroForm field (FPDF_FORMFIELD_TEXTFIELD / _CHECKBOX / _RADIOBUTTON /
// _COMBOBOX / _LISTBOX mapped onto three kinds).
struct FormFieldInfo {
  enum class Kind { Text, Check, Choice };

  int page = 0;         // 0-based
  int annot_index = 0;  // widget index on the page (FPDFPage_GetAnnot)
  Kind kind = Kind::Text;
  std::string name;     // fully qualified field name
  std::string value;    // committed value ("" = empty / unchecked)
  bool checked = false; // checkbox / radio state
  std::vector<std::string> options;  // choice option labels (in Opt order)
};

class Document {
 public:
  // Opens from a file path; empty password for none. Returns null when
  // the document cannot be opened (wrong password → last_error() == 4,
  // FPDF_ERR_PASSWORD).
  static std::unique_ptr<Document> open_file(std::string const& path,
                                             std::string const& password);
  // Opens from memory (the bytes are copied by PDFium, the buffer is not
  // retained).
  static std::unique_ptr<Document> open_memory(std::uint8_t const* data,
                                               std::size_t size,
                                               std::string const& password);
  ~Document();

  Document(Document const&) = delete;
  Document& operator=(Document const&) = delete;

  int page_count() const { return page_count_; }
  unsigned long last_error() const { return last_error_; }
  void* handle() const { return handle_; }

  // Page size in points, rotation applied (matches what a rendered page
  // looks like).
  bool page_size(int index, double* width, double* height);

  // Renders one page at `scale` (device pixels per point) into a fresh
  // ARGB32 cairo surface. With `dark`, painted pixels invert (PDF.js-style
  // dark reading, mirroring windows/PdfDocument.cpp) and unpainted margins
  // take the dark page background so pages sit flush in dark mode. Returns
  // null when the page cannot be loaded.
  cairo_surface_t* render_page(int index, double scale, bool dark,
                               int* out_width_px, int* out_height_px);

  // ------------------------------------------------------------- forms --
  // AcroForm surface. The form fill environment initializes lazily on the
  // first call and lives until the document is destroyed (exit must precede
  // FPDF_CloseDocument — the destructor owns that order). Values set here
  // live in the in-memory document only.

  // Fillable fields (text / checkbox / radio / choice), deduplicated by
  // field name (one entry per field, like the mac host).
  std::vector<FormFieldInfo> form_fields();

  // Setters verify the committed value (FORM_* path first, annotation
  // dictionary "V" as fallback) and return whether the verified document
  // state matches the request.
  bool form_set_text(int page, int annot_index, std::string const& value);
  bool form_set_checked(int page, int annot_index, bool on);
  bool form_set_choice(int page, int annot_index, std::string const& value);

  // Writes the document (including committed form values) to `path`.
  bool form_save(std::string const& path, std::string* error);

 private:
  Document() = default;
  void* handle_{nullptr};
  int page_count_{0};
  unsigned long last_error_{0};
  struct FormState;
  std::unique_ptr<FormState> form_state_;
  void* ensure_form_env();  // FPDF_FORMHANDLE, lazily initialized
};

// ----------------------------------------------------------------- text --

// One highlight rectangle in PDF page (user) space: origin BOTTOM-LEFT,
// y grows upward — `top` is the upper edge (top > bottom). This is the same
// space the stored annotation rects use, so no conversion happens on the
// way into the sidecar JSON; the VIEW layer flips into top-left origin with
// `y_screen = page_height - top` when painting.
struct TextRect {
  double left;
  double top;
  double right;
  double bottom;
};

// TextPage owns FPDFText_LoadPage for one page of one document. All offsets
// are PDFium *char* indexes; text extraction converts to text indexes.
class TextPage {
 public:
  // Returns null when the page has no text layer.
  static std::unique_ptr<TextPage> load(void* doc, int page_index);
  ~TextPage();

  TextPage(TextPage const&) = delete;
  TextPage& operator=(TextPage const&) = delete;

  int char_count() const { return char_count_; }

  // Whole-page text as UTF-8 (empty when there is no text).
  std::string text() const;

  // Character index at a top-left-origin page-space point, -1 when none.
  // `page_height` is the rotated page height in points (for the y flip into
  // PDF user space).
  int char_index_at(double x, double y_top_left, double page_height) const;

  // UTF-8 text of the inclusive char range [start_char, end_char].
  std::string text_of_chars(int start_char, int end_char) const;

  // Line rectangles covering the inclusive char range.
  std::vector<TextRect> rects_of_chars(int start_char, int end_char) const;

  // Whole-page text as UTF-16 code units — the search corpus. Positions map
  // to text indexes (FPDFText_GetText semantics).
  std::vector<unsigned short> text_utf16() const;

  // Char index for a text index obtained from text_utf16().
  int char_from_text_index(int text_index) const;

  // Unicode code point of one char (-1 out of range) — word-selection
  // boundary heuristics.
  unsigned int unicode_at(int char_index) const;

  // Boundary expansion heuristic for word selection.
  static bool is_word_char(unsigned int unicode);

 private:
  TextPage() = default;
  void* page_{nullptr};
  FPDF_TEXTPAGE handle_{nullptr};
  int char_count_{0};
};

// ---------------------------------------------------------------- usage --

// Outline node flattened depth-first.
struct OutlineItem {
  std::string title;
  int page;  // 0-based destination page, -1 when unresolvable
  int level;
};

// Harvests the document outline (empty when the PDF has none).
std::vector<OutlineItem> outline(void* doc);

}  // namespace pdfium

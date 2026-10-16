#pragma once

// PDFium wrapper for the Windows host — the thin rendering/text/outline seam
// between the WinUI view layer and PDFium's C API. The domain core owns PDF
// surgery (page edits ride the edit-* RPCs); this class only reads documents
// for display. All public rectangles use top-left origin points (y grows
// down); PDFium's bottom-left page space stays an implementation detail.
//
// PDFium is not thread-safe: every call below serializes on a process-wide
// mutex, so the view layer is free to render/find/extract from worker threads.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pdfgist {

enum class PdfOpenError {
  None,
  NeedsPassword,  // the document is encrypted; a password must be supplied
  Invalid,        // not a PDF / structurally broken / out of memory
};

// Process-wide PDFium state. Idempotent; OpenFromBytes calls it for you.
class PdfiumLibrary {
 public:
  static void Startup();
  static std::mutex& Mutex();
};

struct OutlineItem {
  std::wstring title;
  int page = 0;  // 1-based; 0 when the bookmark has no page destination
  int level = 0;
};

struct FindHit {
  int page = 0;  // 1-based
  // top-left origin points: [left, top, right, bottom]
  std::array<double, 4> rect{};
};

// Document information dictionary (the shelf's import feeds it into the
// catalog; empty strings mean the field is absent).
struct DocumentMeta {
  std::wstring title;
  std::wstring author;
};

class PdfDocument {
 public:
  // bytes are copied (PDFium keeps a pointer into them for the document's
  // lifetime); an empty password opens unencrypted documents and fails with
  // NeedsPassword on encrypted ones.
  static std::unique_ptr<PdfDocument> OpenFromBytes(std::vector<std::uint8_t> bytes,
                                                    std::string const& password,
                                                    PdfOpenError* error);

  ~PdfDocument();

  PdfDocument(PdfDocument const&) = delete;
  PdfDocument& operator=(PdfDocument const&) = delete;

  int PageCount() const { return static_cast<int>(page_sizes_.size()); }

  // Document information dictionary (Title / Author), whitespace-trimmed.
  DocumentMeta MetaInfo() const;

  // Display size of a page in points (rotation applied).
  void PageSize(int index, double* width, double* height) const;

  struct RenderedPage {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<std::uint8_t> bgra;  // top-down rows, BGRA8
  };

  // Renders one page at `scale` pixels per point. With `dark`, painted
  // pixels invert (PDF.js-style dark reading) and empty margins take the
  // dark page background so pages sit flush in dark mode.
  RenderedPage RenderPage(int index, double scale, bool dark) const;

  // Full text of one page, UTF-8.
  std::string PageText(int index) const;

  std::vector<OutlineItem> Outline() const;

  // Case-insensitive whole-document search; hits in page order.
  std::vector<FindHit> Find(std::string const& utf8_query) const;

  // Text inside a page rectangle (top-left origin points), plus the line
  // rectangles it covers (same space) for selection overlays.
  struct BoundedSelection {
    std::string utf8;
    std::vector<std::array<double, 4>> rects;
  };
  BoundedSelection SelectionInRect(int index, double left, double top,
                                   double right, double bottom) const;

  // ---- AcroForm (lazily initialized; the 10s startup smoke never hits it) ----

  struct FormField {
    std::wstring name;
    int kind = 0;  // 0 text / 1 checkbox / 2 choice
    std::wstring value;
    bool checked = false;
    std::vector<std::wstring> options;  // choice fields only
    int page = 0;                       // 1-based page of the first widget
  };

  // Creates the PDFium form-fill environment on first use (subsequent page
  // renders paint filled values through FPDF_FFLDraw).
  bool EnsureFormEnv() const;
  std::vector<FormField> FormFields() const;

  // Text/choice fill goes through the FORM_* interaction path (focus,
  // select-all, replace, kill-focus) so PDFium regenerates appearances.
  bool SetFormFieldText(std::wstring const& name, std::wstring const& value);
  // Checkbox/radio via FPDFAnnot_SetChecked/SetUnchecked.
  bool SetFormCheck(std::wstring const& name, bool checked);

  // Full rewrite of the in-memory document (form values included) to bytes.
  std::vector<std::uint8_t> SaveToBytes() const;

 private:
  PdfDocument() = default;

  std::vector<std::uint8_t> bytes_;
  void* document_ = nullptr;  // FPDF_DOCUMENT (opaque; keeps pdfium out of this header)
  std::vector<std::pair<double, double>> page_sizes_;  // points, rotation applied
  mutable void* form_info_ = nullptr;  // FPDF_FORMFILLINFO (opaque)
  mutable void* form_env_ = nullptr;   // FPDF_FORMHANDLE (opaque)
};

}  // namespace pdfgist

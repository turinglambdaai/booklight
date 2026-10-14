#include "PdfDocument.h"

#include <windows.h>

#include "fpdf_doc.h"
#include "fpdf_text.h"
#include "fpdfview.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace pdfgist {
namespace {

std::mutex& pdfium_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::wstring utf8_to_utf16(std::string const& text) {
  if (text.empty()) return {};
  int const size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
  if (size > 0) {
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          wide.data(), size);
  }
  return wide;
}

std::string utf16_to_utf8(wchar_t const* data, size_t length) {
  if (data == nullptr || length == 0) return {};
  int const size = ::WideCharToMultiByte(CP_UTF8, 0, data,
                                         static_cast<int>(length),
                                         nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
  if (size > 0) {
    ::WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(length),
                          result.data(), size, nullptr, nullptr);
  }
  return result;
}

// PDFium's FPDFText_GetText counts UTF-16 code units.
std::string utf16le_to_utf8(unsigned short const* data, int count) {
  if (data == nullptr || count <= 0) return {};
  return utf16_to_utf8(reinterpret_cast<wchar_t const*>(data),
                       static_cast<size_t>(count));
}

double clamp_min(double value, double floor) {
  return value < floor ? floor : value;
}

}  // namespace

// ---- PdfiumLibrary ------------------------------------------------------------

void PdfiumLibrary::Startup() {
  std::lock_guard<std::mutex> lock(pdfium_mutex());
  static bool started = false;
  if (started) return;
  FPDF_LIBRARY_CONFIG config{};
  config.version = 2;
  config.m_pUserFontPaths = nullptr;
  config.m_pIsolate = nullptr;
  config.m_v8EmbedderSlot = 0;
  FPDF_InitLibraryWithConfig(&config);
  started = true;
}

std::mutex& PdfiumLibrary::Mutex() { return pdfium_mutex(); }

// ---- lifecycle ------------------------------------------------------------------

std::unique_ptr<PdfDocument> PdfDocument::OpenFromBytes(
    std::vector<std::uint8_t> bytes, std::string const& password,
    PdfOpenError* error) {
  PdfiumLibrary::Startup();
  if (error != nullptr) *error = PdfOpenError::None;
  if (bytes.empty()) {
    if (error != nullptr) *error = PdfOpenError::Invalid;
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_DOCUMENT document = FPDF_LoadMemDocument(
      bytes.data(), static_cast<int>(bytes.size()),
      password.empty() ? nullptr : password.c_str());
  if (document == nullptr) {
    unsigned long const last = FPDF_GetLastError();
    if (error != nullptr) {
      *error = last == FPDF_ERR_PASSWORD ? PdfOpenError::NeedsPassword
                                         : PdfOpenError::Invalid;
    }
    return nullptr;
  }

  // Cache display sizes once: every later consumer (layout, fit-width,
  // thumbnails) reads them without re-loading pages.
  int const count = FPDF_GetPageCount(document);
  auto doc = std::unique_ptr<PdfDocument>(new PdfDocument());
  doc->bytes_ = std::move(bytes);
  doc->document_ = document;
  doc->page_sizes_.reserve(static_cast<size_t>(count));
  for (int index = 0; index < count; ++index) {
    double width = 612.0;
    double height = 792.0;
    if (FPDF_PAGE page = FPDF_LoadPage(document, index)) {
      width = FPDF_GetPageWidth(page);
      height = FPDF_GetPageHeight(page);
      FPDF_ClosePage(page);
    }
    doc->page_sizes_.emplace_back(clamp_min(width, 1.0), clamp_min(height, 1.0));
  }
  return doc;
}

PdfDocument::~PdfDocument() {
  if (document_ != nullptr) {
    std::lock_guard<std::mutex> lock(pdfium_mutex());
    FPDF_CloseDocument(static_cast<FPDF_DOCUMENT>(document_));
  }
}

void PdfDocument::PageSize(int index, double* width, double* height) const {
  int const slot = std::max(0, std::min(index, PageCount() - 1));
  if (width != nullptr) *width = page_sizes_[static_cast<size_t>(slot)].first;
  if (height != nullptr) *height = page_sizes_[static_cast<size_t>(slot)].second;
}

// ---- rendering -------------------------------------------------------------------

PdfDocument::RenderedPage PdfDocument::RenderPage(int index, double scale,
                                                  bool dark) const {
  RenderedPage result;
  double page_width = 612.0;
  double page_height = 792.0;
  PageSize(index, &page_width, &page_height);
  int const width = std::max(1, static_cast<int>(page_width * scale + 0.5));
  int const height = std::max(1, static_cast<int>(page_height * scale + 0.5));
  int const stride = width * 4;
  result.width = width;
  result.height = height;
  result.stride = stride;
  result.bgra.assign(static_cast<size_t>(stride) * height, 0);

  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
  if (FPDF_PAGE page = FPDF_LoadPage(document, index)) {
    // Opaque fill first: transparent margins would composite as black in the
    // WriteableBitmap and eat the page edges in dark mode.
    uint32_t const background = dark ? 0xFF141217u : 0xFFFFFFFFu;
    if (FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
            width, height, FPDFBitmap_BGRA, result.bgra.data(), stride)) {
      FPDFBitmap_FillRect(bitmap, 0, 0, width, height, background);
      FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, 0);
      FPDFBitmap_Destroy(bitmap);
    }
    FPDF_ClosePage(page);
  }

  if (dark) {
    // PDF.js-style negative filter: invert painted RGB, map the transparent
    // fill to the dark background, keep antialiasing alphas.
    constexpr uint8_t kDarkR = 0x1A, kDarkG = 0x17, kDarkB = 0x14;
    for (int y = 0; y < height; ++y) {
      uint8_t* row = result.bgra.data() + static_cast<size_t>(y) * stride;
      for (int x = 0; x < width; ++x) {
        uint8_t* px = row + static_cast<size_t>(x) * 4;
        uint8_t const alpha = px[3];
        if (alpha == 0) {
          px[0] = kDarkB;
          px[1] = kDarkG;
          px[2] = kDarkR;
          px[3] = 0xFF;
        } else {
          px[0] = static_cast<uint8_t>(0xFF - px[0]);
          px[1] = static_cast<uint8_t>(0xFF - px[1]);
          px[2] = static_cast<uint8_t>(0xFF - px[2]);
          px[3] = 0xFF;
        }
      }
    }
  }
  return result;
}

// ---- text --------------------------------------------------------------------------

std::string PdfDocument::PageText(int index) const {
  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
  if (FPDF_PAGE page = FPDF_LoadPage(document, index)) {
    std::string text;
    if (FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page)) {
      int const count = FPDFText_CountChars(text_page);
      if (count > 0) {
        std::vector<unsigned short> buffer(static_cast<size_t>(count));
        int const written = FPDFText_GetText(text_page, 0, count, buffer.data());
        text = utf16le_to_utf8(buffer.data(), written);
      }
      FPDFText_ClosePage(text_page);
    }
    FPDF_ClosePage(page);
    return text;
  }
  return {};
}

// Convert a PDFium rect (bottom-left origin, y up) to top-left origin points.
void to_top_left(double page_height, double* top, double* bottom) {
  double const t = *top;
  double const b = *bottom;
  *top = page_height - t;
  *bottom = page_height - b;
}

std::vector<FindHit> PdfDocument::Find(std::string const& utf8_query) const {
  std::vector<FindHit> hits;
  std::wstring const needle = utf8_to_utf16(utf8_query);
  if (needle.empty()) return hits;

  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
  int const pages = PageCount();
  for (int index = 0; index < pages; ++index) {
    if (FPDF_PAGE page = FPDF_LoadPage(document, index)) {
      double const page_height = FPDF_GetPageHeight(page);
      if (FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page)) {
        // No FPDF_MATCHCASE: case-insensitive, matching the mac find.
        if (FPDF_SCHHANDLE search = FPDFText_FindStart(
                text_page, needle.c_str(), 0, 0)) {
          while (FPDFText_FindNext(search)) {
            int const start = FPDFText_GetSchResultIndex(search);
            FindHit hit;
            hit.page = index + 1;
            double left = 0, right = 0, bottom = 0, top = 0;
            if (FPDFText_GetRect(text_page, start, &left, &right, &bottom,
                                 &top)) {
              to_top_left(page_height, &top, &bottom);
              hit.rect = {left, top, right, bottom};
              hits.push_back(hit);
            }
          }
          FPDFText_FindClose(search);
        }
        FPDFText_ClosePage(text_page);
      }
      FPDF_ClosePage(page);
    }
  }
  return hits;
}

PdfDocument::BoundedSelection PdfDocument::SelectionInRect(
    int index, double left, double top, double right, double bottom) const {
  BoundedSelection selection;
  if (right <= left || bottom <= top) return selection;

  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
  if (FPDF_PAGE page = FPDF_LoadPage(document, index)) {
    double const page_height = FPDF_GetPageHeight(page);
    // PDFium takes bottom-left page space.
    double const pdf_top = page_height - top;
    double const pdf_bottom = page_height - bottom;

    if (FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page)) {
      // FPDFText_GetBoundedText answers the text; the character walk collects
      // the line rectangles it covers so the overlay can paint them.
      std::vector<unsigned short> text;
      auto append_rect = [&](int char_index) {
        double l = 0, r = 0, b = 0, t = 0;
        if (FPDFText_GetRect(text_page, char_index, &l, &r, &b, &t)) {
          to_top_left(page_height, &t, &b);
          std::array<double, 4> rect{l, t, r, b};
          if (selection.rects.empty() || selection.rects.back() != rect) {
            selection.rects.push_back(rect);
          }
        }
      };

      int const count = FPDFText_CountChars(text_page);
      std::vector<unsigned short> chars;
      chars.reserve(static_cast<size_t>(std::max(0, count)));
      for (int i = 0; i < count; ++i) {
        double cl = 0, cr = 0, cb = 0, ct = 0;
        if (!FPDFText_GetRect(text_page, i, &cl, &cr, &cb, &ct)) continue;
        to_top_left(page_height, &ct, &cb);
        // Character center inside the band (top-left space).
        double const cy = (ct + cb) / 2.0;
        double const cx = (cl + cr) / 2.0;
        if (cx >= left && cx <= right && cy >= top && cy <= bottom) {
          unsigned short code = FPDFText_GetUnicode(text_page, i);
          if (code == 0) code = L' ';
          chars.push_back(code);
          append_rect(i);
        }
      }
      selection.utf8 = utf16le_to_utf8(chars.data(),
                                       static_cast<int>(chars.size()));
      FPDFText_ClosePage(text_page);
    }
    FPDF_ClosePage(page);
  }
  return selection;
}

// ---- outline -------------------------------------------------------------------------

namespace {

// FPDFBookmark_GetTitle answers UTF-16LE code units; FPDF APIs use unsigned
// short, the Win32 converters want wchar_t — same width on Windows.
std::wstring utf16le_to_utf16(unsigned short const* data, int count) {
  if (data == nullptr || count <= 0) return {};
  return std::wstring(reinterpret_cast<wchar_t const*>(data),
                      static_cast<size_t>(count));
}

void collect_outline(FPDF_DOCUMENT document, FPDF_BOOKMARK bookmark, int level,
                     std::vector<OutlineItem>* out) {
  for (FPDF_BOOKMARK node = bookmark; node != nullptr;
       node = FPDFBookmark_GetNextSibling(document, node)) {
    OutlineItem item;
    item.level = level;
    unsigned long const length = FPDFBookmark_GetTitle(node, nullptr, 0);
    if (length > 0) {
      std::vector<unsigned short> title(static_cast<size_t>(length));
      unsigned long const written =
          FPDFBookmark_GetTitle(node, title.data(), length);
      item.title = utf16le_to_utf16(title.data(),
                                    static_cast<int>(written / 2));
    }
    if (FPDF_DEST dest = FPDFBookmark_GetDest(document, node)) {
      int const page = FPDFDest_GetDestPageIndex(document, dest);
      item.page = page >= 0 ? page + 1 : 0;
    }
    out->push_back(std::move(item));
    if (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(document, node)) {
      collect_outline(document, child, level + 1, out);
    }
  }
}

}  // namespace

std::vector<OutlineItem> PdfDocument::Outline() const {
  std::vector<OutlineItem> items;
  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
  if (FPDF_BOOKMARK root = FPDFBookmark_GetFirstChild(document, nullptr)) {
    collect_outline(document, root, 0, &items);
  }
  return items;
}

}  // namespace pdfgist

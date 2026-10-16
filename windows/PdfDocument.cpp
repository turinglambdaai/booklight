#include "pch.h"

#include "PdfDocument.h"

#include <windows.h>

#include "fpdf_annot.h"
#include "fpdf_doc.h"
#include "fpdf_formfill.h"
#include "fpdf_save.h"
#include "fpdf_text.h"
#include "fpdfview.h"

#include <algorithm>
#include <cstddef>
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

// ---- form-fill environment callbacks (no-ops; the host drives no JS) ----
// Member names follow the pinned chromium/8100 fpdf_formfill.h.

void FPDF_CALLCONV ffi_release(struct _FPDF_FORMFILLINFO*) {}
void FPDF_CALLCONV ffi_invalidate(struct _FPDF_FORMFILLINFO*, FPDF_PAGE, double,
                                  double, double, double) {}
void FPDF_CALLCONV ffi_output_selected_rect(struct _FPDF_FORMFILLINFO*,
                                            FPDF_PAGE, double, double, double,
                                            double) {}
void FPDF_CALLCONV ffi_set_cursor(struct _FPDF_FORMFILLINFO*, int) {}
int FPDF_CALLCONV ffi_set_timer(struct _FPDF_FORMFILLINFO*, int,
                                TimerCallback) {
  return 0;
}
void FPDF_CALLCONV ffi_kill_timer(struct _FPDF_FORMFILLINFO*, int) {}
FPDF_SYSTEMTIME FPDF_CALLCONV ffi_get_local_time(struct _FPDF_FORMFILLINFO*) {
  FPDF_SYSTEMTIME time{};
  ::GetLocalTime(reinterpret_cast<LPSYSTEMTIME>(&time));
  return time;
}
void FPDF_CALLCONV ffi_on_change(struct _FPDF_FORMFILLINFO*) {}
FPDF_PAGE FPDF_CALLCONV ffi_get_page(struct _FPDF_FORMFILLINFO*, FPDF_DOCUMENT,
                                     int) {
  return nullptr;
}
int FPDF_CALLCONV ffi_get_rotation(struct _FPDF_FORMFILLINFO*, FPDF_PAGE) {
  return 0;
}
void FPDF_CALLCONV ffi_execute_named_action(struct _FPDF_FORMFILLINFO*,
                                            FPDF_BYTESTRING) {}
void FPDF_CALLCONV ffi_set_text_field_focus(struct _FPDF_FORMFILLINFO*,
                                            FPDF_WIDESTRING, FPDF_DWORD,
                                            FPDF_BOOL) {}
void FPDF_CALLCONV ffi_do_uri_action(struct _FPDF_FORMFILLINFO*,
                                     FPDF_BYTESTRING) {}
void FPDF_CALLCONV ffi_do_go_to_action(struct _FPDF_FORMFILLINFO*, int, int,
                                       float*, int) {}

std::wstring form_utf16_to_wide(unsigned short const* data,
                                unsigned long length) {
  if (data == nullptr || length == 0) return {};
  return std::wstring(reinterpret_cast<wchar_t const*>(data),
                      static_cast<std::size_t>(length));
}

using FormFieldReader = unsigned long (FPDF_CALLCONV*)(
    FPDF_FORMHANDLE, FPDF_ANNOTATION, FPDF_WCHAR*, unsigned long);

// The fpdf_annot.h form-field getters answer the string length in BYTES
// (UTF-16LE, including the 2-byte terminator; empty string answers 2).
std::wstring form_field_string(FPDF_FORMHANDLE handle, FPDF_ANNOTATION annot,
                               FormFieldReader reader) {
  unsigned long const bytes = reader(handle, annot, nullptr, 0);
  if (bytes <= 2) return {};
  std::vector<unsigned short> buffer(static_cast<std::size_t>(bytes / 2) + 1);
  unsigned long const written = reader(handle, annot, buffer.data(), bytes);
  if (written < 2) return {};
  return form_utf16_to_wide(buffer.data(), written / 2 - 1);
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
    if (form_env_ != nullptr) {
      FPDFDOC_ExitFormFillEnvironment(
          static_cast<FPDF_FORMHANDLE>(form_env_));
      form_env_ = nullptr;
    }
    delete static_cast<FPDF_FORMFILLINFO*>(form_info_);
    form_info_ = nullptr;
    FPDF_CloseDocument(static_cast<FPDF_DOCUMENT>(document_));
  }
}

bool PdfDocument::EnsureFormEnv() const {
  std::lock_guard<std::mutex> lock(pdfium_mutex());
  if (form_env_ != nullptr) return true;
  auto info = new FPDF_FORMFILLINFO{};
  info->version = 1;
  info->Release = &ffi_release;
  info->FFI_Invalidate = &ffi_invalidate;
  info->FFI_OutputSelectedRect = &ffi_output_selected_rect;
  info->FFI_SetCursor = &ffi_set_cursor;
  info->FFI_SetTimer = &ffi_set_timer;
  info->FFI_KillTimer = &ffi_kill_timer;
  info->FFI_GetLocalTime = &ffi_get_local_time;
  info->FFI_OnChange = &ffi_on_change;
  info->FFI_GetPage = &ffi_get_page;
  info->FFI_GetRotation = &ffi_get_rotation;
  info->FFI_ExecuteNamedAction = &ffi_execute_named_action;
  info->FFI_SetTextFieldFocus = &ffi_set_text_field_focus;
  info->FFI_DoURIAction = &ffi_do_uri_action;
  info->FFI_DoGoToAction = &ffi_do_go_to_action;
  FPDF_FORMHANDLE handle = FPDFDOC_InitFormFillEnvironment(
      static_cast<FPDF_DOCUMENT>(document_), info);
  if (handle == nullptr) {
    delete info;
    return false;
  }
  form_info_ = info;
  form_env_ = handle;
  return true;
}

std::vector<PdfDocument::FormField> PdfDocument::FormFields() const {
  std::vector<FormField> fields;
  if (!EnsureFormEnv()) return fields;

  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_FORMHANDLE handle = static_cast<FPDF_FORMHANDLE>(form_env_);
  FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
  int const pages = PageCount();
  for (int index = 0; index < pages; ++index) {
    FPDF_PAGE page = FPDF_LoadPage(document, index);
    if (page == nullptr) continue;
    FORM_OnAfterLoadPage(page, handle);
    int const count = FPDFPage_GetAnnotCount(page);
    for (int i = 0; i < count; ++i) {
      FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
      if (annot == nullptr) continue;
      int const type = FPDFAnnot_GetFormFieldType(handle, annot);
      int kind = -1;
      switch (type) {
        case FPDF_FORMFIELD_TEXTFIELD:
          kind = 0;
          break;
        case FPDF_FORMFIELD_CHECKBOX:
        case FPDF_FORMFIELD_RADIOBUTTON:
          kind = 1;
          break;
        case FPDF_FORMFIELD_COMBOBOX:
        case FPDF_FORMFIELD_LISTBOX:
          kind = 2;
          break;
        default:
          break;
      }
      if (kind >= 0) {
        std::wstring const name = form_field_string(
            handle, annot, &FPDFAnnot_GetFormFieldName);
        if (!name.empty()) {
          FormField* field = nullptr;
          for (auto& candidate : fields) {
            if (candidate.name == name) {
              field = &candidate;
              break;
            }
          }
          if (field == nullptr) {
            fields.push_back(FormField{});
            field = &fields.back();
            field->name = name;
            field->kind = kind;
            field->page = index + 1;
          }
          if (kind == 1) {
            field->checked =
                field->checked || FPDFAnnot_IsChecked(handle, annot) != 0;
          } else if (field->value.empty()) {
            field->value = form_field_string(
                handle, annot, &FPDFAnnot_GetFormFieldValue);
            if (kind == 2) {
              int const options = FPDFAnnot_GetOptionCount(handle, annot);
              for (int o = 0; o < options; ++o) {
                unsigned long const bytes =
                    FPDFAnnot_GetOptionLabel(handle, annot, o, nullptr, 0);
                if (bytes <= 2) continue;
                std::vector<unsigned short> label(
                    static_cast<std::size_t>(bytes / 2) + 1);
                unsigned long const written = FPDFAnnot_GetOptionLabel(
                    handle, annot, o, label.data(), bytes);
                if (written < 2) continue;
                field->options.push_back(form_utf16_to_wide(
                    label.data(), written / 2 - 1));
              }
            }
          }
        }
      }
      FPDFPage_CloseAnnot(annot);
    }
    FORM_OnBeforeClosePage(page, handle);
    FPDF_ClosePage(page);
  }
  return fields;
}

bool PdfDocument::SetFormFieldText(std::wstring const& name,
                                   std::wstring const& value) {
  if (!EnsureFormEnv()) return false;

  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_FORMHANDLE handle = static_cast<FPDF_FORMHANDLE>(form_env_);
  FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
  bool updated = false;
  int const pages = PageCount();
  for (int index = 0; index < pages && !updated; ++index) {
    FPDF_PAGE page = FPDF_LoadPage(document, index);
    if (page == nullptr) continue;
    FORM_OnAfterLoadPage(page, handle);
    int const count = FPDFPage_GetAnnotCount(page);
    for (int i = 0; i < count && !updated; ++i) {
      FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
      if (annot == nullptr) continue;
      int const type = FPDFAnnot_GetFormFieldType(handle, annot);
      bool const fillable = type == FPDF_FORMFIELD_TEXTFIELD ||
                            type == FPDF_FORMFIELD_COMBOBOX ||
                            type == FPDF_FORMFIELD_LISTBOX;
      if (fillable) {
        std::wstring const field_name = form_field_string(
            handle, annot, &FPDFAnnot_GetFormFieldName);
        if (field_name == name) {
          if (FORM_SetFocusedAnnot(handle, annot)) {
            FORM_SelectAllText(handle, page);
            FORM_ReplaceSelection(
                handle, page,
                reinterpret_cast<FPDF_WIDESTRING>(value.c_str()));
            FORM_ForceToKillFocus(handle);
            updated = true;
          } else if (FPDFAnnot_SetStringValue(
                         annot, "V",
                         reinterpret_cast<FPDF_WIDESTRING>(value.c_str()))) {
            // Fallback: dictionary V value; appearance may be stale.
            updated = true;
          }
        }
      }
      FPDFPage_CloseAnnot(annot);
    }
    FORM_OnBeforeClosePage(page, handle);
    FPDF_ClosePage(page);
  }
  return updated;
}

bool PdfDocument::SetFormCheck(std::wstring const& name, bool checked) {
  if (!EnsureFormEnv()) return false;

  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_FORMHANDLE handle = static_cast<FPDF_FORMHANDLE>(form_env_);
  FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
  bool updated = false;
  int const pages = PageCount();
  for (int index = 0; index < pages; ++index) {
    FPDF_PAGE page = FPDF_LoadPage(document, index);
    if (page == nullptr) continue;
    FORM_OnAfterLoadPage(page, handle);
    int const count = FPDFPage_GetAnnotCount(page);
    for (int i = 0; i < count; ++i) {
      FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
      if (annot == nullptr) continue;
      int const type = FPDFAnnot_GetFormFieldType(handle, annot);
      if (type == FPDF_FORMFIELD_CHECKBOX ||
          type == FPDF_FORMFIELD_RADIOBUTTON) {
        std::wstring const field_name = form_field_string(
            handle, annot, &FPDFAnnot_GetFormFieldName);
        if (field_name == name) {
          // The pinned build ships FPDFAnnot_IsChecked but no SetChecked, so
          // toggling goes through the interaction path: a synthetic left
          // click at the widget's rect center commits through the form-fill
          // layer (value + appearance), exactly like a user click.
          bool const now_checked = FPDFAnnot_IsChecked(handle, annot) != 0;
          if (now_checked != checked) {
            FS_RECTF rect{};
            if (FPDFAnnot_GetRect(annot, &rect)) {
              double const cx = (rect.left + rect.right) / 2.0;
              double const cy = (rect.top + rect.bottom) / 2.0;
              FORM_OnLButtonDown(handle, page, 0, cx, cy);
              FORM_OnLButtonUp(handle, page, 0, cx, cy);
            }
          }
          updated = (FPDFAnnot_IsChecked(handle, annot) != 0) == checked ||
                    updated;
        }
      }
      FPDFPage_CloseAnnot(annot);
    }
    FORM_OnBeforeClosePage(page, handle);
    FPDF_ClosePage(page);
  }
  return updated;
}

std::vector<std::uint8_t> PdfDocument::SaveToBytes() const {
  std::lock_guard<std::mutex> lock(pdfium_mutex());
  std::vector<std::uint8_t> out;

  struct SaveContext {
    std::vector<std::uint8_t>* out;
    FPDF_FILEWRITE writer;
  };
  SaveContext context{&out, {}};
  context.writer.version = 1;
  context.writer.WriteBlock =
      [](FPDF_FILEWRITE* writer, void const* data, unsigned long size) -> int {
    auto* self = reinterpret_cast<SaveContext*>(
        reinterpret_cast<std::uintptr_t>(writer) - offsetof(SaveContext, writer));
    if (data == nullptr) return 0;
    self->out->insert(self->out->end(), static_cast<std::uint8_t const*>(data),
                      static_cast<std::uint8_t const*>(data) + size);
    return 1;
  };
  FPDF_SaveAsCopy(static_cast<FPDF_DOCUMENT>(document_), &context.writer, 0);
  return out;
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
    FPDF_FORMHANDLE const form =
        form_env_ != nullptr ? static_cast<FPDF_FORMHANDLE>(form_env_)
                             : nullptr;
    if (form != nullptr) FORM_OnAfterLoadPage(page, form);
    // Opaque fill first: transparent margins would composite as black in the
    // WriteableBitmap and eat the page edges in dark mode.
    uint32_t const background = dark ? 0xFF141217u : 0xFFFFFFFFu;
    if (FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
            width, height, FPDFBitmap_BGRA, result.bgra.data(), stride)) {
      FPDFBitmap_FillRect(bitmap, 0, 0, width, height, background);
      FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, 0);
      // Paint the live form-fill layer so filled values show without a save.
      if (form != nullptr) {
        FPDF_FFLDraw(form, bitmap, page, 0, 0, width, height, 0, 0);
      }
      FPDFBitmap_Destroy(bitmap);
    }
    if (form != nullptr) FORM_OnBeforeClosePage(page, form);
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
                text_page,
                reinterpret_cast<FPDF_WIDESTRING>(needle.c_str()), 0, 0)) {
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
          // FPDFText_GetUnicode answers a full code point; non-BMP chars
          // must become a UTF-16 surrogate pair or the narrow store would
          // silently keep only the low 16 bits.
          unsigned int const code_point = FPDFText_GetUnicode(text_page, i);
          if (code_point == 0) {
            chars.push_back(L' ');
          } else if (code_point > 0xFFFF) {
            unsigned int const offset = code_point - 0x10000;
            chars.push_back(static_cast<unsigned short>(0xD800 |
                                                        (offset >> 10)));
            chars.push_back(static_cast<unsigned short>(0xDC00 |
                                                        (offset & 0x3FF)));
          } else {
            chars.push_back(static_cast<unsigned short>(code_point));
          }
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

// ---- metadata ----------------------------------------------------------------------

// FPDF_GetMetaText answers the byte length including the 2-byte NUL
// terminator; trim stray whitespace the way PDFKit does.
std::wstring meta_string(FPDF_DOCUMENT document, FPDF_BYTESTRING tag) {
  unsigned long const bytes = FPDF_GetMetaText(document, tag, nullptr, 0);
  if (bytes < 2) return {};
  std::vector<unsigned short> buffer(static_cast<size_t>(bytes / 2) + 1);
  unsigned long const written =
      FPDF_GetMetaText(document, tag, buffer.data(), bytes);
  if (written < 2) return {};
  std::wstring text = utf16le_to_utf16(buffer.data(),
                                       static_cast<int>(written / 2 - 1));
  std::size_t const first = text.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  std::size_t const last = text.find_last_not_of(L" \t\r\n");
  return text.substr(first, last - first + 1);
}

DocumentMeta PdfDocument::MetaInfo() const {
  std::lock_guard<std::mutex> lock(pdfium_mutex());
  FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
  DocumentMeta meta;
  meta.title = meta_string(document, "Title");
  meta.author = meta_string(document, "Author");
  return meta;
}

}  // namespace pdfgist

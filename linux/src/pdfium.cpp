// PDFium binding implementation — see pdfium.hpp.
#include "pdfium.hpp"

#include <dlfcn.h>

// The pinned pdfium-binaries archive ships the full public headers beside
// lib/libpdfium.so (CMake adds include/ to the search path). They are used
// for TYPE DEFINITIONS ONLY (FPDF_FORMFILLINFO, FPDF_FILEWRITE, FS_RECTF —
// whose exact layout the library depends on); every function still arrives
// through dlsym, so the executable keeps no DT_NEEDED on libpdfium.
#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <system_error>

namespace pdfium {
namespace {

// ---- function pointer signatures (the PDFium C API subset in use) --------

using FPDF_BOOL = int;
using FPDF_BYTESTRING = const char*;
using FPDF_PAGE = void*;
using FPDF_BITMAP = void*;
using FPDF_BOOKMARK = void*;
using FPDF_DEST = void*;
using FPDF_SCHHANDLE = void*;
using FPDF_ERR = unsigned long;

using InitLibraryFn = void (*)(void*);
using LoadDocFn = FPDF_DOCUMENT (*)(const char*, FPDF_BYTESTRING);
using LoadMemDocFn = FPDF_DOCUMENT (*)(const void*, size_t, FPDF_BYTESTRING);
using CloseDocFn = void (*)(FPDF_DOCUMENT);
using GetPageCountFn = int (*)(FPDF_DOCUMENT);
using GetLastErrorFn = FPDF_ERR (*)(void);
using LoadPageFn = FPDF_PAGE (*)(FPDF_DOCUMENT, int);
using ClosePageFn = void (*)(FPDF_PAGE);
using GetPageWidthFFn = float (*)(FPDF_PAGE);
using GetPageHeightFFn = float (*)(FPDF_PAGE);
using RenderPageBitmapFn = void (*)(FPDF_BITMAP, FPDF_PAGE, int, int, int,
                                    int, int, int);
using BitmapCreateExFn = FPDF_BITMAP (*)(int, int, int, void*, int);
using BitmapDestroyFn = void (*)(FPDF_BITMAP);
using PageGetRotationFn = int (*)(FPDF_PAGE);
using TextLoadPageFn = FPDF_TEXTPAGE (*)(FPDF_PAGE);
using TextClosePageFn = void (*)(FPDF_TEXTPAGE);
using TextCountCharsFn = int (*)(FPDF_TEXTPAGE);
using TextGetTextFn = int (*)(FPDF_TEXTPAGE, int, int, unsigned short*);
using TextGetUnicodeFn = unsigned int (*)(FPDF_TEXTPAGE, int);
using TextCountRectsFn = int (*)(FPDF_TEXTPAGE, int, int);
using TextGetRectFn = FPDF_BOOL (*)(FPDF_TEXTPAGE, int, double*, double*,
                                    double*, double*);
using TextGetCharIndexAtPosFn = int (*)(FPDF_TEXTPAGE, double, double, double,
                                        double);
using TextGetCharBoxFn = FPDF_BOOL (*)(FPDF_TEXTPAGE, int, double*, double*,
                                       double*, double*);
using TextGetCharIndexFromTextIndexFn = int (*)(FPDF_TEXTPAGE, int);
using TextGetTextIndexFromCharIndexFn = int (*)(FPDF_TEXTPAGE, int);
using BookmarkGetFirstChildFn = FPDF_BOOKMARK (*)(FPDF_DOCUMENT,
                                                  FPDF_BOOKMARK);
using BookmarkGetNextSiblingFn = FPDF_BOOKMARK (*)(FPDF_BOOKMARK);
using BookmarkGetTitleFn = unsigned long (*)(FPDF_BOOKMARK, void*,
                                             unsigned long);
using BookmarkGetDestFn = FPDF_DEST (*)(FPDF_DOCUMENT, FPDF_BOOKMARK);
using DestGetDestPageIndexFn = int (*)(FPDF_DOCUMENT, FPDF_DEST);
using GetMetaTextFn = unsigned long (*)(FPDF_DOCUMENT, FPDF_BYTESTRING, void*,
                                        unsigned long);

struct Api {
  InitLibraryFn FPDF_InitLibrary{nullptr};
  LoadDocFn FPDF_LoadDocument{nullptr};
  LoadMemDocFn FPDF_LoadMemDocument{nullptr};
  CloseDocFn FPDF_CloseDocument{nullptr};
  GetPageCountFn FPDF_GetPageCount{nullptr};
  GetLastErrorFn FPDF_GetLastError{nullptr};
  LoadPageFn FPDF_LoadPage{nullptr};
  ClosePageFn FPDF_ClosePage{nullptr};
  GetPageWidthFFn FPDF_GetPageWidthF{nullptr};
  GetPageHeightFFn FPDF_GetPageHeightF{nullptr};
  RenderPageBitmapFn FPDF_RenderPageBitmap{nullptr};
  BitmapCreateExFn FPDFBitmap_CreateEx{nullptr};
  BitmapDestroyFn FPDFBitmap_Destroy{nullptr};
  PageGetRotationFn FPDFPage_GetRotation{nullptr};
  TextLoadPageFn FPDFText_LoadPage{nullptr};
  TextClosePageFn FPDFText_ClosePage{nullptr};
  TextCountCharsFn FPDFText_CountChars{nullptr};
  TextGetTextFn FPDFText_GetText{nullptr};
  TextGetUnicodeFn FPDFText_GetUnicode{nullptr};
  TextCountRectsFn FPDFText_CountRects{nullptr};
  TextGetRectFn FPDFText_GetRect{nullptr};
  TextGetCharIndexAtPosFn FPDFText_GetCharIndexAtPos{nullptr};
  TextGetCharBoxFn FPDFText_GetCharBox{nullptr};
  TextGetCharIndexFromTextIndexFn FPDFText_GetCharIndexFromTextIndex{nullptr};
  TextGetTextIndexFromCharIndexFn FPDFText_GetTextIndexFromCharIndex{nullptr};
  BookmarkGetFirstChildFn FPDFBookmark_GetFirstChild{nullptr};
  BookmarkGetNextSiblingFn FPDFBookmark_GetNextSibling{nullptr};
  BookmarkGetTitleFn FPDFBookmark_GetTitle{nullptr};
  BookmarkGetDestFn FPDFBookmark_GetDest{nullptr};
  DestGetDestPageIndexFn FPDFDest_GetDestPageIndex{nullptr};
  GetMetaTextFn FPDF_GetMetaText{nullptr};
  bool complete{false};
};

Api g_api;
std::once_flag g_once;
std::string g_error;

// ---- optional surface: AcroForm fill + save -----------------------------
//
// Bound only from the pinned archive; a libpdfium without these symbols
// degrades to "no form support" instead of disabling rendering outright.
// Signatures use the ::-qualified vendored-header types so they match the
// library entry points exactly (the unqualified names resolve to this
// file's void*-based aliases).

using InitFormFillEnvFn = ::FPDF_FORMHANDLE (*)(::FPDF_DOCUMENT,
                                                ::FPDF_FORMFILLINFO*);
using ExitFormFillEnvFn = void (*)(::FPDF_FORMHANDLE);
using FormAfterLoadPageFn = void (*)(::FPDF_PAGE, ::FPDF_FORMHANDLE);
using FormBeforeClosePageFn = void (*)(::FPDF_PAGE, ::FPDF_FORMHANDLE);
using FormSetFocusedAnnotFn = ::FPDF_BOOL (*)(::FPDF_FORMHANDLE,
                                              ::FPDF_ANNOTATION);
using FormForceToKillFocusFn = ::FPDF_BOOL (*)(::FPDF_FORMHANDLE);
using FormSelectAllTextFn = ::FPDF_BOOL (*)(::FPDF_FORMHANDLE, ::FPDF_PAGE);
using FormReplaceSelectionFn = void (*)(::FPDF_FORMHANDLE, ::FPDF_PAGE,
                                        ::FPDF_WIDESTRING);
using FormSetIndexSelectedFn = ::FPDF_BOOL (*)(::FPDF_FORMHANDLE, ::FPDF_PAGE,
                                               int, ::FPDF_BOOL);
using FormOnLButtonDownFn = ::FPDF_BOOL (*)(::FPDF_FORMHANDLE, ::FPDF_PAGE,
                                            int, double, double);
using FormOnLButtonUpFn = ::FPDF_BOOL (*)(::FPDF_FORMHANDLE, ::FPDF_PAGE, int,
                                          double, double);
using AnnotGetFormFieldNameFn = unsigned long (*)(::FPDF_FORMHANDLE,
                                                  ::FPDF_ANNOTATION,
                                                  ::FPDF_WCHAR*,
                                                  unsigned long);
using AnnotGetFormFieldTypeFn = int (*)(::FPDF_FORMHANDLE, ::FPDF_ANNOTATION);
using AnnotGetFormFieldValueFn = unsigned long (*)(::FPDF_FORMHANDLE,
                                                   ::FPDF_ANNOTATION,
                                                   ::FPDF_WCHAR*,
                                                   unsigned long);
using AnnotGetOptionCountFn = int (*)(::FPDF_FORMHANDLE, ::FPDF_ANNOTATION);
using AnnotGetOptionLabelFn = unsigned long (*)(::FPDF_FORMHANDLE,
                                                ::FPDF_ANNOTATION, int,
                                                ::FPDF_WCHAR*, unsigned long);
using AnnotIsCheckedFn = ::FPDF_BOOL (*)(::FPDF_FORMHANDLE, ::FPDF_ANNOTATION);
using AnnotGetExportValueFn = unsigned long (*)(::FPDF_FORMHANDLE,
                                                ::FPDF_ANNOTATION,
                                                ::FPDF_WCHAR*, unsigned long);
using AnnotSetStringValueFn = ::FPDF_BOOL (*)(::FPDF_ANNOTATION,
                                              ::FPDF_BYTESTRING,
                                              ::FPDF_WIDESTRING);
using AnnotGetRectFn = ::FPDF_BOOL (*)(::FPDF_ANNOTATION, ::FS_RECTF*);
using PageGetAnnotCountFn = int (*)(::FPDF_PAGE);
using PageGetAnnotFn = ::FPDF_ANNOTATION (*)(::FPDF_PAGE, int);
using PageCloseAnnotFn = void (*)(::FPDF_ANNOTATION);
using SaveAsCopyFn = ::FPDF_BOOL (*)(::FPDF_DOCUMENT, ::FPDF_FILEWRITE*,
                                     ::FPDF_DWORD);

struct FormApi {
  InitFormFillEnvFn FPDFDOC_InitFormFillEnvironment{nullptr};
  ExitFormFillEnvFn FPDFDOC_ExitFormFillEnvironment{nullptr};
  FormAfterLoadPageFn FORM_OnAfterLoadPage{nullptr};
  FormBeforeClosePageFn FORM_OnBeforeClosePage{nullptr};
  FormSetFocusedAnnotFn FORM_SetFocusedAnnot{nullptr};
  FormForceToKillFocusFn FORM_ForceToKillFocus{nullptr};
  FormSelectAllTextFn FORM_SelectAllText{nullptr};
  FormReplaceSelectionFn FORM_ReplaceSelection{nullptr};
  FormSetIndexSelectedFn FORM_SetIndexSelected{nullptr};
  FormOnLButtonDownFn FORM_OnLButtonDown{nullptr};
  FormOnLButtonUpFn FORM_OnLButtonUp{nullptr};
  AnnotGetFormFieldNameFn FPDFAnnot_GetFormFieldName{nullptr};
  AnnotGetFormFieldTypeFn FPDFAnnot_GetFormFieldType{nullptr};
  AnnotGetFormFieldValueFn FPDFAnnot_GetFormFieldValue{nullptr};
  AnnotGetOptionCountFn FPDFAnnot_GetOptionCount{nullptr};
  AnnotGetOptionLabelFn FPDFAnnot_GetOptionLabel{nullptr};
  AnnotIsCheckedFn FPDFAnnot_IsChecked{nullptr};
  AnnotGetExportValueFn FPDFAnnot_GetFormFieldExportValue{nullptr};
  AnnotSetStringValueFn FPDFAnnot_SetStringValue{nullptr};
  AnnotGetRectFn FPDFAnnot_GetRect{nullptr};
  PageGetAnnotCountFn FPDFPage_GetAnnotCount{nullptr};
  PageGetAnnotFn FPDFPage_GetAnnot{nullptr};
  PageCloseAnnotFn FPDFPage_CloseAnnot{nullptr};
  SaveAsCopyFn FPDF_SaveAsCopy{nullptr};
  bool complete{false};
};

FormApi g_form;

// dlsym without failing the whole binding: a missing symbol just leaves the
// form surface unavailable.
template <typename T>
bool bind_optional(void* handle, T& fn, char const* name) {
  void* symbol = dlsym(handle, name);
  if (symbol == nullptr) return false;
  fn = *reinterpret_cast<T*>(&symbol);
  return true;
}

bool load_form_api(void* handle) {
  return bind_optional(handle, g_form.FPDFDOC_InitFormFillEnvironment,
                       "FPDFDOC_InitFormFillEnvironment") &&
         bind_optional(handle, g_form.FPDFDOC_ExitFormFillEnvironment,
                       "FPDFDOC_ExitFormFillEnvironment") &&
         bind_optional(handle, g_form.FORM_OnAfterLoadPage,
                       "FORM_OnAfterLoadPage") &&
         bind_optional(handle, g_form.FORM_OnBeforeClosePage,
                       "FORM_OnBeforeClosePage") &&
         bind_optional(handle, g_form.FORM_SetFocusedAnnot,
                       "FORM_SetFocusedAnnot") &&
         bind_optional(handle, g_form.FORM_ForceToKillFocus,
                       "FORM_ForceToKillFocus") &&
         bind_optional(handle, g_form.FORM_SelectAllText,
                       "FORM_SelectAllText") &&
         bind_optional(handle, g_form.FORM_ReplaceSelection,
                       "FORM_ReplaceSelection") &&
         bind_optional(handle, g_form.FORM_SetIndexSelected,
                       "FORM_SetIndexSelected") &&
         bind_optional(handle, g_form.FORM_OnLButtonDown,
                       "FORM_OnLButtonDown") &&
         bind_optional(handle, g_form.FORM_OnLButtonUp, "FORM_OnLButtonUp") &&
         bind_optional(handle, g_form.FPDFAnnot_GetFormFieldName,
                       "FPDFAnnot_GetFormFieldName") &&
         bind_optional(handle, g_form.FPDFAnnot_GetFormFieldType,
                       "FPDFAnnot_GetFormFieldType") &&
         bind_optional(handle, g_form.FPDFAnnot_GetFormFieldValue,
                       "FPDFAnnot_GetFormFieldValue") &&
         bind_optional(handle, g_form.FPDFAnnot_GetOptionCount,
                       "FPDFAnnot_GetOptionCount") &&
         bind_optional(handle, g_form.FPDFAnnot_GetOptionLabel,
                       "FPDFAnnot_GetOptionLabel") &&
         bind_optional(handle, g_form.FPDFAnnot_IsChecked,
                       "FPDFAnnot_IsChecked") &&
         bind_optional(handle, g_form.FPDFAnnot_GetFormFieldExportValue,
                       "FPDFAnnot_GetFormFieldExportValue") &&
         bind_optional(handle, g_form.FPDFAnnot_SetStringValue,
                       "FPDFAnnot_SetStringValue") &&
         bind_optional(handle, g_form.FPDFAnnot_GetRect, "FPDFAnnot_GetRect") &&
         bind_optional(handle, g_form.FPDFPage_GetAnnotCount,
                       "FPDFPage_GetAnnotCount") &&
         bind_optional(handle, g_form.FPDFPage_GetAnnot, "FPDFPage_GetAnnot") &&
         bind_optional(handle, g_form.FPDFPage_CloseAnnot,
                       "FPDFPage_CloseAnnot") &&
         bind_optional(handle, g_form.FPDF_SaveAsCopy, "FPDF_SaveAsCopy");
}

template <typename T>
bool bind(void* handle, T& fn, char const* name) {
  void* symbol = dlsym(handle, name);
  if (symbol == nullptr) {
    g_error = std::string("libpdfium.so is missing symbol ") + name;
    return false;
  }
  fn = *reinterpret_cast<T*>(&symbol);
  return true;
}

void load_once() {
  // Search order: the env override, beside the executable (every rivet
  // stage/package layout keeps the .so next to the binary), then the normal
  // loader search (LD_LIBRARY_PATH / system dirs).
  std::vector<std::string> candidates;
  if (char const* override_path = std::getenv("RIVET_PDFIUM_PATH")) {
    if (override_path[0] != '\0') candidates.push_back(override_path);
  }
  std::error_code ec;
  auto const self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    candidates.push_back((self.parent_path() / "libpdfium.so").string());
  }
  candidates.push_back("libpdfium.so");

  void* handle = nullptr;
  char const* failure = "unknown error";
  for (auto const& candidate : candidates) {
    handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle != nullptr) break;
    char const* err = dlerror();
    failure = err != nullptr ? err : "unknown error";
  }
  if (handle == nullptr) {
    g_error = std::string("cannot load libpdfium.so: ") + failure;
    return;
  }
  if (!bind(handle, g_api.FPDF_InitLibrary, "FPDF_InitLibrary") ||
      !bind(handle, g_api.FPDF_LoadDocument, "FPDF_LoadDocument") ||
      !bind(handle, g_api.FPDF_LoadMemDocument, "FPDF_LoadMemDocument") ||
      !bind(handle, g_api.FPDF_CloseDocument, "FPDF_CloseDocument") ||
      !bind(handle, g_api.FPDF_GetPageCount, "FPDF_GetPageCount") ||
      !bind(handle, g_api.FPDF_GetLastError, "FPDF_GetLastError") ||
      !bind(handle, g_api.FPDF_LoadPage, "FPDF_LoadPage") ||
      !bind(handle, g_api.FPDF_ClosePage, "FPDF_ClosePage") ||
      !bind(handle, g_api.FPDF_GetPageWidthF, "FPDF_GetPageWidthF") ||
      !bind(handle, g_api.FPDF_GetPageHeightF, "FPDF_GetPageHeightF") ||
      !bind(handle, g_api.FPDF_RenderPageBitmap, "FPDF_RenderPageBitmap") ||
      !bind(handle, g_api.FPDFBitmap_CreateEx, "FPDFBitmap_CreateEx") ||
      !bind(handle, g_api.FPDFBitmap_Destroy, "FPDFBitmap_Destroy") ||
      !bind(handle, g_api.FPDFPage_GetRotation, "FPDFPage_GetRotation") ||
      !bind(handle, g_api.FPDFText_LoadPage, "FPDFText_LoadPage") ||
      !bind(handle, g_api.FPDFText_ClosePage, "FPDFText_ClosePage") ||
      !bind(handle, g_api.FPDFText_CountChars, "FPDFText_CountChars") ||
      !bind(handle, g_api.FPDFText_GetText, "FPDFText_GetText") ||
      !bind(handle, g_api.FPDFText_GetUnicode, "FPDFText_GetUnicode") ||
      !bind(handle, g_api.FPDFText_CountRects, "FPDFText_CountRects") ||
      !bind(handle, g_api.FPDFText_GetRect, "FPDFText_GetRect") ||
      !bind(handle, g_api.FPDFText_GetCharIndexAtPos,
            "FPDFText_GetCharIndexAtPos") ||
      !bind(handle, g_api.FPDFText_GetCharBox, "FPDFText_GetCharBox") ||
      !bind(handle, g_api.FPDFText_GetCharIndexFromTextIndex,
            "FPDFText_GetCharIndexFromTextIndex") ||
      !bind(handle, g_api.FPDFText_GetTextIndexFromCharIndex,
            "FPDFText_GetTextIndexFromCharIndex") ||
      !bind(handle, g_api.FPDFBookmark_GetFirstChild,
            "FPDFBookmark_GetFirstChild") ||
      !bind(handle, g_api.FPDFBookmark_GetNextSibling,
            "FPDFBookmark_GetNextSibling") ||
      !bind(handle, g_api.FPDFBookmark_GetTitle, "FPDFBookmark_GetTitle") ||
      !bind(handle, g_api.FPDFBookmark_GetDest, "FPDFBookmark_GetDest") ||
      !bind(handle, g_api.FPDFDest_GetDestPageIndex,
            "FPDFDest_GetDestPageIndex")) {
    return;
  }
  g_api.complete = true;
  g_api.FPDF_InitLibrary(nullptr);
  // Metadata is optional surface too: a trimmed libpdfium loses only the
  // shelf title/author, never rendering.
  (void)bind_optional(handle, g_api.FPDF_GetMetaText, "FPDF_GetMetaText");
  // The form/save surface is optional: older or trimmed libpdfium builds
  // lose only AcroForm support, never rendering.
  g_form.complete = load_form_api(handle);
}

}  // namespace

// ------------------------------------------------------------- lifecycle --

bool load() {
  std::call_once(g_once, load_once);
  return g_api.complete;
}

std::string const& load_error() { return g_error; }

// ------------------------------------------------------------ conversion --

std::string utf16_to_utf8(unsigned short const* text, std::size_t length) {
  if (text == nullptr || length == 0) return {};
  std::string out;
  out.reserve(length);
  for (std::size_t i = 0; i < length;) {
    unsigned int cp = text[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length && text[i + 1] >= 0xDC00 &&
        text[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00);
      i += 2;
    } else {
      i += 1;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::vector<unsigned short> utf8_to_utf16(std::string const& text) {
  std::vector<unsigned short> out;
  out.reserve(text.size() + 1);
  for (std::size_t i = 0; i < text.size();) {
    unsigned char const c = static_cast<unsigned char>(text[i]);
    unsigned int cp = 0;
    std::size_t extra = 0;
    if (c < 0x80) {
      cp = c;
      extra = 0;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1Fu;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0Fu;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07u;
      extra = 3;
    } else {
      cp = 0xFFFD;
      extra = 0;
    }
    bool invalid = extra > 0;
    for (std::size_t k = 1; k <= extra && i + k < text.size(); ++k) {
      unsigned char const cc = static_cast<unsigned char>(text[i + k]);
      if ((cc & 0xC0) != 0x80) {
        invalid = true;
        break;
      }
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    i += extra + 1;
    if (invalid) cp = 0xFFFD;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<unsigned short>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<unsigned short>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<unsigned short>(cp));
    }
  }
  return out;
}

// ------------------------------------------------------------- documents --

std::unique_ptr<Document> Document::open_file(std::string const& path,
                                              std::string const& password) {
  if (!load()) return nullptr;
  auto doc = std::unique_ptr<Document>(new Document());
  doc->handle_ = g_api.FPDF_LoadDocument(path.c_str(), password.c_str());
  if (doc->handle_ == nullptr) {
    doc->last_error_ = g_api.FPDF_GetLastError();
    return nullptr;
  }
  doc->page_count_ = g_api.FPDF_GetPageCount(doc->handle_);
  return doc;
}

std::unique_ptr<Document> Document::open_memory(std::uint8_t const* data,
                                                std::size_t size,
                                                std::string const& password) {
  if (!load() || data == nullptr || size == 0) return nullptr;
  auto doc = std::unique_ptr<Document>(new Document());
  doc->handle_ = g_api.FPDF_LoadMemDocument(data, size, password.c_str());
  if (doc->handle_ == nullptr) {
    doc->last_error_ = g_api.FPDF_GetLastError();
    return nullptr;
  }
  doc->page_count_ = g_api.FPDF_GetPageCount(doc->handle_);
  return doc;
}

// Per-document form fill state (defined here so ~Document sees the complete
// type). The FPDF_FORMFILLINFO must outlive the handle; ~Document exits the
// environment before releasing this.
struct Document::FormState {
  ::FPDF_FORMFILLINFO info{};
  ::FPDF_FORMHANDLE handle{nullptr};
};

Document::~Document() {
  // The form fill environment must exit before the document closes and its
  // FPDF_FORMFILLINFO (owned by form_state_) must outlive the handle.
  if (form_state_ != nullptr && g_form.complete) {
    if (form_state_->handle != nullptr) {
      g_form.FPDFDOC_ExitFormFillEnvironment(form_state_->handle);
    }
    form_state_.reset();
  }
  if (handle_ != nullptr) g_api.FPDF_CloseDocument(handle_);
}

bool Document::page_size(int index, double* width, double* height) {
  if (handle_ == nullptr || index < 0 || index >= page_count_) return false;
  FPDF_PAGE page = g_api.FPDF_LoadPage(handle_, index);
  if (page == nullptr) return false;
  // FPDF_GetPageWidthF/HeightF apply the page rotation (see fpdfview.h),
  // matching what render_page draws.
  if (width != nullptr) *width = g_api.FPDF_GetPageWidthF(page);
  if (height != nullptr) *height = g_api.FPDF_GetPageHeightF(page);
  g_api.FPDF_ClosePage(page);
  return true;
}

std::string Document::metadata(char const* tag) {
  if (g_api.FPDF_GetMetaText == nullptr || handle_ == nullptr) return {};
  // First call with no buffer returns the byte count needed (UTF-16LE with
  // the NUL terminator, per fpdf_doc.h).
  unsigned long const needed = g_api.FPDF_GetMetaText(handle_, tag, nullptr, 0);
  if (needed <= 2) return {};  // nothing but the terminator
  std::vector<unsigned short> buffer(needed / 2 + 1, 0);
  unsigned long const got =
      g_api.FPDF_GetMetaText(handle_, tag, buffer.data(), needed);
  if (got == 0) return {};
  std::size_t units = got / 2;
  while (units > 0 && buffer[units - 1] == 0) --units;  // strip terminator
  return utf16_to_utf8(buffer.data(), units);
}

cairo_surface_t* Document::render_page(int index, double scale, bool dark,
                                       int* out_width_px,
                                       int* out_height_px) {
  if (handle_ == nullptr || index < 0 || index >= page_count_) return nullptr;
  FPDF_PAGE page = g_api.FPDF_LoadPage(handle_, index);
  if (page == nullptr) return nullptr;
  double const width = g_api.FPDF_GetPageWidthF(page);
  double const height = g_api.FPDF_GetPageHeightF(page);
  int const px_width = std::max(1, static_cast<int>(width * scale));
  int const px_height = std::max(1, static_cast<int>(height * scale));
  cairo_surface_t* surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_width, px_height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    g_api.FPDF_ClosePage(page);
    return nullptr;
  }
  // PDFium draws BGRA premultiplied; on little-endian that is byte-identical
  // to cairo's ARGB32, so the render buffer is shared, never copied.
  void* const data = cairo_image_surface_get_data(surface);
  int const stride = cairo_image_surface_get_stride(surface);
  FPDF_BITMAP bitmap =
      g_api.FPDFBitmap_CreateEx(px_width, px_height, kBitmapFormatBgra, data,
                                stride);
  if (bitmap == nullptr) {
    cairo_surface_destroy(surface);
    g_api.FPDF_ClosePage(page);
    return nullptr;
  }
  if (!dark) {
    // White background first: PDFs without a painted background otherwise
    // render translucent and show the window color through. Dark mode skips
    // the fill on purpose: unpainted margins stay alpha 0 so the negative
    // filter below can map them to the dark page background.
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_destroy(cr);
  }
  g_api.FPDF_RenderPageBitmap(bitmap, page, /*start_x=*/0, /*start_y=*/0,
                              px_width, px_height, /*rotate=*/0,
                              /*flags=*/0);
  g_api.FPDFBitmap_Destroy(bitmap);
  if (dark) {
    // PDF.js-style negative filter (semantics of windows/PdfDocument.cpp):
    // invert painted RGB, map the unpainted fill to the dark page background.
    // cairo ARGB32 is premultiplied, so partial-coverage pixels (antialiasing
    // edges) are un-premultiplied first to keep their true color.
    constexpr uint8_t kDarkB = 0x14, kDarkG = 0x17, kDarkR = 0x1A;
    cairo_surface_flush(surface);
    for (int y = 0; y < px_height; ++y) {
      uint8_t* row = static_cast<uint8_t*>(data) +
                     static_cast<std::size_t>(y) * stride;
      for (int x = 0; x < px_width; ++x) {
        uint8_t* px = row + static_cast<std::size_t>(x) * 4;
        uint8_t const alpha = px[3];
        if (alpha == 0) {
          px[0] = kDarkB;
          px[1] = kDarkG;
          px[2] = kDarkR;
        } else if (alpha != 0xFF) {
          px[0] = static_cast<uint8_t>(0xFF - px[0] * 0xFF / alpha);
          px[1] = static_cast<uint8_t>(0xFF - px[1] * 0xFF / alpha);
          px[2] = static_cast<uint8_t>(0xFF - px[2] * 0xFF / alpha);
        } else {
          px[0] = static_cast<uint8_t>(0xFF - px[0]);
          px[1] = static_cast<uint8_t>(0xFF - px[1]);
          px[2] = static_cast<uint8_t>(0xFF - px[2]);
        }
        px[3] = 0xFF;
      }
    }
  }
  cairo_surface_mark_dirty(surface);
  g_api.FPDF_ClosePage(page);
  if (out_width_px != nullptr) *out_width_px = px_width;
  if (out_height_px != nullptr) *out_height_px = px_height;
  return surface;
}

// ----------------------------------------------------------------- forms --

namespace {

// Version-1 embedder surface. PDFium guards most callbacks per call site,
// but the stable v1 members get real no-op bodies: the host fills fields
// programmatically and never draws PWL widgets, so the only entries with
// behavior are SetTimer (caret blink of a focused text field — a fake id
// means no blink and no crash) and GetLocalTime (zeros; only JS reads it).
// Types are the ::-qualified vendored-header spellings so every signature
// matches the FPDF_FORMFILLINFO member pointer types exactly.
void ffi_release(::FPDF_FORMFILLINFO*) {}
void ffi_invalidate(::FPDF_FORMFILLINFO*, ::FPDF_PAGE, double, double, double,
                    double) {}
void ffi_output_selected_rect(::FPDF_FORMFILLINFO*, ::FPDF_PAGE, double,
                              double, double, double) {}
void ffi_set_cursor(::FPDF_FORMFILLINFO*, int) {}
int ffi_set_timer(::FPDF_FORMFILLINFO*, int, ::TimerCallback) { return 1; }
void ffi_kill_timer(::FPDF_FORMFILLINFO*, int) {}
::FPDF_SYSTEMTIME ffi_get_local_time(::FPDF_FORMFILLINFO*) { return {}; }
void ffi_on_change(::FPDF_FORMFILLINFO*) {}
::FPDF_PAGE ffi_get_page(::FPDF_FORMFILLINFO*, ::FPDF_DOCUMENT, int) {
  return nullptr;
}
::FPDF_PAGE ffi_get_current_page(::FPDF_FORMFILLINFO*, ::FPDF_DOCUMENT) {
  return nullptr;
}
int ffi_get_rotation(::FPDF_FORMFILLINFO*, ::FPDF_PAGE) { return 0; }
void ffi_execute_named_action(::FPDF_FORMFILLINFO*, ::FPDF_BYTESTRING) {}
void ffi_set_text_field_focus(::FPDF_FORMFILLINFO*, ::FPDF_WIDESTRING,
                              ::FPDF_DWORD, ::FPDF_BOOL) {}
void ffi_do_uri_action(::FPDF_FORMFILLINFO*, ::FPDF_BYTESTRING) {}

// Loads a page with the form fill notifications PDFium requires around form
// operations (FORM_OnAfterLoadPage / FORM_OnBeforeClosePage). The page
// handle crosses between the void*-based core Api and the header-typed form
// Api, hence the two casts.
struct FormPageScope {
  FormPageScope(FPDF_DOCUMENT doc, ::FPDF_FORMHANDLE handle, int index)
      : handle(handle) {
    page = static_cast<::FPDF_PAGE>(g_api.FPDF_LoadPage(doc, index));
    if (page != nullptr && handle != nullptr) {
      g_form.FORM_OnAfterLoadPage(page, handle);
    }
  }
  ~FormPageScope() {
    if (page == nullptr) return;
    if (handle != nullptr) g_form.FORM_OnBeforeClosePage(page, handle);
    g_api.FPDF_ClosePage(page);
  }
  ::FPDF_FORMHANDLE handle;
  ::FPDF_PAGE page{nullptr};
};

// -1 when the widget is not a fillable field (pushbuttons, signatures…).
int classify_field_kind(int pdfium_type) {
  switch (pdfium_type) {
    case FPDF_FORMFIELD_TEXTFIELD:
      return static_cast<int>(FormFieldInfo::Kind::Text);
    case FPDF_FORMFIELD_CHECKBOX:
    case FPDF_FORMFIELD_RADIOBUTTON:
      return static_cast<int>(FormFieldInfo::Kind::Check);
    case FPDF_FORMFIELD_COMBOBOX:
    case FPDF_FORMFIELD_LISTBOX:
      return static_cast<int>(FormFieldInfo::Kind::Choice);
    default:
      return -1;
  }
}

// UTF-16LE getter with the PDFium size-then-fill protocol (length includes
// the NUL; 0 = error, 2 = empty string).
template <typename Getter>
std::string utf16_via(Getter&& getter) {
  unsigned long const needed = getter(nullptr, 0);
  if (needed <= 2) return {};
  std::vector<unsigned short> buffer(needed / 2 + 2, 0);
  unsigned long const written =
      getter(buffer.data(), static_cast<unsigned long>((buffer.size() - 1) * 2));
  if (written <= 2) return {};
  return utf16_to_utf8(buffer.data(), written / 2 - 1);
}

std::string committed_value(FPDF_FORMHANDLE handle, FPDF_ANNOTATION annot) {
  return utf16_via([&](FPDF_WCHAR* buffer, unsigned long length) {
    return g_form.FPDFAnnot_GetFormFieldValue(handle, annot, buffer, length);
  });
}

std::string option_label(FPDF_FORMHANDLE handle, FPDF_ANNOTATION annot,
                         int index) {
  return utf16_via([&](FPDF_WCHAR* buffer, unsigned long length) {
    return g_form.FPDFAnnot_GetOptionLabel(handle, annot, index, buffer,
                                           length);
  });
}

// Fallback writer for the annotation-dictionary path: sets the field value
// "V" on the widget dictionary directly. Only works for merged widget/field
// dictionaries; the caller verifies what actually committed.
bool annot_set_value(FPDF_ANNOTATION annot, std::string const& value) {
  auto wide = utf8_to_utf16(value);
  wide.push_back(0);
  return g_form.FPDFAnnot_SetStringValue(
      annot, "V", reinterpret_cast<FPDF_WIDESTRING>(wide.data()));
}

}  // namespace

void* Document::ensure_form_env() {
  if (form_state_ != nullptr) return form_state_->handle;
  if (!g_form.complete || handle_ == nullptr) return nullptr;
  form_state_ = std::make_unique<FormState>();
  form_state_->info.version = 1;
  form_state_->info.Release = &ffi_release;
  form_state_->info.FFI_Invalidate = &ffi_invalidate;
  form_state_->info.FFI_OutputSelectedRect = &ffi_output_selected_rect;
  form_state_->info.FFI_SetCursor = &ffi_set_cursor;
  form_state_->info.FFI_SetTimer = &ffi_set_timer;
  form_state_->info.FFI_KillTimer = &ffi_kill_timer;
  form_state_->info.FFI_GetLocalTime = &ffi_get_local_time;
  form_state_->info.FFI_OnChange = &ffi_on_change;
  form_state_->info.FFI_GetPage = &ffi_get_page;
  form_state_->info.FFI_GetCurrentPage = &ffi_get_current_page;
  form_state_->info.FFI_GetRotation = &ffi_get_rotation;
  form_state_->info.FFI_ExecuteNamedAction = &ffi_execute_named_action;
  form_state_->info.FFI_SetTextFieldFocus = &ffi_set_text_field_focus;
  form_state_->info.FFI_DoURIAction = &ffi_do_uri_action;
  form_state_->handle = g_form.FPDFDOC_InitFormFillEnvironment(
      static_cast<::FPDF_DOCUMENT>(handle_), &form_state_->info);
  if (form_state_->handle == nullptr) {
    form_state_.reset();
    return nullptr;
  }
  return form_state_->handle;
}

std::vector<FormFieldInfo> Document::form_fields() {
  std::vector<FormFieldInfo> out;
  auto const handle = static_cast<FPDF_FORMHANDLE>(ensure_form_env());
  if (handle == nullptr) return out;
  std::set<std::string> seen;  // one entry per field name (mac parity)
  for (int p = 0; p < page_count_; ++p) {
    FormPageScope scope(handle_, handle, p);
    if (scope.page == nullptr) continue;
    int const count = g_form.FPDFPage_GetAnnotCount(scope.page);
    for (int i = 0; i < count; ++i) {
      FPDF_ANNOTATION annot = g_form.FPDFPage_GetAnnot(scope.page, i);
      if (annot == nullptr) continue;
      FormFieldInfo info;
      info.page = p;
      info.annot_index = i;
      int const kind = classify_field_kind(
          g_form.FPDFAnnot_GetFormFieldType(handle, annot));
      if (kind < 0) {
        g_form.FPDFPage_CloseAnnot(annot);
        continue;
      }
      info.kind = static_cast<FormFieldInfo::Kind>(kind);
      info.name = utf16_via([&](FPDF_WCHAR* buffer, unsigned long length) {
        return g_form.FPDFAnnot_GetFormFieldName(handle, annot, buffer,
                                                 length);
      });
      if (!info.name.empty() && seen.insert(info.name).second) {
        info.value = committed_value(handle, annot);
        info.checked = g_form.FPDFAnnot_IsChecked(handle, annot) != 0;
        if (info.kind == FormFieldInfo::Kind::Choice) {
          int const options = g_form.FPDFAnnot_GetOptionCount(handle, annot);
          for (int o = 0; o < options; ++o) {
            info.options.push_back(option_label(handle, annot, o));
          }
        }
        out.push_back(std::move(info));
      }
      g_form.FPDFPage_CloseAnnot(annot);
    }
  }
  return out;
}

bool Document::form_set_text(int page, int annot_index,
                             std::string const& value) {
  auto const handle = static_cast<FPDF_FORMHANDLE>(ensure_form_env());
  if (handle == nullptr) return false;
  FormPageScope scope(handle_, handle, page);
  if (scope.page == nullptr) return false;
  FPDF_ANNOTATION annot = g_form.FPDFPage_GetAnnot(scope.page, annot_index);
  if (annot == nullptr) return false;
  // Fill path: focus the widget, select all, replace, then commit by
  // killing focus (PDFium writes the value into the field dictionary).
  if (g_form.FORM_SetFocusedAnnot(handle, annot)) {
    if (g_form.FORM_SelectAllText(handle, scope.page)) {
      auto wide = utf8_to_utf16(value);
      wide.push_back(0);
      g_form.FORM_ReplaceSelection(handle, scope.page,
                                   reinterpret_cast<FPDF_WIDESTRING>(
                                       wide.data()));
    }
    g_form.FORM_ForceToKillFocus(handle);
  }
  bool ok = committed_value(handle, annot) == value;
  if (!ok) {
    // Fallback: write "V" on the widget dictionary (merged field/widget
    // dictionaries only). Verified again — the report is honest about what
    // actually committed.
    annot_set_value(annot, value);
    ok = committed_value(handle, annot) == value;
  }
  g_form.FPDFPage_CloseAnnot(annot);
  return ok;
}

bool Document::form_set_checked(int page, int annot_index, bool on) {
  auto const handle = static_cast<FPDF_FORMHANDLE>(ensure_form_env());
  if (handle == nullptr) return false;
  FormPageScope scope(handle_, handle, page);
  if (scope.page == nullptr) return false;
  FPDF_ANNOTATION annot = g_form.FPDFPage_GetAnnot(scope.page, annot_index);
  if (annot == nullptr) return false;
  auto const is_checked = [&] {
    return g_form.FPDFAnnot_IsChecked(handle, annot) != 0;
  };
  if (is_checked() != on) {
    // Click the widget center (PDF user space) — the mouse path is what
    // radio groups and check boxes both commit through.
    FS_RECTF rect{};
    if (g_form.FPDFAnnot_GetRect(annot, &rect)) {
      double const x = (rect.left + rect.right) / 2.0;
      double const y = (rect.top + rect.bottom) / 2.0;
      g_form.FORM_OnLButtonDown(handle, scope.page, 0, x, y);
      g_form.FORM_OnLButtonUp(handle, scope.page, 0, x, y);
      g_form.FORM_ForceToKillFocus(handle);
    }
  }
  bool ok = is_checked() == on;
  if (!ok) {
    // Fallback: write the export ("on") state name, or "Off", into "V".
    std::string const export_value = utf16_via(
        [&](FPDF_WCHAR* buffer, unsigned long length) {
          return g_form.FPDFAnnot_GetFormFieldExportValue(handle, annot,
                                                          buffer, length);
        });
    annot_set_value(annot, on ? (export_value.empty() ? "Yes" : export_value)
                              : "Off");
    ok = is_checked() == on;
  }
  g_form.FPDFPage_CloseAnnot(annot);
  return ok;
}

bool Document::form_set_choice(int page, int annot_index,
                               std::string const& value) {
  auto const handle = static_cast<FPDF_FORMHANDLE>(ensure_form_env());
  if (handle == nullptr) return false;
  FormPageScope scope(handle_, handle, page);
  if (scope.page == nullptr) return false;
  FPDF_ANNOTATION annot = g_form.FPDFPage_GetAnnot(scope.page, annot_index);
  if (annot == nullptr) return false;
  // Choice path: select the option whose label matches by index
  // (FORM_SetIndexSelected works on the focused annotation).
  int const options = g_form.FPDFAnnot_GetOptionCount(handle, annot);
  int found = -1;
  for (int o = 0; o < options; ++o) {
    if (option_label(handle, annot, o) == value) {
      found = o;
      break;
    }
  }
  if (found >= 0 && g_form.FORM_SetFocusedAnnot(handle, annot)) {
    g_form.FORM_SetIndexSelected(handle, scope.page, found, 1);
    g_form.FORM_ForceToKillFocus(handle);
  }
  bool ok = committed_value(handle, annot) == value;
  if (!ok) {
    annot_set_value(annot, value);
    ok = committed_value(handle, annot) == value;
  }
  g_form.FPDFPage_CloseAnnot(annot);
  return ok;
}

bool Document::form_save(std::string const& path, std::string* error) {
  if (handle_ == nullptr || !g_form.complete) {
    if (error != nullptr) *error = "form support unavailable";
    return false;
  }
  (void)ensure_form_env();  // keep the environment alive across the save
  struct FileWrite : FPDF_FILEWRITE {
    FILE* file;
  } file_write{};
  file_write.version = 1;
  file_write.file = std::fopen(path.c_str(), "wb");
  if (file_write.file == nullptr) {
    if (error != nullptr) *error = "cannot open the target file";
    return false;
  }
  file_write.WriteBlock = +[](FPDF_FILEWRITE* self, void const* data,
                              unsigned long size) -> int {
    auto* writer = reinterpret_cast<FileWrite*>(self);
    return std::fwrite(data, 1, size, writer->file) == size ? 1 : 0;
  };
  bool const ok = g_form.FPDF_SaveAsCopy(
                      static_cast<::FPDF_DOCUMENT>(handle_), &file_write,
                      0) != 0;
  bool const flush_ok = std::fflush(file_write.file) == 0;
  std::fclose(file_write.file);
  if (!ok || !flush_ok) {
    if (error != nullptr) *error = "FPDF_SaveAsCopy failed";
    return false;
  }
  return true;
}

// ------------------------------------------------------------------ text --

std::unique_ptr<TextPage> TextPage::load(void* doc, int page_index) {
  // Only reachable behind an opened Document, so the API is bound already;
  // a plain load() would resolve to this member function anyway.
  if (!g_api.complete || doc == nullptr) return nullptr;
  FPDF_PAGE page = g_api.FPDF_LoadPage(doc, page_index);
  if (page == nullptr) return nullptr;
  FPDF_TEXTPAGE text = g_api.FPDFText_LoadPage(page);
  if (text == nullptr) {
    g_api.FPDF_ClosePage(page);
    return nullptr;
  }
  auto result = std::unique_ptr<TextPage>(new TextPage());
  result->page_ = page;
  result->handle_ = text;
  result->char_count_ = g_api.FPDFText_CountChars(text);
  return result;
}

TextPage::~TextPage() {
  // The text page must close before its page (see fpdf_text.h).
  if (handle_ != nullptr) g_api.FPDFText_ClosePage(handle_);
  if (page_ != nullptr) g_api.FPDF_ClosePage(page_);
}

std::string TextPage::text() const {
  if (handle_ == nullptr || char_count_ <= 0) return {};
  return text_of_chars(0, char_count_ - 1);
}

int TextPage::char_index_at(double x, double y_top_left,
                            double page_height) const {
  if (handle_ == nullptr) return -1;
  return g_api.FPDFText_GetCharIndexAtPos(handle_, x,
                                          page_height - y_top_left, 2.0, 2.0);
}

std::string TextPage::text_of_chars(int start_char, int end_char) const {
  if (handle_ == nullptr || start_char < 0 || end_char < start_char ||
      start_char >= char_count_) {
    return {};
  }
  int const first_text =
      g_api.FPDFText_GetTextIndexFromCharIndex(handle_, start_char);
  int const last_text =
      g_api.FPDFText_GetTextIndexFromCharIndex(handle_, end_char);
  if (first_text < 0 || last_text < first_text) return {};
  int const count = std::min(last_text - first_text + 1, 1 << 20);
  std::vector<unsigned short> buffer(static_cast<std::size_t>(count) + 1, 0);
  int const written =
      g_api.FPDFText_GetText(handle_, first_text, count, buffer.data());
  if (written <= 0) return {};
  return utf16_to_utf8(buffer.data(),
                       static_cast<std::size_t>(std::min(written, count)));
}

std::vector<TextRect> TextPage::rects_of_chars(int start_char,
                                               int end_char) const {
  std::vector<TextRect> out;
  if (handle_ == nullptr || start_char < 0 || end_char < start_char ||
      start_char >= char_count_) {
    return out;
  }
  int const count = std::min(end_char, char_count_ - 1) - start_char + 1;
  int const rects = g_api.FPDFText_CountRects(handle_, start_char, count);
  out.reserve(static_cast<std::size_t>(std::max(rects, 0)));
  for (int i = 0; i < rects; ++i) {
    double left = 0, top = 0, right = 0, bottom = 0;
    if (g_api.FPDFText_GetRect(handle_, i, &left, &top, &right, &bottom)) {
      // FPDFText_GetRect reports PDF user space (bottom-left origin, y-up,
      // top > bottom) — kept as-is; the view layer flips into top-left
      // origin with page_height when painting (see pdfview.cpp).
      out.push_back(TextRect{left, top, right, bottom});
    }
  }
  return out;
}

std::vector<unsigned short> TextPage::text_utf16() const {
  std::vector<unsigned short> out;
  if (handle_ == nullptr || char_count_ <= 0) return out;
  std::vector<unsigned short> buffer(static_cast<std::size_t>(char_count_) + 1,
                                     0);
  int const written =
      g_api.FPDFText_GetText(handle_, 0, char_count_, buffer.data());
  if (written <= 0) return out;
  buffer.resize(static_cast<std::size_t>(std::min(written, char_count_)));
  return buffer;
}

int TextPage::char_from_text_index(int text_index) const {
  if (handle_ == nullptr || text_index < 0) return -1;
  return g_api.FPDFText_GetCharIndexFromTextIndex(handle_, text_index);
}

unsigned int TextPage::unicode_at(int char_index) const {
  if (handle_ == nullptr || char_index < 0 || char_index >= char_count_) {
    return static_cast<unsigned int>(-1);
  }
  return g_api.FPDFText_GetUnicode(handle_, char_index);
}

bool TextPage::is_word_char(unsigned int unicode) {
  if ((unicode & ~0xFFu) != 0) return true;  // non-ASCII: word material
  unsigned char const c = static_cast<unsigned char>(unicode);
  return c == '_' || std::isalnum(c) != 0;
}

// ----------------------------------------------------------------- usage --

std::vector<OutlineItem> outline(void* doc) {
  std::vector<OutlineItem> out;
  if (!load() || doc == nullptr) return out;
  struct Frame {
    FPDF_BOOKMARK node;
    int level;
  };
  std::vector<Frame> stack;
  FPDF_BOOKMARK root = g_api.FPDFBookmark_GetFirstChild(doc, nullptr);
  if (root != nullptr) stack.push_back(Frame{root, 0});
  while (!stack.empty()) {
    Frame const frame = stack.back();
    stack.pop_back();

    OutlineItem item;
    item.level = frame.level;
    item.page = -1;
    // GetTitle wants a byte budget and reports the bytes written (including
    // the terminator). First call sizes, second call fills.
    unsigned long const needed =
        g_api.FPDFBookmark_GetTitle(frame.node, nullptr, 0);
    std::vector<unsigned short> title(needed / 2 + 2, 0);
    if (needed > 0) {
      g_api.FPDFBookmark_GetTitle(
          frame.node, title.data(),
          static_cast<unsigned long>((title.size() - 1) * 2));
    }
    unsigned short const* begin = title.data();
    unsigned long units = 0;
    while (units + 1 < title.size() && begin[units] != 0) ++units;
    item.title = utf16_to_utf8(begin, units);

    FPDF_DEST const dest = g_api.FPDFBookmark_GetDest(doc, frame.node);
    if (dest != nullptr) {
      item.page = g_api.FPDFDest_GetDestPageIndex(doc, dest);
    }
    out.push_back(std::move(item));

    FPDF_BOOKMARK child = g_api.FPDFBookmark_GetFirstChild(doc, frame.node);
    if (child != nullptr) stack.push_back(Frame{child, frame.level + 1});
    FPDF_BOOKMARK sibling = g_api.FPDFBookmark_GetNextSibling(frame.node);
    if (sibling != nullptr) stack.push_back(Frame{sibling, frame.level});
  }
  return out;
}

}  // namespace pdfium

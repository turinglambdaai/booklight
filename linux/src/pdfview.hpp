// PDF reading core: document model, continuous scrolling page view,
// selection, find, outline, thumbnails.
//
// All PDFium calls run on the GTK main thread (PDFium has no per-document
// thread-safety guarantees; the only background work in this host is RPC,
// which never touches a document). The macOS host (PDFKitViews.swift +
// PDFGistModel.swift) is the behavior spec.
#pragma once

#include <cairo/cairo.h>
#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pdfium.hpp"

namespace pdfgist {

// ----------------------------------------------------------- document ----

class PdfDocument {
 public:
  static std::shared_ptr<PdfDocument> open_file(std::string const& path,
                                                std::string const& password,
                                                unsigned long* error);
  static std::shared_ptr<PdfDocument> open_memory(
      std::vector<std::uint8_t> bytes, unsigned long* error);

  int page_count() const { return page_count_; }
  unsigned long last_error() const { return last_error_; }

  // Page size in points, rotation applied. Cached.
  bool page_size(int index, double* width, double* height);

  // Page text layer (cached — selection, find and the AI actions all read).
  std::shared_ptr<pdfium::TextPage> text_page(int index);

  void* handle() const { return impl_ != nullptr ? impl_->handle() : nullptr; }

  // Renders one page at `scale` device pixels per point into a fresh
  // ARGB32 cairo surface (viewer + thumbnails; main thread only). With
  // `dark`, painted pixels invert and margins take the dark page background
  // so pages sit flush in dark mode.
  cairo_surface_t* render(int index, double scale, bool dark,
                          int* out_width_px, int* out_height_px) {
    return impl_ != nullptr
               ? impl_->render_page(index, scale, dark, out_width_px,
                                    out_height_px)
               : nullptr;
  }

 private:
  PdfDocument() = default;
  std::unique_ptr<pdfium::Document> impl_;
  int page_count_{0};
  unsigned long last_error_{0};
  std::vector<std::pair<double, double>> sizes_;
  std::map<int, std::shared_ptr<pdfium::TextPage>> text_pages_;
};

// -------------------------------------------------------------- tab -----

// Text selection across a char range (possibly spanning pages). Empty
// selection = start == end.
struct PdfSelection {
  int start_page = 0;  // 0-based
  int start_char = 0;
  int end_page = 0;
  int end_char = 0;

  bool empty() const {
    return start_page == end_page && start_char == end_char;
  }
};

// One find hit: page + inclusive char range (rectangles derive from the
// page's text layer at highlight time).
struct FindHit {
  int page = 0;
  int start_char = 0;
  int end_char = 0;
};

class PdfTab {
 public:
  PdfTab(std::shared_ptr<PdfDocument> doc, std::string path,
         std::string title);
  ~PdfTab();

  PdfTab(PdfTab const&) = delete;
  PdfTab& operator=(PdfTab const&) = delete;

  std::string const& path() const { return path_; }
  std::string const& title() const { return title_; }
  std::shared_ptr<PdfDocument> document() const { return doc_; }

  // The scrolled container to put into the notebook.
  GtkWidget* widget() const { return scrolled_; }

  int current_page() const { return current_page_; }  // 1-based
  int zoom_percent() const {
    return static_cast<int>(zoom_ * 100.0 + 0.5);
  }
  bool double_mode() const { return double_mode_; }

  // Callbacks owned by main.cpp.
  std::function<void(int page)> on_page_changed;            // 1-based
  std::function<void(bool has_selection)> on_selection_changed;

  void restore_page(int page);  // 1-based resume, before first show
  void go_to_page(int page);    // 1-based
  void set_zoom(double zoom);   // clamped 0.4 .. 4.0
  void zoom_in();
  void zoom_out();
  void zoom_reset();
  void fit_width();
  void set_double_mode(bool on);
  void set_dark(bool on);  // tray background behind pages
  void reload_document(std::shared_ptr<PdfDocument> doc);  // page edit

  std::string selection_text();
  void clear_selection();
  std::string page_text(int page);  // 1-based, "" when empty

  // Find: case-insensitive (ASCII fold) whole-document scan; navigation
  // wraps.
  int run_find(std::string const& query);  // returns hit count
  bool find_next();                        // true when a hit was visited
  bool find_previous();
  void clear_find();
  int find_total() const;
  int find_index() const;  // 0-based, -1 = none

  // Thumbnail texture for the sidebar (rendered on demand, cached).
  GdkTexture* thumbnail(int page);  // 1-based; null when not renderable

  // Right-click on an existing selection (pdfview.cpp invokes it).
  std::function<void()> on_context_menu;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  std::shared_ptr<PdfDocument> doc_;
  std::string path_;
  std::string title_;
  GtkWidget* scrolled_{nullptr};
  int current_page_{1};
  double zoom_{1.0};
  bool double_mode_{false};
};

// --------------------------------------------------------- outline -----

// Flattened outline of a document (empty when the PDF has none).
std::vector<pdfium::OutlineItem> pdf_outline(PdfDocument const& doc);

}  // namespace pdfgist

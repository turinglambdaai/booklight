// EPUB reading over WebKitGTK: the whole book renders as one continuous
// scroll (the v1 epub.ts model the macOS EpubTab.swift keeps). Business —
// parse/sanitize/text — lives in the Racket backend; this module only
// drives presentation.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "GeneratedBackend.hpp"
#include "panels.hpp"

namespace pdfgist {

class EpubTab {
 public:
  EpubTab(PanelContext* ctx, std::string path, std::string title);
  ~EpubTab();

  EpubTab(EpubTab const&) = delete;
  EpubTab& operator=(EpubTab const&) = delete;

  // Destroys the tab safely: flips the shared alive flag so the detached
  // load worker's dispatched completion becomes a no-op.
  void destroy() { *alive_ = false; }

  GtkWidget* widget() const { return root_; }
  std::string const& path() const { return path_; }
  std::string const& title() const { return title_; }

  int chapters() const { return chapters_; }
  int current_page() const { return current_chapter_; }  // chapter, 1-based
  bool loaded() const { return loaded_; }
  std::vector<rivet_app::EpubTocItem> const& toc() const { return toc_; }

  // Last selection relayed from the web view (mouseup bridge).
  std::string const& pending_selection() const { return selection_; }

  // Chapter text from the prefetch cache ("" until loaded).
  std::string chapter_text(int chapter) const;

  void load();  // async serial fetch of the whole book, then one load

  void go_to_chapter(int chapter);
  void apply_font_scale(double scale);
  void apply_theme(std::string const& theme);  // "light" | "dark"

  // Jump to the nth occurrence of `query` (whole-book search): the chapter
  // scroll plus the in-page <mark.epub-hit> highlight.
  void highlight_hit(int chapter, int ordinal, std::string const& query);

  // Callbacks owned by main.cpp.
  std::function<void(int chapter)> on_chapter_changed;      // 1-based
  std::function<void(std::string const&)> on_load_finished;
  std::function<void(std::string const&)> on_load_failed;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  // Shared lifetime flag: the detached load worker and its dispatched
  // completion hold a copy and bail out once the tab is destroyed.
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};

  PanelContext* ctx_;
  std::string path_;
  std::string title_;
  GtkWidget* root_{nullptr};
  GtkWidget* webview_{nullptr};
  int chapters_{0};
  int current_chapter_{1};
  double font_scale_{1.0};
  bool loaded_{false};
  std::vector<rivet_app::EpubTocItem> toc_;
  std::string selection_;
  std::map<int, std::string> text_cache_;
};

}  // namespace pdfgist

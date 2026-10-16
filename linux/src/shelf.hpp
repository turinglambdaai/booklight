// The bookshelf: the launch surface (the notebook's empty state IS the
// shelf). Cover grid over the backend's library catalog, collections
// filter, search, host-side import (hosts own the renderers — PDF page-1
// covers render here through PDFium/cairo) and the context actions
// (collections, reveal, relink, remove).
//
// The macOS host (macos-host/Sources/RivetHost/LibraryView.swift +
// AppModel.swift shelf section) is the behavior spec: catalog state lives
// behind the library RPCs, books are never copied, and opening a book from
// anywhere upserts it into the shelf.
#pragma once

#include <gtk/gtk.h>

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "GeneratedBackend.hpp"

namespace pdfgist {

class Shelf {
 public:
  // Host wiring (main.cpp owns the real implementations).
  struct Hooks {
    std::function<void(std::string const&)> open;  // open for reading
    std::function<void(std::string const&)> alert;
    std::function<rivet_app::API*()> api;          // null until backend ready
    std::function<std::vector<rivet_app::RecentEntry>()> recents;
    std::function<GtkWindow*()> window;
  };

  explicit Shelf(Hooks hooks);
  ~Shelf();

  Shelf(Shelf const&) = delete;
  Shelf& operator=(Shelf const&) = delete;

  // The stack page holding grid / empty state (goes under "shelf" in the
  // window's main stack).
  GtkWidget* page() const { return page_; }

  // Header-bar widgets (packed by main.cpp; visibility follows set_active).
  GtkWidget* header_import() const { return header_import_; }
  GtkWidget* header_spinner() const { return header_spinner_; }
  GtkWidget* header_filter() const { return header_filter_; }
  GtkWidget* header_search() const { return header_search_; }

  // Shelf is the active surface (nothing open in the notebook). Toggles the
  // shelf-only header controls.
  void set_active(bool active);

  // Pulls get-library through the backend and rebuilds the grid. Safe to
  // call any time; no-ops before the backend is ready.
  void refresh();

  // Background missing-status refresh (mac bootstrap parity: the shelf shows
  // whatever is known the moment it is on screen, then stat-freshens).
  void rescan();

  // Import files into the shelf (queued, sequential; failures skip to the
  // next book). Queued work waits for the backend to come up, so calling
  // before ready is fine.
  void import_paths(std::vector<std::string> paths);

  // One shared upsert used by every open path (the open of a book grows the
  // shelf — mac importOpenedBook/importOpenedEpub parity).
  void import_book(std::string const& path);

 private:
  // ---- view assembly ----
  GtkWidget* build_empty_state();
  void rebuild();
  GtkWidget* build_cell(rivet_app::LibraryEntry const& entry);
  void load_cover(GtkWidget* child);
  void show_context_menu(GtkWidget* child, double x, double y);
  void install_actions();

  // ---- filter state ----
  std::vector<std::string> collections() const;
  std::vector<rivet_app::LibraryEntry const*> visible_entries() const;
  void rebuild_filter_model();
  std::optional<std::pair<int, int>> progress_for(
      rivet_app::LibraryEntry const& entry) const;

  // ---- import pump ----
  void pump();
  void pump_step();
  void import_pdf_step(std::string const& path);
  void import_epub_step(std::string const& path);
  void show_import_chooser();

  // ---- context actions ----
  rivet_app::LibraryEntry const* context_entry() const;
  rivet_app::LibraryEntry const* context_entry_of(
      std::string const& path) const;
  void new_collection_dialog();
  void create_collection_and_assign(std::string const& name);
  void relink_chooser();
  void reveal_entry();
  void remove_entry();
  void set_book_collection(std::string const& name, bool add);

  Hooks hooks_;

  // ---- widgets ----
  GtkWidget* page_{nullptr};       // GtkStack: grid | empty | loading
  GtkWidget* grid_scroll_{nullptr};
  GtkFlowBox* flow_{nullptr};
  GtkWidget* empty_state_{nullptr};
  GtkWidget* header_import_{nullptr};
  GtkWidget* header_spinner_{nullptr};
  GtkWidget* header_filter_{nullptr};
  GtkDropDown* dropdown_{nullptr};
  GtkWidget* header_search_{nullptr};
  GtkSearchEntry* search_entry_{nullptr};
  GtkButton* empty_import_{nullptr};

  // ---- model ----
  std::vector<rivet_app::LibraryEntry> library_;
  bool loaded_{false};       // get-library answered at least once
  bool scanned_{false};      // background missing-status rescan done
  std::string filter_;       // "" = all, "unfiled", or a collection name
  std::string search_;       // client-side title/author needle
  bool active_{true};
  bool updating_filter_{false};

  // ---- context menu target (set when the popover opens) ----
  std::string context_path_;
  GtkWidget* context_popover_{nullptr};

  // ---- import queue (sequential, one book per pump step) ----
  std::deque<std::string> queue_;
  bool importing_{false};
  bool pump_scheduled_{false};

  // Window-scoped actions for the context popover ("win.lib-*").
  GtkWindow* window_{nullptr};
};

}  // namespace pdfgist

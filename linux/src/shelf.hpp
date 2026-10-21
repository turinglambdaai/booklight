// The bookshelf, Books arrangement (mac LibraryView.swift parity): a
// source-list sidebar (home / search / smart scopes / collections) over a
// detail area that is either the Home page (continue-reading and
// recently-added cover strips) or the cover grid for the selected scope.
// Books are cataloged in place; nothing here ever copies or moves a file.
//
// The macOS host (macos-host/Sources/RivetHost/LibraryView.swift +
// AppModel.swift shelf section) is the behavior spec: catalog state lives
// behind the library RPCs, books are never copied, and opening a book from
// anywhere upserts it into the shelf.
#pragma once

#include <gtk/gtk.h>

#include <array>
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
    // True while a reader tab owns the window and shows a document (the
    // shelf idling never accrues reading time) and the window is visible.
    std::function<bool()> reading_active;
    // The header-bar title follows the sidebar selection.
    std::function<void()> title_changed;
  };

  explicit Shelf(Hooks hooks);
  ~Shelf();

  Shelf(Shelf const&) = delete;
  Shelf& operator=(Shelf const&) = delete;

  // The whole Books surface (GtkPaned: sidebar | detail) — goes under
  // "shelf" in the window's main stack.
  GtkWidget* page() const { return page_; }

  // Header-bar widgets (packed by main.cpp; visibility follows set_active).
  GtkWidget* header_import() const { return header_import_; }
  GtkWidget* header_spinner() const { return header_spinner_; }
  GtkWidget* header_search() const { return header_search_; }
  // The Books toolbar mini-gauge: today's seconds against the daily goal.
  GtkWidget* header_ring() const { return header_ring_; }

  // The sidebar selection as a header-bar title (mac navigationTitle).
  std::string current_title() const;

  // Shelf is the active surface (nothing open in the notebook). Toggles the
  // shelf-only header controls.
  void set_active(bool active);

  // Pulls get-library through the backend and rebuilds the detail. Safe to
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
  // ---- sidebar (Books source list) ----
  GtkWidget* build_sidebar();
  void rebuild_sidebar();
  GtkWidget* make_nav_row(char const* icon, std::string const& label,
                          std::string const& nav);
  GtkWidget* make_header_row(std::string const& label);
  void append_collection_rows();
  void select_row_for(std::string const& nav);
  void on_sidebar_selection();
  void select_nav(std::string const& nav);

  // ---- detail (home | grid | empty | loading) ----
  GtkWidget* build_home();
  GtkWidget* build_goals_card();
  GtkWidget* build_header_ring();
  void rebuild_home_strips();
  void update_reading_widgets();
  void open_last_read_book();

  GtkWidget* build_empty_state();
  void rebuild();
  GtkWidget* build_cell(rivet_app::LibraryEntry const& entry);
  void load_cover(GtkWidget* child);
  void show_context_menu(GtkWidget* child, double x, double y);
  void install_actions();

  // ---- reading-time ledger (live seconds toward the daily goal; mac
  // AppModel reading section parity) ----
  void refresh_reading_stats();
  void tick_reading_second();

  // ---- scope state ----
  std::vector<std::string> collections() const;
  std::vector<rivet_app::LibraryEntry const*> visible_entries() const;
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
  void run_new_collection_dialog(
      std::function<void(std::string const&)> on_ok);
  void create_collection_and_assign(std::string const& name);
  void create_collection_only(std::string const& name);
  void relink_chooser();
  void reveal_entry();
  void remove_entry();
  void set_book_collection(std::string const& name, bool add);

  Hooks hooks_;

  // ---- widgets ----
  GtkWidget* page_{nullptr};        // GtkPaned: sidebar | detail
  GtkWidget* sidebar_scroll_{nullptr};
  GtkListBox* sidebar_{nullptr};
  GtkStack* detail_{nullptr};       // home | grid | empty | loading
  GtkWidget* grid_scroll_{nullptr};
  GtkFlowBox* flow_{nullptr};
  GtkWidget* empty_state_{nullptr};
  GtkWidget* header_import_{nullptr};
  GtkWidget* header_spinner_{nullptr};
  GtkSearchEntry* search_entry_{nullptr};
  GtkWidget* header_search_{nullptr};
  GtkWidget* header_ring_{nullptr};
  GtkButton* empty_import_{nullptr};

  // home page widgets the strips rebuild in place
  GtkWidget* home_scroll_{nullptr};
  GtkWidget* home_title_{nullptr};
  GtkWidget* continue_title_{nullptr};
  GtkWidget* continue_scroll_{nullptr};
  GtkWidget* continue_box_{nullptr};
  GtkWidget* recent_title_{nullptr};
  GtkWidget* recent_scroll_{nullptr};
  GtkWidget* recent_box_{nullptr};

  // goals-card widgets the 1 s tick updates in place
  GtkWidget* gauge_area_{nullptr};
  GtkWidget* mmss_label_{nullptr};
  GtkWidget* goal_badge_{nullptr};
  std::array<GtkWidget*, 7> streak_day_labels_{};
  GtkWidget* streak_text_{nullptr};
  GtkWidget* continue_pill_{nullptr};   // opens the last-read book
  GtkWidget* card_import_{nullptr};     // import button when no recents

  // ---- navigation state ("home" | "all" | "want" | "finished" | "pdf" |
  // "epub" | "c:<collection name>"; the search row acts, never selects) ----
  std::string selection_{"home"};
  bool updating_nav_{false};
  std::vector<std::string> sidebar_collections_;  // rows already built

  // ---- reading-time stats (live seconds, refreshed from host.conf) ----
  int today_seconds_{0};
  std::array<int, 7> week_seconds_{};  // Mon..Sun
  int streak_days_{0};
  guint reading_tick_{0};  // g_timeout_add_seconds(1)

  // ---- model ----
  std::vector<rivet_app::LibraryEntry> library_;
  // The shelf's own collection list (stored order, includes empty ones).
  std::vector<std::string> shelf_collections_;
  // Right-click on a collection row: 删除收藏集 behind one confirmation.
  void delete_collection(std::string const& name);
  // win.col-delete action entry: the popover menu already staged the name.
  void delete_collection_menu() {
    if (!pending_delete_.empty()) delete_collection(pending_delete_);
  }

  // Right-click on a collection row: 删除收藏集 behind one confirmation.
  void show_collection_menu(GtkWidget* row, std::string const& name,
                            double x, double y);
  void apply_delete_collection();
  std::string pending_delete_;
  bool loaded_{false};       // get-library answered at least once
  bool scanned_{false};      // background missing-status rescan done
  std::string search_;       // client-side title/author needle
  bool active_{true};

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

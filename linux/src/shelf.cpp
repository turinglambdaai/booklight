// The bookshelf implementation — see shelf.hpp. The macOS host
// (LibraryView.swift + AppModel.swift shelf section) is the behavior spec:
// a Books source-list sidebar over the home page / cover grid.
//
// Threading: all GTK + PDFium work happens on the main loop. Backend
// completions arrive on the runtime reader thread and dispatch back into
// the main loop before touching widgets (host::dispatch_ui, the whole-host
// convention). Imports run as a sequential queue, one book per pump step,
// so a big drop never wedges the UI and one bad book skips to the next.
#include "shelf.hpp"

#include <cairo/cairo.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "host.hpp"
#include "i18n.h"
#include "pdfium.hpp"
#include "pdfview.hpp"

namespace pdfgist {

// g_signal_connect is a GLib macro; a lambda whose body contains top-level
// commas would break its argument splitting. Route through the real
// g_signal_connect_data function instead (handlers cast to GCallback).
inline gulong hook(gpointer instance, gchar const* detailed_signal,
                   auto&& handler, gpointer data,
                   GClosureNotify destroy_data, GConnectFlags flags) {
  return g_signal_connect_data(instance, detailed_signal,
                               reinterpret_cast<GCallback>(
                                   std::forward<decltype(handler)>(handler)),
                               data, destroy_data, flags);
}

inline gulong hook(gpointer instance, gchar const* detailed_signal,
                   auto&& handler, gpointer data) {
  return hook(instance, detailed_signal, std::forward<decltype(handler)>(handler),
              data, nullptr, static_cast<GConnectFlags>(0));
}

namespace {

// Cover cell geometry (mac BookCoverView: 128x172 cover, 140 column).
constexpr int kCoverWidth = 132;
constexpr int kCoverHeight = 176;
constexpr int kCoverPxWidth = 320;  // import cover render width (mac parity)

// Recently-added strip size (most recent 12 books, added_at descending).
constexpr std::size_t kRecentLimit = 12;

std::string ascii_lower(std::string const& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return out;
}

bool has_extension(std::string const& path, char const* extension) {
  std::string const lower = ascii_lower(path);
  std::string const tail = ascii_lower(extension);
  return lower.size() >= tail.size() &&
         lower.compare(lower.size() - tail.size(), tail.size(), tail) == 0;
}

std::string path_stem(std::string const& path) {
  std::filesystem::path const p(path);
  std::string stem = p.stem().string();
  if (stem.empty()) stem = p.filename().string();
  return stem;
}

std::string trim(std::string const& s) {
  auto const begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  auto const end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

// Page-1 render as PNG bytes for the shelf cover cache (mac coverPNG:
// width 320, hosts own the renderers). Empty when nothing renders.
rivet::Bytes cover_png(PdfDocument* doc) {
  double width_pts = 0;
  if (!doc->page_size(0, &width_pts, nullptr) || width_pts <= 0) return {};
  cairo_surface_t* surface = doc->render(0, kCoverPxWidth / width_pts, false,
                                         nullptr, nullptr);
  if (surface == nullptr) return {};
  rivet::Bytes bytes;
  cairo_status_t const rc = cairo_surface_write_to_png_stream(
      surface,
      +[](void* closure, unsigned char const* data,
          unsigned int length) -> cairo_status_t {
        auto* out = static_cast<rivet::Bytes*>(closure);
        out->insert(out->end(), data, data + length);
        return CAIRO_STATUS_SUCCESS;
      },
      &bytes);
  cairo_surface_destroy(surface);
  if (rc != CAIRO_STATUS_SUCCESS) return {};
  return bytes;
}

// The cover-card palette. The gradient + scrim live in CSS so the grid
// stays theme-cooperative (the shelf never needs custom snapshot code).
constexpr char const* kShelfCss =
    ".book-cover-frame { border-radius: 6px; }\n"
    ".book-cover-empty { background-image: linear-gradient(to bottom, "
    "alpha(#3584e4, 0.18), alpha(#3584e4, 0.06)); }\n"
    ".book-cover-scrim { background-color: rgba(0, 0, 0, 0.55); "
    "color: white; }\n"
    ".book-title { font-size: 12px; font-weight: 600; }\n"
    ".book-author { font-size: 11px; }\n"
    ".book-progress-text { font-size: 10px; }\n"
    // Home page typography (Books-style big title + strip headers)
    ".home-digest-title { font-size: 34px; font-weight: 700; }\n"
    ".shelf-section-title { font-size: 17px; font-weight: 600; }\n"
    ".shelf-header-row { font-size: 11px; font-weight: 700; "
    "color: alpha(@theme_fg_color, 0.6); padding: 2px 8px 1px; }\n"
    ".shelf-nav-row { padding: 5px 8px; }\n";

}  // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

Shelf::Shelf(Hooks hooks) : hooks_(std::move(hooks)) {
  window_ = hooks_.window();

  // --- header controls (packed into the window header bar by main.cpp) ---
  header_import_ = gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_set_tooltip_text(header_import_, Tr("ui.library.import").c_str());
  hook(
      header_import_, "clicked",
      +[](GtkButton*, gpointer data) {
        static_cast<Shelf*>(data)->show_import_chooser();
      },
      this);

  header_spinner_ = gtk_spinner_new();
  gtk_widget_set_visible(header_spinner_, FALSE);

  search_entry_ = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_search_entry_set_placeholder_text(search_entry_,
                                        Tr("ui.library.search").c_str());
  gtk_widget_set_size_request(GTK_WIDGET(search_entry_), 190, -1);
  header_search_ = GTK_WIDGET(search_entry_);
  hook(
      search_entry_, "search-changed",
      +[](GtkSearchEntry*, gpointer data) {
        auto* self = static_cast<Shelf*>(data);
        self->search_ =
            gtk_editable_get_text(GTK_EDITABLE(self->search_entry_));
        self->rebuild();
      },
      this);

  // --- page: sidebar | detail(home | grid | empty | loading) ---
  page_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

  grid_scroll_ = gtk_scrolled_window_new();
  flow_ = GTK_FLOW_BOX(gtk_flow_box_new());
  gtk_flow_box_set_max_children_per_line(flow_, 8);
  gtk_flow_box_set_selection_mode(flow_, GTK_SELECTION_SINGLE);
  gtk_flow_box_set_column_spacing(flow_, 22);
  gtk_flow_box_set_row_spacing(flow_, 26);
  gtk_widget_set_margin_start(GTK_WIDGET(flow_), 32);
  gtk_widget_set_margin_end(GTK_WIDGET(flow_), 32);
  gtk_widget_set_margin_top(GTK_WIDGET(flow_), 28);
  gtk_widget_set_margin_bottom(GTK_WIDGET(flow_), 28);
  gtk_widget_set_valign(GTK_WIDGET(flow_), GTK_ALIGN_START);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(grid_scroll_),
                                GTK_WIDGET(flow_));
  hook(
      flow_, "child-activated",
      +[](GtkFlowBox*, GtkFlowBoxChild* child, gpointer data) {
        auto* self = static_cast<Shelf*>(data);
        auto const* path = static_cast<char const*>(
            g_object_get_data(G_OBJECT(child), "shelf-path"));
        if (path == nullptr) return;
        auto const* entry = self->context_entry_of(path);
        if (entry == nullptr || entry->status == "missing") return;
        self->hooks_.open(path);
      },
      this);
  empty_state_ = build_empty_state();

  detail_ = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(detail_, GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_stack_add_named(detail_, build_home(), "home");
  gtk_stack_add_named(detail_, grid_scroll_, "grid");
  gtk_stack_add_named(detail_, empty_state_, "empty");
  auto* loading = gtk_label_new(Tr("ui.update.checking").c_str());
  gtk_widget_add_css_class(loading, "dim-label");
  gtk_widget_add_css_class(loading, "caption");
  gtk_widget_set_valign(loading, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(loading, GTK_ALIGN_CENTER);
  gtk_stack_add_named(detail_, loading, "loading");

  gtk_paned_set_start_child(GTK_PANED(page_), build_sidebar());
  gtk_paned_set_shrink_start_child(GTK_PANED(page_), FALSE);
  gtk_paned_set_end_child(GTK_PANED(page_), GTK_WIDGET(detail_));
  gtk_paned_set_position(GTK_PANED(page_), 200);

  // Rounded covers + gradient placeholders + missing scrim.
  auto* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kShelfCss);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  install_actions();
  set_active(active_);
}

Shelf::~Shelf() = default;

// ---------------------------------------------------------------------------
// sidebar (mac shelfSidebar parity — Books source list)
// ---------------------------------------------------------------------------

GtkWidget* Shelf::build_sidebar() {
  sidebar_scroll_ = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll_),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(sidebar_scroll_, 200, -1);
  sidebar_ = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(sidebar_, GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(sidebar_, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(sidebar_), "navigation-sidebar");
  gtk_widget_add_css_class(GTK_WIDGET(sidebar_), "shelf-nav");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll_),
                                GTK_WIDGET(sidebar_));
  hook(
      sidebar_, "selected-rows-changed",
      +[](GtkListBox*, gpointer data) {
        static_cast<Shelf*>(data)->on_sidebar_selection();
      },
      this);
  rebuild_sidebar();
  return sidebar_scroll_;
}

GtkWidget* Shelf::make_nav_row(char const* icon, std::string const& label,
                               std::string const& nav) {
  auto* row = gtk_list_box_row_new();
  auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(box, "shelf-nav-row");
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);
  auto* image = gtk_image_new_from_icon_name(icon);
  gtk_image_set_pixel_size(GTK_IMAGE(image), 16);
  auto* text = gtk_label_new(label.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_box_append(GTK_BOX(box), image);
  gtk_box_append(GTK_BOX(box), text);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  char* owned = g_strdup(nav.c_str());
  g_object_set_data_full(G_OBJECT(row), "shelf-nav", owned, g_free);
  return row;
}

GtkWidget* Shelf::make_header_row(std::string const& label) {
  auto* row = gtk_list_box_row_new();
  auto* text = gtk_label_new(label.c_str());
  gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
  gtk_widget_add_css_class(text, "shelf-header-row");
  gtk_widget_set_margin_start(text, 8);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), text);
  gtk_widget_set_sensitive(row, FALSE);  // headers never select
  return row;
}

void Shelf::append_collection_rows() {
  for (auto const& name : collections()) {
    gtk_list_box_append(
        sidebar_, make_nav_row("folder-symbolic", name, "c:" + name));
  }
  gtk_list_box_append(sidebar_,
                      make_nav_row("list-add-symbolic",
                                   Tr("ui.shelf.new-collection"),
                                   "new-collection"));
}

void Shelf::rebuild_sidebar() {
  auto const cols = collections();
  if (cols == sidebar_collections_ &&
      gtk_widget_get_first_child(GTK_WIDGET(sidebar_)) != nullptr) {
    select_row_for(selection_);  // rows are fresh; just restore the selection
    return;
  }
  updating_nav_ = true;
  gtk_list_box_remove_all(sidebar_);
  gtk_list_box_append(sidebar_, make_nav_row("system-search-symbolic",
                                             Tr("ui.shelf.search"), "search"));
  gtk_list_box_append(sidebar_,
                      make_nav_row("user-home-symbolic", Tr("ui.shelf.home"),
                                   "home"));
  gtk_list_box_append(sidebar_, make_header_row(Tr("ui.shelf.lib")));
  gtk_list_box_append(sidebar_,
                      make_nav_row("view-grid-symbolic", Tr("ui.shelf.all"),
                                   "all"));
  gtk_list_box_append(sidebar_,
                      make_nav_row("bookmark-new-symbolic", Tr("ui.shelf.want"),
                                   "want"));
  gtk_list_box_append(
      sidebar_, make_nav_row("object-select-symbolic", Tr("ui.shelf.finished"),
                             "finished"));
  gtk_list_box_append(
      sidebar_, make_nav_row("text-x-generic-symbolic", Tr("ui.shelf.pdf"),
                             "pdf"));
  gtk_list_box_append(sidebar_,
                      make_nav_row("accessories-dictionary-symbolic",
                                   Tr("ui.shelf.epub"), "epub"));
  gtk_list_box_append(sidebar_, make_header_row(Tr("ui.shelf.mine")));
  append_collection_rows();
  sidebar_collections_ = cols;
  select_row_for(selection_);
  updating_nav_ = false;
}

void Shelf::select_row_for(std::string const& nav) {
  updating_nav_ = true;
  int index = 0;
  GtkListBoxRow* row = nullptr;
  while ((row = gtk_list_box_get_row_at_index(sidebar_, index++)) != nullptr) {
    auto const* row_nav = static_cast<char const*>(
        g_object_get_data(G_OBJECT(row), "shelf-nav"));
    if (row_nav != nullptr && nav == row_nav) {
      gtk_list_box_select_row(sidebar_, row);
      break;
    }
  }
  updating_nav_ = false;
}

void Shelf::on_sidebar_selection() {
  if (updating_nav_) return;
  auto* row = gtk_list_box_get_selected_row(sidebar_);
  if (row == nullptr) return;
  auto const* nav = static_cast<char const*>(
      g_object_get_data(G_OBJECT(row), "shelf-nav"));
  if (nav == nullptr) return;
  std::string const target = nav;
  if (target == "search") {
    // the search row selects the all-scope and focuses its field
    select_nav("all");
    gtk_widget_grab_focus(header_search_);
    return;
  }
  if (target == "new-collection") {
    // action row: restore the semantic selection, then open the sheet
    select_row_for(selection_);
    run_new_collection_dialog([this](std::string const& name) {
      create_collection_only(name);
    });
    return;
  }
  select_nav(target);
}

void Shelf::select_nav(std::string const& nav) {
  if (selection_ == nav) {
    select_row_for(nav);  // e.g. search clicked twice: move the highlight back
    return;
  }
  selection_ = nav;
  rebuild();
  if (hooks_.title_changed) hooks_.title_changed();
}

std::string Shelf::current_title() const {
  if (selection_ == "home") return Tr("ui.shelf.home");
  if (selection_ == "want") return Tr("ui.shelf.want");
  if (selection_ == "finished") return Tr("ui.shelf.finished");
  if (selection_ == "pdf") return Tr("ui.shelf.pdf");
  if (selection_ == "epub") return Tr("ui.shelf.epub");
  if (selection_.rfind("c:", 0) == 0) return selection_.substr(2);
  return Tr("ui.shelf.all");  // "all" — the search row selects all too
}

// ---------------------------------------------------------------------------
// home page (two cover strips: continue-reading, recently-added)
// ---------------------------------------------------------------------------

namespace {

// Clears every child of a strip's container box (GTK4 has no
// gtk_box_remove_all).
void clear_box(GtkWidget* box) {
  GtkWidget* child = nullptr;
  while ((child = gtk_widget_get_first_child(box)) != nullptr) {
    gtk_box_remove(GTK_BOX(box), child);
  }
}

}  // namespace

GtkWidget* Shelf::build_home() {
  home_scroll_ = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(home_scroll_),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
  gtk_widget_set_margin_top(box, 28);
  gtk_widget_set_margin_bottom(box, 32);
  gtk_widget_set_margin_start(box, 32);
  gtk_widget_set_margin_end(box, 32);

  home_title_ = gtk_label_new(Tr("ui.shelf.home").c_str());
  gtk_label_set_xalign(GTK_LABEL(home_title_), 0.0f);
  gtk_widget_add_css_class(home_title_, "home-digest-title");
  gtk_box_append(GTK_BOX(box), home_title_);

  // strip sections (title + horizontal cover scroll, kept tight)
  auto make_section = [](char const* title, GtkWidget** section_title,
                             GtkWidget** section_scroll,
                             GtkWidget** section_box) {
    auto* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    *section_title = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(*section_title), 0.0f);
    gtk_widget_add_css_class(*section_title, "shelf-section-title");
    gtk_box_append(GTK_BOX(section), *section_title);
    *section_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(*section_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    *section_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 22);
    gtk_widget_set_margin_top(*section_box, 4);
    gtk_widget_set_margin_bottom(*section_box, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(*section_scroll),
                                  *section_box);
    gtk_box_append(GTK_BOX(section), *section_scroll);
    return section;
  };
  gtk_box_append(GTK_BOX(box),
                 make_section(Tr("ui.shelf.continue").c_str(), &continue_title_,
                              &continue_scroll_, &continue_box_));
  gtk_box_append(GTK_BOX(box),
                 make_section(Tr("ui.home.recent").c_str(), &recent_title_,
                              &recent_scroll_, &recent_box_));

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(home_scroll_), box);
  return home_scroll_;
}

void Shelf::rebuild_home_strips() {
  clear_box(continue_box_);
  clear_box(recent_box_);

  // strip cells open on double click (the grid gets that from the flow
  // box's child-activated; strips have no flow box to lean on)
  auto append_cell = [this](GtkWidget* box, GtkWidget* cell) {
    auto* gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                  GDK_BUTTON_PRIMARY);
    hook(
        gesture, "pressed",
        +[](GtkGestureClick* gesture, gint n_press, double, double,
            gpointer data) {
          if (n_press != 2) return;
          auto* self = static_cast<Shelf*>(data);
          auto* cell = gtk_event_controller_get_widget(
              GTK_EVENT_CONTROLLER(gesture));
          auto const* path = static_cast<char const*>(
              g_object_get_data(G_OBJECT(cell), "shelf-path"));
          if (path == nullptr) return;
          auto const* entry = self->context_entry_of(path);
          if (entry == nullptr || entry->status == "missing") return;
          self->hooks_.open(path);
        },
        this);
    gtk_widget_add_controller(cell, GTK_EVENT_CONTROLLER(gesture));
    gtk_box_append(GTK_BOX(box), cell);
  };

  // continue-reading: everything opened but not finished (mac continueRow)
  bool any_reading = false;
  for (auto const& entry : library_) {
    auto const progress = progress_for(entry);
    if (!progress.has_value() || progress->first >= progress->second) continue;
    append_cell(continue_box_, build_cell(entry));
    any_reading = true;
  }

  // recently added: the latest 12 cataloged books, added_at descending
  // (path tiebreak keeps the order stable across rebuilds)
  std::vector<rivet_app::LibraryEntry const*> recent;
  recent.reserve(library_.size());
  for (auto const& entry : library_) recent.push_back(&entry);
  std::sort(recent.begin(), recent.end(),
            [](auto const* a, auto const* b) {
              if (a->added_at_ms != b->added_at_ms) {
                return a->added_at_ms > b->added_at_ms;
              }
              return a->path < b->path;
            });
  bool any_recent = false;
  for (std::size_t i = 0;
       i < recent.size() && i < kRecentLimit; ++i) {
    append_cell(recent_box_, build_cell(*recent[i]));
    any_recent = true;
  }

  gtk_widget_set_visible(continue_title_, any_reading);
  gtk_widget_set_visible(continue_scroll_, any_reading);
  gtk_widget_set_visible(recent_title_, any_recent);
  gtk_widget_set_visible(recent_scroll_, any_recent);
}

// ---------------------------------------------------------------------------
// model helpers (mac shelfCollections / shelfEntries / shelfProgress parity)
// ---------------------------------------------------------------------------

std::vector<std::string> Shelf::collections() const {
  std::vector<std::string> out;
  for (auto const& entry : library_) {
    for (auto const& name : entry.collections) {
      if (std::find(out.begin(), out.end(), name) == out.end()) {
        out.push_back(name);
      }
    }
  }
  return out;
}

std::vector<rivet_app::LibraryEntry const*> Shelf::visible_entries() const {
  std::vector<rivet_app::LibraryEntry const*> rows;
  rows.reserve(library_.size());
  for (auto const& entry : library_) {
    // scope semantics (mac shelfEntries switch parity)
    if (selection_ == "want") {
      if (progress_for(entry).has_value()) continue;  // never opened
    } else if (selection_ == "finished") {
      auto const progress = progress_for(entry);
      if (!progress.has_value() || progress->first < progress->second) {
        continue;
      }
    } else if (selection_ == "pdf" || selection_ == "epub") {
      if (entry.kind != selection_) continue;
    } else if (selection_.rfind("c:", 0) == 0) {
      std::string const name = selection_.substr(2);
      if (std::find(entry.collections.begin(), entry.collections.end(),
                    name) == entry.collections.end()) {
        continue;
      }
    }
    std::string const needle = trim(search_);
    if (!needle.empty()) {
      std::string const lower = ascii_lower(needle);
      if (ascii_lower(entry.title).find(lower) == std::string::npos &&
          ascii_lower(entry.author).find(lower) == std::string::npos) {
        continue;
      }
    }
    rows.push_back(&entry);
  }
  return rows;
}

std::optional<std::pair<int, int>> Shelf::progress_for(
    rivet_app::LibraryEntry const& entry) const {
  auto const recents = hooks_.recents();
  for (auto const& recent : recents) {
    if (recent.path != entry.path) continue;
    if (recent.page <= 1 && entry.pages <= 0) return std::nullopt;
    return std::make_pair(static_cast<int>(recent.page),
                          static_cast<int>(std::max<std::int64_t>(entry.pages,
                                                                  1)));
  }
  return std::nullopt;
}

rivet_app::LibraryEntry const* Shelf::context_entry() const {
  return context_entry_of(context_path_);
}

rivet_app::LibraryEntry const* Shelf::context_entry_of(
    std::string const& path) const {
  for (auto const& entry : library_) {
    if (entry.path == path) return &entry;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// shelf surface
// ---------------------------------------------------------------------------

void Shelf::set_active(bool active) {
  active_ = active;
  gtk_widget_set_visible(header_import_, active);
  gtk_widget_set_visible(header_spinner_, active && importing_);
  gtk_widget_set_visible(header_search_, active);
}

GtkWidget* Shelf::build_empty_state() {
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  auto* icon = gtk_image_new_from_icon_name("folder-documents-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 44);
  auto* title = gtk_label_new(Tr("ui.library.empty").c_str());
  gtk_widget_add_css_class(title, "title");
  auto* hint = gtk_label_new(Tr("ui.library.empty-hint").c_str());
  gtk_widget_add_css_class(hint, "dim-label");
  empty_import_ = GTK_BUTTON(gtk_button_new_with_label(
      Tr("ui.library.import").c_str()));
  gtk_widget_add_css_class(GTK_WIDGET(empty_import_), "pill");
  hook(
      empty_import_, "clicked",
      +[](GtkButton*, gpointer data) {
        static_cast<Shelf*>(data)->show_import_chooser();
      },
      this);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), hint);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(empty_import_));
  return box;
}

void Shelf::rebuild() {
  rebuild_sidebar();
  gtk_flow_box_remove_all(flow_);
  auto const rows = visible_entries();
  for (auto const* entry : rows) {
    gtk_flow_box_append(flow_, build_cell(*entry));
  }
  char const* visible = "grid";
  if (selection_ == "home") {
    // the home page shows immediately (the strips do not wait for the
    // library); both re-home on each rebuild
    rebuild_home_strips();
    visible = "home";
  } else if (!loaded_) {
    visible = "loading";
  } else if (rows.empty()) {
    gtk_widget_set_sensitive(GTK_WIDGET(empty_import_),
                             hooks_.api() != nullptr);
    visible = "empty";
  }
  gtk_stack_set_visible_child_name(detail_, visible);
}

GtkWidget* Shelf::build_cell(rivet_app::LibraryEntry const& entry) {
  bool const missing = entry.status == "missing";

  auto* child = gtk_flow_box_child_new();
  {
    char* path = g_strdup(entry.path.c_str());
    g_object_set_data_full(G_OBJECT(child), "shelf-path", path, g_free);
    if (!entry.cover.empty()) {
      char* cover = g_strdup(entry.cover.c_str());
      g_object_set_data_full(G_OBJECT(child), "shelf-cover", cover, g_free);
    }
    gtk_widget_set_tooltip_text(child, (missing ? Tr("ui.library.missing")
                                                : entry.title)
                                          .c_str());
  }

  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), box);

  // Cover: gradient placeholder at the bottom, the (lazily loaded) texture
  // above it, the missing scrim on top — clipped to one rounded frame.
  auto* frame = gtk_overlay_new();
  gtk_widget_set_size_request(frame, kCoverWidth, kCoverHeight);
  gtk_widget_set_overflow(frame, GTK_OVERFLOW_HIDDEN);
  gtk_widget_add_css_class(frame, "book-cover-frame");

  // The tinted gradient covers the whole cover; the icon + title sit
  // centered inside it (mac no-cover art parity).
  auto* placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(placeholder, TRUE);
  gtk_widget_set_vexpand(placeholder, TRUE);
  gtk_widget_add_css_class(placeholder, "book-cover-empty");
  auto* placeholder_center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign(placeholder_center, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(placeholder_center, GTK_ALIGN_CENTER);
  auto* kind_icon = gtk_image_new_from_icon_name(
      entry.kind == "epub" ? "x-office-document-symbolic"
                           : "text-x-generic-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(kind_icon), 34);
  gtk_widget_add_css_class(kind_icon, "dim-label");
  auto* cover_title = gtk_label_new(entry.title.c_str());
  gtk_label_set_wrap(GTK_LABEL(cover_title), TRUE);
  gtk_label_set_lines(GTK_LABEL(cover_title), 3);
  gtk_label_set_justify(GTK_LABEL(cover_title), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(cover_title, "book-author");
  gtk_widget_set_size_request(cover_title, kCoverWidth - 20, -1);
  gtk_box_append(GTK_BOX(placeholder_center), kind_icon);
  gtk_box_append(GTK_BOX(placeholder_center), cover_title);
  gtk_box_append(GTK_BOX(placeholder), placeholder_center);
  gtk_overlay_set_child(GTK_OVERLAY(frame), placeholder);

  if (!entry.cover.empty()) {
    auto* picture = gtk_picture_new();
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_hexpand(picture, TRUE);
    gtk_widget_set_vexpand(picture, TRUE);
    g_object_set_data(G_OBJECT(child), "shelf-picture", picture);
    gtk_overlay_add_overlay(GTK_OVERLAY(frame), picture);
    // Covers load lazily when the cell maps (the headless smoke must not
    // block startup; visible cells fetch their PNG in one small read).
    hook(
        child, "map",
        +[](GtkWidget* widget, gpointer data) {
          static_cast<Shelf*>(data)->load_cover(widget);
        },
        this);
  }

  if (missing) {
    // The scrim paints over the whole cover; the icon + label sit centered.
    auto* scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(scrim, TRUE);
    gtk_widget_set_vexpand(scrim, TRUE);
    gtk_widget_add_css_class(scrim, "book-cover-scrim");
    auto* scrim_center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(scrim_center, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(scrim_center, GTK_ALIGN_CENTER);
    auto* icon = gtk_image_new_from_icon_name("dialog-question-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
    auto* label = gtk_label_new(Tr("ui.library.missing").c_str());
    gtk_widget_add_css_class(label, "book-author");
    gtk_box_append(GTK_BOX(scrim_center), icon);
    gtk_box_append(GTK_BOX(scrim_center), label);
    gtk_box_append(GTK_BOX(scrim), scrim_center);
    gtk_overlay_add_overlay(GTK_OVERLAY(frame), scrim);
  }
  gtk_box_append(GTK_BOX(box), frame);

  auto* title = gtk_label_new(entry.title.c_str());
  gtk_label_set_wrap(GTK_LABEL(title), TRUE);
  gtk_label_set_lines(GTK_LABEL(title), 2);
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "book-title");
  gtk_widget_set_sensitive(title, missing ? FALSE : TRUE);
  gtk_box_append(GTK_BOX(box), title);

  if (!entry.author.empty()) {
    auto* author = gtk_label_new(entry.author.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(author), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(author), 0.0f);
    gtk_widget_add_css_class(author, "book-author");
    gtk_widget_add_css_class(author, "dim-label");
    gtk_box_append(GTK_BOX(box), author);
  }

  if (auto const progress = progress_for(entry); progress.has_value()) {
    auto const [page, of] = *progress;
    auto* bar = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(bar),
        std::clamp(static_cast<double>(page) /
                       static_cast<double>(std::max(of, 1)),
                   0.0, 1.0));
    gtk_box_append(GTK_BOX(box), bar);
    auto* caption = gtk_label_new(
        Trf(entry.kind == "epub" ? "ui.library.chapter-progress"
                                 : "ui.library.progress",
            std::to_string(page), std::to_string(of))
            .c_str());
    gtk_label_set_ellipsize(GTK_LABEL(caption), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(caption), 0.0f);
    gtk_widget_add_css_class(caption, "book-progress-text");
    gtk_widget_add_css_class(caption, "dim-label");
    gtk_box_append(GTK_BOX(box), caption);
  }

  // Right-click context menu (secondary-button click gesture per cell).
  auto* gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                GDK_BUTTON_SECONDARY);
  hook(
      gesture, "pressed",
      +[](GtkGestureClick* g, gint, double x, double y, gpointer data) {
        auto* self = static_cast<Shelf*>(data);
        auto* target = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
        self->show_context_menu(target, x, y);
      },
      this);
  gtk_widget_add_controller(GTK_WIDGET(child),
                            GTK_EVENT_CONTROLLER(gesture));

  return child;
}

void Shelf::load_cover(GtkWidget* child) {
  if (g_object_get_data(G_OBJECT(child), "shelf-cover-loaded") != nullptr) {
    return;
  }
  auto const* path = static_cast<char const*>(
      g_object_get_data(G_OBJECT(child), "shelf-cover"));
  if (path == nullptr || *path == '\0') return;
  g_object_set_data(G_OBJECT(child), "shelf-cover-loaded",
                    GINT_TO_POINTER(1));

  GFile* file = g_file_new_for_path(path);
  GError* error = nullptr;
  GdkTexture* texture = gdk_texture_new_from_file(file, &error);
  g_object_unref(file);
  if (error != nullptr) {
    g_error_free(error);
  }
  if (texture == nullptr) return;
  auto* picture = GTK_PICTURE(
      g_object_get_data(G_OBJECT(child), "shelf-picture"));
  if (picture != nullptr) {
    gtk_picture_set_paintable(picture, GDK_PAINTABLE(texture));
  }
  g_object_unref(texture);
}

// ---------------------------------------------------------------------------
// refresh (mac loadLibrary + the bootstrap background rescan)
// ---------------------------------------------------------------------------

void Shelf::refresh() {
  auto* api = hooks_.api();
  if (api == nullptr) return;
  (void)api->get_library_async(
      [this](rivet_app::Result<std::vector<rivet_app::LibraryEntry>> result) {
        host::dispatch_ui([this, result]() mutable {
          if (!result.succeeded()) return;  // keep whatever is on screen
          library_ = result.get();
          loaded_ = true;
          rebuild();
          if (!scanned_) {
            scanned_ = true;
            rescan();
          }
        });
      });
}

void Shelf::rescan() {
  auto* api = hooks_.api();
  if (api == nullptr) return;
  (void)api->library_scan_async(
      [this](rivet_app::Result<std::vector<rivet_app::LibraryEntry>> result) {
        host::dispatch_ui([this, result]() mutable {
          if (!result.succeeded()) return;
          library_ = result.get();
          rebuild();
        });
      });
}

// ---------------------------------------------------------------------------
// import (mac importBooks / importOpenedBook / importOpenedEpub parity)
// ---------------------------------------------------------------------------

void Shelf::import_paths(std::vector<std::string> paths) {
  for (auto& path : paths) {
    if (has_extension(path, ".pdf") || has_extension(path, ".epub")) {
      queue_.push_back(std::move(path));
    }
  }
  if (queue_.empty()) return;
  if (!importing_) {
    importing_ = true;
    gtk_widget_set_visible(header_spinner_, active_);
    gtk_spinner_start(GTK_SPINNER(header_spinner_));
  }
  pump();
}

void Shelf::import_book(std::string const& path) {
  import_paths({path});
}

void Shelf::pump() {
  if (pump_scheduled_) return;
  pump_scheduled_ = true;
  g_timeout_add(
      60,
      +[](gpointer data) -> int {
        auto* self = static_cast<Shelf*>(data);
        self->pump_scheduled_ = false;
        self->pump_step();
        return G_SOURCE_REMOVE;
      },
      this);
}

void Shelf::pump_step() {
  auto* api = hooks_.api();
  if (api == nullptr) {
    pump();  // backend still booting — keep the queue warm
    return;
  }
  if (queue_.empty()) {
    if (importing_) {
      importing_ = false;
      gtk_spinner_stop(GTK_SPINNER(header_spinner_));
      gtk_widget_set_visible(header_spinner_, FALSE);
      refresh();
    }
    return;
  }
  std::string path = std::move(queue_.front());
  queue_.pop_front();
  if (has_extension(path, ".epub")) {
    import_epub_step(path);
  } else {
    import_pdf_step(path);
  }
}

void Shelf::import_pdf_step(std::string const& path) {
  if (!pdfium::load()) {
    pump();  // no renderer available: skip this book, keep the queue going
    return;
  }
  unsigned long error = 0;
  auto doc = PdfDocument::open_file(path, "", &error);
  if (doc == nullptr) {
    pump();  // unreadable or password-protected: skip (mac parity)
    return;
  }
  std::string title = doc->metadata("Title");
  if (trim(title).empty()) title = path_stem(path);
  std::string const author = doc->metadata("Author");
  auto* api = hooks_.api();
  if (api == nullptr) {
    pump();
    return;
  }
  (void)api->library_import_async(
      path, title, author, "pdf", std::max(doc->page_count(), 1),
      cover_png(doc.get()),
      [this](rivet_app::Result<rivet_app::LibraryEntry>) {
        host::dispatch_ui([this]() mutable { pump(); });
      });
}

void Shelf::import_epub_step(std::string const& path) {
  auto* api = hooks_.api();
  if (api == nullptr) {
    pump();
    return;
  }
  (void)api->epub_open_async(
      path, [this, path](rivet_app::Result<rivet_app::EpubView> result) {
        host::dispatch_ui([this, path, result]() mutable {
          if (!result.succeeded()) {
            pump();  // one bad book never blocks the rest of the import
            return;
          }
          rivet_app::EpubView const view = result.get();
          auto* api2 = hooks_.api();
          if (api2 == nullptr) return;
          (void)api2->library_import_async(
              path, view.title, std::string{}, "epub", view.chapters,
              rivet::Bytes{},
              [this](rivet_app::Result<rivet_app::LibraryEntry>) {
                host::dispatch_ui([this]() mutable { pump(); });
              });
        });
      });
}

void Shelf::show_import_chooser() {
  auto* chooser = gtk_file_chooser_native_new(
      Tr("ui.library.import").c_str(), window_, GTK_FILE_CHOOSER_ACTION_OPEN,
      Tr("ui.dialog.open").c_str(), Tr("ui.password.cancel").c_str());
  auto* filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, Tr("ui.filter.documents").c_str());
  gtk_file_filter_add_mime_type(filter, "application/pdf");
  gtk_file_filter_add_mime_type(filter, "application/epub+zip");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);
  hook(
      chooser, "response",
      +[](GtkNativeDialog* native, int response, gpointer data) {
        auto* self = static_cast<Shelf*>(data);
        if (response == GTK_RESPONSE_ACCEPT) {
          auto* files =
              gtk_file_chooser_get_files(GTK_FILE_CHOOSER(native));
          std::vector<std::string> paths;
          if (files != nullptr) {
            guint const count = g_list_model_get_n_items(files);
            for (guint i = 0; i < count; ++i) {
              GFile* file = G_FILE(g_list_model_get_item(files, i));
              if (file == nullptr) continue;
              gchar* path = g_file_get_path(file);
              if (path != nullptr) {
                paths.emplace_back(path);
                g_free(path);
              }
              g_object_unref(file);
            }
            g_object_unref(files);
          }
          if (!paths.empty()) self->import_paths(std::move(paths));
        }
        g_object_unref(native);
      },
      this, nullptr, static_cast<GConnectFlags>(0));
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

// ---------------------------------------------------------------------------
// context menu (mac BookCoverView.contextMenu parity)
// ---------------------------------------------------------------------------

void Shelf::install_actions() {
  if (window_ == nullptr) return;
  auto add = [&](char const* name, auto&& handler, gpointer user_data,
                 char const* param = nullptr) {
    auto* action =
        param != nullptr
            ? g_simple_action_new(name, G_VARIANT_TYPE(param))
            : g_simple_action_new(name, nullptr);
    hook(action, "activate", handler, user_data);
    g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(action));
    g_object_unref(action);
  };
  add("lib-new-collection",
      +[](GSimpleAction*, GVariant*, gpointer data) {
        auto* self = static_cast<Shelf*>(data);
        self->run_new_collection_dialog([self](std::string const& name) {
          self->create_collection_and_assign(name);
        });
      }, this);
  add("lib-add-to",
      +[](GSimpleAction*, GVariant* value, gpointer data) {
        if (value != nullptr) {
          static_cast<Shelf*>(data)->set_book_collection(
              g_variant_get_string(value, nullptr), true);
        }
      }, this, "s");
  add("lib-remove-from",
      +[](GSimpleAction*, GVariant* value, gpointer data) {
        if (value != nullptr) {
          static_cast<Shelf*>(data)->set_book_collection(
              g_variant_get_string(value, nullptr), false);
        }
      }, this, "s");
  add("lib-reveal",
      +[](GSimpleAction*, GVariant*, gpointer data) {
        static_cast<Shelf*>(data)->reveal_entry();
      }, this);
  add("lib-relink",
      +[](GSimpleAction*, GVariant*, gpointer data) {
        static_cast<Shelf*>(data)->relink_chooser();
      }, this);
  add("lib-remove",
      +[](GSimpleAction*, GVariant*, gpointer data) {
        static_cast<Shelf*>(data)->remove_entry();
      }, this);
}

void Shelf::show_context_menu(GtkWidget* child, double x, double y) {
  auto const* path = static_cast<char const*>(
      g_object_get_data(G_OBJECT(child), "shelf-path"));
  if (path == nullptr) return;
  context_path_ = path;
  auto const* entry = context_entry();
  if (entry == nullptr) return;
  bool const missing = entry->status == "missing";

  // Reveal stays disabled for missing files (mac parity).
  if (auto* reveal = G_SIMPLE_ACTION(g_action_map_lookup_action(
          G_ACTION_MAP(window_), "lib-reveal"));
      reveal != nullptr) {
    g_simple_action_set_enabled(reveal, missing ? FALSE : TRUE);
  }

  auto* menu = g_menu_new();
  if (missing) {
    auto* section = g_menu_new();
    g_menu_append(section, Tr("ui.library.relink").c_str(), "win.lib-relink");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
  }
  {
    auto* submenu = g_menu_new();
    g_menu_append(submenu, Tr("ui.library.new-collection").c_str(),
                  "win.lib-new-collection");
    auto const cols = collections();
    if (!cols.empty()) {
      for (auto const& name : cols) {
        g_menu_append(submenu, name.c_str(),
                      ("win.lib-add-to::" + name).c_str());
      }
    }
    g_menu_append_submenu(menu, Tr("ui.library.add-to").c_str(),
                          G_MENU_MODEL(submenu));
    g_object_unref(submenu);
  }
  if (!entry->collections.empty()) {
    auto* submenu = g_menu_new();
    for (auto const& name : entry->collections) {
      g_menu_append(submenu, name.c_str(),
                    ("win.lib-remove-from::" + name).c_str());
    }
    g_menu_append_submenu(menu, Tr("ui.library.remove-from").c_str(),
                          G_MENU_MODEL(submenu));
    g_object_unref(submenu);
  }
  {
    auto* section = g_menu_new();
    g_menu_append(section, Tr("ui.library.reveal").c_str(), "win.lib-reveal");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
  }
  {
    auto* section = g_menu_new();
    g_menu_append(section, Tr("ui.library.remove").c_str(), "win.lib-remove");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
  }

  if (context_popover_ != nullptr) {
    gtk_widget_unparent(context_popover_);
    context_popover_ = nullptr;
  }
  context_popover_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  // The slot must self-clear whatever kills the popover: the unparent
  // above, a menu action, or a grid rebuild while it is open (the flow
  // child owns it and dies with the grid).
  hook(
      context_popover_, "destroy",
      +[](GtkWidget*, gpointer data) {
        auto* slot = static_cast<GtkWidget**>(data);
        *slot = nullptr;
      },
      &context_popover_);
  gtk_widget_set_parent(context_popover_, child);
  GdkRectangle rect{static_cast<int>(x), static_cast<int>(y), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(context_popover_), &rect);
  gtk_popover_popup(GTK_POPOVER(context_popover_));
}

// ---------------------------------------------------------------------------
// context actions
// ---------------------------------------------------------------------------

void Shelf::set_book_collection(std::string const& name, bool add) {
  auto* api = hooks_.api();
  if (api == nullptr || context_path_.empty() || name.empty()) return;
  std::string const path = context_path_;
  (void)api->library_book_collection_async(
      path, name, add,
      [this](rivet_app::Result<void>) {
        // the refresh below reflects the persisted state
        host::dispatch_ui([this]() mutable { refresh(); });
      });
}

void Shelf::create_collection_and_assign(std::string const& name) {
  auto* api = hooks_.api();
  if (api == nullptr || context_path_.empty() || name.empty()) return;
  std::string const path = context_path_;
  (void)api->library_collection_create_async(
      name, [this, path, name](rivet_app::Result<void>) {
        host::dispatch_ui([this, path, name]() mutable {
          auto* api2 = hooks_.api();
          if (api2 == nullptr) return;
          (void)api2->library_book_collection_async(
              path, name, true,
              [this](rivet_app::Result<void>) {
                host::dispatch_ui([this]() mutable { refresh(); });
              });
        });
      });
}

void Shelf::create_collection_only(std::string const& name) {
  auto* api = hooks_.api();
  if (api == nullptr || name.empty()) return;
  (void)api->library_collection_create_async(
      name, [this, name](rivet_app::Result<void>) {
        host::dispatch_ui([this, name]() mutable {
          // the sidebar rows list only collections books belong to (mac
          // shelfCollections parity) — an empty one shows as an empty grid
          // until a book joins
          selection_ = "c:" + name;
          refresh();
          if (hooks_.title_changed) hooks_.title_changed();
        });
      });
}

void Shelf::run_new_collection_dialog(
    std::function<void(std::string const&)> on_ok) {
  if (window_ == nullptr) return;
  auto* dialog = gtk_dialog_new_with_buttons(
      Tr("ui.library.new-collection").c_str(), window_,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      Tr("ui.password.cancel").c_str(), GTK_RESPONSE_CANCEL,
      Tr("ui.password.ok").c_str(), GTK_RESPONSE_OK, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_box_append(GTK_BOX(area), box);
  auto* entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                 Tr("ui.library.new-collection").c_str());
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_box_append(GTK_BOX(box), entry);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  auto* handler = new std::function<void(std::string const&)>(std::move(on_ok));
  g_object_set_data_full(G_OBJECT(dialog), "shelf-on-ok", handler,
                         +[](gpointer data) {
                           delete static_cast<std::function<
                               void(std::string const&)>*>(data);
                         });
  // The entry dies with the dialog; the response fires before destroy.
  hook(
      dialog, "response",
      +[](GtkDialog* dialog, int response, gpointer user_data) {
        auto* entry = GTK_ENTRY(user_data);
        auto* self = static_cast<Shelf*>(
            g_object_get_data(G_OBJECT(dialog), "shelf-owner"));
        auto* on_ok = static_cast<std::function<void(std::string const&)>*>(
            g_object_get_data(G_OBJECT(dialog), "shelf-on-ok"));
        if (response == GTK_RESPONSE_OK && self != nullptr && on_ok != nullptr) {
          std::string const name =
              trim(gtk_editable_get_text(GTK_EDITABLE(entry)));
          if (!name.empty()) (*on_ok)(name);
        }
        gtk_window_destroy(GTK_WINDOW(dialog));
      },
      entry);
  g_object_set_data(G_OBJECT(dialog), "shelf-owner", this);
  gtk_window_present(GTK_WINDOW(dialog));
}

void Shelf::relink_chooser() {
  if (window_ == nullptr || context_path_.empty()) return;
  auto* chooser = gtk_file_chooser_native_new(
      Tr("ui.library.relink").c_str(), window_, GTK_FILE_CHOOSER_ACTION_OPEN,
      Tr("ui.dialog.open").c_str(), Tr("ui.password.cancel").c_str());
  hook(
      chooser, "response",
      +[](GtkNativeDialog* native, int response, gpointer data) {
        auto* self = static_cast<Shelf*>(data);
        if (response == GTK_RESPONSE_ACCEPT) {
          if (GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
              file != nullptr) {
            gchar* path = g_file_get_path(file);
            g_object_unref(file);
            if (path != nullptr) {
              std::string const to = path;
              g_free(path);
              auto* api = self->hooks_.api();
              std::string const from = self->context_path_;
              if (api != nullptr && !from.empty()) {
                (void)api->library_relink_async(
                    from, to,
                    [self](rivet_app::Result<void>) {
                      host::dispatch_ui([self]() mutable {
                        self->refresh();
                      });
                    });
              }
            }
          }
        }
        g_object_unref(native);
      },
      this, nullptr, static_cast<GConnectFlags>(0));
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

void Shelf::reveal_entry() {
  auto const* entry = context_entry();
  if (entry == nullptr || window_ == nullptr) return;
  auto const dir = std::filesystem::path(entry->path).parent_path();
  if (dir.empty()) return;
  GFile* folder = g_file_new_for_path(dir.string().c_str());
  gchar* uri = g_file_get_uri(folder);
  gtk_show_uri(window_, uri, GDK_CURRENT_TIME);
  g_free(uri);
  g_object_unref(folder);
}

void Shelf::remove_entry() {
  auto* api = hooks_.api();
  if (api == nullptr || context_path_.empty()) return;
  std::string const path = context_path_;
  // mac removeFromShelf: optimistic row drop, then persist. Never touches
  // the book file itself.
  library_.erase(
      std::remove_if(library_.begin(), library_.end(),
                     [&](auto const& e) { return e.path == path; }),
      library_.end());
  rebuild();
  (void)api->library_remove_async(
      path, [this](rivet_app::Result<void>) {
        host::dispatch_ui([this]() mutable { refresh(); });
      });
}

}  // namespace pdfgist

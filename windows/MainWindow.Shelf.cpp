#include "pch.h"
#include "MainWindow.xaml.h"
#include "I18n.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <set>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace imaging = winrt::Microsoft::UI::Xaml::Media::Imaging;
namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;
namespace wut = winrt::Windows::UI::Text;
namespace wu = winrt::Windows::UI;
namespace wss = winrt::Windows::Storage::Streams;

using Microsoft::UI::Xaml::Visibility;

namespace {

std::wstring file_name(std::wstring const& path) {
  std::size_t const slash = path.find_last_of(L"/\\");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Filename without its extension — the shelf title fallback when the PDF
// carries no Title in its info dictionary (mac pickAndOpen parity).
std::wstring stem_name(std::wstring const& path) {
  std::wstring name = file_name(path);
  std::size_t const dot = name.find_last_of(L'.');
  if (dot != std::wstring::npos && dot > 0) name.resize(dot);
  return name;
}

std::wstring lower(std::wstring text) {
  for (auto& c : text) c = static_cast<wchar_t>(::towlower(c));
  return text;
}

std::wstring trim(std::wstring text) {
  std::size_t const first = text.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  std::size_t const last = text.find_last_not_of(L" \t\r\n");
  return text.substr(first, last - first + 1);
}

bool has_extension(std::wstring const& path, std::wstring const& ext) {
  auto const tail = lower(path.size() >= ext.size()
                              ? path.substr(path.size() - ext.size())
                              : path);
  return tail == lower(ext);
}

media::SolidColorBrush Brush(wu::Color color) {
  media::SolidColorBrush brush;
  brush.Color(color);
  return brush;
}

// mac Theme colors for the cover placeholders (Theme.swift), resolved per
// scheme so the shelf sits flush in both modes.
wu::Color AccentSoftColor(bool dark) {
  return dark ? wu::Color{0xFF, 0x2D, 0x22, 0x18} : wu::Color{0xFF, 0xFF, 0xF7, 0xED};
}
wu::Color PanelAltColor(bool dark) {
  return dark ? wu::Color{0xFF, 0x2A, 0x26, 0x24} : wu::Color{0xFF, 0xFA, 0xF8, 0xF5};
}
wu::Color TextDimColor(bool dark) {
  return dark ? wu::Color{0xFF, 0xA8, 0xA2, 0x9E} : wu::Color{0xFF, 0x57, 0x53, 0x4E};
}
wu::Color TextColor(bool dark) {
  return dark ? wu::Color{0xFF, 0xE7, 0xE5, 0xE4} : wu::Color{0xFF, 0x1C, 0x19, 0x17};
}
// The Booklight accent (duplicate of MainWindow.xaml.cpp's file-local
// helper — every part file keeps its own small color table).
wu::Color AccentColor() { return wu::Color{0xFF, 0xB4, 0x53, 0x09}; }
// Color.primary parity: the goals-card fills read as ink on the surface
// (black in light, white in dark) rather than accent orange (mac 581c0e2).
wu::Color PrimaryColor(bool dark) {
  return dark ? wu::Color{0xFF, 0xFF, 0xFF, 0xFF} : wu::Color{0xFF, 0x00, 0x00, 0x00};
}

// ---- day stamps (device-local second buckets) ---------------------------------------
// yyyy-MM-dd keys carried with a serial UTC day number. The conversion is
// anchored at noon before going through UTC math, so DST transitions can
// never skew the ±1-day arithmetic (mac ISO-8601 calendar parity).

struct DayStamp {
  std::wstring key;
  long long serial = 0;
  int weekday = 0;  // 0 = Sunday (tm_wday convention)
};

long long SerialFromTm(struct tm const& parts) {
  struct tm copy = parts;
  std::time_t const t = ::_mkgmtime(&copy);
  return t < 0 ? 0 : static_cast<long long>(t / 86400);
}

DayStamp DayAt(long long serial) {
  std::time_t const t = static_cast<std::time_t>(serial * 86400);
  struct tm out{};
  ::_gmtime64_s(&out, &t);
  wchar_t buffer[16]{};
  swprintf_s(buffer, L"%04d-%02d-%02d", out.tm_year + 1900, out.tm_mon + 1,
             out.tm_mday);
  return {std::wstring(buffer), serial, out.tm_wday};
}

DayStamp Today() {
  SYSTEMTIME st{};
  ::GetLocalTime(&st);
  struct tm parts{};
  parts.tm_year = st.wYear - 1900;
  parts.tm_mon = st.wMonth - 1;
  parts.tm_mday = st.wDay;
  parts.tm_hour = 12;  // noon anchor
  return DayAt(SerialFromTm(parts));
}

// ---- gauge arc geometry ---------------------------------------------------------------

// Top-half arc starting at the left horizon sweeping right; `fraction` is
// relative to the half-circle (mac SemiGauge parity, drawn with a WinUI
// Path/ArcSegment — no composition renderer needed).
media::Geometry SemiArcGeometry(double fraction) {
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kCenterX = 140.0;
  constexpr double kCenterY = 140.0;
  constexpr double kRadius = 115.0;

  media::ArcSegment segment;
  double const angle = kPi * (1.0 - std::clamp(fraction, 0.0, 1.0));  // π → 0
  // Point members are float; the cos/sin results need explicit narrowing.
  segment.Point({static_cast<float>(kCenterX + kRadius * std::cos(angle)),
                 static_cast<float>(kCenterY - kRadius * std::sin(angle))});
  segment.Size({kRadius, kRadius});
  segment.RotationAngle(0);
  segment.IsLargeArc(false);
  segment.SweepDirection(media::SweepDirection::Clockwise);
  media::PathFigure figure;
  figure.IsClosed(false);
  figure.StartPoint({kCenterX - kRadius, kCenterY});
  figure.Segments().Append(segment);
  media::PathFigureCollection figures;
  figures.Append(figure);
  media::PathGeometry geometry;
  geometry.Figures(figures);
  return geometry;
}

// Full-circle progress arc starting at 12 o'clock sweeping clockwise (mac
// todayRing Circle().trim parity). Degenerate at both ends; the caller hides
// the path for fractions outside (0, 1).
media::Geometry RingArcGeometry(double fraction) {
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kCenter = 15.0;
  constexpr double kRadius = 12.0;  // 30px box minus the 2.5 stroke

  double const sweep = 2.0 * kPi * std::clamp(fraction, 0.0, 1.0);
  media::ArcSegment segment;
  segment.Point({static_cast<float>(kCenter + kRadius * std::sin(sweep)),
                 static_cast<float>(kCenter - kRadius * std::cos(sweep))});
  segment.Size({kRadius, kRadius});
  segment.RotationAngle(0);
  segment.IsLargeArc(fraction > 0.5);
  segment.SweepDirection(media::SweepDirection::Clockwise);
  media::PathFigure figure;
  figure.IsClosed(false);
  figure.StartPoint({kCenter, kCenter - kRadius});
  figure.Segments().Append(segment);
  media::PathFigureCollection figures;
  figures.Append(figure);
  media::PathGeometry geometry;
  geometry.Figures(figures);
  return geometry;
}

// Page-1 thumbnail as PNG bytes for the shelf cover cache: at most 320px
// wide (mac coverPNG parity), never scaled past 4x for tiny pages.
rivet::Bytes CoverPng(pdfgist::PdfDocument const& doc) {
  double w = 612, h = 792;
  doc.PageSize(0, &w, &h);
  if (w <= 1 || h <= 1) return {};
  double const scale = std::min(std::min(320.0 / w, 480.0 / h), 4.0);
  auto page = doc.RenderPage(0, scale, false);
  if (page.bgra.empty()) return {};
  return pdfgist::EncodePng(page);
}

}  // namespace

// ---- sidebar (Books arrangement) -----------------------------------------------------

// The NavigationView menu, rebuilt from scratch on every shelf refresh: the
// collections section is derived from the entries (mac shelfCollections
// parity — a collection exists once a book carries it). Search and
// 新建收藏集 are action rows and never hold the selection.
void MainWindow::RebuildShelfNav() {
  bool const dark = Root().ActualTheme() == mux::ElementTheme::Dark;
  shelf_nav_updating_ = true;
  try {
    auto menu = ShelfNav().MenuItems();
    menu.Clear();
    shelf_nav_items_.clear();
    auto add_item = [&](std::wstring const& tag, std::wstring const& label,
                        wchar_t const* glyph, bool selectable) {
      auto item = muxc::NavigationViewItem();
      item.Content(winrt::box_value(label));
      item.Tag(winrt::box_value(winrt::hstring(tag)));
      item.SelectsOnInvoked(selectable);
      muxc::FontIcon icon;
      icon.FontFamily(media::FontFamily(L"Segoe Fluent Icons"));
      icon.FontSize(16);
      icon.Glyph(glyph);
      item.Icon(icon);
      menu.Append(item);
      shelf_nav_items_.emplace(tag, item);
    };
    auto add_header = [&](std::wstring const& label) {
      auto item = muxc::NavigationViewItem();
      item.Content(winrt::box_value(label));
      item.SelectsOnInvoked(false);
      item.FontSize(12);
      item.Foreground(Brush(TextDimColor(dark)));
      menu.Append(item);
    };

    add_item(L"search", pdfgist::Tr("ui.shelf.search"), L"\uE721", false);
    add_item(L"home", pdfgist::Tr("ui.shelf.home"), L"\uE80F", true);
    add_header(pdfgist::Tr("ui.shelf.lib"));
    add_item(L"all", pdfgist::Tr("ui.shelf.all"), L"\uE8F1", true);
    add_item(L"want", pdfgist::Tr("ui.shelf.want"), L"\uE823", true);
    add_item(L"reading", pdfgist::Tr("ui.shelf.reading"), L"\uE736", true);
    add_item(L"finished", pdfgist::Tr("ui.shelf.finished"), L"\uE73E", true);
    add_item(L"pdf", pdfgist::Tr("ui.shelf.pdf"), L"\uE8A5", true);
    add_item(L"epub", pdfgist::Tr("ui.shelf.epub"), L"\uE736", true);
    add_header(pdfgist::Tr("ui.shelf.mine"));
    // The shelf's own list first (stored order, includes empty collections);
    // entry labels only add names the shelf does not know yet.
    std::set<std::string> seen;
    auto add_collection = [&](std::string const& name) {
      if (!seen.insert(name).second) return;
      auto item = muxc::NavigationViewItem();
      item.Content(winrt::box_value(to_wide(name)));
      std::wstring const tag = L"c:" + to_wide(name);
      item.Tag(winrt::box_value(winrt::hstring(tag)));
      muxc::FontIcon icon;
      icon.FontFamily(media::FontFamily(L"Segoe Fluent Icons"));
      icon.FontSize(16);
      icon.Glyph(L"\uE8B7");
      item.Icon(icon);
      // Right-click delete (mac context-menu parity): books survive, the
      // grouping goes, behind one confirmation.
      auto flyout = muxc::MenuFlyout();
      auto delete_item = muxc::MenuFlyoutItem();
      delete_item.Text(pdfgist::Tr("ui.shelf.delete-collection"));
      auto const weak = get_weak();
      delete_item.Click([weak, tag](
                            winrt::Windows::Foundation::IInspectable const&,
                            mux::RoutedEventArgs const&) {
        if (auto window = weak.get()) window->ConfirmDeleteCollection(tag);
      });
      flyout.Items().Append(delete_item);
      item.ContextFlyout(flyout);
      menu.Append(item);
      shelf_nav_items_.emplace(tag, item);
    };
    for (auto const& name : library_collections_) add_collection(name);
    for (auto const& entry : library_) {
      for (auto const& name : entry.collections) add_collection(name);
    }
    add_item(L"new-collection", pdfgist::Tr("ui.shelf.new-collection"),
             L"\uE710", false);

    SelectShelfNavItem(shelf_selection_);
  } catch (...) {
  }
  shelf_nav_updating_ = false;
}

// Keep the highlighted row in step with the selection even when the row
// itself does not exist (a freshly created empty collection shows no row
// until a book joins it — mac binding behavior).
void MainWindow::SelectShelfNavItem(std::wstring const& tag) {
  shelf_nav_updating_ = true;
  try {
    auto const found = shelf_nav_items_.find(tag);
    ShelfNav().SelectedItem(found == shelf_nav_items_.end()
                                ? muxc::NavigationViewItem{nullptr}
                                : found->second);
  } catch (...) {
  }
  shelf_nav_updating_ = false;
}

std::wstring MainWindow::ShelfTitleFor(std::wstring const& selection) const {
  if (selection == L"want") return pdfgist::Tr("ui.shelf.want");
  if (selection == L"reading") return pdfgist::Tr("ui.shelf.reading");
  if (selection == L"finished") return pdfgist::Tr("ui.shelf.finished");
  if (selection == L"pdf") return pdfgist::Tr("ui.shelf.pdf");
  if (selection == L"epub") return pdfgist::Tr("ui.shelf.epub");
  if (selection.rfind(L"c:", 0) == 0) return selection.substr(2);
  return pdfgist::Tr("ui.shelf.all");  // all / search (mac titleFor parity)
}

void MainWindow::ShelfNav_ItemInvoked(
    winrt::Windows::Foundation::IInspectable const&,
    muxc::NavigationViewItemInvokedEventArgs const& args) {
  if (shelf_nav_updating_) return;
  try {
    auto const item =
        args.InvokedItemContainer().try_as<muxc::NavigationViewItem>();
    if (!item) return;
    auto const boxed_tag = item.Tag();
    if (boxed_tag == nullptr) return;
    std::wstring const tag = std::wstring(winrt::unbox_value<winrt::hstring>(boxed_tag));
    if (tag == L"search") {
      // the search row selects the all-scope and focuses the field (mac parity)
      shelf_selection_ = L"all";
      ShowShelfDetail();
      ShelfSearch().Focus(mux::FocusState::Programmatic);
      return;
    }
    if (tag == L"new-collection") {
      ShelfCreateCollectionDialog();
      return;
    }
    if (tag.empty()) return;
    shelf_selection_ = tag;
    ShowShelfDetail();
  } catch (...) {
  }
}

// Home digest or the scope grid, plus the per-scope title (mac detail switch).
void MainWindow::ShowShelfDetail() {
  bool const home = shelf_selection_ == L"home";
  HomePane().Visibility(home ? Visibility::Visible : Visibility::Collapsed);
  GridPane().Visibility(home ? Visibility::Collapsed : Visibility::Visible);
  try {
    ShelfNav().Header(home ? winrt::Windows::Foundation::IInspectable{nullptr}
                           : winrt::box_value(ShelfTitleFor(shelf_selection_)));
  } catch (...) {
  }
  UpdateReadingProgress();  // paint the toolbar ring + live gauge now
  if (home) {
    RefreshHomeDigest();
  } else {
    RefreshShelfGrid();
  }
}

// ---- one shelf refresh to rule them all ------------------------------------------------

void MainWindow::RefreshShelf() {
  try {
    RebuildShelfNav();
    ShowShelfDetail();
  } catch (...) {
  }
}

// ---- rows (scope + search) --------------------------------------------------------------

// Reading progress for a shelf row, joined from recents by path (mac
// shelfProgress parity): nullopt = never opened.
std::optional<std::pair<int, int>> MainWindow::ShelfProgress(
    rivet_app::LibraryEntry const& entry) const {
  for (auto const& recent : recents_) {
    if (recent.path == entry.path) {
      if (recent.page > 1 || entry.pages > 0) {
        return std::make_pair(static_cast<int>(recent.page),
                              std::max(1, static_cast<int>(entry.pages)));
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

// The grid rows after the sidebar selection and the search box (mac
// shelfEntries parity).
std::vector<rivet_app::LibraryEntry> MainWindow::ShelfRows() const {
  std::vector<rivet_app::LibraryEntry> rows;
  rows.reserve(library_.size());
  std::wstring const scope = shelf_selection_;
  for (auto const& entry : library_) {
    if (scope == L"want") {
      if (ShelfProgress(entry).has_value()) continue;  // never opened
    } else if (scope == L"reading") {
      auto const progress = ShelfProgress(entry);
      if (!progress || progress->first >= progress->second) continue;
    } else if (scope == L"finished") {
      auto const progress = ShelfProgress(entry);
      if (!progress || progress->first < progress->second) continue;
    } else if (scope == L"pdf" || scope == L"epub") {
      if (entry.kind != to_utf8(scope)) continue;
    } else if (scope.rfind(L"c:", 0) == 0) {
      std::string const name = to_utf8(scope.substr(2));
      if (std::find(entry.collections.begin(), entry.collections.end(),
                    name) == entry.collections.end()) {
        continue;
      }
    }
    rows.push_back(entry);  // "all" (and any unknown scope) takes everything
  }
  std::wstring const needle = lower(trim(shelf_search_));
  if (!needle.empty()) {
    std::vector<rivet_app::LibraryEntry> filtered;
    filtered.reserve(rows.size());
    for (auto const& entry : rows) {
      if (lower(to_wide(entry.title)).find(needle) != std::wstring::npos ||
          lower(to_wide(entry.author)).find(needle) != std::wstring::npos) {
        filtered.push_back(entry);
      }
    }
    rows.swap(filtered);
  }
  return rows;
}

// ---- grid pane -----------------------------------------------------------------------

void MainWindow::RefreshShelfGrid() {
  try {
    auto const rows = ShelfRows();
    bool const empty = rows.empty();
    ShelfEmpty().Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
    ShelfGrid().Visibility(empty ? Visibility::Collapsed : Visibility::Visible);
    auto items = ShelfGrid().Items();
    items.Clear();
    for (auto const& entry : rows) {
      items.Append(MakeShelfCell(entry));
    }
  } catch (...) {
  }
}

void MainWindow::ShelfImport_Click(winrt::Windows::Foundation::IInspectable const&,
                                   mux::RoutedEventArgs const&) {
  // The shelf import IS pickAndOpen: multi-select, everything lands on the
  // shelf, the first book opens for reading (mac toolbar parity).
  PickAndOpen();
}

void MainWindow::ShelfSearch_TextChanged(
    winrt::Windows::Foundation::IInspectable const&,
    muxc::TextChangedEventArgs const&) {
  try {
    shelf_search_ = std::wstring(ShelfSearch().Text());
    if (ShelfView().Visibility() == Visibility::Visible) RefreshShelfGrid();
  } catch (...) {
  }
}

// 新建收藏集 from the sidebar: create it, then look at it (mac
// NewCollectionSheet parity — the grid is empty until a book joins).
void MainWindow::ShelfCreateCollectionDialog() {
  muxc::ContentDialog dialog;
  dialog.Title(winrt::box_value(pdfgist::Tr("ui.shelf.new-collection")));
  muxc::TextBox box;
  box.Width(260);
  dialog.Content(winrt::box_value(box));
  dialog.PrimaryButtonText(pdfgist::Tr("ui.password.ok"));
  dialog.CloseButtonText(pdfgist::Tr("ui.password.cancel"));
  dialog.DefaultButton(muxc::ContentDialogButton::Primary);
  dialog.XamlRoot(Content().XamlRoot());

  auto op = dialog.ShowAsync();
  auto const weak = get_weak();
  op.Completed([weak, box](
                   winrt::Windows::Foundation::IAsyncOperation<
                       muxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    muxc::ContentDialogResult result{muxc::ContentDialogResult::None};
    try {
      result = sender.GetResults();
    } catch (...) {
      return;  // another dialog was up; the flow can be retried
    }
    if (result != muxc::ContentDialogResult::Primary) return;
    auto const name = trim(std::wstring(box.Text()));
    if (name.empty()) return;
    if (auto window = weak.get()) {
      window->shelf_selection_ = L"c:" + name;
      if (!window->ready_ || !window->backend_) {
        window->RefreshShelf();
        return;
      }
      auto const backend = window->backend_;
      auto const dispatcher = window->DispatcherQueue();
      auto const utf8_name = to_utf8(name);
      std::thread([weak, dispatcher, backend, utf8_name]() mutable {
        try {
          rivet_app::API api(*backend);
          (void)api.library_collection_create(utf8_name).get();
        } catch (...) {
        }
        dispatcher.TryEnqueue([weak]() mutable {
          if (auto window = weak.get()) window->RefreshShelf();
        });
      }).detach();
    }
  });
}

// ---- home digest (goals card + continue-reading + recently added) ----------------------

void MainWindow::RefreshHomeDigest() {
  try {
    bool const dark = Root().ActualTheme() == mux::ElementTheme::Dark;
    HomeTitle().Text(pdfgist::Tr("ui.shelf.home"));

    // goals card: 今日阅读 mm:ss live toward the 20-minute goal (mac
    // goalsCard parity — ink-colored progress, accent badge at 100%)
    GoalTodayLabel().Text(pdfgist::Tr("ui.shelf.today"));
    GoalMmss().Text(Mmss(today_seconds_));
    GoalOfLabel().Text(pdfgist::Tr("ui.shelf.goal-of"));
    GoalTrackPath().Stroke(ThemeBorderBrush());
    GoalTrackPath().Data(SemiArcGeometry(1.0));
    GoalProgressPath().Stroke(Brush(PrimaryColor(dark)));
    double const ratio = std::min(
        1.0, static_cast<double>(today_seconds_) /
                 static_cast<double>(kGoalSeconds));
    GoalProgressPath().Data(SemiArcGeometry(ratio));
    GoalProgressPath().Visibility(ratio > 0.0004 ? Visibility::Visible
                                                 : Visibility::Collapsed);
    bool const goal_hit = today_seconds_ >= kGoalSeconds;
    GoalHitBadge().Visibility(goal_hit ? Visibility::Visible
                                       : Visibility::Collapsed);
    if (goal_hit) {
      GoalHitText().Text(pdfgist::Tr("ui.shelf.goal-hit"));
      GoalHitText().Foreground(Brush(AccentColor()));
      GoalHitBadge().Background(Brush(AccentSoftColor(dark)));
    }

    // the Books store-button slot: last-read book, or import when none
    if (!recents_.empty()) {
      ContinuePill().Visibility(Visibility::Visible);
      ContinuePillImport().Visibility(Visibility::Collapsed);
      ContinuePillLabel().Text(pdfgist::Tr("ui.shelf.continue"));
      ContinuePill().Background(Brush(PrimaryColor(dark)));
      ContinuePillLabel().Foreground(
          Brush(dark ? wu::Color{0xFF, 0x00, 0x00, 0x00}
                     : wu::Color{0xFF, 0xFF, 0xFF, 0xFF}));
    } else {
      ContinuePill().Visibility(Visibility::Collapsed);
      ContinuePillImport().Visibility(Visibility::Visible);
      ContinuePillImportLabel().Text(pdfgist::Tr("ui.library.import"));
    }

    BuildStreakRow(dark);
    StreakLabel().Text(streak_days_ > 0
                           ? pdfgist::Trf("ui.shelf.streak-record",
                                          std::to_wstring(streak_days_))
                           : pdfgist::Tr("ui.shelf.streak-start"));

    // continue-reading strip: covers with progress, reading-now only
    std::vector<rivet_app::LibraryEntry> reading_now;
    for (auto const& entry : library_) {
      auto const progress = ShelfProgress(entry);
      if (progress && progress->first < progress->second) {
        reading_now.push_back(entry);
      }
    }
    bool const any_reading = !reading_now.empty();
    ContinueHeader().Text(pdfgist::Tr("ui.shelf.continue"));
    ContinueHeader().Visibility(any_reading ? Visibility::Visible
                                            : Visibility::Collapsed);
    ContinueScroll().Visibility(any_reading ? Visibility::Visible
                                            : Visibility::Collapsed);
    auto strip = ContinueRow().Children();
    strip.Clear();
    if (any_reading) {
      for (auto const& entry : reading_now) {
        strip.Append(MakeShelfCell(entry));
      }
    }

    // recently added: newest catalog entries first, capped at 12
    std::vector<rivet_app::LibraryEntry> recent = library_;
    std::stable_sort(recent.begin(), recent.end(),
                     [](auto const& a, auto const& b) {
                       return a.added_at_ms > b.added_at_ms;
                     });
    if (recent.size() > 12) recent.resize(12);
    bool const any_recent = !recent.empty();
    RecentHeader().Text(pdfgist::Tr("ui.home.recent"));
    RecentHeader().Visibility(any_recent ? Visibility::Visible
                                         : Visibility::Collapsed);
    RecentScroll().Visibility(any_recent ? Visibility::Visible
                                         : Visibility::Collapsed);
    auto row = RecentRow().Children();
    row.Clear();
    if (any_recent) {
      for (auto const& entry : recent) {
        row.Append(MakeShelfCell(entry));
      }
    }
  } catch (...) {
  }
}

// Seven day circles, 一二三四五六日; a day counts as read at 60s+ (mac
// streakRow parity). Rebuilt per tick so today's dot lights up live.
void MainWindow::BuildStreakRow(bool dark) {
  static wchar_t const* kDayLabels[7] = {L"一", L"二", L"三", L"四",
                                         L"五", L"六", L"日"};
  wu::Color const ink = PrimaryColor(dark);
  wu::Color const inverse = dark ? wu::Color{0xFF, 0x00, 0x00, 0x00}
                                 : wu::Color{0xFF, 0xFF, 0xFF, 0xFF};
  auto cells = StreakRow().Children();
  cells.Clear();
  for (int i = 0; i < 7; ++i) {
    bool const read = week_seconds_[static_cast<std::size_t>(i)] >= 60;
    auto cell = muxc::Grid();
    cell.Width(30);
    cell.Height(30);
    auto circle = shapes::Ellipse();
    circle.Fill(Brush(read ? ink : PanelAltColor(dark)));
    circle.Stroke(ThemeBorderBrush());
    circle.StrokeThickness(1);
    auto label = muxc::TextBlock();
    label.Text(kDayLabels[i]);
    label.FontSize(11);
    label.FontWeight(wut::FontWeights::SemiBold());
    label.Foreground(Brush(read ? inverse : TextDimColor(dark)));
    label.HorizontalAlignment(mux::HorizontalAlignment::Center);
    label.VerticalAlignment(mux::VerticalAlignment::Center);
    cell.Children().Append(circle);
    cell.Children().Append(label);
    cells.Append(cell);
  }
}

// The per-second live update: gauge number/trim, goal badge, streak dots,
// toolbar ring — cheap property sets only, never the cover strips (mac
// SwiftUI re-render parity).
void MainWindow::UpdateReadingProgress() {
  try {
    bool const dark = Root().ActualTheme() == mux::ElementTheme::Dark;
    GoalMmss().Text(Mmss(today_seconds_));
    double const ratio = std::min(
        1.0, static_cast<double>(today_seconds_) /
                 static_cast<double>(kGoalSeconds));
    GoalProgressPath().Data(SemiArcGeometry(ratio));
    GoalProgressPath().Visibility(ratio > 0.0004 ? Visibility::Visible
                                                 : Visibility::Collapsed);
    bool const goal_hit = today_seconds_ >= kGoalSeconds;
    GoalHitBadge().Visibility(goal_hit ? Visibility::Visible
                                       : Visibility::Collapsed);
    if (goal_hit) {
      GoalHitText().Text(pdfgist::Tr("ui.shelf.goal-hit"));
      GoalHitText().Foreground(Brush(AccentColor()));
      GoalHitBadge().Background(Brush(AccentSoftColor(dark)));
    }
    BuildStreakRow(dark);
    StreakLabel().Text(streak_days_ > 0
                           ? pdfgist::Trf("ui.shelf.streak-record",
                                          std::to_wstring(streak_days_))
                           : pdfgist::Tr("ui.shelf.streak-start"));

    // toolbar mini-gauge (mac todayRing parity): accent trim, minutes / 20
    TodayRingTrack().Stroke(ThemeBorderBrush());
    TodayRingArc().Stroke(Brush(AccentColor()));
    TodayRingFill().Stroke(Brush(AccentColor()));
    if (ratio >= 0.999) {
      TodayRingFill().Visibility(Visibility::Visible);
      TodayRingArc().Visibility(Visibility::Collapsed);
    } else if (ratio > 0.0009) {
      TodayRingFill().Visibility(Visibility::Collapsed);
      TodayRingArc().Visibility(Visibility::Visible);
      TodayRingArc().Data(RingArcGeometry(ratio));
    } else {
      TodayRingFill().Visibility(Visibility::Collapsed);
      TodayRingArc().Visibility(Visibility::Collapsed);
    }
    TodayRingMinutes().Text(std::to_wstring(today_seconds_ / 60));
    TodayRingGoal().Text(L"/ " + std::to_wstring(kGoalSeconds / 60));
  } catch (...) {
  }
}

void MainWindow::ContinuePill_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mux::RoutedEventArgs const&) {
  // open the book the user last read, at the resume position
  if (recents_.empty()) return;
  OpenPath(to_wide(recents_.front().path),
           static_cast<int>(recents_.front().page));
}

void MainWindow::ContinuePillImport_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mux::RoutedEventArgs const&) {
  PickAndOpen();
}

// ---- one shelf cell -----------------------------------------------------------------

mux::UIElement MainWindow::MakeShelfCell(rivet_app::LibraryEntry const& entry) {
  bool const dark = Root().ActualTheme() == mux::ElementTheme::Dark;
  bool const missing = entry.status == "missing";
  rivet_app::LibraryEntry const value = entry;

  // resume position from the recents join (books open where reading stopped)
  int resume = 1;
  for (auto const& recent : recents_) {
    if (recent.path == entry.path) {
      resume = static_cast<int>(recent.page);
      break;
    }
  }

  auto cell = muxc::StackPanel();
  cell.Orientation(muxc::Orientation::Vertical);
  cell.Spacing(6);
  cell.Width(148);
  cell.Padding({0, 4, 0, 0});

  // ---- cover art ----
  auto cover = muxc::Border();
  cover.Width(128);
  cover.Height(172);
  cover.CornerRadius({6, 6, 6, 6});
  cover.BorderThickness({1, 1, 1, 1});
  cover.BorderBrush(ThemeBorderBrush());
  cover.HorizontalAlignment(mux::HorizontalAlignment::Left);
  auto gradient = media::LinearGradientBrush();
  gradient.StartPoint({0, 0});
  gradient.EndPoint({0, 1});
  auto stop_top = media::GradientStop();
  stop_top.Color(AccentSoftColor(dark));
  stop_top.Offset(0);
  auto stop_bottom = media::GradientStop();
  stop_bottom.Color(PanelAltColor(dark));
  stop_bottom.Offset(1);
  gradient.GradientStops().Append(stop_top);
  gradient.GradientStops().Append(stop_bottom);
  cover.Background(gradient);

  auto zone = muxc::Grid();
  zone.Width(128);
  zone.Height(172);

  // no-cover placeholder: kind glyph + title (mac emptyCover parity)
  auto placeholder = muxc::StackPanel();
  placeholder.HorizontalAlignment(mux::HorizontalAlignment::Center);
  placeholder.VerticalAlignment(mux::VerticalAlignment::Center);
  placeholder.Spacing(8);
  auto glyph = muxc::FontIcon();
  glyph.FontFamily(media::FontFamily(L"Segoe Fluent Icons"));
  glyph.FontSize(34);
  glyph.Foreground(Brush(dark ? wu::Color{0xFF, 0xE8, 0x92, 0x6A}
                              : wu::Color{0xFF, 0xD9, 0x77, 0x57}));
  glyph.Glyph(entry.kind == "epub" ? L"\uE736" : L"\uE8A5");
  placeholder.Children().Append(glyph);
  if (!entry.title.empty()) {
    auto ph_title = muxc::TextBlock();
    ph_title.Text(to_wide(entry.title));
    ph_title.FontSize(11);
    ph_title.FontWeight(wut::FontWeights::SemiBold());
    ph_title.TextWrapping(mux::TextWrapping::Wrap);
    ph_title.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    ph_title.MaxLines(3);
    ph_title.TextAlignment(mux::TextAlignment::Center);
    ph_title.Margin({10, 0, 10, 0});
    ph_title.Foreground(Brush(TextDimColor(dark)));
    placeholder.Children().Append(ph_title);
  }
  zone.Children().Append(placeholder);

  if (missing) {
    auto dim = shapes::Rectangle();
    dim.Width(128);
    dim.Height(172);
    dim.RadiusX(6);
    dim.RadiusY(6);
    dim.Fill(Brush(wu::Color{0x8C, 0x00, 0x00, 0x00}));
    zone.Children().Append(dim);
    auto miss = muxc::StackPanel();
    miss.HorizontalAlignment(mux::HorizontalAlignment::Center);
    miss.VerticalAlignment(mux::VerticalAlignment::Center);
    miss.Spacing(4);
    auto miss_icon = muxc::FontIcon();
    miss_icon.FontFamily(media::FontFamily(L"Segoe Fluent Icons"));
    miss_icon.FontSize(16);
    miss_icon.Foreground(Brush(wu::Color{0xFF, 0xFF, 0xFF, 0xFF}));
    miss_icon.Glyph(L"\uE897");  // Help (question mark in circle)
    auto miss_label = muxc::TextBlock();
    miss_label.Text(pdfgist::Tr("ui.library.missing"));
    miss_label.FontSize(11);
    miss_label.FontWeight(wut::FontWeights::SemiBold());
    miss_label.Foreground(Brush(wu::Color{0xFF, 0xFF, 0xFF, 0xFF}));
    miss.Children().Append(miss_icon);
    miss.Children().Append(miss_label);
    zone.Children().Append(miss);
  }

  cover.Child(zone);
  cell.Children().Append(cover);
  if (!entry.cover.empty()) {
    LoadShelfCover(entry.cover, cover, placeholder);
  }

  // ---- title / author ----
  auto title = muxc::TextBlock();
  title.Text(to_wide(entry.title));
  title.FontSize(12);
  title.TextWrapping(mux::TextWrapping::Wrap);
  title.MaxLines(2);
  title.Height(34);  // fixed: rows stay aligned with 1-line titles
  title.TextTrimming(mux::TextTrimming::CharacterEllipsis);
  title.Foreground(Brush(TextColor(dark)));
  if (missing) title.Opacity(0.45);
  cell.Children().Append(title);

  if (!entry.author.empty()) {
    auto author = muxc::TextBlock();
    author.Text(to_wide(entry.author));
    author.FontSize(11);
    author.MaxLines(1);
    author.Height(15);
    author.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    author.Foreground(Brush(TextDimColor(dark)));
    if (missing) author.Opacity(0.4);
    cell.Children().Append(author);
  }

  // ---- reading progress joined from recents by path ----
  rivet_app::RecentEntry const* recent = nullptr;
  for (auto const& candidate : recents_) {
    if (candidate.path == entry.path) {
      recent = &candidate;
      break;
    }
  }
  bool const has_progress =
      recent != nullptr && (recent->page > 1 || entry.pages > 0);
  auto progress = muxc::StackPanel();
  progress.Spacing(3);
  auto bar = muxc::ProgressBar();
  bar.Minimum(0);
  bar.Maximum(entry.pages > 0 ? static_cast<double>(entry.pages) : 1.0);
  bar.Value(recent != nullptr ? static_cast<double>(recent->page) : 0.0);
  bar.Height(4);
  bar.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  bar.Visibility(has_progress ? Visibility::Visible : Visibility::Collapsed);
  auto label = muxc::TextBlock();
  label.FontSize(10);
  label.Height(14);  // fixed: keeps row heights aligned (mac parity)
  label.Opacity(0.5);
  label.Foreground(Brush(TextDimColor(dark)));
  if (has_progress) {
    label.Text(pdfgist::Trf(
        entry.kind == "epub" ? "ui.library.chapter-progress"
                             : "ui.library.progress",
        std::to_wstring(recent->page),
        std::to_wstring(entry.pages > 0 ? entry.pages : 1)));
  }
  progress.Children().Append(bar);
  progress.Children().Append(label);
  cell.Children().Append(progress);

  // ---- interactions ----
  auto const weak = get_weak();
  cell.DoubleTapped(
      [weak, value, resume](
          winrt::Windows::Foundation::IInspectable const&,
          mux::Input::DoubleTappedRoutedEventArgs const& args) {
        args.Handled(true);
        if (value.status == "missing") return;
        if (auto window = weak.get()) {
          window->OpenPath(to_wide(value.path), resume);
        }
      });

  // Right-click flyout, built from this cell's snapshot; every mutation
  // rebuilds the grid, so stale menus never linger.
  auto flyout = muxc::MenuFlyout();
  if (missing) {
    auto relink_item = muxc::MenuFlyoutItem();
    relink_item.Text(pdfgist::Tr("ui.library.relink"));
    relink_item.Click(
        [weak, value](winrt::Windows::Foundation::IInspectable const&,
                      mux::RoutedEventArgs const&) {
          if (auto window = weak.get()) window->ShelfRelink(value);
        });
    flyout.Items().Append(relink_item);
    flyout.Items().Append(muxc::MenuFlyoutSeparator());
  }
  auto add_sub = muxc::MenuFlyoutSubItem();
  add_sub.Text(pdfgist::Tr("ui.library.add-to"));
  auto new_item = muxc::MenuFlyoutItem();
  new_item.Text(pdfgist::Tr("ui.library.new-collection"));
  new_item.Click([weak, value](winrt::Windows::Foundation::IInspectable const&,
                               mux::RoutedEventArgs const&) {
    if (auto window = weak.get()) window->ShelfNewCollection(value);
  });
  add_sub.Items().Append(new_item);
  std::set<std::string> seen;
  bool any_collection = false;
  for (auto const& e : library_) {
    for (auto const& name : e.collections) {
      if (!seen.insert(name).second) continue;
      if (!any_collection) {
        add_sub.Items().Append(muxc::MenuFlyoutSeparator());
        any_collection = true;
      }
      auto item = muxc::MenuFlyoutItem();
      std::wstring const wide_name = to_wide(name);
      item.Text(wide_name);
      item.Click([weak, value, wide_name](
                     winrt::Windows::Foundation::IInspectable const&,
                     mux::RoutedEventArgs const&) {
        if (auto window = weak.get()) {
          window->ShelfAddToCollection(value, wide_name);
        }
      });
      add_sub.Items().Append(item);
    }
  }
  flyout.Items().Append(add_sub);

  if (!value.collections.empty()) {
    auto remove_sub = muxc::MenuFlyoutSubItem();
    remove_sub.Text(pdfgist::Tr("ui.library.remove-from"));
    for (auto const& name : value.collections) {
      auto item = muxc::MenuFlyoutItem();
      std::wstring const wide_name = to_wide(name);
      item.Text(wide_name);
      item.Click([weak, value, wide_name](
                     winrt::Windows::Foundation::IInspectable const&,
                     mux::RoutedEventArgs const&) {
        if (auto window = weak.get()) {
          window->ShelfRemoveFromCollection(value, wide_name);
        }
      });
      remove_sub.Items().Append(item);
    }
    flyout.Items().Append(remove_sub);
  }

  flyout.Items().Append(muxc::MenuFlyoutSeparator());

  auto reveal_item = muxc::MenuFlyoutItem();
  reveal_item.Text(pdfgist::Tr("ui.library.reveal"));
  reveal_item.IsEnabled(!missing);
  reveal_item.Click([weak, value](winrt::Windows::Foundation::IInspectable const&,
                                  mux::RoutedEventArgs const&) {
    if (auto window = weak.get()) window->RevealInExplorer(to_wide(value.path));
  });
  flyout.Items().Append(reveal_item);

  auto remove_item = muxc::MenuFlyoutItem();
  remove_item.Text(pdfgist::Tr("ui.library.remove"));
  remove_item.Click([weak, value](winrt::Windows::Foundation::IInspectable const&,
                                  mux::RoutedEventArgs const&) {
    if (auto window = weak.get()) window->ShelfRemove(value);
  });
  flyout.Items().Append(remove_item);

  cell.ContextFlyout(flyout);
  return cell;
}

// The cover file lives in the backend's covers cache; read it off the UI
// thread and decode through an in-memory stream so a slow disk never stalls
// the shelf render (the 10s launch smoke shows whatever the backend returned).
void MainWindow::LoadShelfCover(
    std::string const& png_path, muxc::Border const& border,
    muxc::StackPanel const& placeholder) {
  if (png_path.empty()) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const bytes = std::make_shared<std::vector<std::uint8_t>>();
  auto const path = std::make_shared<std::wstring>(to_wide(png_path));
  std::thread([weak, dispatcher, bytes, path, border,
               placeholder]() mutable {
    try {
      std::ifstream file(*path, std::ios::binary);
      if (file) {
        bytes->assign(std::istreambuf_iterator<char>(file),
                      std::istreambuf_iterator<char>());
      }
    } catch (...) {
    }
    if (bytes->empty()) return;
    dispatcher.TryEnqueue([weak, bytes, border, placeholder]() mutable {
      try {
        auto stream = wss::InMemoryRandomAccessStream();
        auto writer = wss::DataWriter(stream);
        writer.WriteBytes(winrt::array_view<std::uint8_t const>(
            bytes->data(), bytes->data() + bytes->size()));
        auto store_op = writer.StoreAsync();
        store_op.Completed(
            [weak, stream, writer, border, placeholder](
                winrt::Windows::Foundation::IAsyncOperation<
                    std::uint32_t> const&,
                winrt::Windows::Foundation::AsyncStatus status) mutable {
              if (status != winrt::Windows::Foundation::AsyncStatus::Completed) {
                return;
              }
              auto flush_op = writer.FlushAsync();
              flush_op.Completed(
                  [weak, stream, writer, border, placeholder](
                      winrt::Windows::Foundation::IAsyncOperation<
                          bool> const&,
                      winrt::Windows::Foundation::AsyncStatus status) mutable {
                    if (status !=
                        winrt::Windows::Foundation::AsyncStatus::Completed) {
                      return;
                    }
                    try {
                      writer.DetachStream();
                      auto bitmap = imaging::BitmapImage();
                      auto set_op = bitmap.SetSourceAsync(stream);
                      set_op.Completed(
                          [weak, border, placeholder, bitmap](
                              winrt::Windows::Foundation::IAsyncAction const&,
                              winrt::Windows::Foundation::AsyncStatus status)
                              mutable {
                            if (status !=
                                winrt::Windows::Foundation::AsyncStatus::
                                    Completed) {
                              return;
                            }
                            try {
                              auto brush = media::ImageBrush();
                              brush.ImageSource(bitmap);
                              brush.Stretch(media::Stretch::UniformToFill);
                              border.Background(brush);
                              placeholder.Visibility(Visibility::Collapsed);
                            } catch (...) {
                            }
                          });
                    } catch (...) {
                    }
                  });
            });
      } catch (...) {
      }
    });
  }).detach();
}

// ---- mutations (optimistic UI, RPC follows) ------------------------------------------

void MainWindow::ShelfReplaceEntry(rivet_app::LibraryEntry const& updated) {
  for (auto& candidate : library_) {
    if (candidate.path == updated.path) {
      candidate = updated;
      return;
    }
  }
}

// The mutation RPCs run on a worker; the library re-fetch rides the same
// thread so server truth can never overtake the mutation and visually undo
// it. A failed RPC leaves the optimistic state standing until the next sync.
void MainWindow::ShelfAddToCollection(rivet_app::LibraryEntry const& entry,
                                      std::wstring const& name) {
  std::string const utf8_name = to_utf8(name);
  if (std::find(entry.collections.begin(), entry.collections.end(),
                utf8_name) != entry.collections.end()) {
    return;
  }
  rivet_app::LibraryEntry updated = entry;
  updated.collections.push_back(utf8_name);
  ShelfReplaceEntry(updated);
  RefreshShelf();
  if (!ready_ || !backend_) return;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend, path = entry.path,
               utf8_name]() mutable {
    try {
      rivet_app::API api(*backend);
      (void)api.library_collection_create(utf8_name).get();
      (void)api.library_book_collection(path, utf8_name, true).get();
      auto library = api.get_library().get();
      auto collections = api.library_collections().get();
      dispatcher.TryEnqueue(
          [weak, library = std::move(library),
           collections = std::move(collections)]() mutable {
            if (auto window = weak.get()) {
              window->library_ = std::move(library);
              window->library_collections_ = std::move(collections);
              window->RefreshShelf();
            }
          });
    } catch (...) {
    }
  }).detach();
}

void MainWindow::ShelfRemoveFromCollection(rivet_app::LibraryEntry const& entry,
                                           std::wstring const& name) {
  std::string const utf8_name = to_utf8(name);
  rivet_app::LibraryEntry updated = entry;
  updated.collections.erase(
      std::remove(updated.collections.begin(), updated.collections.end(),
                  utf8_name),
      updated.collections.end());
  if (updated.collections.size() == entry.collections.size()) return;
  ShelfReplaceEntry(updated);
  RefreshShelf();
  if (!ready_ || !backend_) return;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend, path = entry.path,
               utf8_name]() mutable {
    try {
      rivet_app::API api(*backend);
      (void)api.library_book_collection(path, utf8_name, false).get();
      auto library = api.get_library().get();
      auto collections = api.library_collections().get();
      dispatcher.TryEnqueue(
          [weak, library = std::move(library),
           collections = std::move(collections)]() mutable {
            if (auto window = weak.get()) {
              window->library_ = std::move(library);
              window->library_collections_ = std::move(collections);
              window->RefreshShelf();
            }
          });
    } catch (...) {
    }
  }).detach();
}

// ---- collection delete (mac context-menu parity) ---------------------------------

void MainWindow::ConfirmDeleteCollection(std::wstring const& name) {
  ShowDialog(pdfgist::Tr("app.name"),
             pdfgist::Trf("ui.shelf.delete-collection-confirm", to_utf8(name)),
             pdfgist::Tr("ui.shelf.delete-collection"),
             pdfgist::Tr("ui.password.cancel"),
             [weak = get_weak(), name] {
               if (auto window = weak.get()) window->DeleteCollection(name);
             });
}

void MainWindow::DeleteCollection(std::wstring const& name_wide) {
  std::string const utf8_name = to_utf8(name_wide);
  // Optimistic: the label strips from every book; books stay on the shelf.
  for (auto& entry : library_) {
    auto& cols = entry.collections;
    cols.erase(std::remove(cols.begin(), cols.end(), utf8_name), cols.end());
  }
  library_collections_.erase(
      std::remove(library_collections_.begin(), library_collections_.end(),
                  utf8_name),
      library_collections_.end());
  if (shelf_selection_ == name_wide) shelf_selection_ = L"all";
  RefreshShelf();
  if (!ready_ || !backend_) return;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend, utf8_name]() mutable {
    try {
      rivet_app::API api(*backend);
      (void)api.library_collection_delete(utf8_name).get();
      auto library = api.get_library().get();
      auto collections = api.library_collections().get();
      dispatcher.TryEnqueue(
          [weak, library = std::move(library),
           collections = std::move(collections)]() mutable {
            if (auto window = weak.get()) {
              window->library_ = std::move(library);
              window->library_collections_ = std::move(collections);
              window->RefreshShelf();
            }
          });
    } catch (...) {
    }
  }).detach();
}

// 新收藏集… → create the collection and put this book into it in one move.
void MainWindow::ShelfNewCollection(rivet_app::LibraryEntry const& entry) {
  muxc::ContentDialog dialog;
  dialog.Title(winrt::box_value(pdfgist::Tr("ui.library.new-collection")));
  muxc::TextBox box;
  box.Width(260);
  dialog.Content(winrt::box_value(box));
  dialog.PrimaryButtonText(pdfgist::Tr("ui.password.ok"));
  dialog.CloseButtonText(pdfgist::Tr("ui.password.cancel"));
  dialog.DefaultButton(muxc::ContentDialogButton::Primary);
  dialog.XamlRoot(Content().XamlRoot());

  auto op = dialog.ShowAsync();
  auto const weak = get_weak();
  op.Completed([weak, entry, box](
                   winrt::Windows::Foundation::IAsyncOperation<
                       muxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    muxc::ContentDialogResult result{muxc::ContentDialogResult::None};
    try {
      result = sender.GetResults();
    } catch (...) {
      return;  // another dialog was up; the flow can be retried
    }
    if (result != muxc::ContentDialogResult::Primary) return;
    auto const name = trim(std::wstring(box.Text()));
    if (name.empty()) return;
    if (auto window = weak.get()) window->ShelfAddToCollection(entry, name);
  });
}

// Missing book: pick the moved file, the shelf re-links to it (books are
// never re-copied, mac relocateEntry parity).
void MainWindow::ShelfRelink(rivet_app::LibraryEntry const& entry) {
  auto picker = winrt::Windows::Storage::Pickers::FileOpenPicker();
  auto const window_native = try_as<::IWindowNative>();
  if (window_native) {
    HWND hwnd = nullptr;
    if (SUCCEEDED(window_native->get_WindowHandle(&hwnd))) {
      if (auto initializer = picker.try_as<::IInitializeWithWindow>()) {
        initializer->Initialize(hwnd);
      }
    }
  }
  picker.FileTypeFilter().Append(L"*");  // any file: the book may have renamed
  auto const weak = get_weak();
  picker.PickSingleFileAsync().Completed(
      [weak, entry](winrt::Windows::Foundation::IAsyncOperation<
                    winrt::Windows::Storage::StorageFile> const& sender,
                    winrt::Windows::Foundation::AsyncStatus) {
        try {
          auto const file = sender.GetResults();
          if (file == nullptr) return;
          if (auto window = weak.get()) {
            rivet_app::LibraryEntry updated = entry;
            updated.path = to_utf8(std::wstring(file.Path()));
            updated.status = "ok";
            window->ShelfReplaceEntry(updated);
            window->RefreshShelf();
            if (window->ready_ && window->backend_) {
              auto const backend = window->backend_;
              auto const dispatcher = window->DispatcherQueue();
              auto const relink_weak = window->get_weak();
              auto const from = entry.path;
              auto const to = updated.path;
              std::thread([relink_weak, dispatcher, backend, from,
                           to]() mutable {
                try {
                  rivet_app::API api(*backend);
                  (void)api.library_relink(from, to).get();
                  auto library = api.get_library().get();
                  auto collections = api.library_collections().get();
                  dispatcher.TryEnqueue(
                      [relink_weak, library = std::move(library),
                       collections =
                           std::move(collections)]() mutable {
                        if (auto w = relink_weak.get()) {
                          w->library_ = std::move(library);
                          w->library_collections_ = std::move(collections);
                          w->RefreshShelf();
                        }
                      });
                } catch (...) {
                }
              }).detach();
            }
          }
        } catch (...) {
        }
      });
}

// 移出书架 removes the catalog entry only — the book file is never touched.
void MainWindow::ShelfRemove(rivet_app::LibraryEntry const& entry) {
  library_.erase(std::remove_if(library_.begin(), library_.end(),
                                [&entry](auto const& candidate) {
                                  return candidate.path == entry.path;
                                }),
                 library_.end());
  RefreshShelf();
  if (!ready_ || !backend_) return;
  auto const backend = backend_;
  std::thread([backend, path = entry.path]() mutable {
    try {
      rivet_app::API api(*backend);
      (void)api.library_remove(path).get();
    } catch (...) {
    }
  }).detach();
}

void MainWindow::RevealInExplorer(std::wstring const& path) {
  std::wstring const params = L"/select,\"" + path + L"\"";
  ::ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

// ---- import --------------------------------------------------------------------------

// Opening a book from anywhere grows the shelf organically: the open path
// feeds the catalog a cover + metadata (upsert = no duplicates).
void MainWindow::ImportBook(std::wstring const& path) {
  ImportBooks(std::vector<std::wstring>{path});
}

// Sequential import; per-file failure skips to the next (mac importBooks
// parity). One library refresh at the end covers the whole batch.
void MainWindow::ImportBooks(std::vector<std::wstring> const& paths) {
  if (!ready_ || !backend_) return;
  std::vector<std::wstring> books;
  for (auto const& path : paths) {
    if (has_extension(path, L".pdf") || has_extension(path, L".epub")) {
      books.push_back(path);
    }
  }
  if (books.empty()) return;
  if (!library_importing_) {
    library_importing_ = true;
    try {
      ShelfBusy().IsActive(true);
    } catch (...) {
    }
  }

  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const shared = std::make_shared<std::vector<std::wstring>>(
      std::move(books));

  std::thread([weak, dispatcher, backend, shared]() mutable {
    bool failed = true;
    try {
      rivet_app::API api(*backend);
      for (auto const& path : *shared) {
        try {
          auto const utf8_path = to_utf8(path);
          if (has_extension(path, L".epub")) {
            // EPUB: the backend reads title + chapter count; no cover yet.
            auto view = api.epub_open(utf8_path).get();
            (void)api.library_import(utf8_path, view.title, "", "epub",
                                     view.chapters,
                                     rivet::Bytes{}).get();
          } else {
            // PDF: host-side metadata + page-1 cover (hosts own renderers).
            std::vector<std::uint8_t> bytes;
            std::ifstream file(std::filesystem::path(path), std::ios::binary);
            if (!file) continue;
            bytes.assign(std::istreambuf_iterator<char>(file),
                         std::istreambuf_iterator<char>());
            file.close();
            auto doc = pdfgist::PdfDocument::OpenFromBytes(bytes, "", nullptr);
            if (!doc) continue;
            auto const meta = doc->MetaInfo();
            std::wstring const title =
                meta.title.empty() ? stem_name(path) : meta.title;
            (void)api.library_import(utf8_path, to_utf8(title),
                                     to_utf8(meta.author), "pdf",
                                     doc->PageCount(),
                                     CoverPng(*doc)).get();
          }
        } catch (...) {
          continue;  // one bad book never blocks the rest of the import
        }
      }
      failed = false;
      auto library = api.get_library().get();
      auto collections = api.library_collections().get();
      dispatcher.TryEnqueue([weak, library = std::move(library),
                             collections =
                                 std::move(collections)]() mutable {
        if (auto window = weak.get()) {
          window->library_ = std::move(library);
          window->library_collections_ = std::move(collections);
          window->library_loaded_ = true;
          window->library_importing_ = false;
          try {
            window->ShelfBusy().IsActive(false);
          } catch (...) {
          }
          window->RefreshShelf();
        }
      });
    } catch (...) {
    }
    if (failed) {
      dispatcher.TryEnqueue([weak]() mutable {
        if (auto window = weak.get()) {
          window->library_importing_ = false;
          try {
            window->ShelfBusy().IsActive(false);
          } catch (...) {
          }
        }
      });
    }
  }).detach();
}

// ---- reading time (device-local: live seconds per day, Books-style goal) ---------------
// A 1-second clock accrues while a reader tab is up and the window is
// showing. The store is a flat {yyyy-MM-dd: seconds} JSON in
// %APPDATA%\site.jrtx.booklight\reading-minutes.json (the file kept its
// pre-upgrade name; the values are seconds since 9201fb3) and it
// deliberately never rides any sync drive — cross-device merging would
// double-count (mac AppModel parity; mac keeps the same data in
// UserDefaults for the same reason).

std::wstring MainWindow::ReadingSecondsPath() const {
  wchar_t buffer[MAX_PATH]{};
  if (::GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH) == 0) {
    return {};
  }
  return (std::filesystem::path(buffer) / L"site.jrtx.booklight" /
          L"reading-minutes.json")
      .wstring();
}

int MainWindow::LookupSeconds(std::wstring const& day) const {
  auto const found = reading_seconds_.find(day);
  return found == reading_seconds_.end() ? 0 : found->second;
}

void MainWindow::LoadReadingStats() {
  try {
    auto const path = ReadingSecondsPath();
    if (path.empty()) return;
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) return;
    std::string json((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    static std::regex const pattern("\"(\\d{4}-\\d{2}-\\d{2})\"\\s*:\\s*(\\d+)");
    for (std::sregex_iterator it(json.begin(), json.end(), pattern), end;
         it != end; ++it) {
      reading_seconds_[to_wide((*it)[1].str())] = std::stoi((*it)[2].str());
    }
  } catch (...) {
    // a damaged store falls back to zeroed stats, never a crash
  }
  RefreshReadingStats();
}

void MainWindow::SaveReadingStats() {
  try {
    auto const path = ReadingSecondsPath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream file(std::filesystem::path(path),
                       std::ios::binary | std::ios::trunc);
    file << "{";
    bool first = true;
    for (auto const& [day, seconds] : reading_seconds_) {
      if (!first) file << ",";
      first = false;
      file << "\"" << to_utf8(day) << "\":" << seconds;
    }
    file << "}\n";
  } catch (...) {
    // best effort — the in-memory buckets still feed the UI
  }
}

// Recompute today / week (Mon..Sun) / streak from the buckets; a day counts
// as read at 60s+ (mac refreshReadingStats parity).
void MainWindow::RefreshReadingStats() {
  DayStamp const today = Today();
  today_seconds_ = LookupSeconds(today.key);
  long long const monday = today.serial - ((today.weekday + 6) % 7);
  for (int i = 0; i < 7; ++i) {
    week_seconds_[static_cast<std::size_t>(i)] =
        LookupSeconds(DayAt(monday + i).key);
  }
  // current streak: consecutive 60s+ days ending today (or yesterday, if
  // today hasn't started yet)
  long long cursor = today.serial;
  if (LookupSeconds(today.key) < 60) cursor -= 1;
  streak_days_ = 0;
  while (LookupSeconds(DayAt(cursor).key) >= 60) {
    ++streak_days_;
    --cursor;
  }
}

void MainWindow::AddReadingSeconds(int seconds) {
  reading_seconds_[Today().key] += seconds;
  SaveReadingStats();
  RefreshReadingStats();
  UpdateReadingProgress();  // cheap: gauge, dots, ring — no cover strips
}

void MainWindow::StartReadingClock() {
  reading_timer_ = DispatcherQueue().CreateTimer();
  reading_timer_.Interval(std::chrono::seconds{1});
  reading_timer_.IsRepeating(true);
  reading_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) window->TickReadingSecond();
  });
}

// One second of reading accrues when a reader tab is up and the window is
// showing — the shelf idling never counts (mac tickReadingSecond parity).
void MainWindow::TickReadingSecond() {
  bool const reader_up = !tabs_.empty() || epub_ != nullptr;
  if (!reader_up) return;
  HWND hwnd = nullptr;
  if (auto native = try_as<::IWindowNative>()) {
    native->get_WindowHandle(&hwnd);
  }
  if (hwnd != nullptr && ::IsIconic(hwnd)) return;
  AddReadingSeconds(1);
}

std::wstring MainWindow::Mmss(int seconds) {
  if (seconds < 0) seconds = 0;
  wchar_t buffer[16]{};
  swprintf_s(buffer, L"%d:%02d", seconds / 60, seconds % 60);
  return std::wstring(buffer);
}

}  // namespace winrt::RivetHost::implementation

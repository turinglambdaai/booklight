#include "pch.h"
#include "MainWindow.xaml.h"
#include "I18n.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
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

// ---- PNG encode (WIC) --------------------------------------------------------
// The cover render comes out of PDFium as top-down BGRA; WIC turns it into
// the PNG the backend stores in its covers cache. Self-contained so the
// import path has no UI dependencies.

struct ComApartment {
  bool ok = false;
  ~ComApartment() {
    if (ok) ::CoUninitialize();
  }
};

std::vector<std::uint8_t> EncodePng(pdfgist::PdfDocument::RenderedPage const& page) {
  std::vector<std::uint8_t> png;
  ComApartment co;
  co.ok = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  if (!co.ok) return png;

  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
    return png;
  }
  Microsoft::WRL::ComPtr<IStream> stream;
  if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return png;
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, GUID_NULL,
                                    &encoder))) {
    return png;
  }
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    return png;
  }
  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return png;
  if (FAILED(frame->Initialize(nullptr))) return png;
  if (FAILED(frame->SetSize(static_cast<UINT>(page.width),
                            static_cast<UINT>(page.height)))) {
    return png;
  }
  GUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format))) return png;
  if (FAILED(frame->WritePixels(
          static_cast<UINT>(page.height), static_cast<UINT>(page.stride),
          static_cast<UINT>(page.bgra.size()),
          const_cast<BYTE*>(page.bgra.data())))) {
    return png;
  }
  if (FAILED(frame->Commit())) return png;
  if (FAILED(encoder->Commit())) return png;

  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return png;
  if (stat.cbSize.QuadPart <= 0) return png;
  HGLOBAL global = nullptr;
  if (FAILED(::GetHGlobalFromStream(stream.Get(), &global))) return png;
  void const* data = ::GlobalLock(global);
  if (data == nullptr) return png;
  png.assign(static_cast<std::uint8_t const*>(data),
             static_cast<std::uint8_t const*>(data) +
                 static_cast<std::size_t>(stat.cbSize.QuadPart));
  ::GlobalUnlock(global);
  return png;
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
  return EncodePng(page);
}

}  // namespace

// ---- rows (filter + search) --------------------------------------------------------

// The shelf rows after the collections filter and the search box (mac
// shelfEntries parity).
std::vector<rivet_app::LibraryEntry> MainWindow::ShelfRows() const {
  std::vector<rivet_app::LibraryEntry> rows;
  rows.reserve(library_.size());
  std::string const filter = to_utf8(shelf_filter_);
  bool const unfiled = shelf_filter_ == L"unfiled";
  for (auto const& entry : library_) {
    if (!filter.empty()) {
      bool const in_collection =
          !unfiled && std::find(entry.collections.begin(),
                                entry.collections.end(),
                                filter) != entry.collections.end();
      if (unfiled ? !entry.collections.empty() : !in_collection) continue;
    }
    rows.push_back(entry);
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

// ---- toolbar ------------------------------------------------------------------------

// The collections filter: 全部书籍 / 未分类 / each collection, derived from
// the entries (mac shelfCollections parity — an empty collection is a view
// over books and only exists once a book carries it).
void MainWindow::RefreshShelfFilterBox() {
  shelf_ui_updating_ = true;
  try {
    auto items = ShelfFilter().Items();
    items.Clear();
    shelf_filter_values_.clear();
    items.Append(winrt::box_value(pdfgist::Tr("ui.library.all")));
    shelf_filter_values_.push_back(L"");
    items.Append(winrt::box_value(pdfgist::Tr("ui.library.unfiled")));
    shelf_filter_values_.push_back(L"unfiled");
    std::set<std::string> seen;
    for (auto const& entry : library_) {
      for (auto const& name : entry.collections) {
        if (!seen.insert(name).second) continue;
        auto const wide = to_wide(name);
        items.Append(winrt::box_value(wide));
        shelf_filter_values_.push_back(wide);
      }
    }
    std::uint32_t index = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < shelf_filter_values_.size(); ++i) {
      if (shelf_filter_values_[i] == shelf_filter_) {
        index = i;
        found = true;
        break;
      }
    }
    if (!found) {
      shelf_filter_.clear();
      index = 0;
    }
    ShelfFilter().SelectedIndex(index);
  } catch (...) {
  }
  shelf_ui_updating_ = false;
}

void MainWindow::RefreshShelf() {
  try {
    RefreshShelfFilterBox();
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

void MainWindow::ShelfFilter_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    muxc::SelectionChangedEventArgs const&) {
  if (shelf_ui_updating_) return;
  try {
    auto const index = ShelfFilter().SelectedIndex();
    if (index >= 0 && index < static_cast<int>(shelf_filter_values_.size())) {
      shelf_filter_ = shelf_filter_values_[static_cast<std::size_t>(index)];
    }
    RefreshShelf();
  } catch (...) {
  }
}

void MainWindow::ShelfSearch_TextChanged(
    winrt::Windows::Foundation::IInspectable const&,
    muxc::TextChangedEventArgs const&) {
  try {
    shelf_search_ = std::wstring(ShelfSearch().Text());
    RefreshShelf();
  } catch (...) {
  }
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
                winrt::Windows::Foundation::IAsyncAction const&,
                winrt::Windows::Foundation::AsyncStatus status) mutable {
              if (status != winrt::Windows::Foundation::AsyncStatus::Completed) {
                return;
              }
              auto flush_op = writer.FlushAsync();
              flush_op.Completed(
                  [weak, stream, writer, border, placeholder](
                      winrt::Windows::Foundation::IAsyncOperation<bool> const&,
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
      dispatcher.TryEnqueue([weak, library = std::move(library)]() mutable {
        if (auto window = weak.get()) {
          window->library_ = std::move(library);
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
      dispatcher.TryEnqueue([weak, library = std::move(library)]() mutable {
        if (auto window = weak.get()) {
          window->library_ = std::move(library);
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
                  dispatcher.TryEnqueue(
                      [relink_weak,
                       library = std::move(library)]() mutable {
                        if (auto w = relink_weak.get()) {
                          w->library_ = std::move(library);
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
      dispatcher.TryEnqueue([weak,
                             library = std::move(library)]() mutable {
        if (auto window = weak.get()) {
          window->library_ = std::move(library);
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

}  // namespace winrt::RivetHost::implementation

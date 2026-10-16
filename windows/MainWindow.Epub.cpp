#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <cwchar>
#include <fstream>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace wu = winrt::Windows::UI;
namespace wut = winrt::Windows::UI::Text;

using Microsoft::UI::Xaml::Visibility;

namespace {

std::wstring file_name(std::wstring const& path) {
  std::size_t const slash = path.find_last_of(L"/\\");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring escape_html(std::wstring const& s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t const c : s) {
    switch (c) {
      case L'&': out += L"&amp;"; break;
      case L'<': out += L"&lt;"; break;
      case L'>': out += L"&gt;"; break;
      default: out += c;
    }
  }
  return out;
}

std::wstring js_string(std::wstring const& s) {
  std::wstring out = L"\"";
  for (wchar_t const c : s) {
    switch (c) {
      case L'\\': out += L"\\\\"; break;
      case L'"': out += L"\\\""; break;
      case L'\n': out += L"\\n"; break;
      case L'\r': out += L"\\r"; break;
      default: out += c;
    }
  }
  out += L"\"";
  return out;
}

}  // namespace

// ---- open / close ---------------------------------------------------------------------

void MainWindow::OpenEpub(std::wstring const& path, int resume_chapter) {
  if (epub_ && epub_->path == path) {
    ActivateEpub();
    if (resume_chapter > 1) GoToChapter(*epub_, resume_chapter);
    return;
  }
  if (!ready_ || !api_) {
    Alert(pdfgist::Tr("ui.backend-not-ready"));
    return;
  }
  // one EPUB tab at a time (mac parity)
  if (epub_) CloseEpub();

  auto book = std::make_shared<EpubBook>();
  book->path = path;
  book->title = file_name(path);
  epub_ = book;

  muxc::TabViewItem item;
  muxc::TextBlock header;
  header.Text(book->title);
  header.MaxWidth(150);
  header.TextTrimming(mux::TextTrimming::CharacterEllipsis);
  item.Header(header);
  item.IsClosable(true);

  // WebView2 needs a placeholder in the tree before the async controller
  // creation completes.
  auto web = muxc::WebView2();
  book->web = web;
  book->tab_item = item;
  item.Content(web);

  DocTabs().TabItems().Append(item);
  ActivateEpub();
  RefreshLeftPanel();
  UpdateChrome();
  NoteActivity();
  LoadEpubBook(book);
  if (resume_chapter > 1) book->current_chapter = resume_chapter;
}

void MainWindow::ActivateEpub() {
  if (!epub_) return;
  DocTabs().SelectedIndex(IndexOfTabItem(epub_->tab_item));
}

void MainWindow::CloseEpub() {
  if (!epub_) return;
  auto const book = epub_;
  SaveRecentNow();
  auto const index = IndexOfTabItem(book->tab_item);
  if (index < DocTabs().TabItems().Size()) {
    DocTabs().TabItems().RemoveAt(index);
  }
  epub_ = nullptr;
  epub_active_ = false;
  UpdateChrome();
  RefreshLeftPanel();
}

void MainWindow::LoadEpubBook(std::shared_ptr<EpubBook> book) {
  if (!api_) return;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const path_utf8 = to_utf8(book->path);

  // Serial chapter fetches (mac loadBook parity: concurrent epub RPCs can
  // stall on the embedded channel).
  std::thread([weak, dispatcher, backend, book, path_utf8]() mutable {
    std::string failure;
    rivet_app::EpubView view{};
    std::vector<std::pair<int, std::wstring>> sections;
    try {
      rivet_app::API api(*backend);
      view = api.epub_open(path_utf8).get();
      int const chapters = static_cast<int>(view.chapters);
      for (int i = 1; i <= std::max(1, chapters); ++i) {
        auto body_bytes = api.epub_chapter_html(path_utf8, i).get();
        // sanitized UTF-8 chapter body; widen for NavigateToString
        sections.emplace_back(
            i, to_wide(std::string(body_bytes.begin(), body_bytes.end())));
        if (book->text_cache.find(i) == book->text_cache.end()) {
          book->text_cache[i] = to_wide(api.epub_chapter_text(path_utf8, i).get());
        }
      }
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue([weak, book, view, sections = std::move(sections),
                           failure = std::move(failure)]() mutable {
      auto window = weak.get();
      if (!window) return;
      if (window->epub_.get() != book.get()) return;  // tab closed meanwhile
      if (!failure.empty()) {
        window->Alert(pdfgist::Trf("ui.pages.edit-failed", to_wide(failure)));
        return;
      }
      book->chapters = static_cast<int>(view.chapters);
      book->toc = std::move(view.toc);
      book->text_ready = true;
      window->NavigateToEpubController(book, sections);
      window->ApplyEpubTheme(*book);
      window->RefreshLeftPanel();
      // Opening grows the shelf (mac importOpenedEpub parity).
      window->ImportBook(book->path);
    });
  }).detach();
}

// Controller creation is async; the HTML load starts once it completes.
// The default environment keeps the user-data folder under the per-user
// local app data for unpackaged apps, so read-only install directories do
// not break the controller; the event handlers use generic lambdas because
// the wv2winrt-generated projection is not always include-ready.
void MainWindow::NavigateToEpubController(
    std::shared_ptr<EpubBook> book,
    std::vector<std::pair<int, std::wstring>> const& sections) {
  auto const weak = get_weak();
  auto html = BuildEpubHtml(*book, sections);

  auto const weak_book = std::weak_ptr<EpubBook>(book);
  book->web.EnsureCoreWebView2Async().Completed(
      [weak, weak_book, html = std::move(html)](
          winrt::Windows::Foundation::IAsyncAction const& sender,
          winrt::Windows::Foundation::AsyncStatus) {
        auto window = weak.get();
        auto locked_book = weak_book.lock();
        if (!window || !locked_book || !locked_book->web) return;
        try {
          sender.GetResults();
        } catch (winrt::hresult_error const& e) {
          window->Alert(
              pdfgist::Trf("ui.pages.edit-failed", std::wstring(e.message())));
          return;
        } catch (...) {
          return;
        }
        auto const webview = locked_book->web.CoreWebView2();
        if (!webview) return;
        // selection + chapter-tracking bridge (window.chrome.webview)
        webview.WebMessageReceived(
            [weak, weak_book](winrt::Windows::Foundation::IInspectable const&,
                              auto const& args) {
              if (auto current = weak.get()) {
                if (auto current_book = weak_book.lock()) {
                  current->OnEpubWebMessage(*current_book, args);
                }
              }
            });
        // NavigateToString's limit is 2 MB; books beyond that would need a
        // virtual host mapping — a documented later change.
        webview.NavigateToString(html);
      });
}

void MainWindow::OnEpubWebMessage(
    EpubBook& book,
    winrt::Microsoft::Web::WebView2::Core::CoreWebView2WebMessageReceivedEventArgs const&
        args) {
  // The page posts JSON strings: {kind:'selection'|'chapter', ...}
  try {
    auto const message = args.WebMessageAsJson();
    auto const json = std::wstring(message);
    auto const find_string_field = [&json](wchar_t const* key) -> std::wstring {
      // tiny extractor for machine-generated {"key":"value"} pairs
      std::wstring const needle = std::wstring(L"\"") + key + L"\":\"";
      std::size_t const at = json.find(needle);
      if (at == std::wstring::npos) return {};
      std::size_t const start = at + needle.size();
      std::size_t cursor = start;
      while (cursor < json.size()) {
        if (json[cursor] == L'\\') {
          cursor += 2;
          continue;
        }
        if (json[cursor] == L'"') break;
        ++cursor;
      }
      std::wstring raw = json.substr(start, cursor - start);
      // minimal unescape
      std::wstring out;
      for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\\' && i + 1 < raw.size()) {
          ++i;
          switch (raw[i]) {
            case L'n': out += L'\n'; break;
            case L't': out += L'\t'; break;
            default: out += raw[i];
          }
        } else {
          out += raw[i];
        }
      }
      return out;
    };
    auto const kind = find_string_field(L"kind");
    if (kind == L"selection") {
      book.selection_text = find_string_field(L"text");
      return;
    }
    if (kind == L"chapter") {
      // {"kind":"chapter","chapter":N}
      std::size_t const at = json.find(L"\"chapter\":");
      if (at != std::wstring::npos) {
        int const chapter = std::wcstol(json.c_str() + at + 10, nullptr, 10);
        if (chapter >= 1 && chapter <= std::max(1, book.chapters) &&
            chapter != book.current_chapter) {
          book.current_chapter = chapter;
          NoteActivity();
          if (left_mode_ == 1 && epub_active_) RenderEpubToc(book);
        }
      }
      return;
    }
  } catch (...) {
  }
}

// ---- presentation ----------------------------------------------------------------------

void MainWindow::ApplyEpubTheme(EpubBook& book) {
  if (!book.web || !book.web.CoreWebView2()) return;
  std::wstring const theme =
      Root().ActualTheme() == mux::ElementTheme::Dark ? L"dark" : L"light";
  auto const script = L"document.documentElement.setAttribute('data-theme', " +
                      js_string(theme) + L");";
  try {
    book.web.CoreWebView2().ExecuteScriptAsync(script);
  } catch (...) {
  }
}

void MainWindow::ApplyEpubFontScale(EpubBook& book) {
  if (!book.web || !book.web.CoreWebView2()) return;
  auto const script =
      L"document.documentElement.style.setProperty('--fs', " +
      std::to_wstring(book.font_scale) + L");";
  try {
    book.web.CoreWebView2().ExecuteScriptAsync(script);
  } catch (...) {
  }
}

void MainWindow::GoToChapter(EpubBook& book, int chapter) {
  book.current_chapter = std::clamp(chapter, 1, std::max(1, book.chapters));
  if (!book.web || !book.web.CoreWebView2()) return;
  auto const script =
      L"(() => { const els = document.querySelectorAll('.epub-chapter');"
      L" const el = els[" +
      std::to_wstring(book.current_chapter - 1) +
      L"]; if (el) el.scrollIntoView({ block: 'start' }); })()";
  try {
    book.web.CoreWebView2().ExecuteScriptAsync(script);
  } catch (...) {
  }
  NoteActivity();
}

// Whole-book search over the prefetched chapter texts (mac runEpubFind).
void MainWindow::RunEpubFind(std::wstring const& query) {
  if (!epub_) return;
  auto& book = *epub_;
  epub_hits_.clear();
  if (query.empty()) {
    FindCount().Text(L"");
    ClearEpubHighlights();
    return;
  }
  std::wstring const needle = query;
  auto const lower_of = [](std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(::towlower(c));
    return text;
  };
  std::wstring const haystack_needle = lower_of(needle);
  for (int i = 1; i <= std::max(1, book.chapters); ++i) {
    auto const found = book.text_cache.find(i);
    if (found == book.text_cache.end()) continue;
    std::wstring const text = lower_of(found->second);
    std::size_t at = 0;
    int ordinal = 0;
    while ((at = text.find(haystack_needle, at)) != std::wstring::npos) {
      epub_hits_.push_back({i, ordinal});
      ++ordinal;
      at += haystack_needle.size();
    }
  }
  FindCount().Text(epub_hits_.empty()
                       ? L"0"
                       : L"1/" + std::to_wstring(epub_hits_.size()));
  epub_find_index_ = 0;
  if (!epub_hits_.empty()) {
    HighlightEpubHit(epub_hits_.front().first, epub_hits_.front().second, needle);
  }
}

void MainWindow::NavigateEpubFind(int step) {
  if (!epub_ || epub_hits_.empty()) return;
  epub_find_index_ = (epub_find_index_ + step +
                      static_cast<int>(epub_hits_.size())) %
                     static_cast<int>(epub_hits_.size());
  auto const& hit = epub_hits_[static_cast<std::size_t>(epub_find_index_)];
  FindCount().Text(std::to_wstring(epub_find_index_ + 1) + L"/" +
                   std::to_wstring(epub_hits_.size()));
  HighlightEpubHit(hit.first, hit.second, std::wstring(FindInput().Text()));
}

void MainWindow::HighlightEpubHit(int chapter, int ordinal,
                                  std::wstring const& query) {
  if (!epub_) return;
  auto& book = *epub_;
  GoToChapter(book, chapter);
  if (!book.web || !book.web.CoreWebView2()) return;
  auto const script =
      L"(() => { const els = document.querySelectorAll('.epub-chapter');"
      L" const root = els[" +
      std::to_wstring(chapter - 1) +
      L"]; if (!root) return; clearMarks(root);"
      L" markOccurrence(root, " +
      js_string(query) + L", " + std::to_wstring(ordinal) +
      L");"
      L" const mark = root.querySelector('mark.epub-hit');"
      L" if (mark) mark.scrollIntoView({ block: 'center' }); })()";
  try {
    book.web.CoreWebView2().ExecuteScriptAsync(script);
  } catch (...) {
  }
}

void MainWindow::ClearEpubHighlights() {
  if (!epub_ || !epub_->web || !epub_->web.CoreWebView2()) return;
  try {
    epub_->web.CoreWebView2().ExecuteScriptAsync(
        L"document.querySelectorAll('.epub-chapter').forEach(el => clearMarks(el));");
  } catch (...) {
  }
}

// ---- TOC (left panel) ---------------------------------------------------------------------

void MainWindow::RenderEpubToc(EpubBook& book) {
  auto const host = LeftContent();

  // recents stay in the welcome overlay + 打开最近 flyout — TOC only here
  auto content = muxc::StackPanel();
  content.Spacing(1);
  if (book.toc.empty()) {
    auto empty = muxc::TextBlock();
    empty.Text(pdfgist::Tr("ui.epub.toc-empty"));
    empty.FontSize(11);
    empty.Opacity(0.5);
    empty.Margin({4, 8, 0, 0});
    content.Children().Append(empty);
    for (int i = 1; i <= std::max(1, book.chapters); ++i) {
      rivet_app::EpubTocItem chapter_item{};
      chapter_item.title = to_utf8(
          pdfgist::Trf("ui.epub.chapter-n", std::to_wstring(i)));
      chapter_item.chapter = i;
      chapter_item.level = 0;
      book.toc.push_back(chapter_item);
    }
  }
  auto const weak = get_weak();
  for (auto const& toc_item : book.toc) {
    auto button = muxc::Button();
    button.Padding({2, 4, 4, 4});
    button.MinWidth(0);
    button.MinHeight(0);
    button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    button.Margin({static_cast<double>(10 + toc_item.level * 12), 0, 0, 0});
    muxc::TextBlock label;
    label.Text(to_wide(toc_item.title));
    label.FontSize(12);
    label.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    button.Content(label);
    int const chapter = static_cast<int>(toc_item.chapter);
    button.Foreground(ThemeBrush(
        book.current_chapter == chapter
            ? wu::Color{0xFF, 0xB4, 0x53, 0x09}
            : (Root().ActualTheme() == mux::ElementTheme::Dark
                   ? wu::Color{0xFF, 0xE7, 0xE5, 0xE4}
                   : wu::Color{0xFF, 0x29, 0x25, 0x24})));
    button.Click([weak, chapter](winrt::Windows::Foundation::IInspectable const&,
                                 mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) {
        if (window->epub_) {
          window->GoToChapter(*window->epub_, chapter);
        }
      }
    });
    content.Children().Append(button);
  }

  host.Children().Clear();
  host.Children().Append(content);
}

// ---- html ----------------------------------------------------------------------------------

std::wstring MainWindow::BuildEpubHtml(
    EpubBook const& book,
    std::vector<std::pair<int, std::wstring>> const& sections) {
  // v1 READING_CSS typography + theme; the marks helpers power whole-book
  // search; the bridge posts to window.chrome.webview (WebView2).
  std::wstring html = L"<!doctype html><html><head><meta charset=\"utf-8\">";
  html += LR"(
<style>
:root { --fs: 1; }
:root[data-theme="light"] { --fg: #292524; --bg: #F5F2ED; }
:root[data-theme="dark"] { --fg: #D6D3D1; --bg: #171412; }
html { background: var(--bg); }
body { margin: 0; padding: 0 0 40vh; color: var(--fg);
  font-family: Georgia, "Source Han Serif SC", "Noto Serif SC", "Songti SC", serif;
  font-size: calc(16px * var(--fs)); line-height: 1.9; }
p { margin: 0 0 1em; text-align: justify; }
h1, h2, h3, h4 { line-height: 1.35; margin: 1.4em 0 0.6em; }
img, svg { max-width: 100%; height: auto; }
blockquote { margin: 1em 0; padding-left: 1em; border-left: 3px solid #8884; }
a { color: inherit; pointer-events: none; }
.epub-book-title { padding: 18vh 8vw 10vh; text-align: center; }
.book-title-text { font-size: 2em; font-weight: 700; }
.epub-chapter { padding: 0 8vw; }
mark.epub-hit { background: #FFD500; color: inherit; }
</style>
<script>
function clearMarks(root) {
  const marks = root.querySelectorAll('mark.epub-hit');
  marks.forEach(m => {
    const parent = m.parentNode;
    parent.replaceChild(document.createTextNode(m.textContent), m);
    parent.normalize();
  });
}
function markOccurrence(root, needle, ordinal) {
  let count = 0;
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const nodes = [];
  while (walker.nextNode()) nodes.push(walker.currentNode);
  for (const node of nodes) {
    const lower = node.nodeValue.toLowerCase();
    let from = 0, at;
    while ((at = lower.indexOf(needle.toLowerCase(), from)) !== -1) {
      if (count === ordinal) {
        const range = document.createRange();
        range.setStart(node, at);
        range.setEnd(node, at + needle.length);
        const mark = document.createElement('mark');
        mark.className = 'epub-hit';
        try { range.surroundContents(mark); } catch (e) {}
        return;
      }
      count += 1;
      from = at + needle.length;
    }
  }
}
function post(message) {
  if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage(message);
}
window.addEventListener('mouseup', () => {
  const sel = window.getSelection();
  if (!sel || sel.isCollapsed || sel.rangeCount === 0) return;
  const text = sel.toString().trim();
  if (text.length < 2) return;
  post(JSON.stringify({ kind: 'selection', text: text.slice(0, 800) }));
});
window.addEventListener('scroll', () => {
  const sections = document.querySelectorAll('.epub-chapter');
  const line = window.scrollY + window.innerHeight * 0.35;
  let current = 1;
  sections.forEach((el, i) => { if (el.offsetTop <= line) current = i + 1; });
  post(JSON.stringify({ kind: 'chapter', chapter: current }));
});
</script>
</head><body>)";
  html += L"<div class=\"epub-book-title\"><div class=\"book-title-text\">" +
          escape_html(book.title) + L"</div></div>";
  for (auto const& [index, body] : sections) {
    (void)index;
    html += L"<div class=\"epub-chapter\">" + body + L"</div>";
  }
  html += L"</body></html>";
  return html;
}

}  // namespace winrt::RivetHost::implementation

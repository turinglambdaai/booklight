// EPUB over WebKitGTK — see epub.hpp.
#include "epub.hpp"

#ifdef PDFGIST_HAVE_WEBKIT

#include <jsc/jsc.h>
#include <json-glib/json-glib.h>
// WebKitGTK 2.46+ installs the umbrella as webkit/webkit.h; older 6.0
// packages (2.40-2.44) shipped webkit/WebKit.h.
#if __has_include(<webkit/webkit.h>)
#include <webkit/webkit.h>
#else
#include <webkit/WebKit.h>
#endif

#include <algorithm>
#include <thread>

#include "host.hpp"

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

std::string escape_html(std::string const& s) {
  std::string out;
  out.reserve(s.size());
  for (char const c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string js_string(std::string const& s) {
  std::string out = "\"";
  for (char const c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  out += "\"";
  return out;
}

// v1 READING_CSS typography + theme; the marks helpers power search.
char const* kReadingCss = R"CSS(
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
)CSS";

char const* kBridgeScript = R"JS(
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
window.addEventListener('mouseup', () => {
  const sel = window.getSelection();
  if (!sel || sel.isCollapsed || sel.rangeCount === 0) return;
  const text = sel.toString().trim();
  if (text.length < 2) return;
  window.webkit.messageHandlers.bridge.postMessage(
    JSON.stringify({ kind: 'selection', text: text.slice(0, 800) }));
});
window.addEventListener('scroll', () => {
  const sections = document.querySelectorAll('.epub-chapter');
  const line = window.scrollY + window.innerHeight * 0.35;
  let current = 1;
  sections.forEach((el, i) => { if (el.offsetTop <= line) current = i + 1; });
  window.webkit.messageHandlers.bridge.postMessage(
    JSON.stringify({ kind: 'chapter', chapter: current }));
});
)JS";

}  // namespace

struct EpubTab::Impl {
  explicit Impl(EpubTab* owner) : tab(owner) {}
  EpubTab* tab;
  std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
  WebKitUserContentManager* ucm{nullptr};
  gulong message_handler_id{0};

  static void script_message(WebKitUserContentManager*, JSCValue* value,
                             gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    if (value == nullptr || !jsc_value_is_string(value)) return;
    char* raw = jsc_value_to_string(value);
    if (raw == nullptr) return;
    std::string const json = raw;
    g_free(raw);
    JsonParser* parser = json_parser_new();
    GError* error = nullptr;
    json_parser_load_from_data(parser, json.c_str(),
                               static_cast<gssize>(json.size()), &error);
    if (error != nullptr) {
      g_error_free(error);
      g_object_unref(parser);
      return;
    }
    JsonObject* obj = json_node_get_object(json_parser_get_root(parser));
    std::string const kind = obj != nullptr && json_object_has_member(obj, "kind")
                                 ? json_object_get_string_member(obj, "kind")
                                 : "";
    if (kind == "selection") {
      std::string const text =
          json_object_get_string_member(obj, "text");
      self->tab->selection_ = text;
    } else if (kind == "chapter") {
      int const chapter = static_cast<int>(
          json_object_get_int_member(obj, "chapter"));
      if (chapter >= 1 && chapter <= self->tab->chapters_ &&
          chapter != self->tab->current_chapter_) {
        self->tab->current_chapter_ = chapter;
        if (self->tab->on_chapter_changed) {
          self->tab->on_chapter_changed(chapter);
        }
      }
    }
    g_object_unref(parser);
  }

  void evaluate(std::string const& script) const {
    webkit_web_view_evaluate_javascript(
        WEBKIT_WEB_VIEW(tab->webview_), script.c_str(),
        static_cast<gssize>(script.size()), nullptr, nullptr, nullptr,
        nullptr, nullptr);
  }
};

EpubTab::EpubTab(PanelContext* ctx, std::string path, std::string title)
    : impl(std::make_unique<Impl>(this)),
      ctx_(ctx),
      path_(std::move(path)),
      title_(std::move(title)) {
  root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  impl->ucm = webkit_user_content_manager_new();
  webkit_user_content_manager_register_script_message_handler(impl->ucm,
                                                              "bridge");
  impl->message_handler_id = hook(
      impl->ucm, "script-message-received::bridge",
      Impl::script_message, impl.get());
  webview_ = GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                     "user-content-manager", impl->ucm,
                                     nullptr));
  // The page paints its own background; avoid the white flash and honor
  // the reading theme behind it.
  GdkRGBA transparent = {1.0, 1.0, 1.0, 0.0};
  webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(webview_),
                                       &transparent);
  WebKitSettings* settings =
      webkit_web_view_get_settings(WEBKIT_WEB_VIEW(webview_));
  webkit_settings_set_enable_javascript(settings, TRUE);
  webkit_settings_set_enable_webaudio(settings, FALSE);
  webkit_settings_set_enable_media_stream(settings, FALSE);
  webkit_settings_set_enable_mediasource(settings, FALSE);
  gtk_box_append(GTK_BOX(root_), webview_);
  gtk_widget_set_vexpand(webview_, TRUE);
}

EpubTab::~EpubTab() {
  if (impl->message_handler_id != 0) {
    g_signal_handler_disconnect(impl->ucm, impl->message_handler_id);
    impl->message_handler_id = 0;
  }
}

std::string EpubTab::chapter_text(int chapter) const {
  auto it = text_cache_.find(chapter);
  return it != text_cache_.end() ? it->second : std::string{};
}

void EpubTab::load() {
  std::string const path = path_;
  rivet_app::API* api = ctx_->api;
  if (api == nullptr) {
    if (on_load_failed) on_load_failed("backend not ready");
    return;
  }
  auto alive = alive_;
  // Whole-book assembly runs on a worker: the serial chapter loop blocks
  // on N RPCs, and embedded-channel requests must stay serial (mac
  // prefetchEpubText comment). Every UI touch happens on the main loop.
  std::thread([this, alive, api, path]() {
    std::string error;
    rivet_app::EpubView view;
    std::vector<std::string> sections;
    std::map<int, std::string> texts;
    bool failed = false;
    try {
      view = api->epub_open(path).get();
      sections.push_back(
          "<div class=\"epub-book-title\"><div class=\"book-title-text\">" +
          escape_html(view.title) + "</div></div>");
      int const count = static_cast<int>(std::max<std::int64_t>(
          view.chapters, 1));
      for (int i = 1; i <= count; ++i) {
        auto const body = api->epub_chapter_html(path, i).get();
        sections.push_back("<div class=\"epub-chapter\" data-chapter=\"" +
                           std::to_string(i) + "\">" +
                           std::string(body.begin(), body.end()) + "</div>");
        if (texts.find(i) == texts.end()) {
          texts[i] = api->epub_chapter_text(path, i).get();
        }
      }
    } catch (std::exception const& e) {
      error = e.what();
      failed = true;
    } catch (...) {
      error = "unknown backend failure";
      failed = true;
    }
    host::dispatch_ui([this, alive, view, sections, texts, error, failed]() {
      if (!*alive) return;  // tab destroyed while the worker ran
      if (failed) {
        if (on_load_failed) on_load_failed(error);
        return;
      }
      chapters_ = static_cast<int>(std::max<std::int64_t>(view.chapters, 1));
      toc_ = view.toc;
      text_cache_ = std::move(texts);
      std::string html = "<!doctype html><html data-theme=\"light\">"
                         "<head><meta charset=\"utf-8\"><style>";
      html += kReadingCss;
      html += "</style><script>";
      html += kBridgeScript;
      html += "</script></head><body>";
      for (auto const& section : sections) html += section;
      html += "</body></html>";
      webkit_web_view_load_html(WEBKIT_WEB_VIEW(webview_), html.c_str(),
                                nullptr);
      loaded_ = true;
      apply_font_scale(font_scale_);
      if (on_load_finished) on_load_finished({});
    });
  }).detach();
}

void EpubTab::go_to_chapter(int chapter) {
  if (chapters_ <= 0) return;
  current_chapter_ = std::clamp(chapter, 1, chapters_);
  impl->evaluate(
      "(() => { const el = document.querySelectorAll('.epub-chapter')[" +
      std::to_string(current_chapter_ - 1) +
      "]; if (el) el.scrollIntoView({ block: 'start' }); })()");
}

void EpubTab::apply_font_scale(double scale) {
  font_scale_ = scale;
  impl->evaluate("document.documentElement.style.setProperty('--fs', " +
                 std::to_string(scale) + ")");
}

void EpubTab::apply_theme(std::string const& theme) {
  impl->evaluate("document.documentElement.setAttribute('data-theme', " +
                 js_string(theme == "dark" ? "dark" : "light") + ")");
}

void EpubTab::highlight_hit(int chapter, int ordinal,
                            std::string const& query) {
  if (chapter > 0) go_to_chapter(chapter);
  impl->evaluate(
      std::string("(() => { const root = ") +
      "document.querySelectorAll('.epub-chapter')[" +
      std::to_string(std::max(chapter - 1, 0)) +
      "]; if (!root) return; clearMarks(root); markOccurrence(root, " +
      js_string(query) + ", " + std::to_string(std::max(ordinal, 0)) +
      "); const mark = root.querySelector('mark.epub-hit'); "
      "if (mark) mark.scrollIntoView({ block: 'center' }); })()");
}

}  // namespace pdfgist

#else  // !PDFGIST_HAVE_WEBKIT

namespace pdfgist {

// WebKitGTK-less builds keep the host functional without EPUB: opening an
// EPUB surfaces a readable message instead of a missing feature crash.
EpubTab::EpubTab(PanelContext*, std::string path, std::string title)
    : impl(nullptr), ctx_(nullptr), path_(std::move(path)),
      title_(std::move(title)) {
  root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  auto* label = gtk_label_new(
      "EPUB support is unavailable in this build: install WebKitGTK "
      "(webkitgtk-6.0) or use a full package.");
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand(label, TRUE);
  gtk_box_append(GTK_BOX(root_), label);
}

EpubTab::~EpubTab() = default;

std::string EpubTab::chapter_text(int) const { return {}; }

void EpubTab::highlight_hit(int, int, std::string const&) {}
void EpubTab::load() {
  if (on_load_failed) on_load_failed("WebKitGTK is not available");
}
void EpubTab::go_to_chapter(int) {}
void EpubTab::apply_font_scale(double) {}
void EpubTab::apply_theme(std::string const&) {}

}  // namespace pdfgist

#endif  // PDFGIST_HAVE_WEBKIT

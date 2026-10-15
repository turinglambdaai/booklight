// Annotation storage schema implementation — see annotations.hpp.
#include "annotations.hpp"

#include <json-glib/json-glib.h>

#include <charconv>
#include <cstdio>
#include <random>
#include <chrono>

namespace pdfgist {
namespace {

JsonArray* array_member(JsonObject* obj, char const* key) {
  if (obj == nullptr || !json_object_has_member(obj, key)) return nullptr;
  auto* node = json_object_get_member(obj, key);
  if (node == nullptr || json_node_get_node_type(node) != JSON_NODE_ARRAY) {
    return nullptr;
  }
  return json_node_get_array(node);
}

std::string string_member(JsonObject* obj, char const* key) {
  if (obj == nullptr || !json_object_has_member(obj, key)) return {};
  auto* node = json_object_get_member(obj, key);
  if (node == nullptr || json_node_get_node_type(node) != JSON_NODE_VALUE) {
    return {};
  }
  char const* value = json_node_get_string(node);
  return value != nullptr ? std::string(value) : std::string();
}

std::int64_t int_member(JsonObject* obj, char const* key) {
  if (obj == nullptr || !json_object_has_member(obj, key)) return 0;
  auto* node = json_object_get_member(obj, key);
  if (node == nullptr || json_node_get_node_type(node) != JSON_NODE_VALUE) {
    return 0;
  }
  return static_cast<std::int64_t>(json_node_get_int(node));
}

double double_member(JsonObject* obj, char const* key) {
  if (obj == nullptr || !json_object_has_member(obj, key)) return 0;
  auto* node = json_object_get_member(obj, key);
  if (node == nullptr || json_node_get_node_type(node) != JSON_NODE_VALUE) {
    return 0;
  }
  return json_node_get_double(node);
}

std::string json_escape(std::string const& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char const raw : text) {
    switch (raw) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(raw) < 0x20) {
          char escape[8];
          std::snprintf(escape, sizeof(escape), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(raw)));
          out += escape;
        } else {
          out += raw;  // UTF-8 passes through (valid JSON)
        }
        break;
    }
  }
  return out;
}

// Shortest round-trip representation (std::to_chars, no trailing zeros).
std::string number(double value) {
  char buffer[64];
  auto const result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

}  // namespace

DocData parse_doc_data(std::vector<std::uint8_t> const& bytes) {
  DocData data;
  if (bytes.empty()) return data;
  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  json_parser_load_from_data(parser,
                             reinterpret_cast<char const*>(bytes.data()),
                             static_cast<gssize>(bytes.size()), &error);
  if (error != nullptr) {
    g_error_free(error);
    g_object_unref(parser);
    return data;
  }
  JsonNode* root_node = json_parser_get_root(parser);
  JsonObject* root =
      root_node != nullptr && json_node_get_node_type(root_node) ==
                                  JSON_NODE_OBJECT
          ? json_node_get_object(root_node)
          : nullptr;
  if (root == nullptr) {
    g_object_unref(parser);
    return data;
  }

  if (JsonArray* annotations = array_member(root, "annotations")) {
    guint const count = json_array_get_length(annotations);
    for (guint i = 0; i < count; ++i) {
      auto* node = json_array_get_element(annotations, i);
      if (node == nullptr ||
          json_node_get_node_type(node) != JSON_NODE_OBJECT) {
        continue;
      }
      JsonObject* entry = json_node_get_object(node);
      StoredAnnotation annotation;
      annotation.id = string_member(entry, "id");
      annotation.page = static_cast<int>(int_member(entry, "page"));
      annotation.excerpt = string_member(entry, "excerpt");
      annotation.color = string_member(entry, "color");
      annotation.kind = string_member(entry, "kind");
      annotation.note = string_member(entry, "note");
      annotation.created = int_member(entry, "created");
      if (JsonArray* rects = array_member(entry, "rects")) {
        guint const rect_count = json_array_get_length(rects);
        for (guint k = 0; k < rect_count; ++k) {
          auto* rect_node = json_array_get_element(rects, k);
          if (rect_node == nullptr ||
              json_node_get_node_type(rect_node) != JSON_NODE_OBJECT) {
            continue;
          }
          JsonObject* rect = json_node_get_object(rect_node);
          annotation.rects.push_back(AnnoRect{double_member(rect, "x"),
                                              double_member(rect, "y"),
                                              double_member(rect, "width"),
                                              double_member(rect, "height")});
        }
      }
      if (!annotation.id.empty() && annotation.page > 0 &&
          !annotation.rects.empty()) {
        data.annotations.push_back(std::move(annotation));
      }
    }
  }
  if (JsonArray* bookmarks = array_member(root, "bookmarks")) {
    guint const count = json_array_get_length(bookmarks);
    for (guint i = 0; i < count; ++i) {
      auto* node = json_array_get_element(bookmarks, i);
      if (node == nullptr ||
          json_node_get_node_type(node) != JSON_NODE_OBJECT) {
        continue;
      }
      JsonObject* entry = json_node_get_object(node);
      StoredBookmark bookmark;
      bookmark.page = static_cast<int>(int_member(entry, "page"));
      bookmark.label = string_member(entry, "label");
      bookmark.created = int_member(entry, "created");
      if (bookmark.page > 0) {
        data.bookmarks.push_back(std::move(bookmark));
      }
    }
  }
  g_object_unref(parser);
  return data;
}

std::vector<std::uint8_t> encode_doc_data(DocData const& data) {
  std::string out;
  out.reserve(256 + data.annotations.size() * 128 +
              data.bookmarks.size() * 48);
  out += "{\"annotations\":[";
  bool first = true;
  for (auto const& annotation : data.annotations) {
    if (!first) out += ",";
    first = false;
    out += "{\"id\":\"" + json_escape(annotation.id) + "\"";
    out += ",\"page\":" + std::to_string(annotation.page);
    out += ",\"rects\":[";
    for (std::size_t k = 0; k < annotation.rects.size(); ++k) {
      auto const& rect = annotation.rects[k];
      if (k != 0) out += ",";
      out += "{\"x\":" + number(rect.x);
      out += ",\"y\":" + number(rect.y);
      out += ",\"width\":" + number(rect.width);
      out += ",\"height\":" + number(rect.height);
      out += "}";
    }
    out += "]";
    out += ",\"excerpt\":\"" + json_escape(annotation.excerpt) + "\"";
    out += ",\"color\":\"" + json_escape(annotation.color) + "\"";
    out += ",\"kind\":\"" + json_escape(annotation.kind) + "\"";
    out += ",\"note\":\"" + json_escape(annotation.note) + "\"";
    out += ",\"created\":" + std::to_string(annotation.created);
    out += "}";
  }
  out += "],\"bookmarks\":[";
  first = true;
  for (auto const& bookmark : data.bookmarks) {
    if (!first) out += ",";
    first = false;
    out += "{\"page\":" + std::to_string(bookmark.page);
    out += ",\"label\":\"" + json_escape(bookmark.label) + "\"";
    out += ",\"created\":" + std::to_string(bookmark.created);
    out += "}";
  }
  out += "]}";
  return std::vector<std::uint8_t>(out.begin(), out.end());
}

AnnoRect anno_rect_from_text_rect(double left, double top, double right,
                                  double bottom) {
  // PDFium user space (y-up, top > bottom) is already the stored space:
  // y is the bottom edge, height spans up to `top`.
  return AnnoRect{left, bottom, right - left, top - bottom};
}

void annotation_rgba(std::string const& name, double* r, double* g, double* b,
                     double* a) {
  double red = 0xFF, green = 0xD5, blue = 0x00, alpha = 0.9;  // yellow
  if (name == "green") {
    red = 0x4C;
    green = 0xC3;
    blue = 0x83;
    alpha = 0.85;
  } else if (name == "blue") {
    red = 0x56;
    green = 0x9C;
    blue = 0xFF;
    alpha = 0.85;
  }
  if (r != nullptr) *r = red / 255.0;
  if (g != nullptr) *g = green / 255.0;
  if (b != nullptr) *b = blue / 255.0;
  if (a != nullptr) *a = alpha;
}

std::string make_annotation_id() {
  auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now()
                               .time_since_epoch())
                           .count();
  static thread_local std::mt19937 random(
      static_cast<unsigned>(seconds) ^
      (static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(&random)) << 17));
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "a-%lld-%04d",
                static_cast<long long>(seconds),
                static_cast<int>(random() % 10000));
  return buffer;
}

}  // namespace pdfgist

#pragma once

// i18n.h — host-side access to the shared i18n source for the GTK host.
//
// The string tables live in I18nGenerated.h (generated from
// shared/i18n/{zh,en}.json — the single source; never edit by hand).
// zh is the catalog default; the language is detected once at startup from
// the process environment (LC_ALL / LC_MESSAGES / LANG) and mirrored into
// the backend via set-locale during bootstrap. Unknown keys return an empty
// string rather than throwing — UI text is never worth a crash.
//
// Tr() also swaps the mac-only ⌘ symbol for "Ctrl+" so keyboard hints stay
// readable on Linux without forking the shared catalog.

#include <cstring>
#include <string>
#include <string_view>

#include "I18nGenerated.h"

namespace pdfgist {

enum class Lang {
  Zh,
  En,
};

inline Lang g_ui_lang = Lang::Zh;

inline void SetUiLanguage(std::string const& code) {
  g_ui_lang = code == "en" ? Lang::En : Lang::Zh;
}

// "zh" | "en" — mirrors the UI language choice into set-locale.
inline char const* UiLanguageCode() {
  return g_ui_lang == Lang::En ? "en" : "zh";
}

inline std::string Ctrlize(std::string text) {
  std::string const needle = "\u2318";  // ⌘
  std::string const replacement = "Ctrl+";
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), replacement);
    at += replacement.size();
  }
  return text;
}

inline std::string_view Lookup(char const* key) {
  for (auto const& entry : kLinuxStrings) {
    if (std::strcmp(entry.key, key) == 0) {
      auto const& column =
          (g_ui_lang == Lang::En && entry.en != nullptr && *entry.en != '\0')
              ? entry.en
              : entry.zh;
      return std::string_view(column);
    }
  }
  return std::string_view("");
}

inline std::string Tr(char const* key) { return Ctrlize(std::string(Lookup(key))); }

// Sequential "{0}"-style substitution.
template <typename... Args>
std::string Trf(char const* key, Args const&... args) {
  std::string text = Tr(key);
  std::string const expansions[] = {std::string(args)...};
  for (std::size_t i = 0; i < sizeof...(args); ++i) {
    std::string const token = "{" + std::to_string(i) + "}";
    std::size_t at = 0;
    while ((at = text.find(token, at)) != std::string::npos) {
      text.replace(at, token.size(), expansions[i]);
      at += expansions[i].size();
    }
  }
  return text;
}

}  // namespace pdfgist

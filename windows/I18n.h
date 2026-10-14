#pragma once

// I18n.h — host-side access to the shared i18n source for the WinUI host.
//
// The string tables live in I18nGenerated.h (generated from
// shared/i18n/{zh,en}.json — the single source; never edit by hand).
// zh is the default, matching the v1 UI; the language flips once during
// bootstrap from the system UI language and mirrors the choice into the
// backend via set-locale. Unknown keys return an empty string rather than
// throwing — UI text is never worth a crash.
//
// Tr() also swaps the mac-only ⌘ symbol for "Ctrl+" so keyboard hints stay
// readable on Windows without forking the shared catalog.

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

// "zh" | "en" — mirrors the L10n.language choice into set-locale.
inline char const* UiLanguageCode() {
  return g_ui_lang == Lang::En ? "en" : "zh";
}

inline std::wstring Winize(std::wstring text) {
  std::wstring const needle = L"\u2318";  // ⌘
  std::wstring const replacement = L"Ctrl+";
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::wstring::npos) {
    text.replace(at, needle.size(), replacement);
    at += replacement.size();
  }
  return text;
}

inline std::wstring_view Lookup(char const* key) {
  for (auto const& entry : kWinStrings) {
    if (std::strcmp(entry.key, key) == 0) {
      auto const& column =
          (g_ui_lang == Lang::En && entry.en != nullptr && *entry.en != L'\0')
              ? entry.en
              : entry.zh;
      return std::wstring_view(column);
    }
  }
  return std::wstring_view(L"");
}

inline std::wstring Tr(char const* key) { return Winize(std::wstring(Lookup(key))); }

// Sequential "{0}"-style substitution.
template <typename... Args>
std::wstring Trf(char const* key, Args const&... args) {
  std::wstring text = Tr(key);
  std::wstring const expansions[] = {std::wstring(args)...};
  for (std::size_t i = 0; i < sizeof...(args); ++i) {
    std::wstring const token = L"{" + std::to_wstring(i) + L"}";
    std::size_t at = 0;
    while ((at = text.find(token, at)) != std::wstring::npos) {
      text.replace(at, token.size(), expansions[i]);
      at += expansions[i].size();
    }
  }
  return text;
}

}  // namespace pdfgist

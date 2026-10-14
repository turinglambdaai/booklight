#!/usr/bin/env node
// Generate windows/I18nGenerated.h from the shared/i18n single source
// (zh.json / en.json) — the Windows counterpart of gen-swift-strings.mjs.
// Run from the repo root:
//   node scripts/gen-win-strings.mjs            (write)
//   node scripts/gen-win-strings.mjs --check    (verify up to date)
import { readFileSync, writeFileSync } from "node:fs";

const check = process.argv.includes("--check");

const zh = JSON.parse(readFileSync("shared/i18n/zh.json", "utf8"));
const en = JSON.parse(readFileSync("shared/i18n/en.json", "utf8"));
const keys = Object.keys(zh);
const missing = keys.filter((k) => !(k in en));
if (missing.length > 0) {
  console.error(`en.json is missing keys: ${missing.join(", ")}`);
  process.exit(1);
}

const escWide = (s) =>
  String(s)
    .replace(/\\/g, "\\\\")
    .replace(/"/g, '\\"')
    .replace(/\n/g, "\\n")
    .replace(/\r/g, "\\r")
    .replace(/\t/g, "\\t");

const rows = keys
  .map(
    (k) =>
      `    {"${escWide(k)}", L"${escWide(String(zh[k]))}", L"${escWide(String(en[k]))}"},`)
  .join("\n");

const out = `// Generated from shared/i18n/{zh,en}.json — the single i18n source.
// Do not edit; run: node scripts/gen-win-strings.mjs
#pragma once

#include <string_view>

namespace pdfgist {

inline constexpr struct WinStringEntry {
  char const* key;
  wchar_t const* zh;
  wchar_t const* en;
} kWinStrings[] = {
${rows}
};

}  // namespace pdfgist
`;

if (check) {
  const current = readFileSync("windows/I18nGenerated.h", "utf8");
  if (current !== out) {
    console.error(
      "windows/I18nGenerated.h is stale — run: node scripts/gen-win-strings.mjs");
    process.exit(1);
  }
  console.log(`I18nGenerated.h up to date (${keys.length} keys)`);
} else {
  writeFileSync("windows/I18nGenerated.h", out);
  console.log(`windows/I18nGenerated.h: ${keys.length} keys`);
}

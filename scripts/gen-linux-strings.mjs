#!/usr/bin/env node
// Generate linux/I18nGenerated.h from the shared/i18n single source
// (zh.json / en.json) — the Linux counterpart of gen-win-strings.mjs.
// Linux strings are narrow UTF-8 (GTK expects UTF-8), so no wide-char
// conversion happens here — the catalog is embedded verbatim.
// Run from the repo root:
//   node scripts/gen-linux-strings.mjs            (write)
//   node scripts/gen-linux-strings.mjs --check    (verify up to date)
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

const esc = (s) =>
  String(s)
    .replace(/\\/g, "\\\\")
    .replace(/"/g, '\\"')
    .replace(/\n/g, "\\n")
    .replace(/\r/g, "\\r")
    .replace(/\t/g, "\\t");

const rows = keys
  .map(
    (k) =>
      `    {"${esc(k)}", "${esc(String(zh[k]))}", "${esc(String(en[k]))}"},`)
  .join("\n");

const out = `// Generated from shared/i18n/{zh,en}.json — the single i18n source.
// Do not edit; run: node scripts/gen-linux-strings.mjs
#pragma once

#include <cstring>
#include <string_view>

namespace pdfgist {

inline constexpr struct LinuxStringEntry {
  char const* key;
  char const* zh;
  char const* en;
} kLinuxStrings[] = {
${rows}
};

}  // namespace pdfgist
`;

if (check) {
  const current = readFileSync("linux/I18nGenerated.h", "utf8");
  if (current !== out) {
    console.error(
      "linux/I18nGenerated.h is stale — run: node scripts/gen-linux-strings.mjs");
    process.exit(1);
  }
  console.log(`I18nGenerated.h up to date (${keys.length} keys)`);
} else {
  writeFileSync("linux/I18nGenerated.h", out);
  console.log(`linux/I18nGenerated.h: ${keys.length} keys`);
}

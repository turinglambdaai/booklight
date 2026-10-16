#!/usr/bin/env bash
# Declare the app bundle's localizations (zh-Hans + en) so AppKit renders
# system-provided UI — About / Hide / Quit / Close / Enter Full Screen and
# friends — in the user's language instead of the development-region
# fallback. Without this, a zh system sees a mixed-language menu bar:
# in-app strings follow the user language while the AppKit boilerplate
# stays English.
#
# raco rivet package writes a fixed Info.plist (no CFBundleLocalizations),
# so this runs as a packaging post-step: stage the lproj markers, inject
# the key, then re-sign exactly like the framework's ad-hoc pass (same
# identity, same entitlements; production notarization would need a rerun,
# so this step belongs only to the ad-hoc release job).
set -euo pipefail

APP="${1:?usage: localize-app.sh <app-bundle>}"
PLIST="$APP/Contents/Info.plist"
RES="$APP/Contents/Resources"

[[ -d "$APP" ]] || { echo "no app bundle: $APP" >&2; exit 1; }
[[ -f "$PLIST" ]] || { echo "no Info.plist in $APP" >&2; exit 1; }

for lang in zh-Hans en; do
  mkdir -p "$RES/$lang.lproj"
  printf 'CFBundleDisplayName = "Booklight";\n' > "$RES/$lang.lproj/InfoPlist.strings"
done

LOCALIZATIONS_XML='<array><string>zh-Hans</string><string>en</string></array>'
/usr/bin/plutil -replace CFBundleLocalizations -xml "$LOCALIZATIONS_XML" "$PLIST"

# Re-sign the outer bundle only — the nested Racket.framework signature is
# untouched by this edit. Extract the existing entitlements so none are
# silently dropped by the re-sign.
ENT="$(mktemp)"
if /usr/bin/codesign -d --entitlements - --xml "$APP" > "$ENT" 2>/dev/null && [[ -s "$ENT" ]]; then
  /usr/bin/codesign --force --sign - --entitlements "$ENT" "$APP"
else
  /usr/bin/codesign --force --sign - "$APP"
fi
rm -f "$ENT"

/usr/bin/codesign --verify "$APP"
echo "localized (zh-Hans, en) + re-signed: $APP"

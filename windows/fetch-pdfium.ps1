# Fetch the pinned PDFium binaries for the Windows host.
#
# Downloads the matching pdfium-win-<arch>.tgz from bblanchon/pdfium-binaries,
# verifies its SHA-256 against the pin below, and extracts bin\pdfium.dll,
# lib\pdfium.dll.lib and include\*.h into <OutputRoot>\<version>\<platform>.
# Nothing is committed to git: the cache lives under windows\build (gitignored)
# and the download is re-fetched on a fresh checkout or CI runner.
#
# Usage (normally invoked by the FetchPdfium target in RivetHost.vcxproj):
#   powershell -NoProfile -ExecutionPolicy Bypass -File fetch-pdfium.ps1 `
#     -Platform x64 -OutputRoot <dir>
param(
  [Parameter(Mandatory = $true)][ValidateSet("x64", "ARM64", "Win32")]
  [string]$Platform,
  [Parameter(Mandatory = $true)][string]$OutputRoot
)

$ErrorActionPreference = "Stop"

$PdfiumVersion = "chromium/8100"
# sha256(pdfium-win-<arch>.tgz) at the pinned tag, verified at pin time.
$PinnedSha256 = @{
  "x64"   = "51621bcc1d9be9ea448f4b84b0516862b928e3968d3fca899af1a6d6e2848d6d"
  "ARM64" = "cca9b28d406ae2a902b0b710a92ab699c4c2931564ab6efe44630014d492a9ca"
}

$arch = if ($Platform -eq "Win32") { "x86" } else { $Platform.ToLowerInvariant() }
if (-not $PinnedSha256.ContainsKey($arch.ToUpperInvariant())) {
  throw "No pinned PDFium sha256 for platform '$Platform'"
}
$expected = $PinnedSha256[$arch.ToUpperInvariant()]

$versionDir = $PdfiumVersion.Replace("/", "-")
$target = Join-Path $OutputRoot (Join-Path $versionDir $arch)
$stamp = Join-Path $target ".stamp"
if (Test-Path $stamp) {
  Write-Host "PDFium $PdfiumVersion ($arch) already present: $target"
  exit 0
}

$asset = "pdfium-win-$arch.tgz"
$baseUrl = "https://github.com/bblanchon/pdfium-binaries/releases/download/$PdfiumVersion"

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("pdfium-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
try {
  $tgz = Join-Path $tmp $asset
  Write-Host "Downloading $baseUrl/$asset"
  # Retries: transient release-CDN hiccups must not fail the whole build.
  $attempt = 0
  while ($true) {
    try {
      Invoke-WebRequest -Uri "$baseUrl/$asset" -OutFile $tgz -MaximumRedirection 10 -ErrorAction Stop
      break
    } catch {
      $attempt++
      if ($attempt -ge 3) { throw }
      Write-Host "Download failed (attempt $attempt), retrying..."
      Start-Sleep -Seconds (3 * $attempt)
    }
  }

  $actual = (Get-FileHash -Path $tgz -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $expected) {
    throw ("sha256 mismatch for {0}: expected {1}, got {2}" -f $asset, $expected, $actual)
  }

  if (Test-Path $target) { Remove-Item -Recurse -Force $target }
  New-Item -ItemType Directory -Path $target -Force | Out-Null
  # bsdtar ships with Windows 10 1803+; the runners have it.
  tar -xzf $tgz -C $target
  if ($LASTEXITCODE -ne 0) { throw "tar extraction failed with exit code $LASTEXITCODE" }

  foreach ($required in @("bin\pdfium.dll", "lib\pdfium.dll.lib", "include\fpdfview.h")) {
    if (-not (Test-Path (Join-Path $target $required))) {
      throw "Archive is missing $required"
    }
  }
  Set-Content -Path $stamp -Value $PdfiumVersion
  Write-Host "PDFium $PdfiumVersion ($arch) ready: $target"
} finally {
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# Fetch the pinned PDFium binaries for the Windows host.
#
# Downloads the matching pdfium-win-<arch>.tgz from bblanchon/pdfium-binaries,
# verifies its SHA-256 against the pin below, and extracts bin\pdfium.dll,
# lib\pdfium.dll.lib and include\*.h into <OutputRoot>\<version>\<platform>.
# Nothing is committed to git: the cache lives under windows\build (gitignored)
# and the download is re-fetched on a fresh checkout or CI runner.
#
# Runs under MSBuild Exec, which sanitizes PSModulePath and the default .NET
# references — so this script only uses plain PowerShell syntax plus the two
# executables Windows 10 1803+ ships in System32: curl.exe and certutil.
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

# Single source for the PDFium pin. RivetHost.vcxproj repeats the derived
# cache-dir name (PDFIUM_VERSION_DIR) because MSBuild evaluates its
# properties before this script runs; the check below turns any drift into
# a named build error instead of a confusing "pdfium.lib not found" later.
$expectedVersionDir = $PdfiumVersion.Replace("/", "-")
$vcxproj = Join-Path $PSScriptRoot "RivetHost.vcxproj"
if (Test-Path $vcxproj -PathType Leaf) {
  $pin = Select-String -Path $vcxproj `
    -Pattern "<PDFIUM_VERSION_DIR>(.+)</PDFIUM_VERSION_DIR>" |
    Select-Object -First 1
  if ($null -eq $pin -or
      $pin.Matches[0].Groups[1].Value -ne $expectedVersionDir) {
    throw ("PDFium pin drift: this script pins '{0}' but RivetHost.vcxproj " +
           "declares a different PDFIUM_VERSION_DIR. Update the vcxproj " +
           "literal to '{1}' (or move this script's pin)." -f `
           $expectedVersionDir, $expectedVersionDir)
  }
}

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
$url = "https://github.com/bblanchon/pdfium-binaries/releases/download/$PdfiumVersion/$asset"

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("pdfium-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
try {
  $tgz = Join-Path $tmp $asset
  Write-Host "Downloading $url"
  # Retries: transient release-CDN hiccups must not fail the whole build.
  $attempt = 0
  while ($true) {
    & curl.exe -sSL --retry 3 --retry-delay 3 -o $tgz $url
    if ($LASTEXITCODE -eq 0 -and (Test-Path $tgz)) { break }
    $attempt++
    if ($attempt -ge 3) {
      throw "curl download failed with exit code $LASTEXITCODE"
    }
    Write-Host "Download failed (attempt $attempt), retrying..."
    Start-Sleep -Seconds (3 * $attempt)
  }

  # SHA-256 via certutil (external; Get-FileHash is unavailable under
  # MSBuild's sanitized environment).
  $hashOutput = & certutil -hashfile $tgz SHA256
  $actual = ""
  foreach ($line in $hashOutput) {
    $trimmed = $line.Trim()
    if ($trimmed -match '^[0-9a-fA-F]{64}$') {
      $actual = $trimmed.ToLowerInvariant()
      break
    }
  }
  if ($actual -ne $expected) {
    throw ("sha256 mismatch for {0}: expected {1}, got {2}" -f $asset, $expected, $actual)
  }

  if (Test-Path $target) { Remove-Item -Recurse -Force $target }
  New-Item -ItemType Directory -Path $target -Force | Out-Null
  & tar -xzf $tgz -C $target
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

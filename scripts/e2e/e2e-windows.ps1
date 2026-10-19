$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repo '.rivet\acceptance\windows'
$fixture = Join-Path $repo 'racket\tests\fixtures\sample.epub'
$executable = Get-ChildItem -LiteralPath (Join-Path $repo 'dist') `
    -Filter RivetHost.exe -File -Recurse | Select-Object -First 1
if (-not $executable) { throw 'packaged Booklight executable was not found' }
if (-not (Test-Path -LiteralPath $fixture)) { throw 'sample EPUB fixture was not found' }
$appData = Join-Path $output "appdata-$PID"
$localData = Join-Path $output "local-$PID"
New-Item -ItemType Directory -Force -Path $output,$appData,$localData | Out-Null

function Wait-Until {
    param([string]$Description, [scriptblock]$Probe, [int]$TimeoutSeconds = 90)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $value = & $Probe
        if ($null -ne $value -and $value -ne $false) { return $value }
        Start-Sleep -Milliseconds 100
    }
    throw "timed out waiting for $Description"
}

function Contains-Text {
    param($Root, [string]$Fragment)
    $nodes = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    for ($index = 0; $index -lt $nodes.Count; $index++) {
        if ($nodes[$index].Current.Name -like "*$Fragment*") { return $true }
    }
    return $false
}

function Save-Snapshot {
    param($Root, [string]$Path)
    $nodes = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    $snapshot = for ($index = 0; $index -lt [Math]::Min($nodes.Count, 800); $index++) {
        $node = $nodes[$index]
        [ordered]@{ name = $node.Current.Name; controlType = $node.Current.ControlType.ProgrammaticName; automationId = $node.Current.AutomationId }
    }
    ConvertTo-Json -InputObject @($snapshot) -Depth 4 |
        Set-Content -LiteralPath $Path -Encoding utf8
}

$process = $null
$savedAppData = $env:APPDATA
$savedLocalData = $env:LOCALAPPDATA
$savedOpen = $env:BOOKLIGHT_OPEN
try {
    $env:APPDATA = $appData
    $env:LOCALAPPDATA = $localData
    $env:BOOKLIGHT_OPEN = $fixture
    $process = Start-Process -FilePath $executable.FullName -PassThru `
        -WorkingDirectory $repo `
        -RedirectStandardOutput (Join-Path $output 'application.stdout.log') `
        -RedirectStandardError (Join-Path $output 'application.stderr.log')
    $desktop = [System.Windows.Automation.AutomationElement]::RootElement
    $window = Wait-Until 'Booklight window' {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $desktop.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
    }
    try {
        [void](Wait-Until 'EPUB title parsed by the embedded backend' { Contains-Text $window 'The Sample Book' })
        [void](Wait-Until 'EPUB table of contents parsed by the embedded backend' { Contains-Text $window 'First Chapter' })
    } catch {
        Save-Snapshot $window (Join-Path $output 'accessibility-failure.json')
        throw
    }
    Save-Snapshot $window (Join-Path $output 'accessibility.json')
    [ordered]@{
        platform = 'windows'
        artifact = $executable.FullName
        actions = @('launch:sample.epub')
        assertions = @('packaged WinUI window activated','sample EPUB crossed RVT1 into the embedded backend','native accessibility tree contains The Sample Book','native accessibility tree contains First Chapter')
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'interaction-trace.json') -Encoding utf8

    $pattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    ([System.Windows.Automation.WindowPattern]$pattern).Close()
    [void](Wait-Until 'Booklight process exit after native window close' { $process.Refresh(); if ($process.HasExited) { return $true }; return $false } 30)
} finally {
    $env:APPDATA = $savedAppData
    $env:LOCALAPPDATA = $savedLocalData
    $env:BOOKLIGHT_OPEN = $savedOpen
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    if ($process) { $process.Dispose() }
}

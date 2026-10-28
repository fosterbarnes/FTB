#requires -Version 7.0
#. "$PSScriptRoot\scriptHelper.ps1"
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Path $PSScriptRoot -Parent
$projectName = 'FTB'
$outputFolder = "$repoRoot\output"
$targetOutput = "$outputFolder\3ds-arm"
$publishFolder = "$repoRoot\publish"
# .version/version holds one X.Y.Z; the Makefile reads it for the About screen, the updater
# compare and the CIA title version. versionTag is only the commit codename (AGENTS.md Revision).
$versionFolder = "$repoRoot\.version"
$version = "$versionFolder\version"
$versionTag = "$versionFolder\versionTag"
$buildNotes = "$repoRoot\buildNotes.txt"
$buildNotesMd = "$repoRoot\buildNotes.md"
$readme = "$repoRoot\README.md"
$appPublisher = 'fosterbarnes'
$appURL = "https://github.com/$appPublisher/$projectName"
$ghRepo = "$appPublisher/$projectName"
$buttonURL = 'https://raw.githubusercontent.com/fosterbarnes/res/main/btn'
# agentPush.ps1 publishes the GitHub release after every push (user decision 2026-10-27).
$agentPushRelease = $true
# Private word list for version tags (copied from oc, gitignored).
$yapWords = "$repoRoot\.res\yapBot.json"
# Commit subject labels in decision order; AGENTS.md (buildNotes.txt section) defines each.
$commitLabels = 'fix', 'feat.', 'patch', 'perf.', 'deps.', 'scripts', 'cleanup', 'docs', 'misc.'
$versionContents = ([IO.File]::ReadAllText($version)).Trim()
# Releases and README links always use the version; versionTag is only the commit codename.
$releaseTag = "v$versionContents"
$noBom = New-Object System.Text.UTF8Encoding $false
# Release assets. Names stay unversioned: the in-app updater looks for exactly FTB.cia / FTB.3dsx.
$publishAssets = @(
    "$targetOutput\$projectName.cia"
    "$targetOutput\3ds\$projectName\$projectName.3dsx"
    "$targetOutput\$projectName.3ds"
    "$outputFolder\$projectName.zip"
)
# devkitPro lives inside MSYS2; its login shell sets DEVKITPRO/DEVKITARM and the tool PATH.
$msysRoot = 'C:\msys64'
$msysEnv = "$msysRoot\usr\bin\env.exe"
$tex3ds = "$msysRoot\opt\devkitpro\tools\bin\tex3ds.exe"
$weztermExe = (Get-Command wezterm.exe -ErrorAction SilentlyContinue)?.Source
Set-Location -LiteralPath $repoRoot

function writeFileNoBom {
    param([Parameter(Mandatory)][string]$LiteralPath, [Parameter(Mandatory)][string]$Content)
    [IO.File]::WriteAllText($LiteralPath, $Content, $noBom)
}

function requireVersion {
    $contents = [IO.File]::ReadAllText($version).Trim()
    if ($contents -notmatch '^\d+\.\d+\.\d+$') { throw "Invalid $version`: expected one version in X.Y.Z format" }
    $contents
}

# The Makefile packs X.Y.Z into the u16 CIA title version as X*10000 + Y*100 + Z (makerom -ver),
# so minor and patch stay under 100 and major under 6.
function assertTitleVersion {
    param([string]$SemVer = (requireVersion))
    [int[]]$parts = $SemVer.Split('.')
    if ($parts[0] -gt 5 -or $parts[1] -gt 99 -or $parts[2] -gt 99) {
        throw "Version $SemVer does not fit the CIA title version (major 0-5, minor and patch 0-99)."
    }
}

# Throws unless the CIA's ticket and TMD title versions equal the packed .version/version. makerom
# 0.19.0 drops the minor bits of -ver (Project_CTR #169): v0.0.16 shipped as title version 0 and
# the 3DS refused it as a downgrade.
function assertCiaTitleVersion {
    param([Parameter(Mandatory)][string]$LiteralPath)
    [int[]]$parts = (requireVersion).Split('.')
    $expected = $parts[0] * 10000 + $parts[1] * 100 + $parts[2]
    $bytes = [IO.File]::ReadAllBytes($LiteralPath)
    # Header u32s at 0/8/12: header, cert chain and ticket sizes; sections follow 64-byte aligned.
    $sizes = 0, 8, 12 | ForEach-Object { [long][Math]::Ceiling([BitConverter]::ToUInt32($bytes, $_) / 64) * 64 }
    $ticket = $sizes[0] + $sizes[1]
    $tmd = $ticket + $sizes[2]
    # Signed blobs start with a big-endian type 0x1000X (RSA-4096, RSA-2048, ECDSA), then the
    # signature and padding; the version field is a big-endian u16 in the body after that.
    $readVersion = {
        param([long]$Blob, [int]$Field)
        $at = $Blob + 4 + @{ 3 = 0x23C; 4 = 0x13C; 5 = 0x7C }[[int]$bytes[$Blob + 3]] + $Field
        ($bytes[$at] -shl 8) -bor $bytes[$at + 1]
    }
    $ticketVersion = & $readVersion $ticket 0xA6
    $tmdVersion = & $readVersion $tmd 0x9C
    if ($ticketVersion -ne $expected -or $tmdVersion -ne $expected) {
        throw "$LiteralPath has title version ticket $ticketVersion / TMD $tmdVersion, expected $expected " +
            "for $($parts -join '.'). Use makerom 0.18.4; 0.19.0 drops the minor bits (Project_CTR #169)."
    }
}

# Bumps one part of X.Y.Z, zeroes the lower parts and rolls a new version tag. Patch 99 rolls into
# the next minor (and minor 99 into the next major) to stay inside the title version encoding.
function bumpVersion {
    param([ValidateSet('Patch', 'Minor', 'Major')][string]$Part = 'Patch')
    $current = requireVersion
    [int[]]$parts = $current.Split('.')
    switch ($Part) {
        'Major' { $parts = @(($parts[0] + 1), 0, 0) }
        'Minor' { $parts = @($parts[0], ($parts[1] + 1), 0) }
        'Patch' { $parts[2]++ }
    }
    if ($parts[2] -gt 99) { $parts = @($parts[0], ($parts[1] + 1), 0) }
    if ($parts[1] -gt 99) { $parts = @(($parts[0] + 1), 0, 0) }
    $next = $parts -join '.'
    assertTitleVersion $next
    writeFileNoBom -LiteralPath $version -Content "$next`n"
    "Version $current -> $next ($(newVersionTag))"
}

function newVersionTag { setVersionTag (getVersionTags 1) }

# Returns $Count distinct random camelCase two-word tags without writing anything.
function getVersionTags {
    param([int]$Count = 1)
    if (-not (Test-Path -LiteralPath $yapWords)) {
        throw "Missing word list $yapWords. Copy it from oc\.res\yapBot.json; it is private and gitignored."
    }
    $words = @(Get-Content -LiteralPath $yapWords -Raw | ConvertFrom-Json | Where-Object { $_ -match '^[A-Za-z]+$' })
    if (-not $words.Count) { throw "No usable words found in $yapWords." }
    $tags = [Collections.Generic.HashSet[string]]::new()
    while ($tags.Count -lt $Count) {
        $first = $words[[Security.Cryptography.RandomNumberGenerator]::GetInt32($words.Count)]
        $second = $words[[Security.Cryptography.RandomNumberGenerator]::GetInt32($words.Count)]
        [void]$tags.Add($first.ToLowerInvariant() + $second.Substring(0, 1).ToUpperInvariant() + $second.Substring(1).ToLowerInvariant())
    }
    @($tags)
}

function setVersionTag {
    param([Parameter(Mandatory)][string]$Tag)
    if ($Tag -cnotmatch '^[a-z]+[A-Z][a-z]*$') { throw "Invalid tag '$Tag': expected one camelCase two-word tag" }
    writeFileNoBom -LiteralPath $versionTag -Content "$Tag`n"
    $Tag
}

function requireVersionTag {
    if (-not (Test-Path -LiteralPath $versionTag)) {
        throw "Missing $versionTag`: run .scripts\bumpVersion.ps1 -RerollTag to generate one."
    }
    $contents = [IO.File]::ReadAllText($versionTag).Trim()
    if ($contents -cnotmatch '^[a-z]+[A-Z][a-z]*$') { throw "Invalid $versionTag`: expected one camelCase two-word tag" }
    $contents
}

# AGENTS.md Revision: a push with code changes needs bumpVersion.ps1. Docs and notes alone do not.
# Returns the changed or untracked code paths when the version still matches HEAD, else nothing.
# A HEAD without .version/version (the first commit) needs no bump.
function getUnbumpedCodeChanges {
    $headVersion = (& git -C $repoRoot show HEAD:.version/version 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -or $headVersion -ne (requireVersion)) { return @() }
    $changed = @(& git -C $repoRoot diff --name-only HEAD)
    if ($LASTEXITCODE) { throw "git diff failed (exit $LASTEXITCODE)." }
    $changed += @(& git -C $repoRoot ls-files -o --exclude-standard)
    if ($LASTEXITCODE) { throw "git ls-files failed (exit $LASTEXITCODE)." }
    @($changed | Where-Object { $_ -notmatch '(\.md$|^\.version/)' })
}

# buildNotes.txt is local, so git cannot tell a new message from the last push's: it is new when
# line 1 is set and differs from the last commit subject.
function testBuildNotesChanged {
    if (-not (Test-Path -LiteralPath $buildNotes)) { return $false }
    $subject = @([IO.File]::ReadAllLines($buildNotes))[0]
    if (-not "$subject".Trim()) { return $false }
    $head = (& git -C $repoRoot log -1 --format=%s | Out-String).Trim()
    if ($LASTEXITCODE) { throw "git log failed (exit $LASTEXITCODE)." }
    "$subject".Trim() -ne $head
}

# buildNotes.md entries (newest first) dated after the last commit. Headings start with
# "## yyyy-MM-dd (~HH:mm)", which sorts as text, so the walk stops at the first older entry.
function getNewBuildNotesEntries {
    if (-not (Test-Path -LiteralPath $buildNotesMd)) { return @() }
    $head = (& git -C $repoRoot log -1 --format=%cd --date=format-local:'%Y-%m-%d (~%H:%M)' | Out-String).Trim()
    if ($LASTEXITCODE) { throw "git log failed (exit $LASTEXITCODE)." }
    $entries = [Collections.Generic.List[string]]::new()
    $inHistory = $false
    foreach ($line in [IO.File]::ReadAllLines($buildNotesMd)) {
        if (-not $inHistory) { $inHistory = $line -eq '# Historical entries (newest first)'; continue }
        if (-not $entries.Count -and -not $line.StartsWith('## ')) { continue }
        if ($line -match '^## (\d{4}-\d{2}-\d{2} \(~\d{2}:\d{2}\))' -and
            [string]::CompareOrdinal($Matches[1], $head) -le 0) { break }
        $entries.Add($line)
    }
    @($entries)
}

# Throws unless $Subject is "<Prefix> [<label> > <summary>]", optionally "; <label> > <summary>".
# Summaries: no edge spaces, no trailing period, none of [ ] > ;. .NET regex is case-sensitive.
function assertCommitSubject {
    param([Parameter(Mandatory)][string]$Subject, [Parameter(Mandatory)][string]$Prefix)
    $labels = ($commitLabels | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $segment = "($labels) > [^\s\[\]>;](?:[^\[\]>;]*[^\s.\[\]>;])?"
    $match = [regex]::Match($Subject, "^$([regex]::Escape($Prefix)) \[$segment(?:; $segment)?\]$")
    if (-not $match.Success) {
        throw "Line 1 must be '$Prefix [<label> > <summary>]', optionally '; <label> > <summary>'. " +
            "Labels: $($commitLabels -join ' ')"
    }
    $first, $second = $match.Groups[1].Value, $match.Groups[2].Value
    if ($second -and $first -eq $second) { throw "Line 1 repeats label '$first'; merge the segments." }
    if ($second -and 'misc.' -in $first, $second) { throw "'misc.' must be the only segment." }
    if ($Subject -match '[^\x20-\x7E]') { throw 'Line 1 must be plain ASCII.' }
    if ($Subject.Length -gt 72) { throw "Line 1 is $($Subject.Length) characters; the limit is 72." }
}

function runNativeCommand {
    param([Parameter(Mandatory)][string]$FilePath, [Parameter(Mandatory)]$ArgumentList, [Parameter(Mandatory)][string]$Name)
    & $FilePath @ArgumentList
    if ($LASTEXITCODE) { throw "$Name failed (exit $LASTEXITCODE)." }
}

# Runs a sibling script in-process as part of a pipeline, so its closeOut does not count down or exit.
function invokeScript {
    param([Parameter(Mandatory)][string]$Name, [hashtable]$Params = @{})
    $previousPipeline = $env:BASE_BUILD_PIPELINE
    try {
        $env:BASE_BUILD_PIPELINE = '1'
        & "$PSScriptRoot\$Name" @Params
    } finally {
        $env:BASE_BUILD_PIPELINE = $previousPipeline
    }
}

# Runs a bash command in an MSYS2 login shell at the repo root.
function invokeMsys {
    param([Parameter(Mandatory)][string]$Command, [Parameter(Mandatory)][string]$Name)
    if (-not (Test-Path -LiteralPath $msysEnv)) { throw "MSYS2 not found at $msysRoot." }
    runNativeCommand $msysEnv @('MSYSTEM=MSYS', 'CHERE_INVOKING=1', '/usr/bin/bash', '-lc', $Command) $Name
}

function deleteDir {
    param([Parameter(Mandatory)][string]$Path)
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
    if (Test-Path -LiteralPath $Path) { throw "Could not remove '$Path'." }
}

function openUrl {
    param([Parameter(Mandatory)][string]$Url)
    if ([string]::IsNullOrWhiteSpace($Url)) { throw 'openUrl requires a URL.' }
    # Agent shells run with redirected input; only a person at a console gets a browser tab.
    if ([Console]::IsInputRedirected) {
        Write-Host "URL: $Url"
        return
    }
    Start-Process $Url
}

# Clean build, stage the release assets in publish/, then point the README links at this version.
function buildAll {
    deleteDir $publishFolder
    invokeScript 'build.ps1' @{ Clean = $true }
    New-Item -ItemType Directory -Path $publishFolder -Force | Out-Null
    foreach ($asset in $publishAssets) { Copy-Item -LiteralPath $asset -Destination $publishFolder }
    invokeScript 'updateReadme.ps1'
}

function writeClearedLine {
    param([Parameter(Mandatory)][string]$Text, [Parameter(Mandatory)][int]$PadWidth, [switch]$NoNewline)
    Write-Host "`r$Text$(' ' * [Math]::Max(0, $PadWidth - $Text.Length))" -NoNewline:$NoNewline
}

function closeOut {
    param([int]$Seconds = 5, [switch]$KeepOpen)
    if ($KeepOpen) { return }
    if ($env:BASE_BUILD_PIPELINE) { return }
    if ($Seconds -lt 0) { $Seconds = 0 }
    if ($Seconds -gt 0) {
        $pad = "closing after $Seconds seconds..."
        foreach ($n in $Seconds..1) {
            writeClearedLine -Text "closing after $n seconds..." -PadWidth $pad.Length -NoNewline
            Start-Sleep -Seconds 1
        }
        writeClearedLine -Text 'closing...' -PadWidth $pad.Length
    }
    $caller = $MyInvocation.PSCommandPath
    if ([string]::IsNullOrWhiteSpace($caller)) { return }
    $argv = [Environment]::GetCommandLineArgs()
    $fileArg = $null
    for ($i = 0; $i -lt $argv.Length; $i++) {
        if ("$($argv[$i])" -match '^(?i)-File$|^(?i)-f$') {
            if ($i + 1 -lt $argv.Length) { $fileArg = $argv[$i + 1] }
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($fileArg)) { return }
    try {
        $fileFull = [IO.Path]::GetFullPath($fileArg)
        $callerFull = [IO.Path]::GetFullPath($caller)
    } catch { return }
    if (-not [string]::Equals($fileFull, $callerFull, [StringComparison]::OrdinalIgnoreCase)) { return }
    try {
        if ($env:SCRIPT_OWN_PANE -and $env:WEZTERM_PANE -and $weztermExe) { & $weztermExe @('cli', 'kill-pane', '--pane-id', $env:WEZTERM_PANE) }
    } catch { }
    [Environment]::Exit(0)
}

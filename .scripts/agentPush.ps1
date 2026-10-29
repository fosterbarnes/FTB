#requires -Version 7.0
# Non-interactive push for the agent-push skill, run only when the user asks an agent to push.
# -Context prints what the agent needs to write buildNotes.txt; the default mode validates it, runs
# prePush.ps1 and push.ps1 quietly, then reports. With $agentPushRelease in scriptHelper.ps1 (or
# -Release) it then runs pushRelease.ps1 (tags v<version>, publishes the GitHub release with the
# assets prePush staged), unless that release already exists. -Tags prints 10 candidate version
# tags; -SetTag writes the one the user picked.
param([Alias('h')][switch]$Help, [switch]$Context, [switch]$Tags, [string]$SetTag, [switch]$Release)
$ErrorActionPreference = 'Stop'
if ($Help) { Write-Host 'agentPush.ps1 [-Context | -Tags | -SetTag <tag> | -Release]'; return }
. "$PSScriptRoot\scriptHelper.ps1"
Set-Location -LiteralPath $repoRoot
$current = requireVersion
if ($Tags) { getVersionTags 10; return }
if ($SetTag) {
    $set = setVersionTag $SetTag
    Write-Host "line 1: v$current $set [<label> > <summary>]"
    return
}
$tagWord = requireVersionTag
$notesChanged = testBuildNotesChanged

$unbumped = getUnbumpedCodeChanges
$bumpFix = "Code changed but the version is still $current. Run .scripts\bumpVersion.ps1, update this " +
    "session's buildNotes.md heading and Version bullet, then rerun agentPush.ps1 -Context."

# 'on', 'off' or 'exists'. An existing release is never redone: its tag sits on an older commit and
# pushRelease.ps1 never moves tags. A failed lookup counts as missing, so pushRelease.ps1 reports
# the real problem.
$releasePlan = 'off'
if ($Release -or (Get-Variable agentPushRelease -ValueOnly -ErrorAction Ignore)) {
    gh release view $releaseTag --repo $ghRepo *> $null
    $releasePlan = $LASTEXITCODE -eq 0 ? 'exists' : 'on'
}
$releaseText = switch ($releasePlan) {
    'on' { "on: publishes $releaseTag after the push" }
    'exists' { "$releaseTag already exists, skipped" }
    default { 'off' }
}

if ($Context) {
    Write-Host "branch: $(git branch --show-current)  version: $current $tagWord"
    Write-Host "release: $releaseText"
    Write-Host "line 1: v$current $tagWord [<label> > <summary>]"
    Write-Host "labels (decision order; rules in AGENTS.md 'buildNotes.txt' section): $($commitLabels -join ' ')"
    Write-Host '--- status'
    git status --short 2>$null
    Write-Host '--- new buildNotes.md entries (dated after the last commit)'
    getNewBuildNotesEntries
    Write-Host '--- last 10 subjects'
    git log -10 --format=%s
    Write-Host '--- last commit message (style sample)'
    git log -1 --format=%B
    if ($notesChanged) { Write-Host 'WARN: buildNotes.txt already holds a new message; overwrite it.' }
    if ($unbumped) { Write-Host "ACTION: $bumpFix" }
    return
}

# Fail on a bad message before the slow checks run.
if ($unbumped) { throw "$bumpFix Changed: $($unbumped -join ', ')" }
if (-not $notesChanged) { throw 'buildNotes.txt still holds the last commit subject; write the new message first.' }
$lines = @([IO.File]::ReadAllLines($buildNotes))
assertCommitSubject $lines[0].Trim() "v$current $tagWord"
if ($lines.Count -lt 3 -or $lines[1].Trim() -or -not ($lines[2..($lines.Count - 1)] -join '').Trim()) {
    throw 'buildNotes.txt needs line 1, one blank line, then a non-empty body.'
}

# Run each script in-process as one pipeline so closeOut stays a no-op; print the tail only on failure.
$previousPipeline = $env:BASE_BUILD_PIPELINE
try {
    $env:BASE_BUILD_PIPELINE = '1'
    $steps = @('prePush', 'push') + @(if ($releasePlan -eq 'on') { 'pushRelease' })
    foreach ($step in $steps) {
        $log = [Collections.Generic.List[string]]::new()
        try { & "$PSScriptRoot\$step.ps1" *>&1 | ForEach-Object { $log.Add("$_") } }
        catch {
            $log | Select-Object -Last 40
            throw "$step.ps1 failed: $_"
        }
        Write-Host "$step.ps1 passed."
    }
} finally {
    $env:BASE_BUILD_PIPELINE = $previousPipeline
}

Set-Location -LiteralPath $repoRoot
Write-Host "--- pushed $(git rev-parse --short=7 HEAD) to origin/$(git branch --show-current)"
git show -s --format=%B HEAD
git show --name-status --format= HEAD
switch ($releasePlan) {
    'on' { Write-Host "--- released $releaseTag`: $appURL/releases/tag/$releaseTag" }
    'exists' { Write-Host "--- release $releaseTag already exists; skipped" }
}

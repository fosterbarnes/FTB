#requires -Version 7.0
# Tags v<version> and publishes the GitHub release from the assets prePush.ps1 staged in publish/.
# -DryRun prints the commands without tagging or publishing.
param([Alias('h')][switch]$Help, [Alias('n')][switch]$DryRun)
$ErrorActionPreference = 'Stop'
if ($Help) { Write-Host 'pushRelease.ps1 [-DryRun]'; return }
. "$PSScriptRoot\scriptHelper.ps1"
Set-Location -LiteralPath $repoRoot
$assets = @(Get-ChildItem -LiteralPath $publishFolder -File -ErrorAction SilentlyContinue)
if (-not $assets -and -not $DryRun) { throw "No release assets found in $publishFolder; run prePush.ps1 first." }
# Assets staged for an older version fail here instead of shipping under the new tag.
if (-not $DryRun) { assertCiaTitleVersion "$publishFolder\$projectName.cia" }
$assetArgs = @($assets | ForEach-Object FullName)

$releaseTitle = $releaseTag
$releaseNotes = ''
if (Test-Path -LiteralPath $buildNotes) {
    $lines = @([IO.File]::ReadAllLines($buildNotes))
    if ($lines.Count -gt 0 -and $lines[0].Trim()) {
        $releaseTitle = $lines[0].Trim()
        if ($lines.Count -ge 2) {
            if ($lines[1].Trim()) {
                throw 'buildNotes.txt must have one blank line after the first line, then the commit description.'
            }
            if ($lines.Count -gt 2) {
                $releaseNotes = ($lines[2..($lines.Count - 1)] -join "`n").Trim()
            }
        }
    }
}

$releaseArgs = @('release', 'create', $releaseTag, '--title', $releaseTitle, '--repo', $ghRepo)
if ($releaseNotes) { $releaseArgs += @('--notes', $releaseNotes) } else { $releaseArgs += '--generate-notes' }
$releaseUrl = "$appURL/releases/tag/$releaseTag"

# Tags are never moved or force-pushed. An existing tag is reused only when it already points at
# HEAD (a retry after gh release create failed); anywhere else, this version shipped from another
# commit and needs a bump.
$head = git rev-parse HEAD
if ($LASTEXITCODE) { throw "git rev-parse HEAD failed (exit $LASTEXITCODE)." }
$localTag = git rev-parse -q --verify "refs/tags/$releaseTag^{commit}"
$originTag = git ls-remote origin "refs/tags/$releaseTag"
if ($LASTEXITCODE) { throw "git ls-remote failed (exit $LASTEXITCODE)." }
$tagCommits = [ordered]@{ local = $localTag; origin = "$originTag".Split("`t")[0] }
foreach ($where in $tagCommits.Keys) {
    if ($tagCommits[$where] -and $tagCommits[$where] -ne $head) {
        throw "Tag $releaseTag exists ($where) at $($tagCommits[$where]), not HEAD $head. " +
            'Tags are never moved; bump the version.'
    }
}

if ($DryRun) {
    if (-not $tagCommits.local) { Write-Host "Dry run: git tag $releaseTag" }
    if (-not $tagCommits.origin) { Write-Host "Dry run: git push origin refs/tags/$releaseTag" }
    Write-Host "Dry run: gh $((($releaseArgs + $assetArgs) -join ' '))"
    Write-Host "Dry run: open $releaseUrl"
    return
}
if (-not $tagCommits.local) { runNativeCommand git @('tag', $releaseTag) 'git tag' }
if (-not $tagCommits.origin) { runNativeCommand git @('push', 'origin', "refs/tags/$releaseTag") 'git push tag' }
runNativeCommand gh ($releaseArgs + $assetArgs) 'gh release create'
if (-not $env:BASE_BUILD_PIPELINE) { openUrl $releaseUrl }
closeOut 0

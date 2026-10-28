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
if ($DryRun) {
    Write-Host "Dry run: git tag -f $releaseTag"
    Write-Host "Dry run: git push origin refs/tags/$releaseTag --force"
    Write-Host "Dry run: gh $((($releaseArgs + $assetArgs) -join ' '))"
    Write-Host "Dry run: open $releaseUrl"
    return
}
runNativeCommand git @('tag', '-f', $releaseTag) 'git tag'
runNativeCommand git @('push', 'origin', "refs/tags/$releaseTag", '--force') 'git push tag'
runNativeCommand gh ($releaseArgs + $assetArgs) 'gh release create'
if (-not $env:BASE_BUILD_PIPELINE) { openUrl $releaseUrl }
closeOut 0

#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\scriptHelper.ps1"
Set-Location -LiteralPath $repoRoot
if (-not (Test-Path -LiteralPath $readme)) { throw "README not found: $readme" }
$content = [IO.File]::ReadAllText($readme)
$start = '<!-- Quick Reference -->'
$end = '<!-- End Quick Reference -->'
$startIndex = $content.IndexOf($start, [StringComparison]::Ordinal)
$endIndex = $content.IndexOf($end, [StringComparison]::Ordinal)
if ($startIndex -lt 0 -or $endIndex -lt $startIndex) { throw 'README Quick Reference markers are missing.' }
# One link cell per release asset, pinned to this version's release.
$names = @($publishAssets | ForEach-Object { Split-Path -Path $_ -Leaf })
$cells = $names | ForEach-Object { "[$_]($appURL/releases/download/$releaseTag/$_)" }
$lines = @($start; "| $($cells -join ' | ') |"; "|$(':---:|' * $names.Count)"; $end)
$prefix = $content.Substring(0, $startIndex) ; $suffix = $content.Substring($endIndex + $end.Length)
writeFileNoBom -LiteralPath $readme -Content ($prefix + ($lines -join "`n") + $suffix)
closeOut 0

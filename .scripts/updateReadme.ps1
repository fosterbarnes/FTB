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
# One button cell per release asset, pinned to this version's release. res/btn names buttons by
# file extension (cia.svg, 3dsx.svg, 3ds.svg, zip.svg).
$cells = foreach ($asset in $publishAssets) {
    $name = Split-Path -Path $asset -Leaf
    $ext = [IO.Path]::GetExtension($name).TrimStart('.')
    $href = "$appURL/releases/download/$releaseTag/$name"
    "<td valign=`"top`"><a href=`"$href`"><img src=`"$buttonURL/$ext.svg`" width=`"180`" height=`"auto`" alt=`"$name`"/></a></td>"
}
$lines = @($start; '<table border="0">'; '<tbody>'; '<tr>'; $cells; '</tr>'; '</tbody>'; '</table>'; $end)
$prefix = $content.Substring(0, $startIndex) ; $suffix = $content.Substring($endIndex + $end.Length)
writeFileNoBom -LiteralPath $readme -Content ($prefix + ($lines -join "`n") + $suffix)
closeOut 0

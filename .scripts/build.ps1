#requires -Version 7.0
param([Alias('h')][switch]$Help, [switch]$Clean)
$ErrorActionPreference = 'Stop'
if ($Help) { Write-Host 'build.ps1 [-Clean]   Build FTB (.cia, .3dsx, .3ds, zip) with devkitPro in MSYS2.'; return }
. "$PSScriptRoot\scriptHelper.ps1"
Set-Location -LiteralPath $repoRoot
assertTitleVersion

# The Makefile finds the compiler through DEVKITARM, so it is checked by path, not PATH.
$tools = 'make picasso 3dsxtool xxd zip makerom bannertool'
$checkTools = "missing=; for t in $tools; do command -v `$t >/dev/null || missing=`"`$missing `$t`"; done; " +
    '[ -x "$DEVKITARM/bin/arm-none-eabi-gcc" ] || missing="$missing arm-none-eabi-gcc"; ' +
    '[ -z "$missing" ] || { echo "Missing build tools:$missing" >&2; exit 127; }'
invokeMsys $checkTools 'Build tool check'

if ($Clean) { invokeMsys 'make clean' 'make clean' }
# Serial on purpose: buildtools/make_base does not order screen.c after its generated shader
# header, so parallel clean builds race.
invokeMsys 'make' 'make'

foreach ($asset in $publishAssets) {
    if (-not (Test-Path -LiteralPath $asset)) { throw "Missing build output: $asset" }
    Write-Host $asset
}
assertCiaTitleVersion "$targetOutput\$projectName.cia"
closeOut 0

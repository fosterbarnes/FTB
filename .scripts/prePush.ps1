#requires -Version 7.0
# Checks before a push: clean build, release assets staged in publish/, README download links
# (buildAll). Tagging and upload live in pushRelease.ps1.
param([Alias('h')][switch]$Help)
$ErrorActionPreference = 'Stop'
if ($Help) { Write-Host 'prePush.ps1'; return }
. "$PSScriptRoot\scriptHelper.ps1"
Set-Location -LiteralPath $repoRoot
buildAll
Write-Host 'Pre-push checks passed. No commit or push was performed.'
closeOut 0

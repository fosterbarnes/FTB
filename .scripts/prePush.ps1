#requires -Version 7.0
# Checks before a push: incremental build and README download links. The clean build, staging and
# upload live in pushRelease.ps1.
param([Alias('h')][switch]$Help)
$ErrorActionPreference = 'Stop'
if ($Help) { Write-Host 'prePush.ps1'; return }
. "$PSScriptRoot\scriptHelper.ps1"
Set-Location -LiteralPath $repoRoot
invokeScript 'build.ps1'
invokeScript 'updateReadme.ps1'
Write-Host 'Pre-push checks passed. No commit or push was performed.'
closeOut 0

#requires -Version 7.0
# Agents run it once per code session (AGENTS.md Revision); -Minor/-Major only when the user asks.
param([Alias('h')][switch]$Help, [switch]$Minor, [switch]$Major, [switch]$RerollTag)
$ErrorActionPreference = 'Stop'
if ($Help) { Write-Host 'bumpVersion.ps1 [-Minor|-Major] [-RerollTag]  (no args: patch bump + new tag)'; return }
. "$PSScriptRoot\scriptHelper.ps1"
if ($Minor -and $Major) { throw 'Use -Minor or -Major, not both.' }
if ($RerollTag -and ($Minor -or $Major)) { throw '-RerollTag changes only the tag; do not combine it with a bump.' }
if ($RerollTag) {
    Write-Host "Version tag $(newVersionTag)"
} else {
    Write-Host (bumpVersion ($Major ? 'Major' : $Minor ? 'Minor' : 'Patch'))
}
closeOut 0

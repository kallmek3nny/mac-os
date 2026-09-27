<#
.SYNOPSIS
  Restore C:\ProgramData\VMware\vmnetnat.conf from a backup. Restore ONLY --
  no port forward, no service restart.

.DESCRIPTION
  Why this exists: on 2026-08-14 an earlier build of tools/vmnat-ssh-forward.ps1
  built its regex replacement as "`$0$entry`r`n". PowerShell expanded $entry
  BEFORE .NET regex parsed the string, so the substitution text arrived as
  `$02222 = 192.168.255.129:22`. .NET read `$02222` as a group reference, found
  no group 2222, emitted it literally -- and because the match being replaced
  WAS the [incomingtcp] header line, the header was consumed.

  Measured damage, exactly one line:
    diff line 81   expected: [incomingtcp]
    diff line 81   actual  : $02222 = 192.168.255.129:22

  The verify step in that build did not catch it. It counted active `2222 =`
  entries and said "found 0", which reads as "the insert did not happen" rather
  than "the insert ate the anchor". A count of the thing you ADDED cannot detect
  the loss of the thing you MATCHED ON.

  Requires elevation. vmnetnat.conf is owned by NT AUTHORITY\SYSTEM and grants
  BUILTIN\Users only ReadAndExecute; both a shared ReadWrite open and a Set-Acl
  attempt fail with UnauthorizedAccessException unelevated. Windows `sudo` is
  disabled on this machine, so a UAC prompt is the only elevation path.

.NOTES
  The corrected tools/vmnat-ssh-forward.ps1 carries this same repair as a
  built-in first pass, so running THAT script both repairs the file and adds
  the forward in one elevation. Use this script when you want the repair
  without the forward.
#>
[CmdletBinding()]
param(
    [string]$Conf    = 'C:\ProgramData\VMware\vmnetnat.conf',
    [string]$Backup  = '',
    [string]$LogPath = "$env:TEMP\vmnat-restore.log"
)

$ErrorActionPreference = 'Stop'
$log = New-Object System.Collections.Generic.List[string]
function Say([string]$m) { $log.Add($m); Write-Host $m }

try {
    $elevated = ([Security.Principal.WindowsPrincipal] `
        [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Say "elevated            : $elevated"
    if (-not $elevated) { throw 'not elevated -- vmnetnat.conf is not writable by BUILTIN\Users' }

    if (-not (Test-Path $Conf)) { throw "no such file: $Conf" }

    # Pick the newest backup that actually contains the header, rather than the
    # newest backup full stop. A backup taken AFTER the damage would restore
    # the damage.
    if ([string]::IsNullOrWhiteSpace($Backup)) {
        $cand = Get-ChildItem -Path "$Conf.bak-*" -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Where-Object { [System.IO.File]::ReadAllText($_.FullName).Contains('[incomingtcp]') } |
                Select-Object -First 1
        if (-not $cand) { throw "no backup matching $Conf.bak-* contains [incomingtcp]" }
        $Backup = $cand.FullName
    }
    Say "backup              : $Backup"

    # Validate the backup BEFORE overwriting anything with it. Restoring from an
    # unchecked file swaps one unknown state for another.
    $b = [System.IO.File]::ReadAllText($Backup)
    Say "backup bytes        : $($b.Length)"
    if (-not $b.Contains('[incomingtcp]')) { throw 'backup lacks [incomingtcp]' }
    if (-not $b.Contains('[incomingudp]')) { throw 'backup lacks [incomingudp]' }
    if ([regex]::Matches($b, '(?m)^\$\d+\s*=.*$').Count -ne 0) { throw 'backup itself carries junk lines' }
    Say 'backup validated    : both headers present, no junk lines'

    [System.IO.File]::Copy($Backup, $Conf, $true)

    # Re-read from disk. Checking the in-memory copy would prove only that the
    # Copy call returned, not that the bytes landed.
    $a = [System.IO.File]::ReadAllText($Conf)
    Say "conf bytes now      : $($a.Length)"
    Say "has [incomingtcp]   : $($a.Contains('[incomingtcp]'))"
    Say "has [incomingudp]   : $($a.Contains('[incomingudp]'))"
    Say "junk lines remaining: $([regex]::Matches($a, '(?m)^\$\d+\s*=.*$').Count)"
    Say "identical to backup : $($a -eq $b)"
    if ($a -ne $b) { throw 'restored file does not match the backup byte for byte' }

    Say 'RESULT: restored'
}
catch {
    Say "ERROR: $($_.Exception.Message)"
}
finally {
    [System.IO.File]::WriteAllLines($LogPath, $log)
}

<#
.SYNOPSIS
  Remove the diagnostic vmnet8 NAT forwards added while localising the
  SSH-through-VPN block, keeping only the ones that are actually wanted.

.DESCRIPTION
  Five forwards were added on 2026-08-14 purely as controls. Two of them are
  worth removing promptly rather than leaving behind:

    4445 -> 192.168.255.1:445   the LIVE control. This republishes the HOST's
                                own SMB service on a loopback port. Harmless in
                                itself, but it is host file sharing reachable
                                by a new name, and it exists only to prove the
                                relay carried traffic. That is proven.
    4446 -> 192.168.255.200:22  the DEAD-ADDRESS control. No DHCP lease, no ARP
                                entry, so it can never reach anything.
    6106 -> 192.168.255.129:631 expected-negative control
    8088 -> 192.168.255.129:88  expected-negative control

  KEPT: 2222 -> guest 22 (the SSH path), 5901 -> guest 5900 (Screen Sharing),
  5488 -> guest 548 (AFP). Those are the services actually wanted.

  What the controls established, and why they are now disposable: every forward
  to the guest EOFs ~1ms after connect, while BOTH controls -- the live one and
  the dead one -- are held open silent for 12s. vmnat therefore HANGS when
  nobody answers and EOFs fast only on an active RST. So the fast EOF from the
  guest is the guest itself refusing the port: relay works, guest is alive, the
  service is off.

.NOTES
  String.Insert / regex-free line removal only. Interpolating data into a .NET
  regex REPLACEMENT string made `$02222` parse as a group reference and consume
  the [incomingtcp] header earlier the same day.
#>
[CmdletBinding()]
param(
    [string]$Conf    = 'C:\ProgramData\VMware\vmnetnat.conf',
    [int[]] $Ports   = @(4445, 4446, 6106, 8088),
    [string]$LogPath = "$env:TEMP\vmnat-cleanup.log"
)

$ErrorActionPreference = 'Stop'
$log = New-Object System.Collections.Generic.List[string]
function Say([string]$m) { $log.Add($m); Write-Host $m }

$hdr = '[incomingtcp]'

try {
    $elevated = ([Security.Principal.WindowsPrincipal] `
        [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Say "elevated        : $elevated"
    if (-not $elevated) { throw 'not elevated -- vmnetnat.conf grants BUILTIN\Users ReadAndExecute only' }

    $text = [System.IO.File]::ReadAllText($Conf)
    if (-not $text.Contains($hdr)) { throw "'$hdr' missing -- run tools\vmnat-restore-conf.ps1 first" }

    $backup = "$Conf.bak-$([System.IO.Path]::GetRandomFileName())"
    [System.IO.File]::WriteAllText($backup, $text)
    Say "backup          : $backup"

    # Rebuild line by line. No regex replacement string is involved anywhere.
    $keep = New-Object System.Collections.Generic.List[string]
    $dropped = 0
    foreach ($line in ($text -split "`r?`n")) {
        $t = $line.Trim()
        $hit = $false
        foreach ($p in $Ports) {
            if ($t -match "^$p\s*=") { $hit = $true; break }
        }
        if ($hit) { Say "drop            : $t"; $dropped++ }
        else      { $keep.Add($line) }
    }
    Say "dropped         : $dropped"

    [System.IO.File]::WriteAllText($Conf, ($keep -join "`r`n"))

    # Re-read from disk and assert the ANCHOR survived plus the KEEPERS are
    # still there. Counting only what was removed cannot detect collateral loss.
    $after = [System.IO.File]::ReadAllText($Conf)
    if (-not $after.Contains($hdr)) { throw 'the [incomingtcp] header did not survive the rewrite' }
    Say "header survived : True"
    foreach ($p in $Ports) {
        $c = [regex]::Matches($after, ("(?m)^\s*{0}\s*=" -f $p)).Count
        Say ("removed {0,-5}   : {1}" -f $p, ($c -eq 0))
        if ($c -ne 0) { throw "port $p still present after removal" }
    }
    foreach ($p in @(2222, 5901, 5488)) {
        $c = [regex]::Matches($after, ("(?m)^\s*{0}\s*=" -f $p)).Count
        Say ("kept {0,-5}      : {1}" -f $p, $c)
        if ($c -ne 1) { throw "expected to keep exactly 1 entry for $p, found $c" }
    }

    Say '--- restarting VMware NAT Service ---'
    Restart-Service -Name 'VMware NAT Service' -Force
    Start-Sleep -Seconds 4
    Say ("service state   : " + (Get-Service -Name 'VMware NAT Service').Status)

    foreach ($p in @(2222, 5901, 5488, 4445, 4446, 6106, 8088)) {
        $l = Get-NetTCPConnection -State Listen -LocalPort $p -ErrorAction SilentlyContinue
        if ($l) { Say ("listening {0,-5} : yes pid={1}" -f $p, $l[0].OwningProcess) }
        else    { Say ("listening {0,-5} : no" -f $p) }
    }
    Say 'DONE'
}
catch {
    Say "ERROR: $($_.Exception.Message)"
}
finally {
    [System.IO.File]::WriteAllLines($LogPath, $log)
}

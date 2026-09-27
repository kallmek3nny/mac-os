<#
.SYNOPSIS
  Add (or remove) a vmnet8 NAT port forward so the macOS guest's sshd is
  reachable at a HOST-LOCAL address while ProtonVPN's kill switch is up.

.DESCRIPTION
  Measured on this host 2026-08-14: ProtonVPN Standard kill switch installs a
  WFP filter that fails TCP connect to 192.168.255.129 with WSAEACCES (10013),
  while TCP to tunnel-routed addresses is OPEN and UDP sendto to the same guest
  address is ACCEPTED. So the filter is on TCP connect authorization scoped by
  destination address -- not on the interface, and not on all sockets.

  Two consequences make this forward the right lever:

    * 192.168.255.1:445 and 127.0.0.1 both connect fine, so a HOST-LOCAL
      destination is not filtered.
    * vmnat does not call connect() to reach the guest -- it writes frames into
      the vmnet8 device, so it never trips an ALE_AUTH_CONNECT filter. That is
      already witnessed indirectly: vmnetdhcp keeps answering the guest (leases
      renewed minutes ago) with the kill switch up.

  So: ssh -p <HostPort> user@127.0.0.1 is a host-local connect that the filter
  permits, and vmnat carries it the rest of the way.

  Requires elevation: vmnetnat.conf is not writable by a normal user, and the
  NAT service must be restarted for it to re-read the file.

.NOTES
  This does NOT turn on sshd inside the guest. If Remote Login is off, the
  forward will report WSAECONNREFUSED -- which is itself the measurement that
  could not be taken before, because the host filter masked every reply with
  WSAEACCES.

  WHY THIS USES String.Insert AND NOT [regex]::Replace
  ----------------------------------------------------
  The first version of this script built its replacement as
  "`$0$entry`r`n". PowerShell expanded $entry BEFORE .NET regex saw the
  string, so the substitution text arrived as `$02222 = 192.168.255.129:22`.
  .NET then read `$02222` as a group reference, found no group 2222, emitted it
  literally -- and because the match being replaced WAS the `[incomingtcp]`
  header line, the header was consumed. The file ended up with the header gone
  and a junk `$02222 = ...` line stranded under [netbios]. A substitution string
  that carries user data must never be handed to a regex engine; plain string
  insertion has no metacharacters at all.
#>
[CmdletBinding()]
param(
    [string]$Conf      = 'C:\ProgramData\VMware\vmnetnat.conf',
    [string]$GuestIp   = '192.168.255.129',
    [int]   $HostPort  = 2222,
    [int]   $GuestPort = 22,
    [string]$LogPath   = "$env:TEMP\vmnat-ssh-forward.log",
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$lines = New-Object System.Collections.Generic.List[string]
function Say([string]$m) { $lines.Add($m); Write-Host $m }

try {
    $elevated = ([Security.Principal.WindowsPrincipal] `
        [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Say "elevated       : $elevated"
    if (-not $elevated) { throw 'not elevated -- vmnetnat.conf is not writable and the NAT service cannot be restarted' }

    if (-not (Test-Path $Conf)) { throw "no such file: $Conf" }
    Say "conf           : $Conf"

    $hdr  = '[incomingtcp]'
    $text = [System.IO.File]::ReadAllText($Conf)

    # --- repair pass -------------------------------------------------------
    # If a previous run ate the section header, put the file back from the
    # newest backup that still contains it. Without this the script would
    # cheerfully add a forward to a section that no longer exists.
    if (-not $text.Contains($hdr)) {
        Say "DAMAGED        : '$hdr' header missing, restoring from backup"
        $bak = Get-ChildItem -Path "$Conf.bak-*" -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime -Descending |
               Where-Object { [System.IO.File]::ReadAllText($_.FullName).Contains($hdr) } |
               Select-Object -First 1
        if (-not $bak) { throw "header missing and no backup contains '$hdr'" }
        [System.IO.File]::Copy($bak.FullName, $Conf, $true)
        $text = [System.IO.File]::ReadAllText($Conf)
        Say "restored from  : $($bak.Name)"
        if (-not $text.Contains($hdr)) { throw 'restore did not bring the header back' }
    }

    # Drop any stranded artifact line left by the broken substitution.
    $junk = [regex]::Matches($text, '(?m)^\$\d+\s*=.*$').Count
    if ($junk -gt 0) {
        Say "junk lines     : $junk (removing)"
        $text = [regex]::Replace($text, '(?m)^\$\d+\s*=.*\r?\n', '')
        [System.IO.File]::WriteAllText($Conf, $text)
    }

    $entry = "$HostPort = ${GuestIp}:$GuestPort"
    Say "entry          : $entry"

    # An ACTIVE entry is one that is not commented out. Anchor on the port at
    # the start of a line so a commented example (#8889 = ...) never counts.
    $activeRe = "(?m)^\s*$HostPort\s*=.*$"
    $already  = [regex]::Matches($text, $activeRe).Count
    Say "existing active: $already"

    if ($Remove) {
        if ($already -eq 0) { Say 'RESULT: nothing to remove' }
        else {
            $backup = "$Conf.bak-remove"
            [System.IO.File]::WriteAllText($backup, $text)
            Say "backup         : $backup"
            $text = [regex]::Replace($text, "(?m)^\s*$HostPort\s*=.*\r?\n", '')
            [System.IO.File]::WriteAllText($Conf, $text)
            Say 'RESULT: entry removed'
        }
    }
    elseif ($already -gt 0) {
        Say 'RESULT: entry already present, file untouched'
    }
    else {
        $backup = "$Conf.bak-$([System.IO.Path]::GetRandomFileName())"
        [System.IO.File]::WriteAllText($backup, $text)
        Say "backup         : $backup"

        # Insert directly after the [incomingtcp] header line. Anchoring on the
        # header rather than appending at EOF matters: vmnat is section-scoped,
        # and the file ends inside [PrivilegedUDP].
        $i = $text.IndexOf($hdr)
        $nl = $text.IndexOf("`n", $i)
        if ($nl -lt 0) { throw 'no newline after the [incomingtcp] header' }
        $text = $text.Insert($nl + 1, "$entry`r`n")
        [System.IO.File]::WriteAllText($Conf, $text)

        # Re-read from disk. Verifying the in-memory string would only prove
        # the string was built, not that the file on disk carries it.
        $after  = [System.IO.File]::ReadAllText($Conf)
        $verify = [regex]::Matches($after, $activeRe).Count
        Say "verified active: $verify"
        if (-not $after.Contains($hdr)) { throw 'the [incomingtcp] header did not survive the write' }
        if ($verify -ne 1) { throw "expected exactly 1 active entry after write, found $verify" }
        Say 'RESULT: entry added'
    }

    Say '--- restarting VMware NAT Service (it re-reads the conf only on start) ---'
    Restart-Service -Name 'VMware NAT Service' -Force
    Start-Sleep -Seconds 3
    $svc = Get-Service -Name 'VMware NAT Service'
    Say "service state  : $($svc.Status)"

    $listen = Get-NetTCPConnection -State Listen -LocalPort $HostPort -ErrorAction SilentlyContinue
    if ($listen) {
        foreach ($l in $listen) { Say "listening      : $($l.LocalAddress):$($l.LocalPort) pid=$($l.OwningProcess)" }
    } else {
        Say "listening      : NOTHING on $HostPort"
    }
    Say 'DONE'
}
catch {
    Say "ERROR: $($_.Exception.Message)"
}
finally {
    [System.IO.File]::WriteAllLines($LogPath, $lines)
}

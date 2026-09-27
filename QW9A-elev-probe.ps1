# QW9A-elev-probe.ps1 -- run tools/_token-probe.py at HIGH integrity.
#
# Why this file exists: `Start-Process -Verb RunAs` and
# `-RedirectStandardOutput/-RedirectStandardError` belong to MUTUALLY EXCLUSIVE
# parameter sets. Combining them fails with
#   "Parameter set cannot be resolved using the specified named parameters."
# So the elevated child must do its own redirection: we launch cmd.exe elevated
# and let the shell inside the elevated process write the files.
#
# The probe is read-only (queries this process's own token, then a single
# SendInput mouse-move of zero distance). It changes nothing on the host.

$ErrorActionPreference = 'Stop'

$py    = 'C:\Python314\python.exe'
$wd    = 'D:\nerd shit\other projects\mac os'
$probe = Join-Path $wd 'tools\_token-probe.py'
$out   = Join-Path $wd 'QW9A-elev-token.txt'
$err   = Join-Path $wd 'QW9A-elev-token.err'

Remove-Item -LiteralPath $out, $err -ErrorAction SilentlyContinue

# cmd /c needs the whole command wrapped in an extra pair of quotes when the
# individual tokens are themselves quoted -- hence the doubled quotes.
$inner = '""{0}" "{1}" > "{2}" 2> "{3}""' -f $py, $probe, $out, $err

Start-Process -FilePath 'cmd.exe' `
              -ArgumentList ('/c ' + $inner) `
              -WorkingDirectory $wd `
              -Verb RunAs `
              -WindowStyle Hidden `
              -Wait

Write-Output ('elevated child exited; out exists=' + (Test-Path $out) + ' err exists=' + (Test-Path $err))

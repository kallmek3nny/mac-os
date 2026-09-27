# OPERATOR-ACTION — SSH-through-VPN: RESOLVED

**Written 2026-08-14, updated the same day. The path works.**

```
ssh -p 2222 kenny@127.0.0.1
```

That reaches the macOS guest's sshd with the ProtonVPN kill switch **up**. Type
the account password at the prompt; nothing else is needed.

---

## 0. The measurement that closes this

After Remote Login was enabled in the guest, the same forward that had been
returning a 1 ms RST now returns a real SSH server:

```
2222 -> guest 22    BANNER b'SSH-2.0-OpenSSH_9.9\r\n'   after 0.432s
```

and a full connection attempt completes key exchange before stopping at auth:

```
Warning: Permanently added '[127.0.0.1]:2222' (ED25519) to the list of known hosts.
kenny@127.0.0.1: Permission denied (publickey,password,keyboard-interactive).
```

Both lines are positive results, not failures. The host key was received and
recorded, which only happens after a successful KEX — so the whole transport is
up end to end. "Permission denied" appeared because that probe ran with
`BatchMode=yes` and `PreferredAuthentications=publickey`, which offers no
password; the method list in the message is sshd telling us `password` and
`keyboard-interactive` **are** accepted. Run the command interactively and type
the password.

Note the contrast with the pre-VPN-era failure mode: "Permission denied" from a
WFP block is `WSAEACCES` and arrives with no banner and no host key. Read the
wording and check whether a host key was added before calling this an auth
failure.

§5's warning still stands and is unchanged — keystroke injection into the guest
is dead on this host, so the Remote Login toggle had to be, and did have to be,
clicked by a human.

---

## 1. What is already in place, and verified

`C:\ProgramData\VMware\vmnetnat.conf`, `[incomingtcp]` section, all live on the
VMware NAT Service and confirmed listening:

| Host port | Guest destination | Service |
|---|---|---|
| 2222 | `192.168.255.129:22` | sshd |
| 5901 | `192.168.255.129:5900` | Screen Sharing / VNC |
| 5488 | `192.168.255.129:548` | AFP |

Re-add or repair them with:

```
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\vmnat-ssh-forward.ps1"
```

Four further forwards were added purely as diagnostic controls (4445, 4446,
6106, 8088) and have been removed again by `tools\vmnat-cleanup-probes.ps1`.
The removal was verified: 4445 now refuses at the host with `WSAECONNREFUSED`,
while 2222 / 5901 / 5488 still reach the guest.

All of these scripts write a transcript to `%TEMP%` whether they succeed or
fail, because an elevated child's console closes before its output can be read.

---

## 2. Why a NAT forward beats the VPN, in one line

`vmnat` does not `connect()` to reach the guest — it writes frames into the
vmnet8 device, so it never trips an `ALE_AUTH_CONNECT` filter. `127.0.0.1:2222`
is then a host-local connect, which the kill switch permits.

The measurements that localised the block:

| Probe | Result | What it rules out |
|---|---|---|
| TCP to `1.1.1.1:443`, `9.9.9.9:443`, `10.2.0.1:53` | OPEN | the sandbox outbound-block rule, whose range covers these |
| TCP to `192.168.255.129:22` | `WSAEACCES` (10013) | a guest-side cause; this is a host filter |
| Same, source pinned to `192.168.255.1` | `WSAEACCES` | an interface-scoped filter |
| UDP `sendto` to the same guest address | ACCEPTED | a filter on all sockets |
| TCP to `192.168.255.1:445`, `127.0.0.1:22` | OPEN / `WSAECONNREFUSED` | a filter on host-local destinations |
| `ssh.exe` and `curl` to the guest | both fail identically | anything process-specific |

So the filter is **TCP connect authorization scoped by destination address**,
owned by ProtonVPN's Standard kill switch (`IsKillSwitchEnabled` is `true`).

**A supported alternative to the whole forward:** Proton VPN app → Settings →
Connection → **Allow LAN connections**. Proton's own assemblies here carry that
setting (`AllowLanConnections` / `IsLanConnectionsEnabled` in
`ProtonVPN.Client.Core.dll` and `ProtonVPN.Client.Logic.Connection.dll`, v5.1.5)
and the key appears in neither settings file, so it has never been set. Turning
it on makes `192.168.255.129:22` reachable directly, no forward needed. The
forward exists because it works without changing your VPN posture at all.

---

## 3. How we know the guest is alive and merely refusing

Every forward to the guest returns an **immediate EOF ~1 ms** after connect. On
its own that is ambiguous — it could equally mean vmnat never reached anything.
Two controls separate the cases, and they disagree with the guest:

| Forward | Behaviour |
|---|---|
| `4445 -> 192.168.255.1:445` (live control, measured OPEN) | **held open silent 12 s** |
| `4446 -> 192.168.255.200:22` (dead address: no lease, no ARP) | **held open silent 12 s** |
| `2222 -> guest 22`, `5901 -> guest 5900`, `5488 -> guest 548` | **EOF in ~1 ms** |

`vmnat` hangs when nobody answers and EOFs fast only on an **active RST**. So
the relay reaches the guest, the guest's IP stack is live, and it is refusing
those ports itself. Corroborated at the link layer: ARP holds the guest at
`00-0C-29-E7-9F-2B` and `vmnetdhcp` renewed its lease at 16:54 the same day,
with the kill switch up throughout.

The first survey used only expected-negative guest ports (631, 88) as controls.
They EOF'd identically to port 22 — a null result, because "guest RSTs
everything" and "vmnat reaches nothing" predict the same output. A control has
to differ in the mechanism under test, not merely in the expected answer.

---

## 4. Why the terminal route does not work, and it is not the password

The guest console is sitting at a live `kenny@kennys-Mac` shell. Read off that
screen by OCR:

```
sudo systemsetup -setremotelogin on
  ... requires Full Disk Access privileges
sudo systemsetup -getremotelogin
  Remote Login: Off
```

That is macOS TCC, not authentication. `systemsetup` refuses regardless of sudo
unless the calling app — Terminal — holds Full Disk Access. So the terminal path
needs either

- **System Settings → Privacy & Security → Full Disk Access → add Terminal**,
  then re-run `sudo systemsetup -setremotelogin on`; or
- the plain **Sharing → Remote Login** toggle in §0, which needs no FDA at all.

The second is fewer clicks and is the recommended route.

---

## 5. Keystroke injection is dead on this host — do not retry it

Driving that guest terminal from Windows was attempted three ways and fails at
the Windows input layer, not in the guest:

| Attempt | Result |
|---|---|
| unelevated `tools/vm-type.py` | "2 event group(s) accepted by SendInput", screenshot md5 **unchanged** |
| elevated, with `--click` | refused: `GetForegroundWindow` returned None |
| elevated, full foreground-unlock (lock-timeout 0, synthetic ALT, `AttachThreadInput`, `SwitchToThisWindow`) | focus **achieved** (`match = True`), SendInput accepted, screenshot md5 **still unchanged** |

The last row is the conclusive one: focus was proven before injecting and the
framebuffer still did not change, so the events are being discarded somewhere
below the window. `SendInput` reports success even when every event is dropped —
the identical md5 (`73a964bea32cd09eb0f71b07ab00b923` across four captures) is
the evidence, never the accept count.

---

## 6. Alternatives checked and dead

- **IPv6 link-local to the guest.** vmnet8 has no IPv6 address at all, so
  Windows cannot source a packet on that interface regardless of the filter.
- **Windows `sudo`.** Present at `C:\WINDOWS\system32\sudo.exe` but disabled on
  this machine.
- **`vmrun` guest ops.** Still need the account password, and still hang on an
  interactive prompt if it is omitted — see `OPERATOR-ACTION-guest-creds.md`.
  Not needed for §0.

---

## 7. A config file was damaged and repaired during this work

Recorded because the repair is verified and the cause is worth not repeating.
An earlier build of `tools/vmnat-ssh-forward.ps1` interpolated its entry into a
.NET regex **replacement** string, so `$02222` parsed as a group reference and
consumed the `[incomingtcp]` header line it had matched on. Exactly one line of
`vmnetnat.conf` differed from the backup.

The current script repairs that as its first pass and did so: its log shows
`DAMAGED -> restoring from backup`, then `header survived : True`. The file is
intact. Both defects are fixed — the insert now uses `String.Insert`, and the
verify asserts the **anchor** survived rather than only counting the entry it
added. A count of what you added cannot detect the loss of what you matched on.

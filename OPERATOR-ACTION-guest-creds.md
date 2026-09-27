# OPERATOR ACTION — one password unblocks the guest

**Filed by** `bold-fox-9c`, 2026-08-10, against crew task **t-396740**.

The original ask was for *two* things: the macOS guest account **short name** and its
**password**. The short name is now settled from guest-originated evidence. **Only the
password is still needed.**

---

## What you have to do

From the project root, in `cmd.exe`:

```
set /p MACOS_GUEST_PASS=password:
tools\guest-enable-ssh-kenny.cmd
```

That keeps the password out of `argv` and out of your command history. If you don't care
about that, the one-liner works too:

```
tools\guest-enable-ssh-kenny.cmd yourpassword
```

Nothing else. The wrapper hardcodes `MACOS_GUEST_USER=kenny` and delegates to the existing
`tools/guest-enable-ssh-via-tools.cmd`, which authenticates, copies
`tools/guest-enable-ssh.sh` into the guest, runs it under `sudo -S`, copies the output back
to `shots/guest-enable-ssh.out.txt`, and verifies with `systemsetup -getremotelogin`,
`launchctl`, and `netstat`.

---

## Why the short name is settled

The full **8,589,934,592 bytes** of the *live* guest memory file
`C:\Users\Kentl\macos-vm\sonoma\564d3ffb-d11c-edd5-265a-225898e79f2b.vmem` were scanned end
to end on 2026-08-10:

| pattern | count |
|---|---|
| `/Users/kenny/Library` | 2106 |
| `/Users/kenny/Library/{Preferences,Containers,Application Support,Caches}` | 631 |
| `/Users/kenny/Movies` | 20 |
| `/Users/<not-kenny>/Library/{Preferences,Containers,Application Support}` | **0** |

A bare `/Users/<name>` hit proves nothing — `aliu` (9), `cloud` (5), `rdimac` (4) and `eric`
(3) also appear, and those are Apple **build-machine** paths baked into shipped binaries.
The discriminator is a *runtime* subpath: only an account that has actually logged in leaves
thousands of resident `Library/Preferences` and `Library/Containers` references. Exactly one
name does, and no other name produces even one.

Also measured: `kenny_zsh` = 0 and `kenny_ssh` = 0. There is no evidence of a prior terminal
session or an existing `~/.ssh` in the guest, which is consistent with a GUI-only account.

---

## Do not guess, and do not read the error text

Both of these were run on 2026-08-10 against the running guest:

```
vmrun -gu kenny           -gp <wrong>  ->  Error: Invalid user name or password ... exit 127
vmrun -gu zzznosuchuser9c -gp <wrong>  ->  Error: Invalid user name or password ... exit 127
```

**Byte-identical output, identical exit code.** VMware collapses unknown-user and
bad-password into one response. So:

- A failure tells you nothing about *which* half was wrong.
- "The short name must be wrong, let me try another" is unfalsifiable reasoning here.
- Repeated attempts risk tripping the macOS account lockout, which would cost real time.

If `guest-enable-ssh-kenny.cmd` fails to authenticate, **question the password first.**

---

## Separate problem: you cannot SSH to the guest yet (and it isn't the guest's fault)

The host currently cannot reach `192.168.255.129` at all. This is **not** VMware and **not**
sshd. Control matrix, all from one window on the host:

| target | what it is | result |
|---|---|---|
| `192.168.255.1:445` | host's own VMnet8 address | **CONNECTED** |
| `192.168.0.237:445` | host's own Wi-Fi address | **CONNECTED** |
| `8.8.8.8:53`, `1.1.1.1:443` | public internet | **CONNECTED** |
| `192.168.0.1:80` and `:443` | **physical Wi-Fi gateway** | WSAEACCES |
| `192.168.255.129:22` | the macOS guest | WSAEACCES |
| `192.168.255.254:22` | VMware NAT device | WSAEACCES |

The physical LAN gateway failing *identically* to the guest is the discriminator: every
off-host **private** destination is refused before a SYN leaves the stack, while everything
host-local or public works.

**Cause:** the `ProtonVPN` adapter is `Up` owning the `0.0.0.0/0` route at metric 0, both
`ProtonVPN Service` and `ProtonVPN WireGuard` are Running, and
`%LOCALAPPDATA%\Proton\Proton VPN\Storage\GlobalSettings.json` contains
`IsKillSwitchEnabled: "true"` and `KillSwitchMode: "Standard"` with **no LAN-allow key
stored** (default = LAN blocked).

**Ruled out — do not re-chase these:**

- *Mullvad* — measured `Disconnected`, lockdown-mode `off`, LAN sharing `allow`.
- *The `codex_sandbox_offline_block_outbound` firewall rule* — `8.8.8.8` sits inside its
  `0.0.0.0-126.255.255.255` range and connects fine, so the rule is not what's biting.
- *No route / adapter down* — VMnet8 is `Up` at `192.168.255.1/24`, and `Find-NetRoute` for
  `192.168.255.129` resolves via VMnet8 on `192.168.255.0/24`.

Positive evidence the guest itself is alive at L2: `Get-NetNeighbor` lists
`192.168.255.129` with the correct MAC `00-0C-29-E7-9F-2B` in state `Stale` — the guest
answered an ARP request.

### Scope of that problem

| affected | not affected |
|---|---|
| `ssh kenny@192.168.255.129` from the host | `tools\guest-enable-ssh-kenny.cmd` |
| VNC to the guest | any `vmrun -gu/-gp` guest operation |
| the runner's *post-run SSH verification* step | the runner's copy/run/read-back steps |

`vmrun` guest ops ride the VMCI/backdoor channel to `vmware-vmx.exe` on the host and never
touch TCP/IP. **So the credential task is not blocked by the network** — run the wrapper
regardless.

### Fix, when you want host→guest IP reachability

Operator UI action, not something to script around: enable **Allow LAN connections** in the
ProtonVPN client, or disconnect the VPN. Then re-run the control matrix above before reading
any port result as a fact about the guest.

---

## Preflight already verified on this host

| item | state |
|---|---|
| `vmrun list` | 1 running VM, `Sonoma.vmx` |
| `vmrun checkToolsState` | `running` |
| `vmrun.exe` path in the runner | exists |
| `Sonoma.vmx` path in the runner | exists |
| `tools/guest-enable-ssh.sh` | exists, **pure LF** (correct for a Unix guest) |
| `tools/guest-enable-ssh-kenny.cmd` | exists, **all CRLF** (114 CRLF / 0 bare LF) |
| `shots/` output directory | exists |
| `C:\Users\Kentl\.ssh\id_ed25519.pub` | exists, will be installed into the guest |
| wrapper no-password path | self-tested: exits **3**, no interactive prompt |
| `shots/guest-enable-ssh.out.txt` | **absent** — the runner has never completed a run |

---

## Fix applied 2026-08-10: the password no longer lands on a second command line

The wrapper used to end with `call "%~dp0guest-enable-ssh-via-tools.cmd" %*`. That
forwarded your password as `%1` to the callee — which **never reads it**. Verified by
grep: every `%~1` in `guest-enable-ssh-via-tools.cmd` is either a `%~dp0` path expansion
or the step-label parameter of the `:toolsguard` subroutine at line 306, called from lines
144, 154, 173, 190, 205, 225 and 240 with strings like `"step 3 (copy in)"`. The callee
takes credentials from `MACOS_GUEST_USER` / `MACOS_GUEST_PASS` in the **environment**
only.

So the forwarding bought nothing and cost you a cleartext password on an extra process
command line, visible to anything that can enumerate processes. The `call` is now bare;
`setlocal` already scopes both variables into the child. Dropping `%*` is behavior-neutral
and the no-password self-test still exits **3** with no interactive prompt.

Both usages in **What you have to do** above are unaffected — the wrapper itself still
reads `%~1` into `MACOS_GUEST_PASS`. The `set /p` form remains the better one.

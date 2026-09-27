# NETWORK-DIAGNOSIS.md — the Recovery "Internet connection is required" wall

**Written 2026-08-07 for `t-ab1317`. Cause found and fix verified the same day
— see the banner below.** Localises the failure hop by hop. The host side is
measured and recorded below; the guest-side runbook is retained because it is
the method that would localise the *next* failure, not because this one is
still open.

**Read the banner first, then the host section.**

> **RESOLVED 2026-08-07 — hop 1. The cause is confirmed and it is not Mullvad.**
> The 15.4.1 / 24E263 recovery kernelcache contains **no Intel ethernet kext of
> any kind**, while the VM is configured `ethernet0.virtualDev = "e1000e"`. So
> `en0` never comes up and no `DHCPDISCOVER` is ever sent. Established from
> `BaseSystem.img` host-side, by enumerating every prelinked network kext —
> `AppleVmxnet3Ethernet` is present, nothing Intel is. Fix:
> `ethernet0.virtualDev = "vmxnet3"`, and delete `ethernet0.pciSlotNumber`.
> Full evidence and the four-step procedure are in `HANDOFF.md` §8 and
> `STEPS.md` Step 5.
>
> **The fix is applied and verified (2026-08-07, `t-09d0fd`).** New instance
> powered on `18:51:31.641Z` with `vmxnet3` (`vmware.log` `DICT ethernet0.*`).
> A **new** lease for `00:0c:29:e7:9f:2b` starts `18:52:35` — 64 s *after*
> power-on — and now carries `client-hostname "Mac"`, which the stale one did
> not. Host → guest `ping 192.168.255.129` is 3/3, 0% loss, TTL 64. Under
> `e1000e` that guest was silent on the wire for 87 minutes across a full boot.
>
> **The timestamp is the test, not the lease's existence.** The MAC does not
> change across a NIC swap, so "a lease exists for this MAC" passes on the
> stale record. Compare `starts` against the power-on line in `vmware.log`.
>
> **Everything below is preserved as written, and is still worth reading.** The
> hop chain, the eliminations, and the Mullvad analysis were correct work — the
> Mullvad hypothesis was properly labelled unproven and carried the right
> one-step test. It is now *eliminated for this failure* by measurement: ARP and
> `DHCPDISCOVER` are L2 broadcasts on VMnet8 that never touch the tunnel or the
> default route, so a VPN cannot produce a flat `BytesIn` counter or a missing
> ARP entry — both are **pre-egress**. Mullvad remains a live **post-swap**
> suspect the moment `en0` exists, because the tunnel does own the default route
> at metric 1 with MTU 1380.

> **CORRECTED 2026-08-13 — a VPN does block host→guest TCP, but it is NOT
> Mullvad.** An earlier note in this file blamed Mullvad. Measured the same day:
> `mullvad.exe status` reports **Disconnected** and `lockdown-mode get` reports
> **off**, so Mullvad filters nothing. The active tunnel is **Proton VPN** —
> `ProtonVPN.Client.exe` plus `ProtonVPNService.exe` running, adapter `ProTUN` at
> `10.2.0.2/32` owning `0.0.0.0/0` at metric 1, and
> `GlobalSettings.json` carrying `IsKillSwitchEnabled = true`,
> `KillSwitchMode = Standard`.
>
> The lesson is the one this file already teaches elsewhere: a *running process*
> is not a *connected tunnel*. Two VPN clients were installed; the one with
> processes in the task list was the innocent one. Query each client's own status
> before attributing a filter to it.
>
> This does **not** reopen the DHCP diagnosis: the guest still holds its lease and
> still answers ARP.

---

## The VPN blocks the host, not the guest — measured 2026-08-13 ~23:10 UTC

Asked whether the guest could simply be reached over SSH. It cannot, and the
reason is on the **host**, one layer above the wire.

| Target | ICMP | TCP |
|---|---|---|
| `192.168.255.129` (guest) | 3/3 **General failure** | `:22`, `:5900`, `:445` → WinError **10013** |
| `192.168.0.1` (physical Wi-Fi gateway) | 2/2 **General failure** | `:80` → WinError **10013** |
| `192.168.255.1` (host's own vmnet8 adapter) | 2/2, 0% loss | `:445` → **OPEN** |
| `127.0.0.1` | 2/2, 0% loss | `:445` → **OPEN** |
| `1.1.1.1` | — | `:443` → **OPEN** |

Three things follow, and the third is the one that settles it:

1. **`General failure` is not a network timeout.** Windows emits it when the
   local stack refuses to hand the packet to an interface at all. Paired with
   `WSAEACCES` (10013) on every TCP attempt, that is a host packet filter, not a
   silent peer. A dropped packet on the wire looks like `Request timed out`.
2. **The physical LAN gateway fails identically to the guest.** `192.168.0.1` is
   not a VM, not on vmnet8, and not something VMware touches. Whatever refuses
   the guest refuses the operator's own router the same way, so no VMware or
   guest-side explanation can account for it.
3. **The guest is provably still on the wire.** `arp -a -N 192.168.255.1` holds
   `192.168.255.129` at `00-0c-29-e7-9f-2b`, type **dynamic** — a learned entry,
   which requires the guest to have answered an ARP request. Loopback and
   `192.168.255.1` also pass, so vmnet8 itself is not filtered; only routed
   RFC1918 destinations are.

The tunnel at `10.2.0.2/32` owns `0.0.0.0/0` at metric **1** against the physical
NIC's **35**, and it belongs to **Proton VPN**, not Mullvad. Attribution, each
read from the product's own state rather than from the task list:

| Client | Processes running | Own reported state |
|---|---|---|
| Mullvad | `mullvad-daemon.exe` + 2 GUI | `status` = **Disconnected**, `lockdown-mode` = **off**, `lan get` = **allow** |
| Proton | `ProtonVPN.Client.exe`, `ProtonVPNService.exe`, `ProtonVPN.NrptWatchdog.exe` | adapter `ProTUN` up at `10.2.0.2/32`, `IsKillSwitchEnabled` = **true**, `KillSwitchMode` = **Standard** |

Mullvad had the more conspicuous process footprint and was the innocent one. A
running daemon is not a connected tunnel.

Proton's settings store (`%LOCALAPPDATA%\Proton\Proton VPN\Storage\`) has **no
allow-LAN key at all** across either settings file's 30 and 27 keys — only
`IsKillSwitchEnabled`, `KillSwitchMode`, and `SplitTunnelingStandardAppsList`
(empty). So there is no LAN-sharing toggle to flip the way Mullvad has one.

### Three host-side workarounds, all measured, all blocked

Asked whether SSH could be reached without disabling the VPN. Each candidate was
tested rather than reasoned about:

| Route | Result | Why |
|---|---|---|
| Bind the source socket to `192.168.255.1` and connect out | **10013** on both `:22` and `:445` | The filter keys on DESTINATION, not source interface. `bind()` changes nothing. |
| VMware NAT port forward (`127.0.0.1:2222` → guest `:22`) | **cannot install** | `vmnetnat.conf` is `BUILTIN\Users:(I)(RX)` — read+execute. Opening it `a` or `r+` raises `PermissionError`; the atomic-rename write failed `EPERM`. Restarting `VMware NAT Service` also needs elevation (`sc stop` → error 5). |
| Reverse direction: host listens, guest dials out | **listener binds OK**, but unusable | `bind()`+`listen()` on `192.168.255.1:2222` and `0.0.0.0:2222` both succeed, and the guest connecting TO a host-owned IP is the one direction the filter permits. But the active firewall profile is `BlockInbound`, and adding a rule needs elevation. |

A caution about the second row: `os.access(path, os.W_OK)` returned **True** for
`vmnetnat.conf`, and copying it into the same directory succeeded. Neither is a
write test. `os.access` on Windows only consults the read-only ATTRIBUTE and
ignores the ACL, and a successful `cp` measures the DIRECTORY's permissions.
Only opening the file for write, or reading `icacls`, answers the question.

### Why none of it would get a shell anyway

Worth writing down so nobody spends the evening on the firewall:

- **Nothing has ever shown `sshd` listening in this guest.** Remote Login has
  never been enabled — that is card `t-ed1a53`, still open, and blocked on
  `t-396740`, the password.
- **SSH needs the same credential that blocks `vmrun`.** It is not a way around
  the missing password; it is another consumer of it.
- A 10013 refusal describes the **host** filter and says nothing about whether
  the guest port is open. Re-probe by another path before calling port 22
  anything.

---

## The hop chain

```
guest NIC -> vmnet8 switch -> vmnetdhcp.exe -> vmnat.exe -> host uplink -> Apple
   (1)            (2)              (3)            (4)           (5)
```

| Hop | Status | Evidence |
|---|---|---|
| 1 guest NIC | **was the cause — now up on `vmxnet3`** | no Intel ethernet kext in the 15.4.1 recovery kernelcache; `e1000e` could not bind. After the swap: guest answers ARP and ICMP |
| 2 vmnet8 switch | **up** | host adapter `192.168.255.1`, VMX link propagation on |
| 3 DHCP | **up, serving the guest** | new lease `starts 18:52:35`, 64 s after the `18:51:31` power-on, `client-hostname "Mac"` |
| 4 NAT | **process up, egress unproven** | `vmnat.exe` running; nothing proves it forwards |
| 5 host uplink | **up** | `ping 8.8.8.8` 0% loss, DNS resolves |

---

## Host-side findings — all measured 2026-08-07 ~18:11–18:17 UTC

### The host itself is online. Do not chase this.

```
ping 8.8.8.8              -> 2/2 replies, avg 18 ms
ping swscan.apple.com     -> resolves to 23.222.197.13, 2/2 replies
```

Apple's software-update host is reachable **from the host**. Whatever is broken
is between the guest and the host uplink, not beyond it.

### vmnet8 is configured and its daemons are running

| Fact | Value | Source |
|---|---|---|
| Applied NIC | `nat`, **`vmxnet3`**, `startConnected = TRUE` | boot `18:51:31.356Z`, L338–342 (`DICT ethernet0.*`) |
| Guest MAC | `00:0c:29:e7:9f:2b` | boot `18:51:31.356Z`, L687 |
| Link propagation | `lsp.state = 5` (L1102), `MACVNetConnectToNetwork` (L1103–1104) | boot `18:51:31.356Z` |
| Host vmnet8 | `192.168.255.1/24` | `ipconfig /all` |
| NAT gateway | `192.168.255.2`, `device = vmnet8` | `vmnetnat.conf` |
| `vmnat.exe` | PID 5740, running | `tasklist` |
| `vmnetdhcp.exe` | PID 5660, running | `tasklist` |

> **Sourcing note — corrected 2026-08-07.** An earlier revision of this table
> cited `Sonoma.vmx` lines 142–146 and 189 for the NIC facts. **That is the
> wrong source.** The on-disk VMX was edited at 16:59 UTC, 22 minutes *after*
> this instance powered on at 16:36, and VMware reads the VMX once at power-on —
> so the file on disk is not a valid description of the running VM. The values
> happen to agree here, but the citation was unsound. The authoritative record
> of what the running instance actually got is the `DICT` dump in `vmware.log`.
> **Cite the log, not the VMX, for anything about a VM that is already up.**

> **Two further corrections to the table above, measured 2026-08-09.**
>
> 1. **The applied NIC was `vmxnet3`, not `e1000e`.** The strings `e1000e` and
>    `82574L` appear **zero times** in the log this table cites. L340 reads
>    `DICT ethernet0.virtualDev = "vmxnet3"`. The `e1000e` row was wrong at the
>    time it was written — that value describes the *pre-swap* configuration
>    recorded in `BASELINE-preswap-20260807T1841Z.txt`, not the running
>    instance. This does **not** weaken the "Hop 2 is eliminated" conclusion:
>    the vNIC was instantiated and connected either way, and `AppleVmxnet3Ethernet`
>    is prelinked in `BaseSystem.img`, so `vmxnet3` is the *more* claimable
>    device, not less.
> 2. **The line numbers drifted.** The guest MAC is L687, not L690;
>    `lsp.state = 5` is L1102, and only `MACVNetConnectToNetwork` is at
>    L1103–1104.
>
> Both errors share one cause: **`vmware.log` is not a stable citation target.**
> VMware rotates it on every power-on and keeps only four generations
> (`vmware.log` → `-0` → `-1` → `-2`), so a bare `vmware.log:<N>` silently
> repoints at a *different boot* whose line N holds different text — which is
> exactly how an `e1000e` reading survived into a `vmxnet3` boot. Cite the
> boot's **L1 timestamp** with every line number, and quote the line inline. See
> the rotation table in `VM-BRINGUP.md` for the surviving generations.

The VM is powered on (`vmrun list` -> 1 running) and the log shows the vNIC
connected to the network at power-on. **Hop 2 is eliminated.**

### Two ping failures that are NOT evidence

```
ping 192.168.255.2    -> 100% loss   (NAT gateway)
ping 192.168.255.254  -> 100% loss   (DHCP server)
```

**Do not read these as failures.** VMware's NAT and DHCP pseudo-addresses are
not required to answer ICMP from the host's own vmnet8 adapter, and routinely
don't. Recorded here specifically so nobody re-runs them and "discovers" a
broken gateway. The ARP table *does* hold `192.168.255.254` at
`00-50-56-e4-43-4c`, so the DHCP endpoint is live on the wire.

### The lease is stale, and the guest is not on the wire

```
lease 192.168.255.129 {
        starts 5 2026/08/07 16:20:12;
        ends   5 2026/08/07 16:50:12;
        hardware ethernet 00:0c:29:e7:9f:2b;
}
```

Leases file mtime: **16:20:12 UTC**. Current time when checked: **18:17 UTC**.

Two things follow, and they matter:

1. **The guest did get a lease once** — at 16:20, sixteen minutes *before* the
   current run's 16:36 power-on. So this lease belongs to a **previous boot**.
   The currently-running guest has never obtained an address.
2. **It expired at 16:50 and was never renewed.** A guest with a working NIC
   renews at T/2. This one didn't, across 87 minutes.

Live confirmation:

```
arp -a -N 192.168.255.1   -> 192.168.255.254 and broadcast only
                             NO entry for 192.168.255.129
                             NO entry for 00-0c-29-e7-9f-2b
ping 192.168.255.129      -> "Destination host unreachable" from 192.168.255.1
```

**The guest is not answering ARP on vmnet8.** That is hop 1 or hop 2, and hop 2
is already eliminated — so this points at the guest's own interface being down
or unconfigured.

---

## Superseded hypothesis: Mullvad is holding the default route

**Eliminated as the cause of *this* failure (see the resolution note at the top),
but retained in full** — every measurement below is accurate, and Mullvad becomes
a live suspect again the moment `en0` exists. It is invisible from inside the
guest, so it will not resurface on its own.

```
Active Routes:
  0.0.0.0  0.0.0.0   192.168.0.1    192.168.0.237    metric 35
  0.0.0.0  0.0.0.0    10.64.0.1    10.148.93.182    metric  1   <- wins
```

`mullvad-daemon.exe` (PID 5372) plus four `Mullvad VPN.exe` processes are
running, and the tunnel interface `10.148.93.182` owns the default route at
**metric 1** against the physical NIC's 35. Host DNS resolves via `10.64.0.1` —
Mullvad's resolver.

That is why the host's own connectivity tests pass: *the host* is inside the
tunnel. The question those tests do **not** answer is whether `vmnat.exe` is
allowed to forward guest traffic into it.

Mullvad ships a firewall/kill-switch that blocks non-tunnel traffic. A VMware
NAT adapter is exactly the kind of interface that gets blocked: `vmnat.exe`
sources packets from `192.168.255.x`, which is not the tunnel's subnet.

**This is a hypothesis, not a measurement.** Nothing here proves Mullvad is
dropping the traffic — it proves Mullvad owns the route and has the means. The
test is one action, below.

### How this was settled — and why it was a reasonable hypothesis

For a while there were two candidates, both consistent with every measurement,
because the host-side evidence (no lease this boot, guest MAC absent from ARP,
flat VMnet8 counters) is what a **silent guest** looks like — and *both* produce
a silent guest:

| Hypothesis | Hop | Guest emits frames? | Status |
|---|---|---|---|
| **No NIC driver** (`e1000e` unsupported in 15.4.1 Recovery) | 1 | no | **confirmed** — kext enumeration of `BaseSystem.img` |
| **Mullvad blocks `vmnat.exe`** | 3–4 | yes, they die on the host | eliminated for this failure; live again post-swap |

What separates them is *where in the stack the evidence sits*. ARP and
`DHCPDISCOVER` are **L2 broadcasts on VMnet8** — they never reach the tunnel or
the default route. A VPN kill-switch therefore **cannot** produce a flat
`BytesIn` counter or a missing ARP entry, because both are **pre-egress**. That
is a measurement, not an argument, and it is what ruled Mullvad out without ever
touching the VPN.

**Method note worth keeping:** pre-egress evidence (byte counters, ARP tables)
discriminates between "guest not talking" and "guest talking but blocked". No
host-side *reachability* test can make that distinction — which is why the host
passing every connectivity check told us nothing for so long.

---

## Do this first — the vmxnet3 swap, not the VPN

**Superseded 2026-08-07.** This section used to say *disconnect Mullvad first*.
Do **not** do that now: it changes host state, buys nothing diagnostically, and
if the NIC swap then worked nobody could say which change was responsible.

The actual first step is the four-part procedure in `HANDOFF.md` §8 /
`STEPS.md` Step 5:

1. `ethernet0.virtualDev = "vmxnet3"` (was `e1000e`).
2. **Delete** `ethernet0.pciSlotNumber` — the VMX's own comment block records
   two power-on crashes from that field being wrong for the device.
3. Fix the VMX Network comment; it asserts the opposite of the truth for
   macOS 15 and is what produced this blocker.
4. **Full power cycle**, not a guest reboot — the `.vmx` is read once at
   power-on.

### Acceptance test, and it is falsifiable both ways

Note the power-on timestamp from `vmware.log` line 1, then:

```
grep -A 4 '00:0c:29:e7:9f:2b' "C:/ProgramData/VMware/vmnetdhcp.leases"
```

A lease whose `starts` timestamp is **later than that power-on** closes this.

> **The lease file is a trap.** A stale entry carries the *same MAC* and reads
> as a success at a glance — the MAC does not change across the NIC swap. Only
> the timestamp distinguishes them, so compare against the power-on time every
> single time.

**If `en0` appears and no fresh lease follows, this diagnosis is dead** and
Mullvad's kill-switch returns to the front — at which point the superseded
procedure above becomes the right next move again.

---

## Guest-side runbook — macOS Recovery Terminal

`Utilities -> Terminal` from the Recovery menu bar. Run in order and **stop at
the first one that fails** — that's your hop.

### 1. Does the guest see the NIC at all?

```sh
ifconfig -a
networksetup -listallhardwareports
```

Expect an `en0` with `ether 00:0c:29:e7:9f:2b`.

- **No `en0`** -> macOS has no driver bound to the vNIC. **This is what actually
  happened here, and it is settled, not suspected:** macOS 15.4.1 Recovery ships
  no driver for VMware's `e1000e`. Fix is `ethernet0.virtualDev = "vmxnet3"` with
  no `ethernet0.pciSlotNumber`, then a full power cycle — proven working
  2026-08-07, see the banner at the top of this file. Do not re-run this step to
  "confirm" it on an `e1000e` config; change the VMX instead.
- **`en0` present but no `inet`** -> NIC exists, DHCP failed. Go to 2.

### 2. Force a DHCP request

```sh
ipconfig set en0 DHCP
sleep 5
ipconfig getifaddr en0
```

Expect something in `192.168.255.128–254`.

Check the host side immediately after — a new `starts` timestamp in
`vmnetdhcp.leases` proves the request reached hop 3.

- **Address obtained, host sees a fresh lease** -> hops 1–3 good, go to 3.
- **No address, no lease movement on the host** -> the request never left the
  guest or never crossed the switch. Re-check `ifconfig en0` for
  `status: active`; if it says `inactive`, it's the vNIC/driver again.

### 3. Can it reach the gateway?

```sh
route -n get default
ping -c 3 192.168.255.2
```

Unlike the host->gateway ping above, **the guest->gateway ping is meaningful** —
VMware's NAT does answer its own clients.

- **Gateway replies** -> L3 to NAT works, go to 4.
- **No reply but an address is assigned** -> NAT is not servicing the guest.
  Back to the Mullvad test.

### 4. Is it DNS or is it transport?

```sh
ping -c 3 17.253.144.10       # Apple, by IP — no DNS involved
dig +short swscan.apple.com   # or: nslookup swscan.apple.com
```

This split is the whole point of the step:

- **IP ping works, DNS fails** -> NAT forwards but the DNS relay doesn't.
  `vmnetnat.conf` has `[dns] policy = order, autodetect = 1`, and autodetect
  picks up the *host's* resolver — which is currently Mullvad's `10.64.0.1`.
  Work around it by setting the guest's DNS explicitly to `8.8.8.8`.
- **Both fail** -> NAT egress is blocked. Mullvad again.
- **Both work but Recovery still refuses** -> go to 5.

### 5. Is it TLS/time, not connectivity?

```sh
date
curl -sSI https://swscan.apple.com | head -3
```

**Check `date` before anything else here.** A guest clock far from real time
fails certificate validation, and Recovery reports that as "Internet connection
is required" — same message, completely different cause. If the date is wrong:

```sh
systemsetup -setusingnetworktime on
# or set it directly:  date MMDDhhmmYY
```

---

## What would make this stop being a blocker

A network-dependent Recovery reinstall has at least four independent ways to
fail, three of which are outside the guest. The offline path — a full installer
app written to the target rather than a network fetch — removes all of them at
once. That is tracked separately as `t-1fb348`.

**The immediate blocker is cleared** — the guest holds a lease and answers ICMP
since the `vmxnet3` swap. The offline path is no longer urgent, but it is still
the more robust route if the Apple-side fetch turns out to fail for its own
reasons (throttling, a withdrawn image, TLS/date).

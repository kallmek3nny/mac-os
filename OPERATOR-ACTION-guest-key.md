# OPERATOR ACTION — one line at the guest console buys permanent access

**Filed 2026-08-14.** This is the third operator card, and it makes the other
two unnecessary. It needs **no password** and **no long typing** — one short
command at the macOS console, once, and every future host→guest operation runs
without a human.

---

## The command

At the `kenny@kennys-Mac ~ %` prompt in the VMware window, type exactly:

```
curl 192.168.255.1:8000/s|sh
```

Expected output:

```
KEY_INSTALLED: 1 line(s) in authorized_keys
```

That is 28 characters. It is also already sitting in the Windows clipboard, so
if VMware's copy/paste is working (no `isolation.tools.*` keys exist in
`Sonoma.vmx`, so the defaults apply and it should be) a right-click paste into
the guest terminal is enough.

---

## Why an SSH key and not the password

A password unblocks `vmrun` guest ops for as long as it is held in a shell
variable. A key in `authorized_keys` unblocks **everything, permanently, with
no secret stored anywhere**: `ssh -p 2222 kenny@127.0.0.1` and
`scp -P 2222` both start working, which means the STEP 7 build can be launched
and its log retrieved without a human at the console ever again.

The forward that makes `127.0.0.1:2222` reach the guest is already live and is
documented in `OPERATOR-ACTION-vmnat-forward.md`. It survives the ProtonVPN
kill switch.

---

## What the one-liner actually does

It fetches a four-line shell script off a plain HTTP server running on the
Windows host and pipes it to `sh`. The script:

1. creates `~/.ssh` with mode 700 if absent,
2. appends the host's **public** key to `~/.ssh/authorized_keys`,
3. chmods that file 600,
4. de-duplicates it with `sort -u`, so running the command twice is harmless,
5. prints the resulting line count as proof.

No private key crosses the wire. Nothing outside `~/.ssh` is touched.

---

## The two host-side pieces, and why each was needed

| Piece | Why |
|---|---|
| `python -m http.server 8000` bound to `0.0.0.0`, serving a two-file directory | The guest has no other credential-free pull path; a CD needs a power cycle |
| A Windows Firewall inbound rule on TCP 8000, **scoped to `192.168.255.0/24`** | Without it the guest's fetch is dropped. Scoped to vmnet8 so joining a café network does not expose the directory |

**The trap worth knowing:** `curl http://192.168.255.1:8000/` succeeding *on the
host* proves only that the listener is bound. Host-local traffic never
traverses the firewall's inbound path, so that success says nothing at all
about whether the guest can reach it. The only positive evidence is a line in
the server's access log whose source address is `192.168.255.129` — the
guest's DHCP lease — rather than `192.168.255.1`, which is the host's own
vmnet8 address and therefore the host talking to itself.

As of filing, the log holds five entries and **all five are from
`192.168.255.1`**. Every one is a host-side verification curl. The guest has
never fetched.

---

## If the command fails

| Symptom | Cause | Fix |
|---|---|---|
| `zsh: command not found: ssh-ed25519` | The raw key was pasted instead of the command. A key is data, not a program | Type the `curl` line above instead |
| `curl: (7) Failed to connect` | The host server or the firewall rule is gone (both live in the host's temp directory and do not survive a reboot) | Ask the assistant to restart both |
| `curl: (28) timeout` | ProtonVPN posture changed, or vmnet8 lost its address | Re-check `ipconfig` shows `192.168.255.1` on VMnet8 |

---

## After it succeeds

Nothing further is asked of you. The next steps run from the host:

1. `ssh -p 2222 kenny@127.0.0.1 'sh /Volumes/PAYLOAD/guest-build-kext.sh'` —
   the payload ISO is already mounted, built from `build-payload/step7-payload.iso`.
2. The build log comes back over the same connection.

**Both of those have since been done, and STEP 7 is answered:** `make exit
status: 0`, bundle PRESENT, Apple clang 17.0.0 (clang-1700.0.13.5) on macOS
15.7.9 (24G830). Log: `step7-results/step7-build-SUCCESS.log`.

This card originally predicted the first result would be `Kernel.framework:
ABSENT` meaning "Command Line Tools but not full Xcode". Both halves of that were
wrong. The real first result was ABSENT because the guest had **no developer
tools at all** — `xcode-select` errored and `clang` did not exist. And full Xcode
turned out to be irrelevant: measured in this guest after the successful build,
`/Applications/Xcode.app` does not exist, `xcode-select -p` is
`/Library/Developer/CommandLineTools`, and `Kernel.framework` is present inside
that SDK. `xcode-select --install` alone was the whole fix — no App Store, no
source changes. See `OPERATOR-ACTION-step7-build.md`.

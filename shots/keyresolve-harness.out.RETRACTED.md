# `keyresolve-harness.out.txt` is a PRE-FIX artifact — do not cite its `[FAIL]`s

**Retracted 2026-08-08 by `bright-otter-11`.**

File: `shots/keyresolve-harness.out.txt`
md5: `a3a491c93e2283e9831e45a16efd51e3`
mtime: 2026-08-07 19:21:09 -0400

The original bytes are kept unchanged. What is retracted is the *reading* of them,
not the file.

## What that file appears to say

    === CASE 3: multi-line .pub -> FIRST line only ===
      composed: /bin/sh /tmp/guest-enable-ssh.sh ''
      [FAIL] key did not survive into the quoted argument
      [FAIL] closing quote not immediately after the key comment
    ...
    [FAIL] 2 key-resolution case(s) misbehaved.
      [FAIL] wrong line selected:

    === CASE 4: quoting survives the guest command line ===

Read naively, that is a defect in the public-key resolution block of
`tools/guest-enable-ssh-via-tools.cmd`: the key vanishing, the quoting breaking,
the wrong line being selected.

## What it actually says

Nothing about that block. It is the harness tearing itself apart.

`tools/_keyresolve-harness.cmd` was saved with **bare-LF line endings** —
measured `LF=134, CRLF=0`. `cmd.exe` reads a batch file by byte offset, so an
LF-only `.cmd` makes it resume at the wrong offset after a `call`, and blocks
execute out of order or partially.

The tell is in the output above and needs no other evidence: the
`=== CASE 4 ===` header and one `[FAIL]` print **after** the run summary line.
No correctly-executing script prints a case header after its own summary. The
ordering is the proof that the interpreter, not the assertions, was wrong.

`tools/_toolsguard-harness.cmd` had the identical defect (`LF=135, CRLF=0`).

## The fix, and the artifact that supersedes this one

Both files were normalized to CRLF in place. Same text, no assertion touched:

| file | bytes | md5 before | md5 after |
|---|---|---|---|
| `tools/_keyresolve-harness.cmd` | 4959 → 5093 | `751b12b733f9f73e6b28e37bfe550d9e` | `f19b43b64753bad70ef34c7e2a075f31` |
| `tools/_toolsguard-harness.cmd` | 4511 → 4646 | `271477a90767aa5ace59559b5111fb68` | `9b12c0c88447649d240984ec2b4da413` |

After normalization, with nothing else changed:

* `tools/_keyresolve-harness.cmd` → **exit 0**, all five cases `[ok]`, and CASE 4's
  `composed:` line now shows the full key inside the single-quoted argument with
  its spaces, `+` and `/` intact.
* `tools/_toolsguard-harness.cmd` → **exit 0**, all five cases `[ok]`.

**Cite these instead:**

* `shots/keyresolve-harness-postfix.out.txt`
* `shots/toolsguard-harness-postfix.out.txt`

## Rule this establishes

Before diagnosing any `.cmd` misbehaviour on this project, count its line endings
first — compare the count of `b'\n'` against `b'\r\n'`. Output whose sections
appear out of order is the signature. Write every new `.cmd` in `tools/` with
CRLF.

As of this fix, only those two files were affected. `_register-probe-task.cmd`,
`_vm-type-elevated.cmd` and `guest-enable-ssh-via-tools.cmd` were already
CRLF-clean.

This is the same evidence class as the `0.000%` framebuffer-delta incident: an
artifact about the *tooling* misread as an artifact about the *subject*.

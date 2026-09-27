#!/usr/bin/env python3
# The module docstring names a Windows host path, so it must be a raw string --
# an ordinary one makes Python read `\Users` as a truncated \U escape and the
# whole module fails to import with a SyntaxError pointing at line 2.
r"""test_documented_paths.py -- every file path the prose docs name must exist.

Why this exists
---------------
The prose docs are the only files in this repo that tell a cold reader *where
things are*: CLAUDE.md's layout table and four gate commands, tools/README.md's
17 fenced invocations, the read order, the single-suite recipes. None of it was
checked by anything. A file can be renamed or deleted and the docs will keep
confidently pointing at the old name -- and the failure mode is not a red test,
it is an agent running a command that does nothing and reading the silence as a
result.

This is the same hole ``check-header-coverage.py`` and ``check-tool-coverage.py``
close one level down. Those assert that a hand-written list inside a *gate*
still matches the filesystem. This asserts the same thing for the hand-written
lists inside the *documentation*.

Scope, and why it stops where it does
-------------------------------------
Eleven docs are scanned -- see ``DOCS``. The handoff files are deliberately
excluded: they are a chronological record, and they name scratch probes and
intermediate files that were correct when written and have since been deleted.
Gating those would either produce permanent red or pressure someone into
editing history to make a test pass. The docs in ``DOCS`` are different -- they
make present-tense claims about the current tree, so a stale path in one of
them is a defect rather than a record.

Six root docs were added 2026-08-13: README.md, PLAN.md, HARDWARE.md,
PHASE4-GSP-BRINGUP.md, PHASE1-GSP-NOTES.md, PUBLISH-GSP-BRINGUP.md. Each was
probed against this module's own resolver BEFORE being added and every token in
all six already resolved -- no new exemption was needed, so what the widening
buys is a guard from here forward, not a fix landing now.

The rest of the root docs stay out, and for a reason read off the filesystem
rather than taste. STEPS.md names four host-side installer files this tree does
not contain (`fetch-macOS-v2.py`, `win-install.cmd`, `run_offline.md`,
`SystemVersion.plist`); both OPERATOR-ACTION cards, SETUP.md, VM-BRINGUP.md,
NETWORK-DIAGNOSIS.md, VMX-INVENTORY.md and DEVICE-IDS.md each name at least one
path that exists only inside the guest, only after the operator's build, or only
under a Windows install root. Gating those means declaring each token with its
own reason -- real work, and deliberately not folded into this change, because a
blanket exemption per doc is the pattern-skip this module exists to avoid.

How a token resolves
--------------------
Prose names files five ways, and all five had to be implemented before the
READMEs could be gated at all -- CLAUDE.md alone only ever exercised the first
two:

  DIRECT   fully qualified from the repo root -- ``tools/check-registers.py``
  TAIL     basename alone, in a sentence whose subject is already the directory
           -- ``NVDecode.hpp``. Anchored at a separator, so ``Decode.hpp`` must
           NOT be satisfied by ``NVDecode.hpp`` (see the negative controls).
  DOCREL   relative to the doc's own directory -- ``identify-gpu.py`` inside
           tools/README.md means ``tools/identify-gpu.py``.
  PARENT   relative to the doc's parent -- ``kextshim/`` inside
           tests/kextshim/README.md is the doc talking about its own directory.
  ELISION  a literal ``...`` standing in for intermediate directories --
           ``reference/open-gpu-kernel-modules/.../swref/published/``. Accepted
           only when the glob has EXACTLY one hit; two or more is AMBIGUOUS and
           fails, because a doc that points at two places points at neither.

Pair shorthand
--------------
``NV3050Probe.hpp/.cpp`` is one token naming two files. It is expanded and
*both* halves must resolve -- an allowance that only skipped it would let a doc
keep advertising a header whose implementation had been deleted.

The declared exceptions
-----------------------
Six tokens across the gated docs do not resolve, and each is declared with the
reason rather than pattern-skipped. Both tables are keyed by ``(doc, token)``,
so an exemption earned in one doc does not silently cover the same string
appearing in another.

``EXPECTED_ABSENT`` asserts the file is *still missing*. ``tests/__init__.py``
is the load-bearing one: CLAUDE.md's ``unittest discover`` recipe is only
correct while ``tests/`` is not a package, so if someone adds that file the
documented command becomes wrong advice and this goes red. An allowlist entry
that only said "ignore this" would let exactly that regression through.

``OUT_OF_TREE`` is for paths nothing in this repo can assert anything about --
a machine-local CPython install, a host path under ``\Users\Kentl``, and a file
that lives in the OSX-KVM upstream.

Both tables are themselves checked: if a token is removed from its doc, the
stale entry fails. An allowlist nobody prunes is how the next blind spot gets
built.
"""

import glob
import os
import re
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     os.pardir))

# The docs that make present-tense claims about the current tree. That is now
# every root .md except the numbered handoffs and HANDOFF.md, which are dated
# records of past sessions rather than claims about the tree as it stands.
#
# The nine added on 2026-08-14 were skipped wholesale until then, so a dead path
# in any of them was invisible. Widening this tuple was the easy half. The
# honest half was that 22 of their tokens did not resolve, and every one had to
# be either declared with a reason or fixed at the source -- see
# CMD_EXPANSION_RE and _resolve_vendored_dir for the two that turned out to be
# extractor and resolver defects rather than defects in the prose.
DOCS = (
    "CLAUDE.md",
    "tests/README.md",
    "phase0-kext/README.md",
    "tests/kextshim/README.md",
    "tools/README.md",
    "README.md",
    "PLAN.md",
    "HARDWARE.md",
    "PHASE4-GSP-BRINGUP.md",
    "PHASE1-GSP-NOTES.md",
    "PUBLISH-GSP-BRINGUP.md",
    "STEPS.md",
    "SETUP.md",
    "VM-BRINGUP.md",
    "NETWORK-DIAGNOSIS.md",
    "VMX-INVENTORY.md",
    "DEVICE-IDS.md",
    "OPERATOR-ACTION-step7-build.md",
    "OPERATOR-ACTION-guest-creds.md",
    "OPERATOR-ACTION-vmnat-forward.md",
    # Added 2026-08-14 in the same session that filed it. It was invisible to
    # this gate for the ~20 minutes between the two, which is precisely the
    # failure test_docs_covers_every_root_md now makes impossible.
    "OPERATOR-ACTION-guest-key.md",
    # Added 2026-08-14, and this time the gate is what noticed: filing
    # QEMU-LOWSTORAGE.md reddened test_docs_covers_every_root_md within the same
    # session, naming the doc. That is the coverage test doing exactly the job it
    # was written for -- a root doc cannot sit outside this tuple silently.
    "QEMU-LOWSTORAGE.md",
    # Added 2026-08-17, and the gate noticed again: filing INTEL-DEVICE-IDS.md
    # reddened test_docs_covers_every_root_md in the same session, naming the
    # doc. It exists because tools/check-kext-personalities.py refuses to hold
    # its own accept set, and DEVICE-IDS.md is NVIDIA-only -- so the Intel
    # personality needed a DOCUMENT to derive from.
    "INTEL-DEVICE-IDS.md",
)

# Directories that hold no path these docs can legitimately name by basename,
# or that are large enough that walking them costs more than it proves.
# `reference/` is vendored NVIDIA source -- it is still reachable through the
# ELISION path, which globs the filesystem directly rather than the index.
# `shots/` is deliberately NOT skipped: tools/README.md names artifacts in it.
#
# `payload-stage/` is a generated staging copy of `phase0-kext/` plus
# `tools/guest-build-kext.sh`, assembled for the payload ISO. It is skipped for
# a reason that bit this file on 2026-08-13: it duplicates the basename of every
# kext header, so `NVDecode.hpp` stopped naming one file and started naming two,
# and which one a bare-basename lookup returned depended on `os.walk` order.
# No doc names it, and nothing in it is a source of truth.
SKIP_DIRS = {".git", ".claude-crew", "reference", "build", "__pycache__",
             "node_modules", ".venv", "payload-stage"}

# Suffixes that make a token path-shaped. Deliberately excludes `.kext` and
# `.exe`: both name build *outputs* that do not exist on a clean checkout, and
# `phase0-kext/build/NV3050Probe.kext` is produced only on macOS.
SUFFIXES = ("md", "py", "bat", "cmd", "cpp", "hpp", "txt", "json",
            "yml", "yaml", "cmake", "plist")

# cmd.exe variable expansions, stripped before either extractor runs. `%~dp0`
# is the one that mattered: PATH_RE has no `%` in its character class, so it
# begins matching AFTER the `%~`, and the line
# `call "%~dp0guest-enable-ssh-via-tools.cmd"` yielded a file token
# `dp0guest-enable-ssh-via-tools.cmd` that no document ever claimed. Declaring
# that token away would have written an exemption for an extractor artifact --
# the token has to stop being produced instead.
#
# The pattern is deliberately narrow. A loose `%~?[A-Za-z0-9]*%?` also eats the
# `guest` following `%~dp0` and emits `-enable-ssh-via-tools.cmd`, a worse
# artifact than the one it fixes. Measured 2026-08-14: this pattern changes
# ZERO tokens, in either extractor, across every doc that was already gated.
CMD_EXPANSION_RE = re.compile(
    r"%~[dpnxfsatz]*[0-9]"           # %~dp0  %~f0  %~1
    r"|%[0-9*]"                      # %1  %*
    r"|%[A-Za-z_][A-Za-z0-9_]*%"     # %TEMP%  %ERRORLEVEL%
)

PATH_RE = re.compile(r"[A-Za-z0-9_.\-/\\]+\.(?:" + "|".join(SUFFIXES) + r")\b")

# Backtick-quoted tokens ending in a slash -- the layout table's directories.
# They carry no suffix, so PATH_RE cannot see them at all.
DIR_RE = re.compile(r"`([A-Za-z0-9_.\-][A-Za-z0-9_.\-/\\]*/)`")

# `NV3050Probe.hpp/.cpp` -- one token, two files.
PAIR_RE = re.compile(r"^(?P<stem>.+)\.(?P<a>[A-Za-z0-9]+)/\.(?P<b>[A-Za-z0-9]+)$")

# Any fenced block, with its language tag. CLAUDE.md uses only ```sh, but the
# READMEs use bare ``` and ```powershell -- an sh-only parser sees 3 of the 17
# blocks in tools/README.md and silently proves nothing about the other 14.
FENCE_RE = re.compile(r"^```([A-Za-z0-9_+-]*)$")

ELISION = "/.../"

# (doc, token) -> (path asserted absent, why the absence matters)
EXPECTED_ABSENT = {
    ("CLAUDE.md", "__init__.py"): (
        "tests/__init__.py",
        "CLAUDE.md's `unittest discover` recipe is only correct while tests/ is "
        "not a package; creating this file invalidates the documented command",
    ),
    ("tools/README.md", "tests/__init__.py"): (
        "tests/__init__.py",
        "tools/README.md names it for the same reason CLAUDE.md does -- the "
        "discovery invocation depends on tests/ not being a package",
    ),
    ("tests/README.md", "kext/NVDecode.hpp"): (
        "kext/NVDecode.hpp",
        "tests/README.md names this token to EXPLAIN the TAIL anchor: it is the "
        "one that must NOT be satisfied by the real phase0-kext/NVDecode.hpp. "
        "Its non-existence is the documented fact, so resolving it would mean "
        "the prose had gone wrong, not the tree",
    ),
    ("tests/README.md", "Decode.hpp"): (
        "Decode.hpp",
        "named in the same sentence as the counter-example that proves nothing "
        "-- the index is keyed by basename, so this misses on the KEY before "
        "the endswith runs. It must stay absent or the sentence stops being "
        "true",
    ),
    ("OPERATOR-ACTION-guest-creds.md", "shots/guest-enable-ssh.out.txt"): (
        "shots/guest-enable-ssh.out.txt",
        "the output `tools/guest-enable-ssh-via-tools.cmd` copies back OUT of "
        "the guest. It is future tense, not stale: the wrapper has never run "
        "to completion because it needs the operator's password, which is the "
        "t-396740 gate. `shots/` is deliberately NOT in SKIP_DIRS, so this "
        "absence is measured rather than assumed -- the day the run succeeds, "
        "this entry fires and says the doc should stop describing it as "
        "something that will be produced",
    ),
}

# (doc, token) -> why nothing in this repo can check it
OUT_OF_TREE = {
    ("CLAUDE.md", "site-packages/tests/__init__.py"):
        "machine-local CPython install, named only to explain the "
        "ModuleNotFoundError that blames your own test module",
    ("tools/README.md", r"\Users\Kentl\macos-vm\sonoma\out.txt"):
        "absolute host path outside the repo, shown as example output of a "
        "VM helper; machine-specific by construction",
    ("tools/README.md", "run_offline.md"):
        "lives in the OSX-KVM upstream, not vendored here",

    # --- the nine docs gated on 2026-08-14 ---------------------------------
    #
    # Every entry below names something on the OPERATOR's machine or in an
    # upstream project: installer media, host VM-directory artifacts, VPN
    # config, or a command's output on a Linux box this repo never sees. None
    # of them is a stale path, and none could be made to resolve by any edit to
    # this checkout -- which is exactly the claim this table exists to record.
    ("STEPS.md", "fetch-macOS-v2.py"):
        "OSX-KVM upstream downloader, run by the operator before this repo is "
        "involved; not vendored here",
    ("STEPS.md", "win-install.cmd"):
        "OSX-KVM upstream installer script, same provenance as "
        "fetch-macOS-v2.py",
    ("STEPS.md", "run_offline.md"):
        "lives in the OSX-KVM upstream, not vendored here -- the same file "
        "tools/README.md names above",
    ("STEPS.md", "SystemVersion.plist"):
        "embedded inside BaseSystem.img in the recovery image; named to say "
        "which build the installer media reports, never extracted into the tree",

    ("SETUP.md", "fetch-macOS-v2.py"):
        "OSX-KVM upstream downloader; not vendored here",
    ("SETUP.md", "SystemVersion.plist"):
        "embedded in the recovery image, as in STEPS.md",
    ("SETUP.md", "lspci-3050.txt"):
        "output of an lspci command the operator runs on the Linux box that "
        "has the real GA107; a file this repo asks for, not one it contains",

    ("VM-BRINGUP.md", "SystemVersion.plist"):
        "embedded in the recovery image, as in STEPS.md",
    ("VM-BRINGUP.md", "BASELINE-preswap-20260807T1841Z.txt"):
        "md5 baseline captured into the host VM directory "
        "C:\\Users\\Kentl\\macos-vm\\sonoma\\ at 2026-08-07T18:41:48Z; outside "
        "the repo by construction",
    ("VM-BRINGUP.md", "chk.txt"):
        "DrDonk unlocker check.exe output, written into the host VM directory",
    ("VM-BRINGUP.md", r"\Users\Kentl\macos-vm\chk.txt"):
        "the same unlocker output written with its absolute host path; the "
        "bare basename above is the same file named a second way",

    ("NETWORK-DIAGNOSIS.md", "BASELINE-preswap-20260807T1841Z.txt"):
        "the same host-side md5 baseline VM-BRINGUP.md names",
    ("NETWORK-DIAGNOSIS.md", "GlobalSettings.json"):
        "ProtonVPN client configuration under the host user profile, read to "
        "establish that the kill switch was on; host state, not repo state",

    ("VMX-INVENTORY.md", "SystemVersion.plist"):
        "embedded in the recovery image; cited to separate the INSTALLER "
        "build 24E263 from the running guest's own write-back",

    ("OPERATOR-ACTION-guest-creds.md", r"VPN\Storage\GlobalSettings.json"):
        "the ProtonVPN configuration above, named with its enclosing host "
        "directories",
}

# (doc, dir token) -> why the directory must NOT exist.
#
# This is the directory counterpart of EXPECTED_ABSENT, and it is a separate
# table because the allowlist check above asserts its entries appear in
# `file_tokens`. A trailing-slash token never does, so an entry parked in
# EXPECTED_ABSENT would fail that check rather than be policed by it.
#
# It held nothing between 2026-08-13 and 2026-08-14, and that emptiness was the
# reciprocal check working rather than a gap.
# It held one entry for `payload-stage/`, the staging directory an early
# build-payload attempt created inside the tree; its presence is what duplicated
# every kext basename and made index lookup order-dependent. On 2026-08-13
# `tools/build-step7-payload.py` replaced that approach with a system temp
# directory outside the repo, tests/README.md was reworded to stop naming the
# old path, and `test_absent_directory_declarations_are_still_written` fired
# immediately -- an exemption must not outlive the text it was written for.
#
# `payload-stage` stays in SKIP_DIRS regardless. That is a cheap standing guard
# against a staging copy reappearing under the same name, and it is asserted by
# `test_payload_stage_is_excluded_from_the_index`, which does not depend on any
# doc naming it.
EXPECTED_ABSENT_DIRS = {
    ("OPERATOR-ACTION-step7-build.md", "build/"): (
        "phase0-kext/build",
        "the doc's verification table asserts that no stale host `build/` "
        "rides along in the STEP 7 payload. It is a build OUTPUT, produced "
        "only by `cd phase0-kext && make` on macOS -- the same reason "
        "SUFFIXES excludes `.kext` and `.exe`. Development happens on Windows "
        "and the kext has never been compiled, so its absence is a fact about "
        "this checkout today. If someone ever builds in-tree, this entry "
        "fires and says so, which is the signal wanted rather than a nuisance",
    ),
}

# Floors, not counts, per doc: (file tokens, dir tokens, fenced blocks).
# A count would go stale on every doc edit; a floor only fires when an
# extractor has broken badly enough to see almost nothing, which is the failure
# this guards -- a regex that matches nothing passes every resolution check
# below vacuously.
#
# Re-measured 2026-08-13 by running the extractors, because the previous line
# here recorded tests/README.md as 21/1/3 and tests/kextshim/README.md as 8/1/1
# and both were stale within hours: those two docs have mtimes of 15:15 and
# 15:13 against a measurement taken when CLAUDE.md was last touched at 12:15.
# That is the point -- a number in a comment does not move when the file it
# describes does, and nothing here reads it. It is recorded as provenance for
# the floors, never as an assertion. These are what the extractors return now:
#   CLAUDE.md 35/6/4, tests/README.md 50/8/5, phase0-kext/README.md 9/2/4,
#   tests/kextshim/README.md 9/1/1, tools/README.md 37/3/17, README.md 23/4/1,
#   PLAN.md 2/1/0, HARDWARE.md 6/2/4, PHASE4-GSP-BRINGUP.md 9/1/9,
#   PHASE1-GSP-NOTES.md 1/0/2, PUBLISH-GSP-BRINGUP.md 2/0/11.
# The floors below are left where they were set -- every one sits at or under
# its measured value, so raising them to chase the new numbers would only make
# the gate brittle against ordinary doc edits.
#
# Be honest about what the small ones assert: a doc that names two paths cannot
# carry a floor that is both non-vacuous and not a count. Where the measurement
# was 0 or 1 the floor is 0 or 1 and guards only against the extractor seeing
# NOTHING in that doc -- drift protection there comes from the large docs, whose
# floors sit roughly a quarter under measured and would have to lose eight
# tokens to fire.
FLOORS = {
    "CLAUDE.md":                (25, 5, 4),
    "tests/README.md":          (15, 1, 3),
    "phase0-kext/README.md":    (6,  2, 3),
    "tests/kextshim/README.md": (5,  1, 1),
    # Raised 2026-08-14 from (25, 3, 12). Three sections were appended that day
    # (qcow2-read.py, vmdk-read.py, apfs-volumes.py) and the extractors now return
    # 40/3/20 against a floor written for a 37-token file. A floor 15 tokens under
    # measured is not drift protection, it is decoration.
    "tools/README.md":          (30, 3, 15),
    "README.md":                (17, 3, 1),
    "PLAN.md":                  (1,  0, 0),
    "HARDWARE.md":              (4,  1, 3),
    "PHASE4-GSP-BRINGUP.md":    (7,  1, 7),
    "PHASE1-GSP-NOTES.md":      (1,  0, 1),
    "PUBLISH-GSP-BRINGUP.md":   (1,  0, 8),
    # Measured 2026-08-14 by running the extractors over the nine docs added
    # that day, AFTER CMD_EXPANSION_RE landed -- the strip removes one token
    # from OPERATOR-ACTION-guest-creds.md, so a measurement taken before it
    # would have set that floor one too high. Measured: STEPS 28/2/25,
    # SETUP 6/1/8, VM-BRINGUP 13/1/32, NETWORK-DIAGNOSIS 6/0/13,
    # VMX-INVENTORY 2/0/4, DEVICE-IDS 1/3/2, step7-build 8/4/7,
    # guest-creds 7/1/3, vmnat-forward 2/0/5.
    "STEPS.md":                      (21, 1, 18),
    "SETUP.md":                      (4,  1, 6),
    "VM-BRINGUP.md":                 (9,  1, 24),
    "NETWORK-DIAGNOSIS.md":          (4,  0, 9),
    "VMX-INVENTORY.md":              (1,  0, 3),
    "DEVICE-IDS.md":                 (1,  2, 1),
    "OPERATOR-ACTION-step7-build.md": (6, 3, 5),
    "OPERATOR-ACTION-guest-creds.md": (5, 1, 2),
    "OPERATOR-ACTION-vmnat-forward.md": (1, 0, 3),
    # Measured 2026-08-14 by running the extractors over QEMU-LOWSTORAGE.md as
    # filed: 3/0/5 (file tokens PLAN.md, phase0-kext/Info.plist,
    # tools/qemu-boot-sonoma.py -- all three resolve DIRECT). Zero dir tokens is
    # deliberate rather than a gap: the doc's storage numbers are per-FILE, and
    # naming a directory it does not measure would be prose inventing coverage.
    #
    # The measurement was taken with a CONTROL first -- the same three extractor
    # calls over HARDWARE.md and OPERATOR-ACTION-guest-key.md returned 6/2/4 and
    # 2/0/2, matching what the comments above record. Without that, a 0 in the
    # dir column could mean "dir_tokens() is not being called the way I think"
    # rather than "this doc names no directories", and the floor would encode the
    # wrong fact. Same shape as the vacuous-gate failures this file keeps hitting.
    #
    # RAISED later the same day, from (2, 0, 4). Section 5 was added -- the serial
    # capture, the ESP walk, the APFS volume table and the three new readers -- and
    # the extractors now return 6/0/6. Floors are minimums, so nothing went red;
    # that is exactly the problem. A floor of 2 against 6 measured lets the file
    # extractor lose two thirds of its tokens and still pass, which is the vacuity
    # this entry exists to catch. Re-measured with the SAME two controls as the
    # original: HARDWARE.md 6/2/4 and OPERATOR-ACTION-guest-key.md 2/0/2, both
    # unchanged, so the extractors are intact and the new numbers describe the doc
    # rather than a broken regex. The dir column stays 0 and stays deliberate --
    # section 5 describes a directory inside a built kext bundle and a directory
    # inside a FAT volume in WORDS, because writing either as a path reddens
    # test_every_documented_directory_exists, which cannot tell those from a
    # directory in the checkout. That is not hypothetical: it fired on `EFI/`.
    #
    # RAISED AGAIN, same day, from (4, 0, 5) to (5, 0, 6). The (4, 0, 5) figure
    # was measured against the doc BEFORE sections 7 and 8 were rewritten in the
    # same session, and that rewrite added two fences plus tokens for
    # tools/png-crop.py, tools/qcow2-read.py and tools/apfs-volumes.py -- the doc
    # now measures 7/0/7. This is the failure mode in
    # `a-measured-claim-can-be-false-on-day-one`: a floor can be stale within the
    # hour, because the thing that invalidates it is an edit by the same author
    # who just measured it, and floors being minimums means nothing goes red to
    # say so. Re-measured with the SAME two controls, both still at their
    # recorded values -- HARDWARE.md 6/2/4, OPERATOR-ACTION-guest-key.md 2/0/2.
    # All seven file tokens resolve DIRECT.
    "QEMU-LOWSTORAGE.md":            (5, 0, 6),
    # Measured 2026-08-17 by running the extractors over INTEL-DEVICE-IDS.md as
    # filed: 7 file tokens, 0 directory tokens, 1 fenced block. All seven file
    # tokens resolve DIRECT. Controls run in the same invocation:
    # OPERATOR-ACTION-guest-key.md still 2/0/2 and DEVICE-IDS.md still 1/3/2,
    # both at their recorded values. HARDWARE.md measured 6/2/7 against a
    # recorded floor of 6/2/4 -- the doc gained three fenced blocks since the
    # 2026-08-13 measurement and nothing went red, which is a floor behaving as
    # a floor rather than a stale count.
    #
    # The fence floor here is 1 rather than 0. One under a measurement of 1 is
    # zero, and a zero fence floor cannot fail, so it would guard nothing; this
    # doc's single block is the `lspci -nn` confirm command, and losing it means
    # the doc no longer tells a reader how to check their own machine.
    #
    # The file floor is 6 against a measurement of 7 ON PURPOSE, and the gap is
    # not a transcription slip. Six of the seven tokens are the evidence chain
    # (the vendored header's provenance, the two capture artifacts, the NVIDIA
    # doc it parallels, IntelGpuId.hpp, the checker); the seventh is
    # phase0-kext/Info.plist, named only in the closing how-to-confirm section,
    # and a later rewrite of that section should not have to touch this tuple.
    # One row of headroom keeps this a floor that fires on extractor breakage
    # rather than a count that fires on prose edits.
    "INTEL-DEVICE-IDS.md":           (6, 0, 1),
    # Measured 2026-08-14 by running the extractors over the doc as filed:
    # OPERATOR-ACTION-guest-key.md 2/0/2. Both file tokens resolve DIRECT
    # (the other two operator cards). The floor sits one under each non-zero
    # measurement, so it guards only against the extractors going blind on
    # this doc -- a two-path doc cannot carry a floor that is both a floor
    # and not a count.
    "OPERATOR-ACTION-guest-key.md": (1, 0, 1),
}


def read_doc(rel):
    with open(os.path.join(REPO, rel.replace("/", os.sep)), "r",
              encoding="utf-8") as handle:
        return handle.read()


def build_index():
    """basename -> [repo-relative path, ...] for every file in the tree."""
    index = {}
    for dirpath, dirnames, filenames in os.walk(REPO):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            rel = os.path.relpath(os.path.join(dirpath, name), REPO)
            index.setdefault(name, []).append(rel.replace("\\", "/"))
    return index


def normalise(token):
    norm = token.replace("\\", "/")
    while norm.startswith("./"):
        norm = norm[2:]
    return norm


def collapse(rel):
    return os.path.normpath(rel).replace("\\", "/")


def expand_pair(token):
    """``NV3050Probe.hpp/.cpp`` -> both halves. Other tokens pass through.

    Returning both halves rather than skipping the token is the point: the
    shorthand claims two files exist, so both are checked.
    """
    match = PAIR_RE.match(normalise(token))
    if not match:
        return [token]
    return ["%s.%s" % (match.group("stem"), match.group("a")),
            "%s.%s" % (match.group("stem"), match.group("b"))]


# Directories inside the vendored `reference/` tree, resolved by unique tail.
#
# `reference/` is in SKIP_DIRS, so the basename index never contains it, and the
# want_dir branch of resolve() returned MISSING for four tokens that ARE on
# disk -- `ampere/ga102/`, `ada/ad102/`, `swref/published/ada/ad102/` and
# `src/nvidia/src/kernel/gpu/gsp/`. Declaring those away as unverifiable would
# have recorded "this repo cannot check it" about directories this repo vendors.
#
# The ambiguity discipline is ELISION's, for ELISION's reason. Measured
# 2026-08-14: each of those four tails hits exactly ONE directory under
# reference/, while a bare `src/` hits 13 and `inc/` hits 11. A tail that names
# two places names neither, so 2+ hits fails AMBIGUOUS rather than returning
# whichever one os.walk enumerated first.
_VENDOR_DIRS = None


def _vendor_dirs():
    """Repo-relative directories under `reference/`, walked once and cached."""
    global _VENDOR_DIRS
    if _VENDOR_DIRS is None:
        found = []
        for dirpath, dirnames, filenames in os.walk(os.path.join(REPO,
                                                                 "reference")):
            dirnames[:] = [d for d in dirnames
                           if d not in (".git", "__pycache__")]
            for name in dirnames:
                rel = os.path.relpath(os.path.join(dirpath, name), REPO)
                found.append(rel.replace("\\", "/"))
        _VENDOR_DIRS = found
    return _VENDOR_DIRS


def _resolve_vendored_dir(norm):
    """(verdict, path) for a dir token naming exactly one vendored directory.

    ``None`` means no hit, and the caller keeps its own MISSING/ESCAPES answer.
    So this rule can only turn a MISSING into a resolution or into an explicit
    AMBIGUOUS -- it can never rescue a token that was already resolving, which
    is why adding it cannot change a verdict in the docs already gated.
    """
    tail = norm.strip("/")
    if not tail:
        return None
    hits = sorted(d for d in _vendor_dirs()
                  if d == tail or d.endswith("/" + tail))
    if len(hits) == 1:
        return ("VENDORED", hits[0])
    if len(hits) > 1:
        return ("AMBIGUOUS",
                "%d vendored hits: %s" % (len(hits), ", ".join(hits)))
    return None


def _resolve_elision(norm, docdir):
    """Accept a ``...`` path only when it names exactly one thing.

    Two or more hits is AMBIGUOUS and fails. A doc whose elided path could mean
    either of two directories is not telling the reader where anything is, and
    accepting the first glob hit would bake in whichever one the filesystem
    happened to enumerate first.
    """
    head, _, tail = norm.partition(ELISION)
    for base in (docdir, ""):
        pattern = os.path.join(REPO, base.replace("/", os.sep),
                               head.replace("/", os.sep), "**",
                               tail.rstrip("/").replace("/", os.sep))
        hits = glob.glob(pattern, recursive=True)
        if len(hits) == 1:
            return ("ELISION",
                    os.path.relpath(hits[0], REPO).replace("\\", "/"))
        if len(hits) > 1:
            return ("AMBIGUOUS", "%d hits" % len(hits))
    return ("MISSING", None)


def resolve(token, doc="CLAUDE.md", index=None, want_dir=False):
    """Return (verdict, path). See the module docstring for the five verdicts.

    Tail matching is anchored at a separator on purpose: without the leading
    "/" in the endswith, a token naming a file that does not exist would be
    satisfied by any longer basename ending in the same characters.
    """
    # There WAS a back-compat shim here accepting `resolve(token, index)` --
    # the pre-multidoc signature -- because tests/test_documented_paths_multidoc.py
    # and its probe still called it that way. Both were folded into this module
    # and deleted on 2026-08-13, so the shim had no callers left. It is removed
    # rather than kept "just in case": it reinterpreted its own arguments based
    # on their TYPE, so a future caller passing a dict by mistake would have been
    # silently rewritten into a CLAUDE.md lookup instead of failing.
    index = {} if index is None else index
    norm = normalise(token)
    docdir = os.path.dirname(doc)
    check = os.path.isdir if want_dir else os.path.exists

    if ELISION in norm:
        return _resolve_elision(norm, docdir)

    # doc-relative, then repo-relative, then one level above the doc
    bases = []
    if docdir:
        bases.append(("DOCREL", docdir))
    bases.append(("DIRECT", ""))
    parent = os.path.dirname(docdir)
    if docdir:
        bases.append(("PARENT", parent))
    # A join that climbs out of the repo is REPORTED, not silently skipped.
    # `../../../../etc/passwd` written in a doc two levels down is a different
    # failure from a typo: MISSING sends the reader looking for a file to
    # create, when the real problem is that the doc names something outside the
    # tree entirely. ESCAPES is returned only when NO base resolved -- an
    # escaping base alongside one that resolves is the normal case
    # (`../STEPS.md` escapes from PARENT and resolves from DOCREL), so
    # recording it here cannot change any verdict this function already reached.
    escaped = None
    for verdict, base in bases:
        rel = collapse(base + "/" + norm if base else norm)
        if rel.startswith(".."):
            if escaped is None:
                escaped = rel
            continue
        if check(os.path.join(REPO, rel.replace("/", os.sep))):
            return (verdict, rel)

    if want_dir:
        vendored = _resolve_vendored_dir(norm)
        if vendored is not None:
            return vendored
        return ("ESCAPES", escaped) if escaped else ("MISSING", None)

    # Ambiguity is a failure here for the same reason it is under ELISION: a
    # bare basename that names two files is not telling the reader where
    # anything is. Returning the first match would also make the verdict depend
    # on `os.walk` enumeration order -- sorted() and the length check together
    # mean this function cannot silently change answer when a directory that
    # duplicates a basename appears in the tree.
    base = norm.rstrip("/").rsplit("/", 1)[-1]
    hits = sorted(set(candidate for candidate in index.get(base, ())
                      if candidate == norm or candidate.endswith("/" + norm)))
    if len(hits) == 1:
        return ("TAIL", hits[0])
    if len(hits) > 1:
        return ("AMBIGUOUS", "%d hits: %s" % (len(hits), ", ".join(hits)))
    return ("ESCAPES", escaped) if escaped else ("MISSING", None)


def file_tokens(text):
    return sorted(set(PATH_RE.findall(CMD_EXPANSION_RE.sub(" ", text))))


def dir_tokens(text):
    return sorted(set(DIR_RE.findall(CMD_EXPANSION_RE.sub(" ", text))))


def fences(text):
    """[(language, body), ...] for every fenced block, in document order."""
    blocks, lang, current = [], None, None
    for line in text.splitlines():
        stripped = line.strip()
        match = FENCE_RE.match(stripped)
        if current is None:
            if match:
                lang, current = match.group(1), []
        elif stripped == "```":
            blocks.append((lang, "\n".join(current)))
            lang, current = None, None
        else:
            current.append(line)
    return blocks


def sh_blocks(text):
    return [body for lang, body in fences(text) if lang == "sh"]


class DocumentedPathsTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.index = build_index()
        cls.text = {doc: read_doc(doc) for doc in DOCS}
        cls.files = {doc: file_tokens(cls.text[doc]) for doc in DOCS}
        cls.dirs = {doc: dir_tokens(cls.text[doc]) for doc in DOCS}
        cls.fences = {doc: fences(cls.text[doc]) for doc in DOCS}
        cls.declared = set(EXPECTED_ABSENT) | set(OUT_OF_TREE)

    # -- the extractors actually extracted something ----------------------

    def test_docs_covers_every_root_md(self):
        """DOCS is hand-typed, and its failure mode is silent in one direction:
        add a root .md, forget this tuple, and every path in the new doc goes
        unchecked while the suite still prints ok. That is not hypothetical --
        OPERATOR-ACTION-guest-key.md was filed and sat outside DOCS until this
        test was written, and the only reason it was noticed is that someone
        grepped the checker's output for the filename and found zero hits.

        The exemption is DERIVED, not listed: the numbered handoffs and
        HANDOFF.md are dated records of past sessions rather than claims about
        the tree as it stands. Anything else in the repo root is a claim, and
        a claim nothing reads is the thing this whole module exists to stop.
        """
        # The optional track prefix is the 2026-08-18 split: the NVIDIA and
        # Intel sessions collided on the number 26 twice in one day, each
        # destroying the other's file, so new handoffs carry `3050-` or
        # `intel-`. The bare form stays matched because every handoff below 26
        # predates the split and keeps its name. Still DERIVED rather than
        # listed -- a third track would need one more alternative here, which
        # is the point: the exemption names a CONVENTION, not a file set.
        handoff = re.compile(r"^((3050-|intel-)?handoff[0-9]+|HANDOFF)\.md$")
        on_disk = set()
        for path in glob.glob(os.path.join(REPO, "*.md")):
            name = os.path.basename(path)
            if not handoff.match(name):
                on_disk.add(name)

        # Positive control for the exemption itself. If the regex ever stops
        # matching, `on_disk` swells with a dozen handoffs and the assertion
        # below fails for a reason that has nothing to do with a new doc --
        # so pin that the exemption actually exempted something.
        self.assertTrue(
            any(handoff.match(os.path.basename(p))
                for p in glob.glob(os.path.join(REPO, "*.md"))),
            "no handoff files matched the exemption regex -- either they were "
            "all deleted or the pattern no longer matches their names")

        missing = sorted(on_disk - set(DOCS))
        self.assertEqual(
            [], missing,
            "%d root doc(s) exist but are not in DOCS, so every path they "
            "name is unchecked: %s. Add them and measure a FLOORS entry, or "
            "if one is genuinely a dated record, rename it to the handoff "
            "convention so the exemption is derived rather than typed."
            % (len(missing), ", ".join(missing)))

        # NOT `set(DOCS) - on_disk`. DOCS is root docs PLUS four subdirectory
        # READMEs (tests/, tools/, phase0-kext/, tests/kextshim/), and
        # `on_disk` deliberately holds only the repo root. Subtracting one from
        # the other reported all four READMEs as deleted when every one of them
        # exists -- a failure describing the assertion rather than the tree.
        # Ask the filesystem about each entry instead.
        gone = sorted(d for d in DOCS
                      if not os.path.isfile(os.path.join(REPO, d)))
        self.assertEqual(
            [], gone,
            "%d doc(s) in DOCS do not exist on disk: %s. read_doc() would "
            "raise, but say so here rather than as an IOError from "
            "setUpClass." % (len(gone), ", ".join(gone)))

    def test_every_doc_has_a_floor(self):
        """A doc in DOCS with no FLOORS entry is scanned but not guarded: if
        its extractor returns nothing, every resolution check below passes
        over an empty set and reports success."""
        unguarded = sorted(set(DOCS) - set(FLOORS))
        self.assertEqual(
            [], unguarded,
            "%d doc(s) are scanned with no floor, so an extractor seeing "
            "nothing in them is undetectable: %s"
            % (len(unguarded), ", ".join(unguarded)))

    def test_every_doc_is_readable_and_nonempty(self):
        for doc in DOCS:
            self.assertGreater(len(self.text[doc]), 500,
                               doc + " is far shorter than expected -- "
                               "wrong file, or truncated?")

    def test_extractors_clear_their_floors(self):
        for doc in DOCS:
            want_files, want_dirs, want_fences = FLOORS[doc]
            self.assertGreaterEqual(
                len(self.files[doc]), want_files,
                "PATH_RE found only %d path-shaped tokens in %s (floor %d). A "
                "broken regex passes every resolution check vacuously."
                % (len(self.files[doc]), doc, want_files))
            self.assertGreaterEqual(
                len(self.dirs[doc]), want_dirs,
                "DIR_RE found only %d backticked directories in %s (floor %d)."
                % (len(self.dirs[doc]), doc, want_dirs))
            self.assertGreaterEqual(
                len(self.fences[doc]), want_fences,
                "found %d fenced blocks in %s (floor %d) -- the fence parser "
                "is broken, so the command check proves nothing."
                % (len(self.fences[doc]), doc, want_fences))

    def test_index_covers_the_tree(self):
        for name in ("CLAUDE.md", "NVDecode.hpp", "check-registers.py",
                     "NV3050Probe.cpp"):
            self.assertIn(name, self.index)

    # -- the actual assertion ---------------------------------------------

    def test_every_documented_file_path_resolves(self):
        missing = []
        for doc in DOCS:
            for token in self.files[doc]:
                if (doc, token) in self.declared:
                    continue
                for part in expand_pair(token):
                    verdict, detail = resolve(part, doc, self.index)
                    if verdict in ("MISSING", "AMBIGUOUS"):
                        label = part if part == token else \
                            "%s [of %s]" % (part, token)
                        missing.append("%s: %s (%s%s)"
                                       % (doc, label, verdict,
                                          " " + detail if detail else ""))
        self.assertEqual(
            [], missing,
            "%d documented path(s) do not exist: %s. Either the file moved and "
            "the doc is stale, or the path was never right."
            % (len(missing), "; ".join(missing)))

    def test_every_documented_directory_exists(self):
        missing = []
        for doc in DOCS:
            for token in self.dirs[doc]:
                if (doc, token) in self.declared:
                    continue
                if (doc, token) in EXPECTED_ABSENT_DIRS:
                    continue
                verdict, detail = resolve(token, doc, self.index, want_dir=True)
                if verdict in ("MISSING", "AMBIGUOUS"):
                    missing.append("%s: %s (%s%s)"
                                   % (doc, token, verdict,
                                      " " + detail if detail else ""))
        self.assertEqual(
            [], missing,
            "%d documented directory/ies do not exist: %s"
            % (len(missing), "; ".join(missing)))

    def test_pair_shorthand_names_two_real_files(self):
        """Both halves of every `X.hpp/.cpp` must exist, not just one."""
        pairs = 0
        for doc in DOCS:
            for token in self.files[doc]:
                halves = expand_pair(token)
                if len(halves) != 2:
                    continue
                pairs += 1
                for half in halves:
                    verdict, _ = resolve(half, doc, self.index)
                    self.assertNotIn(
                        verdict, ("MISSING", "AMBIGUOUS"),
                        "%s writes %s but %s does not resolve -- the shorthand "
                        "is advertising a file that is not there"
                        % (doc, token, half))
        self.assertGreater(
            pairs, 0,
            "no `X.hpp/.cpp` shorthand found in any doc -- PAIR_RE has stopped "
            "matching, so this check is vacuous")

    # -- the documented absences ------------------------------------------

    def test_documented_absences_are_still_absent(self):
        for (doc, token), (path, why) in EXPECTED_ABSENT.items():
            full = os.path.join(REPO, path.replace("/", os.sep))
            self.assertFalse(
                os.path.exists(full),
                "%s now exists, but %s documents it as absent. %s"
                % (path, doc, why))

    def test_allowlist_entries_are_still_referenced(self):
        """A stale exemption is how the next blind spot gets built."""
        for doc, token in sorted(self.declared):
            self.assertIn(
                doc, DOCS,
                "'%s' is exempted for %s, which is not scanned" % (token, doc))
            self.assertIn(
                token, self.files[doc],
                "'%s' is exempted here but %s no longer mentions it -- drop "
                "the entry rather than leaving a rule with nothing under it."
                % (token, doc))

    def test_absent_directory_declarations_are_still_written(self):
        """The directory counterpart of the check above.

        EXPECTED_ABSENT_DIRS suppresses a MISSING verdict, so an entry left
        behind after the prose is rewritten is a permanently silenced token --
        exactly the blind spot the file-token allowlist check exists to prevent.
        Asserted against `dir_tokens`, not `file_tokens`: a trailing-slash token
        never appears in the latter.
        """
        for doc, token in sorted(EXPECTED_ABSENT_DIRS):
            self.assertIn(
                doc, DOCS,
                "'%s' is exempted for %s, which is not scanned" % (token, doc))
            self.assertIn(
                token, self.dirs[doc],
                "'%s' is declared absent for %s but %s no longer names it -- "
                "drop the entry rather than leaving a rule with nothing under "
                "it." % (token, doc, doc))

    def test_directories_documented_as_absent_are_still_absent(self):
        """The suppression is only correct while the directory really is gone.

        Without this, EXPECTED_ABSENT_DIRS is a one-way mute: `payload-stage/`
        could reappear and the gate would keep passing while the doc that calls
        it abandoned history describes the current tree.
        """
        for (doc, token), (path, why) in EXPECTED_ABSENT_DIRS.items():
            full = os.path.join(REPO, path.replace("/", os.sep))
            self.assertFalse(
                os.path.exists(full),
                "%s now exists, but %s documents it as gone. %s"
                % (path, doc, why))

    # -- the gate commands -------------------------------------------------

    def test_every_documented_command_names_a_real_script(self):
        """Across EVERY fence language, not just ```sh."""
        missing, checked = [], 0
        for doc in DOCS:
            body = "\n".join(b for _, b in self.fences[doc])
            for token in file_tokens(body):
                if (doc, token) in self.declared:
                    continue
                for part in expand_pair(token):
                    checked += 1
                    if resolve(part, doc, self.index)[0] in ("MISSING",
                                                             "AMBIGUOUS"):
                        missing.append("%s: %s" % (doc, part))
        self.assertGreater(
            checked, 0, "no script paths extracted from any doc's fenced blocks")
        self.assertEqual(
            [], missing,
            "documented command(s) invoke script(s) that do not exist: %s"
            % "; ".join(missing))

    def test_the_four_gates_are_all_documented_and_real(self):
        """The gate list is the load-bearing part of CLAUDE.md; name it directly."""
        gates = ("tools/check-registers.py",
                 "tests/build-msvc.bat",
                 "tests/check-kext-syntax.bat",
                 "tests/run-python-tests.bat")
        body = "\n".join(sh_blocks(self.text["CLAUDE.md"])).replace("\\", "/")
        for gate in gates:
            self.assertIn(gate, body,
                          "CLAUDE.md's ```sh blocks no longer invoke " + gate)
            self.assertTrue(
                os.path.isfile(os.path.join(REPO, gate.replace("/", os.sep))),
                "gate %s is documented but missing from disk" % gate)

    # -- negative controls: the resolver can actually fail ----------------

    def test_resolver_reports_missing_for_a_path_that_is_not_there(self):
        verdict, _ = resolve("phase0-kext/NVNoSuchHeader.hpp", "CLAUDE.md",
                             self.index)
        self.assertEqual("MISSING", verdict)

    def test_resolver_reports_missing_for_a_bare_name_that_is_not_there(self):
        verdict, _ = resolve("NVNoSuchHeader.hpp", "CLAUDE.md", self.index)
        self.assertEqual("MISSING", verdict)

    def test_a_bogus_token_fails_under_every_resolution_base(self):
        """DOCREL/DIRECT/PARENT are three chances; none may be a free pass."""
        for doc in DOCS:
            verdict, _ = resolve("no-such-file-anywhere.py", doc, self.index)
            self.assertEqual("MISSING", verdict,
                             "a nonexistent token resolved against " + doc)

    def test_tail_match_is_anchored_at_a_separator(self):
        """`kext/NVDecode.hpp` must not resolve to `phase0-kext/NVDecode.hpp`.

        The token that reads as the obvious test here -- `Decode.hpp` -- proves
        nothing about the anchor, and the mutation probe caught it: the index is
        keyed by basename, so that lookup misses on the KEY and returns MISSING
        before the endswith is ever evaluated. Deleting the separator left the
        check green.

        What exercises the anchor is a token whose basename is real but whose
        directory prefix stops mid-name. Without the leading "/" in the
        endswith, `kext/NVDecode.hpp` matches the real header, and a doc naming
        a directory that does not exist reads as correct.
        """
        self.assertIn("NVDecode.hpp", self.index)
        self.assertIn("phase0-kext/NVDecode.hpp", self.index["NVDecode.hpp"])
        verdict, _ = resolve("kext/NVDecode.hpp", "CLAUDE.md", self.index)
        self.assertEqual(
            "MISSING", verdict,
            "a directory prefix that stops mid-name resolved anyway -- the "
            "endswith has lost its leading separator")
        # The key-miss case is still worth pinning; it just is not anchor proof.
        self.assertEqual("MISSING",
                         resolve("Decode.hpp", "CLAUDE.md", self.index)[0])

    def test_tail_match_reports_ambiguous_when_two_files_share_a_basename(self):
        """A basename naming two files must fail, not silently pick one.

        This is not hypothetical. On 2026-08-13 `payload-stage/` appeared -- a
        generated staging copy of `phase0-kext/` for the payload ISO -- and
        `NVDecode.hpp` went from naming one file to naming two. Returning the
        first index hit made the verdict depend on `os.walk` enumeration order,
        which is exactly the kind of answer that is right until the day it is
        not. The index is synthesised here rather than read off disk so the
        check keeps working whether or not such a directory currently exists.
        """
        fake = {"NVDecode.hpp": ["phase0-kext/NVDecode.hpp",
                                 "somewhere-else/phase0-kext/NVDecode.hpp"]}
        verdict, detail = resolve("NVDecode.hpp", "CLAUDE.md", fake)
        self.assertEqual("AMBIGUOUS", verdict)
        self.assertIn("2 hits", detail)

    def test_payload_stage_is_excluded_from_the_index(self):
        """The staging copy must stay out of the index.

        Scanning the index for it is not enough on its own: `payload-stage/` is
        generated intermittently -- it appeared and was removed again within
        hours on 2026-08-13 -- so while it is absent that loop iterates over a
        tree that could never contain it and passes vacuously. The SKIP_DIRS
        membership assertion holds either way. The index scan stays because it
        is the half that would catch a *differently named* staging copy.
        """
        self.assertIn(
            "payload-stage", SKIP_DIRS,
            "payload-stage/ must stay in SKIP_DIRS -- it is a staging copy of "
            "phase0-kext/, so indexing it doubles every kext basename and "
            "every bare-name lookup goes AMBIGUOUS")
        for paths in self.index.values():
            for path in paths:
                self.assertFalse(
                    path.startswith("payload-stage/"),
                    "payload-stage/ is in the index (%s) -- it duplicates every "
                    "kext basename, so bare-name resolution goes ambiguous" % path)

    def test_tail_match_accepts_a_bare_basename_that_is_real(self):
        verdict, path = resolve("NVDecode.hpp", "CLAUDE.md", self.index)
        self.assertEqual("TAIL", verdict)
        self.assertEqual("phase0-kext/NVDecode.hpp", path)

    def test_direct_match_wins_for_a_qualified_path(self):
        verdict, path = resolve("tools/check-registers.py", "CLAUDE.md",
                                self.index)
        self.assertEqual("DIRECT", verdict)
        self.assertEqual("tools/check-registers.py", path)

    def test_backslash_paths_normalise(self):
        verdict, _ = resolve(r"tests\run-python-tests.bat", "CLAUDE.md",
                             self.index)
        self.assertEqual("DIRECT", verdict)

    def test_docrel_resolves_a_sibling_of_the_doc(self):
        """tools/README.md says `identify-gpu.py`, meaning tools/identify-gpu.py."""
        verdict, path = resolve("identify-gpu.py", "tools/README.md", self.index)
        self.assertEqual("DOCREL", verdict)
        self.assertEqual("tools/identify-gpu.py", path)

    def test_parent_resolves_the_docs_own_directory(self):
        """`kextshim/` inside tests/kextshim/README.md is self-referential."""
        verdict, path = resolve("kextshim/", "tests/kextshim/README.md",
                                self.index, want_dir=True)
        self.assertEqual("PARENT", verdict)
        self.assertEqual("tests/kextshim", path)

    def test_doc_relative_beats_root_relative_on_a_real_collision(self):
        """DOCREL is tried before DIRECT, and that ordering is a CHOICE.

        `README.md` and `tests/README.md` BOTH exist, so the token `README.md`
        written inside tests/README.md resolves to a different file under each
        ordering. It is not a formality that can be flipped for tidiness.

        Doc-relative wins because that is what the author meant: a doc inside a
        subdirectory writes paths as seen from its own directory. Under the
        other ordering every subdirectory doc's self-reference silently retargets
        the root README, and the gate would report a path as verified while
        pointing at a file the doc was not talking about -- green, and wrong,
        which is the failure mode this whole module exists to prevent.

        Folded in from tests/test_documented_paths_multidoc.py on 2026-08-13.
        That module resolved root-relative FIRST, and the two orderings were
        measured against each other before the fold: they agreed on every token
        in every doc EXCEPT this one. The disagreement was real, so the winner
        is recorded as a test rather than left to whoever edits resolve() next.
        (The scratch probe that measured it was deleted with the fold -- rerun
        it by calling resolve("README.md", "tests/README.md", index) with the
        two base orderings swapped.)
        """
        self.assertTrue(os.path.isfile(os.path.join(REPO, "README.md")),
                        "the root README.md is gone -- this collision test no "
                        "longer exercises a collision and proves nothing")
        self.assertTrue(os.path.isfile(
            os.path.join(REPO, "tests", "README.md")))
        verdict, path = resolve("README.md", "tests/README.md", self.index)
        self.assertEqual("DOCREL", verdict)
        self.assertEqual("tests/README.md", path)

    def test_a_climb_that_stays_in_the_repo_resolves_normally(self):
        """`../STEPS.md` in tests/README.md is correct, not an escape.

        Pinned alongside the ESCAPES control below because the two differ only
        in how far the climb goes. A guard that rejected every `..` would fail
        this, and a guard that accepted every `..` would fail that one.
        """
        verdict, path = resolve("../STEPS.md", "tests/README.md", self.index)
        self.assertEqual("DOCREL", verdict)
        self.assertEqual("STEPS.md", path)

    def test_a_join_that_leaves_the_repo_is_reported_as_escaping(self):
        """An out-of-tree join gets its own verdict, not a bare MISSING.

        Both are red, so this is about the MESSAGE. MISSING sends the reader
        looking for a file to create; the actual defect is that the doc names
        something outside the repository entirely, and no file they add will
        ever satisfy it.

        Folded in from tests/test_documented_paths_multidoc.py on 2026-08-13.
        """
        verdict, path = resolve("../../../outside.py",
                                "tests/kextshim/README.md", self.index)
        self.assertEqual("ESCAPES", verdict)
        self.assertTrue(path.startswith(".."),
                        "ESCAPES must report the escaping join so the reader "
                        "can see where it landed, got %r" % (path,))

    def test_elision_resolves_when_exactly_one_thing_matches(self):
        verdict, path = resolve(
            "reference/open-gpu-kernel-modules/.../swref/published/",
            "tools/README.md", self.index, want_dir=True)
        self.assertEqual("ELISION", verdict)
        self.assertEqual(
            "reference/open-gpu-kernel-modules/src/common/inc/swref/published",
            path)

    def test_elision_reports_ambiguous_rather_than_picking_one(self):
        """`tests/.../README.md` matches more than one file; that must fail."""
        verdict, detail = resolve("tests/.../README.md", "CLAUDE.md", self.index)
        self.assertEqual("AMBIGUOUS", verdict)
        self.assertIn("hits", detail)

    def test_elision_reports_missing_when_nothing_matches(self):
        verdict, _ = resolve("tools/.../no-such-thing.py", "CLAUDE.md",
                             self.index)
        self.assertEqual("MISSING", verdict)

    def test_pair_expansion_splits_and_leaves_others_alone(self):
        self.assertEqual(["NV3050Probe.hpp", "NV3050Probe.cpp"],
                         expand_pair("NV3050Probe.hpp/.cpp"))
        self.assertEqual(["tools/check-registers.py"],
                         expand_pair("tools/check-registers.py"))

    def test_fence_parser_reads_every_language_and_ignores_prose(self):
        sample = ("intro\n```sh\nreal command\n```\nprose\n"
                  "```powershell\nGet-Item\n```\nmore\n```\nbare\n```\n")
        self.assertEqual([("sh", "real command"),
                          ("powershell", "Get-Item"),
                          ("", "bare")], fences(sample))
        self.assertEqual(["real command"], sh_blocks(sample))


if __name__ == "__main__":
    unittest.main(verbosity=2)

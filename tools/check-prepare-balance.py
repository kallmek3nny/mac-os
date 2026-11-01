#!/usr/bin/env python3
"""
check-prepare-balance.py -- assert every acquire/release pair in the IOKit shell
is guarded by a flag that tracks the ACQUIRE, and that the flag's assignment
sites sit in the only order that is correct.

Why a static checker rather than a test
---------------------------------------
``IOMemoryDescriptor`` carries a wire count. ``prepare()`` increments it and
``complete()`` decrements it, and a ``complete()`` with no successful
``prepare()`` behind it drives that counter below its own floor -- inside kernel
VM bookkeeping, which is nowhere this driver prints. The driver's own view stays
consistent, so ``NVLOG`` says nothing.

All four existing gates are STRUCTURALLY blind to it, and not by accident:

  * ``check-kext-syntax.bat`` proves the file PARSES. ``fGspFwBuffer->complete()``
    is a well-formed call whatever came before it; a prepare/complete balance is
    not a parse property.
  * The MSVC suites compile only the dependency-free headers. This code is in the
    IOKit shell, on the far side of that split.
  * ``tests/kextshim/`` declares these methods and has no wire count and no
    descriptor ownership to unbalance, so even driving the shell there would
    register nothing.
  * ``check-registers.py`` reads constants.

That leaves the SHAPE of the code as the only thing any gate here can assert.
This script asserts it. It cannot prove the balance holds at runtime -- nothing
available on Windows can -- but the defects that existed in this file were shape
defects, and so are the two ways of getting the fix wrong.

Why a TABLE of pairs and not just prepare/complete
--------------------------------------------------
The first version of this script matched ``prepare``/``complete`` literally. It
went green, and four lines below the defect it was written for sat the SAME
defect in ``setMemoryDescriptor``/``clearMemoryDescriptor`` -- same object, same
teardown, reached from the same failure path -- which it could not see. A gate
written for one instance of a defect class, blind to the second instance in the
same function, teaches the wrong lesson: it converts "this pair is fine" into
"this FILE is fine".

So the pairs live in ``ACQUIRE_RELEASE_PAIRS`` and adding one is the whole cost
of covering it.

Nothing here is typed
---------------------
The flag NAMES are not written down in this file. Writing them here would make
this script a second hand-kept copy of a driver identifier, which is the defect
class this project keeps finding. Instead each flag is DERIVED: find the acquire
failure check, and the boolean the code sets ``true`` immediately after it is
that pair's flag by definition. If a future refactor renames it, this checker
follows.

The two opposite errors
-----------------------
The assignments have to bracket the acquire and the release EXACTLY, because the
two ways of being approximate fail in opposite directions:

  set true too late   -> a bail-out between the acquire and the assignment runs a
                         teardown that skips the release, LEAKING the resource.
  clear too late      -> a re-entrant teardown sees the flag still true and runs
                         the release a SECOND time.

So the checks below are: initialised false in ``init()``; set true after the
acquire success check with no ``return`` in between; every release inside an
``if (flag)``; the flag cleared BEFORE the release; and the object dropped AFTER
it.

Refusal, not silence
--------------------
If an acquire site is not in a shape this script can reason about, it says so and
FAILS. A checker that cannot find its subject and reports OK is worse than no
checker, because it converts an unexamined file into a green tick.

Usage:  python tools/check-prepare-balance.py [project-root]
"""

import glob
import os
import re
import sys

IDENT = r"[A-Za-z_]\w*"

#
# The pairs this script knows about.
#
# Each entry is (acquire-method, release-method, what-the-flag-means). The third
# field is printed in the failure, because "add a guard" is not actionable
# without knowing what the guard is asserting.
#
# NOT in this table, deliberately: IOMemoryDescriptor::map / IOMemoryMap. The
# release there is the map object's own reference -- OSSafeReleaseNULL on the
# IOMemoryMap IS the unmap, and there is no ``->unmap()`` call to pair with. A
# map/unmap entry would therefore report the correct, idiomatic code in
# unmapBar0() as an unreleased acquire. Listing the omission is the point: the
# next person to wonder why it is missing should not have to re-derive it.
#
# What earns a row (t-d1480a)
# ---------------------------
# A pair belongs here when the acquire changes the state of an object that
# ALREADY EXISTS, so the object's pointer is no evidence the acquire happened.
# That is exactly what lets `if (theObject)` read as a guard without being one.
# When the acquire PRODUCES the handle, the handle is non-null exactly when
# there is something to give back: the existence check IS the right guard there,
# and a row would report correct code. That is the general form of "never add a
# pair whose release is OSSafeReleaseNULL".
#
# Every ``->method(`` and ``IO*``/``OS*`` call in phase0-kext was audited
# against that rule. One pair joined the table (open/close, below). These did
# not, and each is written down so the question is not re-asked:
#
#   retain/release, copyProperty, IORegistryIterator, withSpecification and
#   the other creators -- reference counts. The pointer IS the reference, so
#   `if (p) p->release()` and OSSafeReleaseNULL(p) are correct by construction.
#
#   IOMalloc/IOFree, IOLockAlloc/IOLockFree -- the pointer IS the allocation,
#   same reason. They are also free functions, which the ``receiver->method(``
#   pattern here cannot match at all. IntelGtBringup::free() guards
#   IOLockFree() on the lock pointer, and that is the right predicate.
#
#   IOLockLock/IOLockUnlock -- a bracket inside one function body, never a
#   resource that outlives its function and is given back by a shared
#   teardown. Lock balance is a different property from the one checked here.
#
#   attach/detach -- only in newUserClient(), where detach() undoes an attach()
#   one line above it, on a local, on a straight-line failure path. Once the
#   client is handed out, IOKit termination detaches it, not this driver. No
#   member flag exists or should; a row would report "nothing is set true after
#   the attach() success check" over correct code.
#
#   setMemoryEnable, setBusMasterEnable, configWrite16 -- writes to the PCI
#   COMMAND register, not counted resources. Both directions are the SAME
#   method told apart by an argument, which a table keyed on method names
#   cannot express, and repeating the "release" writes the same value again,
#   so the double-release direction this checker exists for is not a failure
#   there. Whether the old COMMAND value comes back is a separate property
#   (IntelGtBringup saves the old value and restores it under its own flag).
#
#   enablePCIPowerManagement -- one-way; nothing in the tree reverses it, so
#   there is no release to guard.
#
#   allocatePersistentSurface32/freePersistentSurface -- the acquire produces
#   an opaque handle, owned by the surface table under the queue lock and freed
#   by that handle; there is no member to carry a flag.
#
#   IOServiceOpen/IOServiceClose, IOServiceGetMatchingService/IOObjectRelease
#   -- userspace: iggt-blit.cpp lives in phase0-kext/ and is globbed with the
#   kext sources. Free functions whose result IS the handle being released.
#
#   Everything else the audit listed (get*, configRead*, gen64IOVMSegments,
#   synchronize, metaCast, the surface fill/copy calls, ...) acquires nothing.
#
ACQUIRE_RELEASE_PAIRS = (
    ("prepare", "complete",
     "the pages are wired -- IOMemoryDescriptor carries a wire count, and a "
     "complete() with no prepare behind it decrements it below its floor"),
    ("setMemoryDescriptor", "clearMemoryDescriptor",
     "the command has taken ownership of a descriptor -- `if (theCommand)` is "
     "true from the moment withSpecification() returns and says nothing about "
     "whether a descriptor was ever set"),
    ("prepareForDMA", "completeDMA",
     "the DMA mapping is live"),
    #
    # open/close, added by the t-d1480a audit. Both callers retain the provider
    # in start() BEFORE trying open(), and open() can refuse (both callers
    # handle "provider already owned") -- so the provider pointer exists on
    # exactly the path where nothing was opened. In the other direction, xnu's
    # didTerminate() delivers kIOMessageServiceIsRequestingClose to a client
    # still holding its provider open, and schedules that client's stop only
    # once it has closed. Whether close() is a no-op for a client that never
    # opened was not established from this tree; the flag makes the teardown
    # correct without depending on that answer, as handoff10.md 10b argues.
    #
    ("open", "close",
     "this driver holds its provider open -- `if (theProvider)` is true from "
     "the moment start() retains the provider and says nothing about whether "
     "open() succeeded"),
)


def method_call_re(method):
    """``receiver->method(`` for one exact method name.

    The trailing ``\\s*\\(`` is what keeps ``prepare`` from matching
    ``prepareForDMA``, which is a different pair with a different release.
    """
    return re.compile(r"\b(" + IDENT + r")\s*->\s*" + re.escape(method) + r"\s*\(")


SET_TRUE_RE = re.compile(r"^\s*(" + IDENT + r")\s*=\s*true\s*;")
SET_FALSE_RE = re.compile(r"^\s*(" + IDENT + r")\s*=\s*false\s*;")
IF_COND_RE = re.compile(r"\bif\s*\(\s*(.*?)\s*\)\s*\{?\s*$")
RELEASE_RE = re.compile(r"(?:OSSafeReleaseNULL|OSSafeRelease)\s*\(\s*(" + IDENT + r")\s*\)")
INIT_RE = re.compile(r"\b" + IDENT + r"::init\s*\(")
BOOL_DECL_RE = re.compile(r"^\s*bool\s+(" + IDENT + r")\s*;")
RETURN_RE = re.compile(r"\breturn\b")

#
# Field flags. Not every flag is a member of the driver class: a resource kept
# in a separately allocated runtime struct carries its flag in that struct.
# IntelGtBringupSurface.cpp keeps its IODMACommand in IntelGtSurfaceRuntimeState
# and records the bind as ``state->dmaBound``. The bare-identifier patterns
# above cannot match that left-hand side, so the flag derivation skipped
# ``state->dmaBound=true;``, ran on to the next bare ``x = true;`` in the
# function (``fQuarantined``) and reported three findings against a flag that
# has nothing to do with the descriptor.
#
# A field flag is accepted ONLY on the object that holds the receiver:
# ``base->field = true;`` counts for an acquire on ``base->receiver->...``, and
# for nothing else. An acquire on a bare member (``fDma->...``) has no base, so
# its flag derivation is exactly what it was before field flags existed.
#
LVALUE = r"(?:" + IDENT + r"\s*->\s*)*" + IDENT
FIELD_SET_TRUE_RE = re.compile(
    r"^\s*(" + LVALUE + r")\s*->\s*(" + IDENT + r")\s*=\s*true\s*;")
FLAG_SET_FALSE_RE = re.compile(r"^\s*(" + LVALUE + r")\s*=\s*false\s*;")
RELEASE_CHAIN_RE = re.compile(
    r"(?:OSSafeReleaseNULL|OSSafeRelease)\s*\(\s*(" + LVALUE + r")\s*\)")


def squash(text):
    """Drop all whitespace, so ``state -> dmaBound`` equals ``state->dmaBound``."""
    return re.sub(r"\s+", "", text)


def receiver_base(line, method):
    """The object path in front of ``method``'s receiver on ``line``, or None.

    ``state->dma->setMemoryDescriptor(`` gives ``state``; ``fDma->prepare(``
    and ``fQueues[i].dma->prepare(`` give None. ``method_call_re`` reports only
    the receiver (``dma``), which is what pairs an acquire with its release;
    this says which object that receiver's flag may live on.
    """
    match = re.search(r"\b((?:" + IDENT + r"\s*->\s*)*)" + IDENT
                      + r"\s*->\s*" + re.escape(method) + r"\s*\(", line)
    if not match or not match.group(1):
        return None
    return squash(match.group(1))[:-len("->")]


def zeroed_re(holder):
    """A line that zeroes ``holder``: bzero, memset to 0, or IOMallocZero.

    The IOMallocZero form must be a single-line assignment; an allocation split
    across lines is recognised by the bzero that has to follow it anyway.
    """
    path = r"\s*->\s*".join(re.escape(part) for part in holder.split("->"))
    return re.compile("|".join((
        r"\bbzero\s*\(\s*" + path + r"\s*,",
        r"\bmemset\s*\(\s*" + path + r"\s*,\s*0\s*,",
        r"^\s*(?:[\w:<>]+\s*\*\s*)?" + path + r"\s*=.*\bIOMallocZero\w*\s*\(")))


def strip_comments(text):
    """Blank out comments while preserving every line and column position.

    Line numbers in this script's output have to match the real file, because the
    operator's next move is to open it. String literals are skipped rather than
    scanned, so an NVLOG format string containing ``//`` does not eat the rest of
    the line.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in "\"'":
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i])
                    out.append(text[i + 1])
                    i += 2
                    continue
                out.append(text[i])
                done = text[i] == quote or text[i] == "\n"
                i += 1
                if done:
                    break
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            out.append("  ")
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def enclosing_openers(lines):
    """For each line index, the indices of the ``{`` lines enclosing it.

    Outermost first. A line that itself opens a block does not appear in its own
    list, which is what we want -- we are asking what CONTAINS this statement.
    """
    stack = []
    per_line = []
    for idx, line in enumerate(lines):
        per_line.append(list(stack))
        for ch in line:
            if ch == "{":
                stack.append(idx)
            elif ch == "}" and stack:
                stack.pop()
    return per_line


def block_close(lines, start):
    """Index of the line closing the block opened on line ``start``.

    Returns None when the line opens no block, which is how an unrecognised
    acquire shape is detected rather than assumed.
    """
    depth = 0
    seen = False
    for idx in range(start, len(lines)):
        for ch in lines[idx]:
            if ch == "{":
                depth += 1
                seen = True
            elif ch == "}":
                depth -= 1
                if seen and depth == 0:
                    return idx
    return None


def function_body(lines, openers, header_re):
    """(open_index, close_index) of the first function whose signature matches."""
    for idx, line in enumerate(lines):
        if header_re.search(line):
            for probe in range(idx, min(idx + 4, len(lines))):
                if "{" in lines[probe]:
                    close = block_close(lines, probe)
                    if close is not None:
                        return probe, close
            return None
    return None


class Checker(object):
    def __init__(self, root):
        self.root = root
        self.failures = []
        self.checks = 0

    def fail(self, message):
        self.failures.append(message)

    def check(self, condition, message):
        self.checks += 1
        if not condition:
            self.fail(message)
        return bool(condition)

    def run(self):
        kext = os.path.join(self.root, "phase0-kext")
        sources = sorted(glob.glob(os.path.join(kext, "*.cpp")))
        headers = sorted(glob.glob(os.path.join(kext, "*.hpp")))

        if not sources:
            self.fail("no .cpp sources under %s -- nothing was examined, so an "
                      "OK here would be a green tick over an unread tree"
                      % kext)
            return

        header_text = ""
        for path in headers:
            header_text += self.read(path)

        examined = 0
        for path in sources:
            examined += self.check_source(path, header_text, headers)

        #
        # The vacuity guard. A source set with no acquire/release pair at all is
        # a legitimate state -- but it must be reported, not silently counted as
        # a pass, because it is indistinguishable in the output from a checker
        # whose patterns have stopped matching.
        #
        if examined == 0:
            self.fail("no acquire/release pair (%s) found in any of %d source "
                      "file(s). Either the driver no longer holds these "
                      "resources, or this checker's patterns have stopped "
                      "matching the code -- and those two look identical from "
                      "here. Resolve it before trusting this gate again."
                      % (", ".join("%s/%s" % (a, r)
                                   for a, r, _ in ACQUIRE_RELEASE_PAIRS),
                         len(sources)))

    def read(self, path):
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read()

    def check_source(self, path, header_text, headers):
        rel = os.path.relpath(path, self.root)
        raw = self.read(path)
        lines = strip_comments(raw).split("\n")
        openers = enclosing_openers(lines)

        pairs = 0
        for acquire, release, meaning in ACQUIRE_RELEASE_PAIRS:
            pairs += self.check_one_pair_kind(
                rel, lines, openers, acquire, release, meaning,
                header_text, headers)
        return pairs

    def check_one_pair_kind(self, rel, lines, openers, acquire, release,
                            meaning, header_text, headers):
        acquire_re = method_call_re(acquire)
        release_re = method_call_re(release)

        acquires = [(i, m.group(1)) for i, line in enumerate(lines)
                    for m in [acquire_re.search(line)] if m]
        releases = [(i, m.group(1)) for i, line in enumerate(lines)
                    for m in [release_re.search(line)] if m]

        if not acquires and not releases:
            return 0

        #
        # A release with no acquire anywhere is the raw form of the bug: a
        # decrement with no matching increment in the whole translation unit.
        #
        for idx, receiver in releases:
            if not any(r == receiver for _, r in acquires):
                self.check(False,
                           "%s:%d calls %s->%s() but nothing in this file "
                           "ever calls %s->%s(). The guard would be asserting "
                           "that %s, and nothing here ever establishes it."
                           % (rel, idx + 1, receiver, release,
                              receiver, acquire, meaning))

        pairs = 0
        for idx, receiver in acquires:
            if not any(r == receiver for _, r in releases):
                #
                # The opposite error, and equally invisible: acquired and never
                # given back.
                #
                self.check(False,
                           "%s:%d calls %s->%s() and no %s() on %s exists in "
                           "this file -- the resource is never released."
                           % (rel, idx + 1, receiver, acquire, release, receiver))
                continue
            pairs += 1
            self.check_pair(rel, lines, openers, idx, receiver,
                            acquire, release, meaning, releases,
                            header_text, headers)
        return pairs

    def check_pair(self, rel, lines, openers, acquire_idx, receiver,
                   acquire, release, meaning, releases,
                   header_text, headers):
        #
        # Derive the flag. The acquire has to sit inside a failure check whose
        # block this script can delimit, because "the assignment after the
        # check" is only meaningful if the check has an end.
        #
        close = block_close(lines, acquire_idx)
        if close is None:
            self.check(False,
                       "%s:%d -- %s->%s() is not inside a recognisable "
                       "`if (...) { ... }` failure check, so this script cannot "
                       "locate the flag that guards its %s(). Refusing rather "
                       "than reporting OK over an unexamined pair."
                       % (rel, acquire_idx + 1, receiver, acquire, release))
            return

        base = receiver_base(lines[acquire_idx], acquire)
        flag = None
        flag_idx = None
        for idx in range(close + 1, len(lines)):
            match = SET_TRUE_RE.match(lines[idx])
            if match:
                flag = match.group(1)
                flag_idx = idx
                break
            field = FIELD_SET_TRUE_RE.match(lines[idx])
            if base and field and squash(field.group(1)) == base:
                flag = base + "->" + field.group(2)
                flag_idx = idx
                break
            #
            # Stop at the end of the enclosing function; a `true` assignment in
            # some later function is not this pair's flag.
            #
            if lines[idx].startswith("}"):
                break

        if not self.check(
                flag is not None,
                "%s:%d -- nothing is set true after the %s->%s() success "
                "check, so no flag records that %s. Every %s() below is "
                "therefore unguarded by construction."
                % (rel, acquire_idx + 1, receiver, acquire, meaning, release)):
            return

        #
        # 1. No bail-out between the successful acquire and the assignment.
        #    A `return` in that window runs a teardown that will skip the
        #    release and strand the resource.
        #
        between = "\n".join(lines[close + 1:flag_idx])
        self.check(not RETURN_RE.search(between),
                   "%s -- there is a `return` between the %s->%s() success "
                   "check (line %d) and `%s = true;` (line %d). That path "
                   "leaves the resource held with nothing recording it, so the "
                   "teardown skips %s() and it leaks."
                   % (rel, receiver, acquire, acquire_idx + 1, flag,
                      flag_idx + 1, release))

        #
        # 2. Declared as a bool member in a header. A local would reset to
        #    garbage on the next call and never survive to the teardown.
        #
        #    A field flag (``state->dmaBound``) is declared in the struct
        #    that holds it, so the field name is what a header shows.
        #
        member = flag.split("->")[-1]
        declared = any(BOOL_DECL_RE.match(line)
                       and BOOL_DECL_RE.match(line).group(1) == member
                       for line in strip_comments(header_text).split("\n"))
        self.check(declared,
                   "`bool %s;` is not declared in any of %s. A flag that is not "
                   "a member cannot survive from the function that acquires to "
                   "the teardown that reads it."
                   % (member, ", ".join(os.path.basename(h) for h in headers)))

        #
        # 3. Initialised false in init(). free() can run on an instance whose
        #    start() never reached this code, and an uninitialised member read as
        #    true there runs the release against something never acquired.
        #
        #    A field flag lives in an object this file allocates, which
        #    init() never sees. The same property there is that the holder
        #    is zeroed before the acquire: IOMalloc() returns uninitialised
        #    memory, and the teardown runs on the failure path of every step
        #    after the allocation.
        #
        if "->" in flag:
            holder = flag.rsplit("->", 1)[0]
            zeroed = zeroed_re(holder)
            self.check(any(zeroed.search(line) for line in lines[:acquire_idx]),
                       "%s:%d -- `%s` lives in `%s`, and nothing before the "
                       "%s->%s() zeroes `%s` (bzero, memset 0 or IOMallocZero). "
                       "IOMalloc() memory is uninitialised; a flag read as true "
                       "there calls %s() against a resource that was never "
                       "acquired."
                       % (rel, acquire_idx + 1, flag, holder, receiver, acquire,
                          holder, release))
        else:
            span = function_body(lines, openers, INIT_RE)
            if self.check(span is not None,
                          "%s -- no ::init() body found, so this script cannot check "
                          "that `%s` starts false." % (rel, flag)):
                body = lines[span[0]:span[1] + 1]
                initialised = any(SET_FALSE_RE.match(line)
                                  and SET_FALSE_RE.match(line).group(1) == flag
                                  for line in body)
                self.check(initialised,
                           "%s -- `%s = false;` does not appear in ::init() (lines "
                           "%d-%d). free() may run on an instance whose start() "
                           "never reached this code; an uninitialised member read as "
                           "true there calls %s() against a resource that was never "
                           "acquired."
                           % (rel, flag, span[0] + 1, span[1] + 1, release))

        #
        # 4/5/6. Each release on this receiver: guarded by the flag, flag
        #        cleared BEFORE the call, object dropped AFTER it.
        #
        for idx, target in releases:
            if target != receiver:
                continue

            guard = None
            for opener in reversed(openers[idx]):
                cond = IF_COND_RE.search(lines[opener])
                if cond and squash(cond.group(1)) == flag:
                    guard = opener
                    break

            if not self.check(
                    guard is not None,
                    "%s:%d -- %s->%s() is not inside an `if (%s)`. The guard has "
                    "to assert that %s. This is the defect class found twice in "
                    "this file on 2026-08-13: the teardown is called from the "
                    "failure path where the ACQUIRE ITSELF failed, so an "
                    "unconditional release there gives back something that was "
                    "never taken."
                    % (rel, idx + 1, receiver, release, flag, meaning)):
                continue

            cleared = [j for j in range(guard, idx)
                       if FLAG_SET_FALSE_RE.match(lines[j])
                       and squash(FLAG_SET_FALSE_RE.match(lines[j]).group(1))
                       == flag]
            self.check(cleared,
                       "%s:%d -- `%s = false;` does not appear between the "
                       "`if (%s)` on line %d and the %s() call. Clearing AFTER "
                       "the call leaves a window where a re-entrant teardown "
                       "sees the flag still true and releases a second time; "
                       "clearing first makes the second entry a no-op."
                       % (rel, idx + 1, flag, flag, guard + 1, release))

            held = base + "->" + receiver if base else receiver
            dropped = [j for j, line in enumerate(lines)
                       if any(squash(m.group(1)) == held
                              for m in RELEASE_CHAIN_RE.finditer(line))]
            self.check(any(j > idx for j in dropped),
                       "%s:%d -- no release of %s appears after its %s(). "
                       "Either the object leaks, or it is released before the "
                       "%s(), which operates on something the driver has "
                       "already dropped its reference to."
                       % (rel, idx + 1, held, release, release))


def main(argv):
    root = argv[1] if len(argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))
    checker = Checker(root)
    checker.run()

    for message in checker.failures:
        print("  %s" % message)

    if checker.failures:
        #
        # Not "N of M failed": the vacuity refusal fires before any check runs,
        # and "1 of 0 check(s) failed" reads as a broken counter rather than as
        # a refusal to examine. Phrase it so both cases are true.
        #
        print("check-prepare-balance: FAILED -- %d failure(s) across %d check(s)"
              % (len(checker.failures), checker.checks))
        return 1

    print("check-prepare-balance: OK -- %d checks, every release is guarded by "
          "a flag that tracks its acquire (%d pair kind(s) known)"
          % (checker.checks, len(ACQUIRE_RELEASE_PAIRS)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

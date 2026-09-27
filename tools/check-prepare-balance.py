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

        flag = None
        flag_idx = None
        for idx in range(close + 1, len(lines)):
            match = SET_TRUE_RE.match(lines[idx])
            if match:
                flag = match.group(1)
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
        declared = any(BOOL_DECL_RE.match(line)
                       and BOOL_DECL_RE.match(line).group(1) == flag
                       for line in strip_comments(header_text).split("\n"))
        self.check(declared,
                   "`bool %s;` is not declared in any of %s. A flag that is not "
                   "a member cannot survive from the function that acquires to "
                   "the teardown that reads it."
                   % (flag, ", ".join(os.path.basename(h) for h in headers)))

        #
        # 3. Initialised false in init(). free() can run on an instance whose
        #    start() never reached this code, and an uninitialised member read as
        #    true there runs the release against something never acquired.
        #
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
                if cond and cond.group(1).strip() == flag:
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
                       if SET_FALSE_RE.match(lines[j])
                       and SET_FALSE_RE.match(lines[j]).group(1) == flag]
            self.check(cleared,
                       "%s:%d -- `%s = false;` does not appear between the "
                       "`if (%s)` on line %d and the %s() call. Clearing AFTER "
                       "the call leaves a window where a re-entrant teardown "
                       "sees the flag still true and releases a second time; "
                       "clearing first makes the second entry a no-op."
                       % (rel, idx + 1, flag, flag, guard + 1, release))

            dropped = [j for j, line in enumerate(lines)
                       for m in [RELEASE_RE.search(line)]
                       if m and m.group(1) == receiver]
            self.check(any(j > idx for j in dropped),
                       "%s:%d -- no release of %s appears after its %s(). "
                       "Either the object leaks, or it is released before the "
                       "%s(), which operates on something the driver has "
                       "already dropped its reference to."
                       % (rel, idx + 1, receiver, release, release))


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

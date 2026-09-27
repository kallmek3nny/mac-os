#!/usr/bin/env python3
"""
test_source_cited_paths.py -- the kext SOURCES cite files too, and nothing read
those citations until now.

Why this exists
---------------
``tests/test_documented_paths.py`` is a real gate and a thorough one, but its
``DOCS`` tuple is a list of MARKDOWN files. Every path token it resolves comes
out of a ``.md``. Meanwhile ``phase0-kext/*.hpp`` and ``*.cpp`` are the most
heavily commented files in the tree -- the whole design of this project is that
the evidence trail lives in the header next to the constant it justifies -- and
those comments cite 25 distinct file paths that no gate has ever opened.

That is exactly the exposure the doc gate was written for, one directory over:
a renamed tool, a moved reference file, or a deleted test leaves a comment
pointing at nothing, and a comment pointing at nothing is worse than no comment
because it reads as provenance. ``NVRiscvUcode.hpp`` cites
``tests/test_riscv_ucode.cpp``; ``IntelDisplay.hpp`` cites
``igpu-results/tgl-capture-2026-08-16-022556.txt``, which is the perishable
capture and the single most citation-load-bearing file in the project.

This module does NOT reimplement path resolution. It imports
``test_documented_paths`` and calls its ``resolve()``, ``build_index()``,
``file_tokens()`` and ``expand_pair()``. Two resolvers that agree on all but one
token silently ratify a precedence winner nobody chose -- see the memory note
``two-resolvers-can-agree-on-all-but-one-token``. There is one resolver in this
tree and this module is a second CALLER of it, not a second copy.

Measured 2026-08-16 against the live tree, using that resolver:

    sources          14   (phase0-kext/*.hpp + *.cpp, tests/*.cpp + *.hpp)
    distinct tokens  41
    DIRECT           16
    DOCREL           24
    MISSING           1   -- and it is an allowance, see below

The kext half alone measured 8 sources and 25 tokens; the six C++ suites in
``tests/`` add the rest, and every one of theirs resolves. They belong in scope
because each suite's header comment cites the header it drives -- a renamed
``phase0-kext/NVFalcon.hpp`` would leave ``tests/test_falcon.cpp`` pointing at
nothing while still compiling and still printing its check count.

The one MISSING
---------------
``NVRiscvUcode.hpp:40`` names ``extract-firmware-nouveau.py`` while quoting what
that script says about descriptor sizing. It is an UPSTREAM nouveau tool. This
tree does not contain it and should not -- vendoring someone else's extraction
script to satisfy a path gate would be the gate driving the code, which is the
inversion this project keeps writing memory notes about. So it is declared as an
allowance WITH its reason, and arm 3 requires every allowance to still be cited:
an allowance for a token nobody spells any more is dead weight that will quietly
absorb a future real defect with the same basename.

What is deliberately NOT asserted
---------------------------------
No ceiling on the token count. The count gate in ``test_check_count_floor.py``
needs one because a C++ suite's check count drifting UP means its recorded
measurement went stale. Here growth is just more provenance, and capping it
would redden the gate for the act of documenting something. The floor is the
whole guard, and it exists because a broken regex, a moved ``phase0-kext/``, or
an import that silently returns nothing all present as "zero problems found".
"""
import glob
import importlib.util
import os
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, os.pardir))
_DOCGATE = os.path.join(_HERE, "test_documented_paths.py")

# Sources come from glob, never from a hand-kept list. A hardcoded tuple stops
# covering the file added tomorrow and gives no sign that it has.
_SOURCE_GLOBS = (
    os.path.join(_ROOT, "phase0-kext", "*.hpp"),
    os.path.join(_ROOT, "phase0-kext", "*.cpp"),
    os.path.join(_ROOT, "tests", "*.cpp"),
    os.path.join(_ROOT, "tests", "*.hpp"),
)

# tools/*.py is DELIBERATELY excluded, and the reason is measured rather than a
# preference. Running the same scan over it produces 141 tokens with 27 MISSING,
# and every one of the 27 was checked by hand: they are argv placeholders in
# usage strings (`notes.txt`, `tools/x.py`), upstream library filenames
# (`pycdlib.py`), filenames the tool WRITES at runtime rather than reads
# (`tgl-display-mmio.txt`), and paths interior to a built bundle
# (`Contents/Info.plist`). Those are not citations of tree files, so a gate over
# them would need a 27-entry allowance list -- and an allowance list that long
# stops being a set of exceptions and becomes the actual rule, absorbing the
# real defect it was supposed to surface. The C++ sources have zero such
# artifacts because their path tokens live in prose comments, not in argv.

# Verdicts from resolve() that mean "this citation points at something real".
GOOD = ("DIRECT", "DOCREL", "TAIL")

# token -> why it is allowed to point at nothing in this tree.
ALLOWED = {
    "extract-firmware-nouveau.py":
        "upstream nouveau tool, quoted at NVRiscvUcode.hpp:40 for what it says "
        "about RM_RISCV_UCODE_DESC sizing. Vendoring it to satisfy this gate "
        "would be the gate driving the code.",
}

# Measured 2026-08-16: 14 sources, 41 distinct tokens. Floors, not equalities --
# see the module docstring for why there is deliberately no ceiling.
MIN_SOURCES = 11
MIN_TOKENS = 30


def _load_doc_gate():
    spec = importlib.util.spec_from_file_location("documented_paths_resolver",
                                                  _DOCGATE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HAVE_DOCGATE = os.path.isfile(_DOCGATE)
dp = _load_doc_gate() if HAVE_DOCGATE else None


def sources():
    found = []
    for pattern in _SOURCE_GLOBS:
        found.extend(glob.glob(pattern))
    return sorted(found)


def load_sources():
    """[(repo-relative path, text)] for every kext source."""
    out = []
    for path in sources():
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            out.append((os.path.relpath(path, _ROOT), fh.read()))
    return out


def scan(pairs, index):
    """Resolve every cited path token in ``pairs``.

    ``pairs`` is [(rel_path, text)] rather than a list of filenames on purpose:
    the non-vacuity arm feeds a MUTATED text with the real file left untouched
    on disk. A gate that can only run against the filesystem cannot be shown to
    be sensitive without editing the thing it guards.

    Returns {token: (verdict, resolved_or_None, citing_rel_path)}.
    """
    seen = {}
    for rel, text in pairs:
        for token in dp.file_tokens(text):
            for half in dp.expand_pair(token):
                if half in seen:
                    continue
                verdict, resolved = dp.resolve(half, doc=rel, index=index)
                seen[half] = (verdict, resolved, rel)
    return seen


@unittest.skipUnless(HAVE_DOCGATE,
                     "tests/test_documented_paths.py is absent, and its "
                     "resolver is the only one this module will use")
class SourceCitedPathTests(unittest.TestCase):

    def setUp(self):
        self.index = dp.build_index()
        self.pairs = load_sources()

    # ------------------------------------------------------------- arm 1

    def test_the_scan_actually_reaches_the_sources(self):
        """A gate that examined nothing reports the same clean green as a gate
        that examined everything and found nothing wrong."""
        self.assertGreaterEqual(
            len(self.pairs), MIN_SOURCES,
            "only %d kext source(s) matched %s; 8 were measured 2026-08-16. "
            "A moved phase0-kext/ or a changed suffix presents as a clean run."
            % (len(self.pairs),
               ", ".join(os.path.relpath(p, _ROOT) for p in _SOURCE_GLOBS)))

        found = scan(self.pairs, self.index)
        self.assertGreaterEqual(
            len(found), MIN_TOKENS,
            "only %d distinct path token(s) were extracted from %d source "
            "file(s); 25 were measured 2026-08-16. A broken tokenizer finds "
            "nothing and reports no defects."
            % (len(found), len(self.pairs)))

    # ------------------------------------------------------------- arm 2

    def test_every_cited_path_resolves(self):
        """The property itself.

        A comment pointing at a file that no longer exists is worse than no
        comment: it reads as provenance, and this project's only real asset is
        that its provenance is true.
        """
        found = scan(self.pairs, self.index)

        broken = []
        for token in sorted(found):
            verdict, _resolved, rel = found[token]
            if verdict in GOOD or token in ALLOWED:
                continue
            broken.append("  %-10s %-46s cited by %s" % (verdict, token, rel))

        self.assertEqual(
            [], broken,
            "%d path citation(s) in the kext sources do not resolve:\n%s\n"
            "Either the file moved and the comment is now false, or the token "
            "names something outside this tree and belongs in ALLOWED with a "
            "reason." % (len(broken), "\n".join(broken)))

    # ------------------------------------------------------------- arm 3

    def test_no_allowance_is_stale(self):
        """An allowance for a token nobody cites is dead weight.

        It costs nothing today and silently absorbs a real defect tomorrow, the
        moment some other comment happens to spell the same name.
        """
        found = scan(self.pairs, self.index)

        for token, reason in sorted(ALLOWED.items()):
            with self.subTest(token=token):
                self.assertIn(
                    token, found,
                    "%r is declared in ALLOWED (%r) but no kext source cites "
                    "it any more. Delete the entry rather than leaving it to "
                    "excuse a future citation with the same name."
                    % (token, reason))
                verdict = found[token][0]
                self.assertNotIn(
                    verdict, GOOD,
                    "%r resolves to %s now, so it no longer needs an "
                    "allowance. Remove it from ALLOWED and let the gate check "
                    "it like everything else." % (token, verdict))

    # ------------------------------------------------------------- arm 4

    def test_a_broken_citation_is_detected(self):
        """Non-vacuity. Arms 1-3 passing prove the scan RAN; they do not prove
        it would redden on the defect it exists to catch.

        The mutation is applied to an in-memory copy. Nothing under
        phase0-kext/ is written -- a gate that must corrupt its subject to
        prove it works is not a gate.
        """
        # Measured 2026-08-16: this token occurs exactly once across all 14
        # sources. `tools/parse-mmiotrace.py` was the original anchor and had to
        # be replaced -- it occurs once in the kext half but TWICE once
        # tests/*.cpp is in scope, which would trip this arm's own
        # exactly-once assertion. That assertion earning its keep on the first
        # scope change is the whole reason it is here.
        anchor = "../phase0-kext/NVFalcon.hpp"
        bogus = "../phase0-kext/NVFalcon-does-not-exist.hpp"

        total = sum(text.count(anchor) for _rel, text in self.pairs)
        self.assertEqual(
            1, total,
            "the mutation anchor %r occurs %d time(s) across the kext "
            "sources; it must occur exactly once or a replace that matches "
            "nothing prints a confident green while testing nothing."
            % (anchor, total))

        clean = scan(self.pairs, self.index)
        self.assertIn(anchor, clean,
                      "%r is not extracted as a token on a clean tree, so "
                      "mutating it proves nothing" % anchor)
        self.assertIn(
            clean[anchor][0], GOOD,
            "%r does not resolve on a clean tree (%s), so this arm cannot "
            "tell a mutation from the status quo" % (anchor, clean[anchor][0]))

        mutated = [(rel, text.replace(anchor, bogus))
                   for rel, text in self.pairs]
        self.assertNotEqual(self.pairs, mutated,
                            "the mutation applied to nothing")

        after = scan(mutated, self.index)
        self.assertIn(bogus, after,
                      "the mutated token was not extracted, so the tokenizer "
                      "rather than the resolver is what this arm measured")
        self.assertNotIn(
            after[bogus][0], GOOD,
            "a citation of %r -- a file this tree does not contain -- resolved "
            "to %s. The resolver is matching something it should not, and "
            "arm 2's clean result says nothing about broken citations."
            % (bogus, after[bogus][0]))

    # ------------------------------------------------------------- arm 5

    def test_the_capture_citation_resolves(self):
        """Named specifically because it is the one that matters most.

        phase0-kext/IntelDisplay.hpp justifies three constants against
        igpu-results/tgl-capture-2026-08-16-022556.txt -- the only artifact in
        this project produced by a register read on the target silicon, from a
        boot that cannot be repeated. If that filename ever stops resolving, a
        generic "1 citation broken" line under-reports what happened.
        """
        found = scan(self.pairs, self.index)
        cited = [t for t in found if "tgl-capture-" in t]

        self.assertTrue(
            cited,
            "no kext source cites the Tiger Lake capture any more. Three "
            "constants in IntelDisplay.hpp rest on it; losing the citation "
            "loses the evidence trail even though the file is still on disk.")

        for token in sorted(cited):
            with self.subTest(token=token):
                verdict, resolved, rel = found[token]
                self.assertIn(
                    verdict, GOOD,
                    "%s cites %r and it resolves to %s. That artifact is the "
                    "only register read ever taken on the target silicon and "
                    "there is no second run of that boot."
                    % (rel, token, verdict))
                self.assertTrue(
                    os.path.isfile(os.path.join(_ROOT, resolved)),
                    "%r resolved to %r, which is not a file"
                    % (token, resolved))


if __name__ == "__main__":
    unittest.main(verbosity=2)

#!/usr/bin/env python3
"""
check-documented-paths.py -- assert that every file and directory path named in
CLAUDE.md resolves on disk, and that anything exempt is exempt for a reason
read off the filesystem rather than remembered.

Why this exists
---------------
CLAUDE.md is the first thing read on resume, and nothing checks a word of it.
It names five headers, four gate commands, seven handoff files, a layout table
of six directories, and a make target. Rename any one of them and the document
keeps its confident tone: the reader is sent to a path that no longer exists
and concludes the repository is broken rather than the prose.

That failure is silent in the way this project has already been bitten by twice
-- a hand-written list of clang lines that parsed 2 of 5 headers while printing
success, and a comment that spelled out a derived bit width as fact. Prose
carrying a path is the same shape of hole one level up: the path is load-bearing
for a human, and no gate reads it.

What this proves
----------------
For every path-shaped token in CLAUDE.md, one of:

  DIRECT  the token resolves relative to the repository root.
  TAIL    the token is a bare basename (``NVDecode.hpp``) that resolves to
          exactly one file in the repository. Ambiguous basenames FAIL -- if
          two files share a name, prose naming only the basename is not a
          reference, it is a coin flip.
  BUILD   the token names a build product, and the build file that produces it
          still declares it. ``build/NV3050Probe.kext`` is checked against
          ``BUILD_DIR``/``KEXT_BUNDLE`` in phase0-kext/Makefile; ``.exe``
          tokens are checked against ``add_kext_test`` in tests/CMakeLists.txt.
          Neither exists on a bare checkout, so requiring them on disk would
          make this gate fail for the wrong reason.
  ABSENT  the prose CLAIMS the path does not exist, and it does not. CLAUDE.md
          says ``tests/`` has no ``__init__.py``; that sentence is only true
          while tests/__init__.py is missing. This inverts the usual check --
          the token FAILS if the file appears.
  OUTSIDE the token is explicitly outside the repository (a ``site-packages``
          path). Its existence is a property of this machine's Python
          installation, which no repository gate can assert.
  GLOB    the token is a pattern (``tests/*.cpp``), so the checkable claim is
          that it still matches at least one file. A pattern matching nothing
          FAILS -- ``tests/*.cpp compiles the identical header files`` is a
          false sentence in a repository with no such files.
  SYSTEM  the token names a host executable, not a repository file. CLAUDE.md
          explains a real failure mode of ``cmd.exe``; the checkable claim is
          that the program is on PATH. This is the weakest verdict here -- it
          describes the machine, so a checkout on a host without that program
          reports FAIL for an environmental reason rather than a stale path.

Gate commands get the stricter treatment. A token inside a fenced code block
naming ``tools/*.py`` or ``tests/*.bat`` must resolve as a real file at exactly
that path -- a tail match is not good enough for something a reader is told to
run, because ``python tools/check-registers.py`` fails if the file moved even
when some other copy of the basename still exists.

Relationship to tests/test_documented_paths.py
----------------------------------------------
That module landed the same afternoon as this one, from another terminal, and
the overlap is real but not total -- so read this before deleting either.

It is WIDER: it scans five docs (CLAUDE.md plus four READMEs), and it resolves
four shapes this does not -- doc-relative, parent-relative, ``...`` elision,
and the ``NV3050Probe.hpp/.cpp`` pair shorthand.

This one is DEEPER on CLAUDE.md alone. It deliberately excludes ``.kext`` and
``.exe`` because those name build outputs absent from a clean checkout; this
admits them and asserts the PRODUCER instead, so ``build/NV3050Probe.kext``
goes red when the Makefile stops declaring a bundle and ``./test_gsp_boot.exe``
goes red when CMake stops registering the target. It also handles globs, guest
absolute paths, and host executables, and its absence check is scoped to the
clause rather than the paragraph.

The two also fail in different places -- this one from ``check-kext-syntax.bat``
(needs VS 2022), that one from ``run-python-tests.bat`` (bare checkout). Neither
subsumes the other today. If they are ever merged, the merged thing has to keep
the producer attribution and the two-sided absence check, because those are the
verdicts that were caught passing while wrong.

Reviewed 2026-08-13 after the OTHER duplication was folded away
(``tests/test_documented_paths_multidoc.py`` merged into
``tests/test_documented_paths.py``). This file was deliberately left alone: the
two differences above are real, and it is the only one of the three that runs
from the clang gate.

ONE DOC, AND THAT IS LOAD-BEARING
---------------------------------
``main()`` reads exactly ``root/CLAUDE.md``, and every resolution here is
root-relative -- there is no doc-relative concept anywhere in ``classify()``.
That is correct ONLY because the single doc sits at the repository root, where
root-relative and doc-relative are the same thing.

So pointing this at a second document in a subdirectory is not a small change.
A subdirectory doc writes paths as seen from its own directory
(``tests/README.md`` says ``../STEPS.md``), and the token ``README.md`` written
inside ``tests/README.md`` names a DIFFERENT REAL FILE under each ordering,
because both ``README.md`` and ``tests/README.md`` exist. Neither ordering
errors; the wrong one reports a path as verified while pointing at a file the
doc was not talking about. That collision was measured and settled on
2026-08-13 in favour of doc-relative-first, and
``test_documented_paths.test_doc_relative_beats_root_relative_on_a_real_collision``
is what holds it.

If a second doc is added here, port that precedence rule FIRST.
``tests/test_check_documented_paths.DocumentScope`` pins the current scope so
the assumption cannot be widened without the test going red.

What this does not prove
------------------------
That the prose around a path is TRUE. A handoff file existing says nothing
about whether it still describes the most recent state, and the layout table
pointing at a real ``tools/`` says nothing about what is in it. This closes the
narrow hole where the path itself is dead.
"""

import re
import shutil
import sys
from pathlib import Path

#
# The one document this checker reads, and the only one it MAY read. Every
# resolution below is root-relative, so a doc anywhere but the repository root
# would have its paths judged against the wrong base. See document_scope_error().
#
DOC_NAME = "CLAUDE.md"

#
# Directories that are not part of the repository's own source, and whose
# contents must not feed the basename index. `reference/` is vendored NVIDIA
# source -- thousands of files, and a basename collision in there would make an
# unrelated CLAUDE.md token ambiguous through no fault of this project.
#
SKIP_DIRS = {
    ".git",
    ".claude-crew",
    "reference",
    "build",
    "__pycache__",
    "shots",
    "node_modules",
    ".venv",
}

#
# A token counts as path-shaped if it carries a separator or a file extension
# this repository actually uses. Register names (`NV_PGSP_BASE`), hex constants
# and function names (`nvGspFalconReg()`) must not be mistaken for paths.
#
PATH_EXTS = {
    ".md",
    ".py",
    ".bat",
    ".cmd",
    ".cpp",
    ".hpp",
    ".txt",
    ".plist",
    ".kext",
    ".exe",
}

INLINE_CODE_RE = re.compile(r"`([^`\n]+)`")
FENCE_RE = re.compile(r"^```", re.MULTILINE)

# Words that turn a sentence naming a path into a claim the path is ABSENT.
ABSENCE_RE = re.compile(r"\b(no|not|has no|have no|does not|doesn't|without)\b",
                        re.IGNORECASE)

# How far before a mention a negation may sit and still be read as negating
# THAT path. The window alone is not the discriminator -- SENTENCE_BREAK_RE
# below does the real work -- so it is generous.
ABSENCE_WINDOW = 120

#
# A negation on the far side of one of these is about a different claim.
#
# This is the second correction to the same mistake, and the first one was not
# enough. An 80-character window with no sentence awareness handed ABSENT to
# `NV3050Probe.cpp`: the bullet above it ends `...include no IOKit, no kernel
# types, allocate nothing, and sleep never.` and `no kernel types` lands 53
# characters before the next bullet's mention. Adjacency in characters is not
# adjacency in meaning; a period between them settles it.
#
# The comma is in this set for a reason worth stating, because a comma looks
# like weak punctuation to exclude. handoff10's summary reads `A virtual CD does
# not, so `tools/guest-build-kext.sh` plus ...` -- the negation belongs to the
# virtual CD, one clause away, and without the comma this checker read it as a
# claim that the SCRIPT does not exist and then failed for the script existing.
# Nothing is lost: every real absence claim in this document puts the negation
# and the path in one comma-free clause (``has no `__init__.py```).
#
# The pipe is the third correction, and it was found the other way round from
# the first two. handoff11 §6 filed the table cell as a case the checker could
# not READ -- `OPERATOR-ACTION-step7-build.md`'s ``| No stale host `build/`
# rides along |``. Measured, that row already returned ABSENT, and for the wrong
# reason: a table row survives the paragraph split as ONE passage and carries no
# punctuation between cells, so before this the last break in the window was
# whatever prose preceded the table. A negation anywhere in an earlier CELL, or
# in an earlier ROW, reached every path in the rest of the table -- exactly the
# wrong-GREEN this regex exists to prevent, just spelled with pipes instead of
# character distance. The real case is unaffected because its negation and its
# path sit in the SAME cell.
SENTENCE_BREAK_RE = re.compile(r"[.!?;,—|]|\s-\s")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def is_path_shaped(token: str) -> bool:
    if "/" in token or "\\" in token:
        return True
    return Path(token).suffix.lower() in PATH_EXTS


def clean(token: str) -> str:
    """Strip the punctuation prose wraps a path in, and normalise separators.

    Order matters here and cost a debugging round: stripping ``.`` before
    handling ``./`` turns ``./test_gsp_boot.exe`` into ``/test_gsp_boot.exe``,
    an absolute path that resolves nowhere, and the checker then reports a dead
    path for a command that works. Separators are normalised first, the
    explicit relative prefix is removed second, and only then is surrounding
    punctuation stripped.
    """
    token = token.strip().replace("\\", "/")
    token = token.strip("()[]\"'`")
    while token.startswith("./"):
        token = token[2:]
    token = token.strip(",;:").rstrip(".")
    return token


def harvest(doc: str):
    """Yield (token, in_fence, line_no) for every path-shaped token in the doc.

    Inline code spans carry the prose references (the layout table, the header
    names). Fenced blocks carry the commands a reader is told to run, and those
    are held to the stricter standard, so the two are distinguished here rather
    than merged.
    """
    in_fence = False
    for line_no, line in enumerate(doc.splitlines(), start=1):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue

        if in_fence:
            body = line.split("#", 1)[0]
            candidates = body.split()
        else:
            candidates = INLINE_CODE_RE.findall(line)

        for raw in candidates:
            token = clean(raw)
            if not token or not is_path_shaped(token):
                continue
            # The trailing slash survives clean() and is removed only here,
            # AFTER the path-shape test. It is the only thing that makes a
            # documented directory look like a path at all: the layout table
            # writes `tools/` and `tests/kextshim/`, and `tools` on its own has
            # neither a separator nor a known extension, so stripping the slash
            # first silently dropped every directory in the table -- six of
            # them -- while the checker reported success on the files.
            yield token.rstrip("/") or token, in_fence, line_no


def basename_index(root: Path) -> dict:
    """Map basename -> list of repo-relative paths, skipping vendored trees."""
    index = {}
    for path in root.rglob("*"):
        rel_parts = path.relative_to(root).parts
        if any(part in SKIP_DIRS for part in rel_parts):
            continue
        if not path.is_file():
            continue
        index.setdefault(path.name, []).append("/".join(rel_parts))
    return index


def make_build_dir(root: Path) -> str:
    """The build directory phase0-kext/Makefile declares, or "" if unreadable."""
    makefile = root / "phase0-kext" / "Makefile"
    if not makefile.is_file():
        return ""
    match = re.search(r"^BUILD_DIR\s*:?=\s*(\S+)", read_text(makefile),
                      re.MULTILINE)
    return match.group(1) if match else ""


def kext_bundle_declared(root: Path) -> bool:
    """True when the Makefile still declares a .kext bundle as its product."""
    makefile = root / "phase0-kext" / "Makefile"
    if not makefile.is_file():
        return False
    return bool(re.search(r"^KEXT_BUNDLE\s*:?=.*\.kext", read_text(makefile),
                          re.MULTILINE))


def cmake_test_targets(root: Path) -> set:
    """Executable stems tests/CMakeLists.txt registers via add_kext_test()."""
    cmake = root / "tests" / "CMakeLists.txt"
    if not cmake.is_file():
        return set()
    return set(re.findall(r"add_kext_test\(\s*([A-Za-z0-9_]+)", read_text(cmake)))


def negated_before(passage: str, position: int) -> bool:
    """True when a negation governs the mention starting at ``position``.

    Looks back at most ABSENCE_WINDOW characters, then discards everything up
    to and including the LAST sentence or clause break in that window. What
    remains is the clause the mention actually sits in, and a negation has to
    be inside it to count.
    """
    window = passage[max(0, position - ABSENCE_WINDOW):position]
    breaks = list(SENTENCE_BREAK_RE.finditer(window))
    if breaks:
        window = window[breaks[-1].end():]
    return bool(ABSENCE_RE.search(window))


def claims_absence(doc: str, token: str) -> bool:
    """True when EVERY mention of the token sits directly behind a negation.

    Three things had to be got right here, and each produced a wrong verdict
    first -- the third produced a wrong GREEN, which is worse.

    Paragraphs, not lines. CLAUDE.md is hard-wrapped, so the claim ``tests/``
    has **no** ``__init__.py`` spans two source lines -- the negation sits on
    one and the token on the next. Scanning line by line finds a mention with
    no absence word beside it and reports a dead path for a file whose absence
    the document is deliberately describing.

    Named as itself, not as a tail. ``__init__.py`` is a substring of
    ``site-packages/tests/__init__.py``, and this document contains both: one
    paragraph says the first does not exist, the next says the second does. A
    plain substring test makes the second paragraph count as a mention of the
    first token, which drags a satisfied absence claim back to FAIL.

    Adjacency, not co-occurrence. This is the one that mattered. Asking only
    whether the paragraph contains a negation somewhere handed ABSENT to six
    headers that exist: the sentence introducing them reads ``They include no
    IOKit, no kernel types, allocate nothing, and sleep never``, so a paragraph
    -wide test finds ``no`` and declares ``NVDecode.hpp`` deliberately missing.
    The checker then printed exit 0 with six verdicts that could not fail --
    delete every header and it would still have passed. So the negation must
    fall within ABSENCE_WINDOW characters BEFORE the mention, which is where a
    negation about the path itself actually sits (``has no `__init__.py```).
    """
    pattern = re.compile(r"(?<![\w/\\.-])" + re.escape(token).replace(r"/", r"[/\\]"))
    mentions = 0
    for block in re.split(r"\n\s*\n", doc):
        passage = " ".join(block.split())
        for match in pattern.finditer(passage):
            mentions += 1
            if not negated_before(passage, match.start()):
                return False
    return mentions > 0


def classify(token, in_fence, root, doc, index, build_dir, kext_ok, targets):
    """Return (verdict, detail) for one token. verdict "FAIL" means broken."""
    segments = token.split("/")

    # A site-packages path is a fact about this machine's Python install.
    if "site-packages" in segments:
        return "OUTSIDE", "outside the repository (Python installation)"

    # An absolute POSIX path is not a repository path and never can be. This
    # document names `/Volumes` and `/Library/Extensions` -- mount points and
    # install locations inside the macOS GUEST. Resolving them against a
    # Windows repository root asks whether a directory on another operating
    # system exists, which is not a question this gate can answer, and answering
    # it FAIL made a correct sentence about the guest look like a dead link.
    if token.startswith("/"):
        return "ABSOLUTE", "absolute path on another host (guest/system), not repo-relative"

    # A pattern cannot be resolved, but it can be required to match something.
    if "*" in token or "?" in token:
        matches = sorted(root.glob(token))
        if matches:
            return "GLOB", f"pattern matches {len(matches)} files"
        return "FAIL", (
            "is a pattern that matches no files -- the sentence describing "
            "what it holds is no longer true of this repository"
        )

    # A host executable is not a repository file; PATH is the only claim here.
    #
    # The `not in_fence` guard is the discriminator, and picking the wrong one
    # first cost a run. Guarding on `stem not in targets` instead looked
    # equivalent and was not: a binary CMake USED to register and no longer does
    # then reached this branch, was found neither in the repository nor on PATH,
    # and was reported as a missing PROGRAM. The sentence was red for a true
    # reason phrased as a false one -- a reader would go looking at their PATH
    # instead of at tests/CMakeLists.txt. The fence tells the two apart on
    # purpose: `cmd.exe` is named in prose as a property of this host, while
    # `./test_gsp_boot.exe` sits in a block the reader is told to run from
    # tests/build, so this repository owes them that binary and the
    # add_kext_test check below is the one that must answer.
    if not in_fence and "/" not in token \
            and Path(token).suffix.lower() == ".exe" and token not in index:
        if shutil.which(token):
            return "SYSTEM", "host executable found on PATH"
        return "FAIL", (
            f"named as a program, but {token} is neither a repository file nor "
            f"on PATH"
        )

    # Build products: not present on a bare checkout, so assert the producer.
    #
    # Which producer matters. `tests/build` and `phase0-kext/build` are both a
    # directory named `build`, and matching on the name alone excused the CMake
    # one by quoting the MAKEFILE's BUILD_DIR -- a green verdict citing a build
    # file that has nothing to do with that path. Rename CMAKE_BINARY_DIR and
    # the checker would have kept pointing at phase0-kext/Makefile. So the two
    # are separated and each is checked against the file that actually makes it.
    if segments[0] == "tests" and "build" in segments:
        if targets:
            return "BUILD", (
                f"CMake binary dir; tests/CMakeLists.txt registers "
                f"{len(targets)} test targets"
            )
        return "FAIL", (
            "documented as the CMake build directory, but "
            "tests/CMakeLists.txt registers no add_kext_test() targets -- "
            "nothing would ever appear there"
        )

    if build_dir and build_dir in segments:
        if token.endswith(".kext") and not kext_ok:
            return "FAIL", (
                "names a .kext under the declared build dir, but "
                "phase0-kext/Makefile no longer declares KEXT_BUNDLE as a "
                ".kext -- the documented make product is stale"
            )
        return "BUILD", (
            f"make product under phase0-kext/Makefile BUILD_DIR={build_dir}"
        )

    if token.endswith(".exe"):
        stem = Path(token).stem
        if stem in targets:
            return "BUILD", f"executable registered by add_kext_test({stem})"
        return "FAIL", (
            f"names {token}, but tests/CMakeLists.txt registers no "
            f"add_kext_test({stem}) -- the documented binary is never built"
        )

    # An absence claim is checked BEFORE existence, because it is the one
    # verdict here that is falsified by the path being present rather than
    # missing. CLAUDE.md's `tests/` has no `__init__.py` paragraph, and the
    # whole invocation recipe that follows from it, become wrong the moment
    # that file is added. With the existence check first, adding it would have
    # returned a cheerful DIRECT and the stale instructions would have gone on
    # sending readers to a form the document says fails.
    if claims_absence(doc, token):
        # Root-relative existence is not the whole test, and taking it as the
        # whole test made this verdict one-sided in exactly the way its own
        # docstring promised it was not. The absence claim in this document
        # names a BARE basename -- ``tests/`` has no ``__init__.py`` -- so the
        # file it forbids lives at tests/__init__.py and `root / token` looks
        # for it in the wrong place. Adding the file passed, cheerfully, which
        # is the one outcome this branch exists to prevent. A claim made by
        # name has to be falsified by name, which is what the index is for.
        found = [str((root / token))] if (root / token).exists() else []
        if "/" not in token:
            found += index.get(token, [])
        if found:
            return "FAIL", (
                f"CLAUDE.md states this path does not exist, but it does "
                f"({', '.join(sorted(found))}) -- the prose built on its "
                f"absence is now wrong"
            )
        return "ABSENT", "prose claims absence, and the path is absent"

    resolved = (root / token)
    if resolved.exists():
        return "DIRECT", "resolves relative to repository root"

    # A gate command must resolve at exactly the path printed, never by tail.
    # This precedes the absence check on purpose: a command in a fenced block
    # is an instruction, and no amount of surrounding prose excuses it.
    if in_fence and re.match(r"^(tools|tests)/", token):
        return "FAIL", (
            "documented as a command to run, but no file exists at that "
            "exact path -- a reader following CLAUDE.md gets an error"
        )

    # Resolution by unique basename comes BEFORE the absence check, and the
    # order is the whole difference between this gate working and this gate
    # being decorative. Bare basenames (`NVDecode.hpp`) never satisfy DIRECT,
    # so with the absence check first they reached it while still resolving
    # perfectly well one directory down -- and the paragraph introducing them
    # contains a negation about something else. Six existing headers were
    # reported ABSENT at exit 0. Existence has to be settled before absence is
    # entertained at all.
    if "/" not in token:
        hits = index.get(token, [])
        if len(hits) == 1:
            return "TAIL", f"unique basename -> {hits[0]}"
        if len(hits) > 1:
            return "FAIL", (
                f"basename matches {len(hits)} files ({', '.join(sorted(hits))}) "
                f"-- prose naming only the basename does not identify one"
            )

    # Nothing on disk answers to this token. Only now does it matter whether
    # the document was claiming its absence deliberately.
    if claims_absence(doc, token):
        return "ABSENT", "prose claims absence, and the path is absent"

    return "FAIL", "does not exist, and no producer or absence claim explains it"


def document_scope_error(root: Path, doc_path: Path):
    """Why this document may not be read, or None if it may.

    Every resolution in ``classify()`` is ROOT-relative; there is no
    doc-relative concept anywhere in this file. That is correct only while the
    document sits at the repository root, where root-relative and doc-relative
    are the same thing. A subdirectory doc writes ``../PLAN.md`` and means the
    root -- this checker would resolve that against the root and call a correct
    reference broken, or worse, resolve a doc-relative ``tools/x.py`` to a
    root-level file that happens to share the name and call it fine.

    Until now that was a property of how ``main()`` happened to be written, not
    a property anything enforced. Pointing the tool at a second document is a
    one-line change, and nothing would have objected. This makes the constraint
    a refusal with a reason, so the next person meets the design decision
    instead of a wrong verdict. Multi-doc resolution already exists in
    ``tests/test_documented_paths.py`` -- add docs there, not here.
    """
    if doc_path.name != DOC_NAME:
        return (f"this checker reads {DOC_NAME} only, not {doc_path.name} -- "
                f"its verdicts (producer attribution, clause-scoped absence) "
                f"are written against that document's prose")
    if doc_path.parent != root:
        return (f"{doc_path} is not at the repository root ({root}) -- every "
                f"resolution here is root-relative, so a subdirectory doc's "
                f"'../PLAN.md' would be judged against the wrong base; use "
                f"tests/test_documented_paths.py for docs below the root")
    return None


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else \
        Path(__file__).resolve().parent.parent

    doc_path = (Path(sys.argv[2]).resolve() if len(sys.argv) > 2
                else root / DOC_NAME)

    scope_error = document_scope_error(root, doc_path)
    if scope_error is not None:
        print(f"FAIL: {scope_error}")
        return 1

    if not doc_path.is_file():
        print(f"FAIL: {doc_path} does not exist -- nothing to check")
        return 1

    doc = read_text(doc_path)
    index = basename_index(root)
    build_dir = make_build_dir(root)
    kext_ok = kext_bundle_declared(root)
    targets = cmake_test_targets(root)

    # Deduplicate, but keep fenced occurrences -- they are checked harder, so a
    # token appearing both in prose and in a command must satisfy the command
    # rule rather than sliding through on the looser prose one.
    seen = {}
    for token, in_fence, line_no in harvest(doc):
        if token not in seen or (in_fence and not seen[token][0]):
            seen[token] = (in_fence, line_no)

    failures = []
    tally = {}
    for token in sorted(seen):
        in_fence, line_no = seen[token]
        verdict, detail = classify(token, in_fence, root, doc, index,
                                   build_dir, kext_ok, targets)
        tally[verdict] = tally.get(verdict, 0) + 1
        if verdict == "FAIL":
            failures.append(f"CLAUDE.md:{line_no}: {token}: {detail}")
        else:
            print(f"  {verdict:<8} {token}")

    if not seen:
        print("FAIL: no path-shaped tokens found in CLAUDE.md -- extraction "
              "is broken, not the document")
        return 1

    if failures:
        for line in failures:
            print(f"FAIL: {line}")
        print(f"check-documented-paths: {len(failures)} of {len(seen)} "
              f"documented paths are broken")
        return 1

    summary = ", ".join(f"{count} {verdict}" for verdict, count in sorted(tally.items()))
    print(f"check-documented-paths: {len(seen)} documented paths all account "
          f"for ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

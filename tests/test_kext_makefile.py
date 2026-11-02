#!/usr/bin/env python3
"""test_kext_makefile.py -- the kext link line, checked without a linker.

Why this exists
---------------
``phase0-kext/Makefile`` is the single most consequential unexercised file in
this repository. Every other artifact has a host-side gate: the decode headers
compile and run under MSVC, the IOKit shell is parsed by clang's front end, the
register constants are diffed against NVIDIA's headers. The Makefile has run
zero times, on any machine, ever -- and it is what STEP 7 puts in front of Apple
clang on the operator's single attempt.

On 2026-08-13 it was read closely for the first time and had a real defect:

    -Xlinker -kext \\
    -Xlinker -object_path_lto \\
    -lkmod \\

``-Xlinker`` passes exactly one token to ld. ld64 splits its flags into two
classes that are indistinguishable by shape -- ``-kext`` takes no argument,
``-object_path_lto`` takes a path read from the next argv entry. So ld saw
``-object_path_lto -lkmod``, took ``-lkmod`` as the LTO object path, and
**libkmod.a was never linked**. That library supplies the ``kmod_info``
start/stop glue; without it a kext either fails to link or links into something
the kernel will not load. The flag was dead weight besides, since no ``-flto``
appears anywhere in ``CXXFLAGS``.

What this module can and cannot claim
-------------------------------------
It cannot claim the kext builds. There is no Apple clang here and no ld64. What
it can do is assert **properties of the command line** that are true or false by
inspection, and the ``-object_path_lto`` bug was exactly that shape: not a
question about the kernel, the SDK, or the hardware, but about whether a flag
that needs an argument was given one.

The generalisation is the point. A test that only banned ``-object_path_lto``
would be a patch for one line. ``ARG_TAKING_LD_FLAGS`` lists the ld64 flags that
read a following argv entry, and the rule is applied to every one of them: if it
appears, it must be followed by a value, and when routed through ``-Xlinker`` it
needs a **second** ``-Xlinker`` to carry that value. Re-adding any of them bare
fails here rather than in the guest.

The negative controls are in ``LinkFlagRuleTests``, which drives the parser
against synthetic flag strings rather than the real file. Without them, every
assertion below would be one that has only ever seen a correct input -- and this
project has been bitten by that repeatedly. Each control is a separating input:
it must go red under the wrong reading and green under the right one.
"""

import os
import re
import unittest

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_TESTS_DIR)
MAKEFILE = os.path.join(_REPO, "phase0-kext", "Makefile")

# ld64 flags that consume the NEXT argv entry as their value. Not exhaustive --
# ld64 has dozens -- but every one here is plausible in a kext link line, and
# the rule generalises to any that get added.
#
# The list is the load-bearing part of this module. `-object_path_lto` was the
# one that was actually wrong; the others are here so that fixing it does not
# leave a gate that only knows about the bug already found.
ARG_TAKING_LD_FLAGS = (
    "-object_path_lto",
    "-exported_symbols_list",
    "-unexported_symbols_list",
    "-order_file",
    "-map",
    "-install_name",
    "-filelist",
    "-bundle_loader",
    "-final_output",
    "-rpath",
    "-syslibroot",
    "-init",
    "-umbrella",
    "-allowable_client",
    "-u",
)

# Flags a kext link line legitimately carries that take NO argument. Listed so a
# test can prove the parser distinguishes the two classes rather than flagging
# everything, which would be a gate that fires on correct input.
NO_ARG_LD_FLAGS = ("-kext", "-dead_strip", "-warn_commons")


def read_makefile():
    with open(MAKEFILE, "r", encoding="utf-8") as handle:
        return handle.read()


def assignment(text, name):
    """The right-hand side of ``NAME := ...``, with backslash continuations
    joined and comment lines dropped.

    Comments must go before joining, not after: a ``#`` line sitting between two
    continued lines is not part of the value, and the Makefile now carries a long
    comment block immediately above ``LDFLAGS`` explaining the bug this module
    exists for. Folding that prose into the flag string would make every
    ``assertNotIn`` below fail on the explanation of why the flag is absent --
    the same defect that hit ``test_build_step7_payload.py``'s docstring checks.
    """
    lines = []
    collecting = False
    for raw in text.splitlines():
        stripped = raw.strip()
        if not collecting:
            match = re.match(r"^%s\s*:?\+?=\s*(.*)$" % re.escape(name), stripped)
            if not match:
                continue
            collecting = True
            body = match.group(1)
        else:
            if stripped.startswith("#"):
                continue
            body = stripped
        if body.endswith("\\"):
            lines.append(body[:-1])
        else:
            lines.append(body)
            break
    if not collecting:
        raise AssertionError("%s is not assigned in the Makefile" % name)
    return " ".join(part.strip() for part in lines if part.strip())


def code_only(text):
    """``text`` with whole-line ``#`` comments removed.

    Needed because this Makefile now *documents* the flags it deliberately does
    not use. The comment above ``LDFLAGS`` explains that ``-object_path_lto``
    was removed partly because no ``-flto`` appears anywhere -- and a naive
    ``assertNotIn("-flto", text)`` therefore fails on the sentence explaining
    the flag's absence. That is a defect in the test, not in the Makefile, and
    it is the second time this exact shape has appeared today:
    ``test_build_step7_payload.py``'s shape checks failed the same way against
    the tool's own docstring.

    Whole lines only, and no attempt to strip trailing ``#`` from a recipe line.
    A ``#`` inside a shell command in a recipe is not a make comment, and a
    parser clever enough to sometimes be right here would be silently wrong the
    rest of the time. ``CodeOnlyTests`` pins both halves: prose gone, flags kept.
    """
    return "\n".join(line for line in text.splitlines()
                     if not line.strip().startswith("#"))


def tokens(flag_string):
    return flag_string.split()


def bare_arg_taking_flags(flag_string):
    """Flags from ``ARG_TAKING_LD_FLAGS`` that were given no value.

    Two shapes count as bare:

    * the flag is the last token, so there is nothing to consume;
    * the next token starts with ``-``, so ld would eat a *different flag* as
      the value. That is the shape the real bug had, and it is the dangerous
      one, because the link does not fail -- it silently drops a library.

    Routed through ``-Xlinker``, the value needs its own ``-Xlinker`` in front of
    it, because the driver passes one token per occurrence. So
    ``-Xlinker -object_path_lto -Xlinker /tmp/lto.o`` is correct and
    ``-Xlinker -object_path_lto /tmp/lto.o`` is not: the path would go to the
    compiler driver rather than to ld.
    """
    found = []
    parts = tokens(flag_string)
    for i, token in enumerate(parts):
        if token not in ARG_TAKING_LD_FLAGS:
            continue
        via_xlinker = i > 0 and parts[i - 1] == "-Xlinker"
        if i + 1 >= len(parts):
            found.append(token)
            continue
        if via_xlinker:
            # Needs -Xlinker <value>: two more tokens, the first being -Xlinker.
            if parts[i + 1] != "-Xlinker" or i + 2 >= len(parts):
                found.append(token)
            elif parts[i + 2].startswith("-"):
                found.append(token)
        elif parts[i + 1].startswith("-"):
            found.append(token)
    return found


class LinkFlagRuleTests(unittest.TestCase):
    """The parser, driven against synthetic input.

    These are the negative controls. ``LinkLineTests`` below asserts the real
    Makefile is clean -- and a check that has only ever seen a clean input is
    indistinguishable from one whose comparison does nothing.
    """

    def test_the_real_bug_is_caught(self):
        # Verbatim the line that shipped, minus the parts that do not matter.
        bad = "-nostdlib -Xlinker -kext -Xlinker -object_path_lto -lkmod -lcc_kext"
        self.assertEqual(bare_arg_taking_flags(bad), ["-object_path_lto"])

    def test_the_fix_is_accepted(self):
        good = "-nostdlib -Xlinker -kext -lkmod -lcc_kext"
        self.assertEqual(bare_arg_taking_flags(good), [])

    def test_a_properly_paired_xlinker_value_is_accepted(self):
        # The far side of the rule. If this went red, the only way to satisfy
        # the gate would be to never use these flags, which is a ban rather
        # than a check.
        ok = "-Xlinker -object_path_lto -Xlinker /tmp/lto.o -lkmod"
        self.assertEqual(bare_arg_taking_flags(ok), [])

    def test_a_single_xlinker_before_the_value_is_still_caught(self):
        # The subtle near-miss: the path is present but goes to the compiler
        # driver, not to ld, because -Xlinker passes only one token.
        near = "-Xlinker -object_path_lto /tmp/lto.o -lkmod"
        self.assertEqual(bare_arg_taking_flags(near), ["-object_path_lto"])

    def test_a_trailing_arg_taking_flag_is_caught(self):
        self.assertEqual(bare_arg_taking_flags("-nostdlib -map"), ["-map"])

    def test_no_arg_flags_are_not_flagged(self):
        # Proves the parser distinguishes the two classes. A rule that fired on
        # every flag would catch the real bug and also condemn `-kext`, which
        # every kext needs -- so it would have to be switched off.
        for flag in NO_ARG_LD_FLAGS:
            line = "-nostdlib -Xlinker %s -lkmod" % flag
            self.assertEqual(bare_arg_taking_flags(line), [], flag)

    def test_every_listed_flag_is_actually_checked(self):
        # Guards the table itself. An entry that the parser never consults would
        # sit there looking like coverage.
        for flag in ARG_TAKING_LD_FLAGS:
            self.assertEqual(bare_arg_taking_flags("-nostdlib %s" % flag),
                             [flag], flag)

    def test_two_bare_flags_are_both_reported(self):
        # A parser that returned on the first hit would hide the second, and the
        # operator gets one attempt per report.
        line = "-map -lkmod -Xlinker -order_file"
        self.assertEqual(sorted(bare_arg_taking_flags(line)),
                         ["-map", "-order_file"])


class AssignmentParserTests(unittest.TestCase):
    """``assignment()`` has to survive the Makefile's real formatting."""

    def test_it_joins_backslash_continuations(self):
        text = "FOO := a \\\n\tb \\\n\tc\nBAR := z\n"
        self.assertEqual(assignment(text, "FOO"), "a b c")

    def test_it_stops_at_the_end_of_the_assignment(self):
        # Without this it would swallow BAR and every later flag test would be
        # measuring the wrong string.
        text = "FOO := a \\\n\tb\nBAR := should-not-appear\n"
        self.assertNotIn("should-not-appear", assignment(text, "FOO"))

    def test_it_drops_comment_lines_inside_a_continuation(self):
        text = "FOO := a \\\n# an explanation of why -map is absent\n\tb\n"
        self.assertEqual(assignment(text, "FOO"), "a b")

    def test_a_missing_assignment_raises_rather_than_returning_empty(self):
        # An empty string would pass every assertNotIn below and fail no
        # assertIn that used a default -- a rename of LDFLAGS would silently
        # gut this module.
        with self.assertRaises(AssertionError):
            assignment("FOO := a\n", "LDFLAGS")

    def test_it_handles_plain_equals_and_append(self):
        self.assertEqual(assignment("FOO = a b\n", "FOO"), "a b")
        self.assertEqual(assignment("FOO += a b\n", "FOO"), "a b")


class CodeOnlyTests(unittest.TestCase):
    """The guard on ``code_only()``.

    If the strip silently returned the whole file, ``test_lto_is_not_requested``
    would go red on prose forever and someone would delete the test. If it
    over-cut and returned nothing, that test would pass vacuously and the flag
    could come back unnoticed. Both directions are pinned.
    """

    def setUp(self):
        self.text = read_makefile()
        self.code = code_only(self.text)

    def test_it_removes_the_prose(self):
        self.assertIn("-object_path_lto", self.text)
        self.assertNotIn("-object_path_lto", self.code,
                         "the flag survives only in a comment; if it appears in"
                         " code_only() output it is back on the real link line")

    def test_it_keeps_the_flags(self):
        for flag in ("-lkmod", "-lcc_kext", "-mkernel", "-fapple-kext",
                     "LDFLAGS", "CXXFLAGS"):
            self.assertIn(flag, self.code, flag)

    def test_it_keeps_recipe_lines_that_contain_a_hash(self):
        # A tab-indented recipe line is never a make comment, and cutting one
        # would quietly shrink what every other check reads.
        sample = "FOO := a\n# a comment\nbar:\n\techo '#notacomment'\n"
        stripped = code_only(sample)
        self.assertIn("#notacomment", stripped)
        self.assertNotIn("a comment", stripped)


class LinkLineTests(unittest.TestCase):
    """The real ``LDFLAGS``."""

    def setUp(self):
        self.text = read_makefile()
        self.code = code_only(self.text)
        self.ldflags = assignment(self.text, "LDFLAGS")

    def test_no_argument_taking_flag_is_left_bare(self):
        bare = bare_arg_taking_flags(self.ldflags)
        self.assertEqual(
            bare, [],
            "%s given no value; ld would consume the following flag as its "
            "argument. LDFLAGS = %s" % (", ".join(bare), self.ldflags))

    def test_libkmod_is_linked(self):
        # The library the -object_path_lto bug silently ate. It supplies
        # kmod_info and the start/stop glue; without it the bundle is not a kext
        # the kernel can load.
        self.assertIn("-lkmod", tokens(self.ldflags))

    def test_libcc_kext_is_linked(self):
        # The kext-safe compiler runtime. -nostdlib removes the normal one.
        self.assertIn("-lcc_kext", tokens(self.ldflags))

    def test_the_kext_link_mode_is_requested(self):
        parts = tokens(self.ldflags)
        self.assertIn("-kext", parts)
        # ...and it must be routed through -Xlinker. A bare -kext is a clang
        # driver flag clang does not have, so it would be rejected outright --
        # a loud failure rather than a silent one, but still a burned attempt.
        self.assertEqual(parts[parts.index("-kext") - 1], "-Xlinker")

    def test_the_standard_library_is_excluded(self):
        # Kernel code has no libc and no C++ runtime. Linking them produces
        # undefined symbols at load time rather than at link time.
        self.assertIn("-nostdlib", tokens(self.ldflags))

    def test_lto_is_not_requested_anywhere(self):
        # The justification for deleting -object_path_lto rather than giving it
        # a path. If someone adds -flto later, that flag becomes meaningful
        # again and this test tells them to reconsider it deliberately.
        #
        # Comment lines are excluded, because the Makefile's own explanation of
        # why the LTO flag was removed necessarily contains the string.
        self.assertNotIn("-flto", self.code)

    def test_the_sysroot_is_passed_to_the_link_too(self):
        # Compiling against the SDK and linking against the host's default is a
        # mismatch that surfaces as missing symbols, not as a path error.
        self.assertIn("-isysroot", tokens(self.ldflags))


class CompileFlagTests(unittest.TestCase):
    """``CXXFLAGS`` and ``KERNEL_FLAGS``.

    The Makefile's own comment says omitting any of these "produces a kext that
    links but fails to load with undefined symbols" -- a failure mode that costs
    a whole operator round trip to observe.
    """

    def setUp(self):
        self.text = read_makefile()
        self.kernel = tokens(assignment(self.text, "KERNEL_FLAGS"))
        self.cxx = assignment(self.text, "CXXFLAGS")

    def test_the_kernel_dialect_flags_are_present(self):
        for flag in ("-mkernel", "-fapple-kext", "-fno-builtin", "-fno-common",
                     "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
                     "-nostdinc", "-nostdinc++"):
            self.assertIn(flag, self.kernel, flag)

    def test_kernel_is_defined(self):
        self.assertIn("-DKERNEL", self.kernel)

    def test_the_kernel_flags_reach_the_compile_line(self):
        # KERNEL_FLAGS existing is not the same as it being used. A refactor
        # that dropped the reference would leave every check above green while
        # compiling the kext as ordinary userspace C++.
        self.assertIn("$(KERNEL_FLAGS)", self.cxx)

    def test_the_kernel_framework_headers_are_on_the_include_path(self):
        includes = assignment(self.text, "INCLUDES")
        self.assertIn("Kernel.framework/Headers", includes)
        self.assertIn("Kernel.framework/PrivateHeaders", includes)
        self.assertIn("$(INCLUDES)", self.cxx)

    def test_the_sdk_is_discovered_not_hardcoded(self):
        # A pinned /Applications/Xcode.app/... path is wrong on any machine but
        # the one it was written on, and the guest is not that machine.
        sdk_line = [line for line in self.text.splitlines()
                    if line.startswith("SDK")][0]
        self.assertIn("xcrun", sdk_line)
        self.assertNotIn("/Applications/Xcode", sdk_line)

    def test_the_compiler_is_discovered_not_hardcoded(self):
        cxx_line = [line for line in self.text.splitlines()
                    if line.startswith("CXX ") or line.startswith("CXX:")
                    or line.startswith("CXX=")][0]
        self.assertIn("xcrun", cxx_line)


class SourceListTests(unittest.TestCase):
    """What actually gets compiled."""

    def setUp(self):
        self.text = read_makefile()
        self.sources = tokens(assignment(self.text, "SOURCES"))

    def test_no_header_is_compiled(self):
        # The whole architecture rests on the headers being header-only. A .hpp
        # appearing here would mean the split had quietly stopped being real.
        #
        # This used to read assertEqual(self.sources, ["NV3050Probe.cpp"]),
        # which is a stricter claim than the comment above it describes -- it
        # pinned the COUNT of shells, not the property that no header is
        # compiled. It went red when the Intel track's IOKit shell was added,
        # which was correct behaviour by the Makefile and a stale expectation by
        # this test. The count is pinned against the directory below instead,
        # where it belongs, because that is a claim the tree can settle.
        self.assertTrue(self.sources, "SOURCES is empty; nothing would compile")
        for name in self.sources:
            self.assertTrue(name.endswith(".cpp"), name)

    def test_every_compiled_source_is_an_iokit_shell(self):
        # The far side of the same rule. "Ends in .cpp" would still admit a
        # decode translation unit, which is what the split exists to prevent:
        # a .cpp of pure logic is a file tests/ can no longer compile under
        # MSVC. Every shell registers its metaclass, and nothing else does.
        #
        # One IOKit class may be spread over several translation units.
        # IntelGtBringup registers its metaclass in IntelGtBringup.cpp, and
        # IntelGtBringupSurface.cpp, IntelGtPersistentSurfaces.cpp and
        # IntelGtFill.cpp define further IntelGtBringup:: members out of line.
        # Those files are still shell code, so they pass, but only on the
        # strict terms in shell_source_problems(): a file that registers no
        # metaclass must define at least one member, and every class it
        # defines members of must be registered by a SOURCES file. A
        # pure-logic TU defines no members and stays red; so does a member of
        # a class nothing registers. ShellSourceRuleTests pins both.
        texts = {}
        for name in self.sources:
            path = os.path.join(_REPO, "phase0-kext", name)
            with open(path, encoding="utf-8", errors="replace") as handle:
                texts[name] = handle.read()
        problems = shell_source_problems(texts)
        self.assertEqual(problems, [], "\n".join(problems))

    def test_every_named_source_exists(self):
        for name in self.sources:
            self.assertTrue(
                os.path.isfile(os.path.join(_REPO, "phase0-kext", name)), name)

    def test_every_cpp_in_the_kext_directory_is_named(self):
        # The far side. A second .cpp added and not listed compiles nowhere and
        # links nowhere, and the symptom is an undefined symbol at load time
        # rather than a build error.
        kext_dir = os.path.join(_REPO, "phase0-kext")
        on_disk = sorted(n for n in os.listdir(kext_dir) if n.endswith(".cpp"))
        self.assertEqual(sorted(self.sources), on_disk)


_REGISTERS = re.compile(r"\bOSDefineMetaClassAndStructors\s*\(\s*(\w+)")

# An out-of-line member definition: "Class::member(" at the very start of a
# line, optionally after a return type ("IOReturn ", "const char *"). The line
# must start with an identifier, so an indented call such as
# "    return IOService::start(provider);" is never read as a definition, and the
# return-type part cannot contain ':', so "A::B::c(" is not read as a member
# of B.
_MEMBER_DEFINITION = re.compile(
    r"^(?:[A-Za-z_][\w \t<>*&,]*?[ \t*&])?(\w+)::~?\w+\s*\(", re.M)


def shell_source_problems(texts):
    """Every reason a SOURCES file is not IOKit shell code, as sorted strings.

    ``texts`` maps each SOURCES name to its contents. A file passes if it
    registers a metaclass with OSDefineMetaClassAndStructors, or if it defines
    at least one out-of-line member and every class it defines members of is
    registered by some file in ``texts``. An empty list means every file passes.
    """
    registered = set()
    for text in texts.values():
        registered.update(_REGISTERS.findall(text))
    problems = []
    for name in sorted(texts):
        text = texts[name]
        if _REGISTERS.search(text):
            continue
        owners = sorted(set(_MEMBER_DEFINITION.findall(text)))
        if not owners:
            problems.append(
                "%s registers no metaclass and defines no class member: a "
                "pure-logic translation unit" % name)
            continue
        for owner in owners:
            if owner not in registered:
                problems.append(
                    "%s defines members of %s, which no SOURCES file "
                    "registers" % (name, owner))
    return problems


class ShellSourceRuleTests(unittest.TestCase):
    """Negative controls for shell_source_problems(), so the split-TU allowance
    cannot quietly turn into a pass for everything."""

    SHELL = "OSDefineMetaClassAndStructors(Owner, IOService);\n"

    def test_a_registering_shell_passes(self):
        self.assertEqual(shell_source_problems({"Owner.cpp": self.SHELL}), [])

    def test_a_member_of_a_registered_class_passes(self):
        split = ("IOReturn Owner::extra(UInt32 value)\n{\n"
                 "    return IOService::start(nullptr);\n}\n")
        self.assertEqual(
            shell_source_problems({"Owner.cpp": self.SHELL, "Owner2.cpp": split}),
            [])

    def test_a_pure_logic_translation_unit_stays_red(self):
        pure = ("namespace {\nbool fits(unsigned v) { return v != 0; }\n}\n"
                "bool gtEncodeFill32(unsigned *b) {\n    return fits(*b);\n}\n")
        problems = shell_source_problems({"Owner.cpp": self.SHELL, "Pure.cpp": pure})
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("Pure.cpp", problems[0])
        self.assertIn("pure-logic", problems[0])

    def test_an_indented_qualified_call_is_not_a_member(self):
        # Only a definition at the start of a line counts. A file whose sole
        # "::" is a call inside a function body is still pure logic.
        calls = "bool helper(void)\n{\n    return Owner::extra(0);\n}\n"
        problems = shell_source_problems({"Owner.cpp": self.SHELL, "Calls.cpp": calls})
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("Calls.cpp", problems[0])

    def test_a_member_of_an_unregistered_class_stays_red(self):
        stray = "bool Stray::start(IOService *provider)\n{\n    return true;\n}\n"
        problems = shell_source_problems({"Owner.cpp": self.SHELL, "Stray.cpp": stray})
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("Stray.cpp", problems[0])
        self.assertIn("Stray,", problems[0])

    def test_one_unregistered_owner_among_registered_ones_stays_red(self):
        mixed = ("IOReturn Owner::extra(void)\n{\n    return 0;\n}\n"
                 "Stray::~Stray()\n{\n}\n")
        problems = shell_source_problems({"Owner.cpp": self.SHELL, "Mixed.cpp": mixed})
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("Stray", problems[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)

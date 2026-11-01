#!/usr/bin/env python3
"""KEXT-LOAD GATE 3/3: no unguarded MMIO store exists in the NVIDIA track.

A stock build is read-only because of ONE function, NV3050Probe::regWrite32(),
whose build gate only -DPHASE4_ALLOW_WRITES opens. That is a fact about the
accessor, and the sibling gate tests/test_probe_register_access_refusals.py
pins the refusals inside it. Nothing pinned the other half of the claim: that
no second store path was added beside it. This gate does.

Subjects are DERIVED, not listed. Every NV* source and header in phase0-kext
is scanned, so a new observer or staging service is audited the day it lands.
BAR pointers are derived too: any name seated from getVirtualAddress() or
getAddress() on a memory map. No pointer name is hardcoded.

Each property is a separate failure reason, so a negative control can assert
WHICH property broke:

  1. Every virtual-address fetch seats a NAMED pointer in a statement of its
     own. An address used inline never gets a name, so nothing below could
     follow it.
  2. Every use of a BAR pointer takes one of six audited shapes: its
     declaration, a re-seat (from nullptr or a map), a null test inside an
     if/while condition, the first argument of an OSRead* call, the first
     argument of an OSWrite* call, or a (void *) cast handed to a log macro.
     Anything else is reported: a dereference, a subscript, pointer
     arithmetic, an alias, a bcopy, an argument to a helper. This is an
     allowlist on purpose. A denylist of store spellings is only as good as
     the spellings somebody thought of.
  3. Every volatile pointer declared is a derived BAR pointer, or is the
     PANIC_TEST null that start() faults through on purpose. An MMIO pointer
     obtained another way would escape property 2 entirely.
  4. Exactly one OSWrite* call exists. It sits inside the guarded accessor and
     stores through a derived BAR pointer.
  5. No IOKit device store primitive (ioWrite*, memoryWrite*, writeBytes) is
     called. Each one reaches the card without a BAR pointer.
  6. Every write callback installed into a struct member (the NVGspBootIo
     write32 route that NVGspBoot.hpp drives) targets a function defined in
     these sources whose body calls the guarded accessor, and every write
     callback that is CALLED is one installed that way.

Scope limits, stated rather than implied: PCI config-space writes are not MMIO
and are not audited here. The Intel track has its own write gates. A callback
installed by aggregate initialisation rather than a field assignment is
invisible to property 6.

Design rule, carried from the sibling gate: the live tree and every mutant go
through ONE audit() function. Mutants are planted in a TEMP COPY of the
sources, so audit() reads real files from a directory in both cases.

Scanning runs over source with comments AND string-literal contents blanked.
NVLOG text names the BAR, and the accessor's own comment describes the store.
A gate that read prose would redden on documentation.
"""

import glob
import os
import re
import shutil
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KEXT = os.path.join(ROOT, 'phase0-kext')

SOURCE_GLOBS = ('NV*.cpp', 'NV*.hpp', 'NV*.h')

# The one function every store must route through, and the file it lives in.
# Its refusals are pinned by the sibling gate; this gate pins that it is alone.
ACCESSOR_FILE = 'NV3050Probe.cpp'
ACCESSOR = 'NV3050Probe::regWrite32'
ACCESSOR_SHORT = ACCESSOR.split('::')[-1]

VADDR_RE = re.compile(
    r'(?:\.|->)\s*(?:getVirtualAddress|getAddress)\s*\(\s*\)')

# A WHOLE statement that seats a named pointer from a map's virtual address,
# optionally declaring it and optionally through a C or reinterpret cast.
SEAT_RE = re.compile(
    r'^(?:(?:volatile\s+)?[\w:]+\s*\*\s*)?'
    r'(?:this\s*->\s*)?([A-Za-z_]\w*)\s*=\s*'
    r'(?:\(\s*(?:volatile\s+)?[\w:]+\s*\*\s*\)\s*'
    r'|reinterpret_cast\s*<\s*(?:volatile\s+)?[\w:]+\s*\*\s*>\s*\(\s*)?'
    r'[A-Za-z_]\w*\s*(?:->|\.)\s*(?:getVirtualAddress|getAddress)'
    r'\s*\(\s*\)\s*\)?$')

VOLATILE_DECL_RE = re.compile(r'\bvolatile\s+[\w:]+\s*\*\s*([A-Za-z_]\w*)')
STORE_CALL_RE = re.compile(r'\b_*OSWrite\w*\s*\(')
READ_OWNER_RE = re.compile(r'^_*OSRead\w*$')
WRITE_OWNER_RE = re.compile(r'^_*OSWrite\w*$')
LOG_OWNER_RE = re.compile(r'^(?:[A-Z0-9_]*LOG[A-Z0-9_]*|IOLog|kprintf)$')
DEVICE_STORE_RE = re.compile(
    r'\b(?:ioWrite|memoryWrite)(?:8|16|32|64)\s*\(|\bwriteBytes\s*\(')
INSTALL_RE = re.compile(
    r'(?:\.|->)\s*(\w*[Ww]rite(?:8|16|32|64))\s*=(?!=)\s*([^;]*);')
CALLBACK_CALL_RE = re.compile(
    r'(?:\.|->)\s*(\w*[Ww]rite(?:8|16|32|64))\s*\(')
NOT_A_CALLBACK_RE = re.compile(
    r'^(?:ioWrite|memoryWrite|configWrite)(?:8|16|32|64)$')
NULL_RE = re.compile(r'^(?:nullptr|NULL|0)$')
VOID_CAST_TAIL_RE = re.compile(r'\(\s*(?:const\s+)?void\s*\*\s*\)$')
MEMBER_TAIL_RE = re.compile(r'\b[A-Za-z_]\w*\s*->\s*$')
MEMBER_HEAD_RE = re.compile(r'^(?:[A-Za-z_]\w*\s*->\s*)+')
POINTER_TYPE_RE = re.compile(r'(?:volatile\s+)?[\w:]+\s*\*')
ACCESS_LABEL_RE = re.compile(r'\b(?:public|private|protected)\s*:')
PANIC_OPEN_RE = re.compile(
    r'^\s*#\s*(?:ifdef\s+PANIC_TEST\b|if\s+defined\s*\(?\s*PANIC_TEST\b)')
PP_IF_RE = re.compile(r'^\s*#\s*if')
PP_ELSE_RE = re.compile(r'^\s*#\s*el(?:se|if)\b')
PP_ENDIF_RE = re.compile(r'^\s*#\s*endif\b')


def read_text(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        return handle.read()


def strip_code(text):
    """Blank comments and the CONTENTS of string and character literals.

    Quotes, every newline and every offset survive, so a line number taken
    from the stripped text indexes the original file. A literal cannot span a
    line, so an unpaired apostrophe in a #warning blanks one line, not the
    rest of the file.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if ch == '/' and nxt == '/':
            while i < n and text[i] != '\n':
                out.append(' ')
                i += 1
            continue
        if ch == '/' and nxt == '*':
            out.append('  ')
            i += 2
            while i < n and not (text[i] == '*' and i + 1 < n
                                 and text[i + 1] == '/'):
                out.append('\n' if text[i] == '\n' else ' ')
                i += 1
            if i < n:
                out.append('  ')
                i += 2
            continue
        if ch == '"' or ch == "'":
            quote = ch
            out.append(ch)
            i += 1
            while i < n and text[i] != '\n' and text[i] != quote:
                if text[i] == '\\' and i + 1 < n and text[i + 1] != '\n':
                    out.append('  ')
                    i += 2
                    continue
                out.append(' ')
                i += 1
            if i < n and text[i] == quote:
                out.append(quote)
                i += 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out)


def line_of(text, index):
    return text.count('\n', 0, index) + 1


def _balanced(text, open_index, opener, closer):
    """Index of the closer matching the opener at open_index, or -1."""
    depth = 0
    i = open_index
    while i < len(text):
        if text[i] == opener:
            depth += 1
        elif text[i] == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def statement_start(code, index):
    return max(code.rfind(';', 0, index), code.rfind('{', 0, index),
               code.rfind('}', 0, index)) + 1


def statement_end(code, index):
    ends = [p for p in (code.find(';', index), code.find('{', index),
                        code.find('}', index)) if p >= 0]
    return min(ends) if ends else len(code)


def code_only(segment):
    """A statement prefix without preprocessor lines or access labels."""
    kept = [line for line in segment.split('\n')
            if not line.lstrip().startswith('#')]
    return ACCESS_LABEL_RE.sub(' ', '\n'.join(kept)).strip()


def enclosing_owner(code, start, index):
    """The name in front of the innermost parenthesis still open at index.

    None when no parenthesis is open in this statement or nothing names it,
    which is what a cast or a grouping looks like.
    """
    depth = 0
    j = index - 1
    while j >= start:
        ch = code[j]
        if ch == ')':
            depth += 1
        elif ch == '(':
            if depth == 0:
                m = re.search(r'([A-Za-z_]\w*)\s*$', code[start:j])
                return m.group(1) if m else None
            depth -= 1
        j -= 1
    return None


def first_argument(args):
    depth = 0
    for i, ch in enumerate(args):
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == ',' and depth == 0:
            return args[:i].strip()
    return args.strip()


def find_definition(code, name):
    """(open, close) brace offsets of the body of function name, or None.

    A declaration or a call is followed by ';' or an operator rather than a
    brace, and is skipped.
    """
    if '::' in name:
        pattern = r'(?<![\w:])' + re.escape(name) + r'\s*\('
    else:
        pattern = (r'(?<![\w:])(?:[A-Za-z_]\w*::)*' + re.escape(name)
                   + r'\s*\(')
    for m in re.finditer(pattern, code):
        close = _balanced(code, m.end() - 1, '(', ')')
        if close < 0:
            continue
        tail = re.match(r'\s*(?:const\s*)?(?:override\s*)?\{',
                        code[close + 1:])
        if tail is None:
            continue
        brace = close + tail.end()
        end = _balanced(code, brace, '{', '}')
        if end > 0:
            return brace, end
    return None


def panic_test_lines(code):
    """1-based line numbers inside the TRUE arm of a PANIC_TEST block."""
    inside = set()
    stack = []
    for number, line in enumerate(code.split('\n'), 1):
        if PANIC_OPEN_RE.match(line):
            stack.append(['panic', 0])
        elif PP_IF_RE.match(line):
            stack.append(['other', 0])
        elif PP_ELSE_RE.match(line):
            if stack:
                stack[-1][1] += 1
        elif PP_ENDIF_RE.match(line):
            if stack:
                stack.pop()
        elif any(kind == 'panic' and arm == 0 for kind, arm in stack):
            inside.add(number)
    return inside


def classify_use(code, index, name):
    """The audited shape one occurrence of a BAR pointer takes, or None."""
    start = statement_start(code, index)
    before = code_only(code[start:index])
    member = MEMBER_TAIL_RE.search(before)
    if member:
        before = before[:member.start()].rstrip()
    after = code[index + len(name):index + len(name) + 200].lstrip()
    owner = enclosing_owner(code, start, index)

    if (after.startswith(';') and 'volatile' in before
            and POINTER_TYPE_RE.fullmatch(before)):
        return 'decl'
    if re.match(r'=(?!=)', after) and (
            before == '' or POINTER_TYPE_RE.fullmatch(before)):
        rhs = code[index + len(name):statement_end(code, index)].strip()
        rhs = rhs[1:].strip()
        if NULL_RE.match(rhs) or VADDR_RE.search(rhs):
            return 'seat'
        return None
    if owner in ('if', 'while'):
        if (before.endswith(('!', '||', '&&', '('))
                and after.startswith(('||', '&&', ')'))):
            return 'test'
        if re.match(r'(?:==|!=)\s*(?:nullptr|NULL|0)\b', after):
            return 'test'
    if owner and before.endswith('(') and after.startswith(','):
        if READ_OWNER_RE.match(owner):
            return 'read'
        if WRITE_OWNER_RE.match(owner):
            return 'write'
    if (owner and LOG_OWNER_RE.match(owner)
            and VOID_CAST_TAIL_RE.search(before)
            and after.startswith((',', ')'))):
        return 'log'
    return None


def source_files(kext_dir):
    names = set()
    for pattern in SOURCE_GLOBS:
        for path in glob.glob(os.path.join(kext_dir, pattern)):
            names.add(os.path.basename(path))
    return sorted(names)


def survey(kext_dir):
    """(failures, census) for the NV sources in kext_dir.

    failures is a list of reason strings, empty when every property holds.
    census counts what was seen, so a positive control can prove the gate
    looked at something rather than passing over an empty tree.
    """
    failures = []
    census = {'files': 0, 'bar_pointers': [], 'decl': 0, 'seat': 0,
              'test': 0, 'read': 0, 'write': 0, 'log': 0, 'installs': 0}

    names = source_files(kext_dir)
    census['files'] = len(names)
    if ACCESSOR_FILE not in names:
        failures.append('%s: not found under %s -- the gate has no subject'
                        % (ACCESSOR_FILE, kext_dir))
        return failures, census

    lines = {}
    code = {}
    for name in names:
        text = read_text(os.path.join(kext_dir, name))
        lines[name] = text.split('\n')
        code[name] = strip_code(text)

    def where(name, index):
        number = line_of(code[name], index)
        return '%s:%d: %s' % (name, number, lines[name][number - 1].strip())

    span = find_definition(code[ACCESSOR_FILE], ACCESSOR)
    if span is None:
        failures.append('guarded accessor %s(): body not found in %s'
                        % (ACCESSOR, ACCESSOR_FILE))

    # ---- property 1: every virtual-address fetch seats a named pointer -----
    bars = set()
    for name in names:
        src = code[name]
        for m in VADDR_RE.finditer(src):
            stmt = code_only(src[statement_start(src, m.start()):
                                 statement_end(src, m.start())])
            seat = SEAT_RE.match(stmt)
            if seat is None:
                failures.append(
                    'virtual address does not seat a named BAR pointer in a '
                    'statement of its own -- an unnamed MMIO address escapes '
                    'every check below: %s' % where(name, m.start()))
            else:
                bars.add(seat.group(1))
    census['bar_pointers'] = sorted(bars)
    if not bars:
        failures.append(
            'no BAR pointer derived: nothing in the %d NV sources is seated '
            'from a map, so every use-shape check would pass over nothing'
            % len(names))

    # ---- property 2: every BAR-pointer use takes an audited shape ----------
    # ---- property 3: every volatile pointer is a BAR pointer or the null ---
    for name in names:
        src = code[name]
        for bar in sorted(bars):
            for m in re.finditer(r'\b%s\b' % re.escape(bar), src):
                shape = classify_use(src, m.start(), bar)
                if shape is None:
                    failures.append(
                        'use of BAR pointer %s matches no audited shape -- a '
                        'store through it would bypass %s(): %s'
                        % (bar, ACCESSOR_SHORT, where(name, m.start())))
                else:
                    census[shape] += 1

        panic = panic_test_lines(src)
        for m in VOLATILE_DECL_RE.finditer(src):
            pointer = m.group(1)
            if pointer in bars:
                continue
            rhs = src[m.end():statement_end(src, m.end())].strip()
            if (line_of(src, m.start()) in panic and rhs.startswith('=')
                    and NULL_RE.match(rhs[1:].strip())):
                continue
            failures.append(
                'volatile pointer %s is neither a derived BAR pointer nor the '
                'PANIC_TEST null -- an MMIO pointer obtained another way '
                'escapes the use-shape check: %s'
                % (pointer, where(name, m.start())))

    # ---- property 4: one OSWrite*, inside the accessor, into a BAR pointer -
    stores = [(name, m) for name in names
              for m in STORE_CALL_RE.finditer(code[name])]
    if len(stores) != 1:
        failures.append(
            'OSWrite* store primitive occurs %d times across the NV sources, '
            'expected 1 -- the guarded accessor is the only permitted store'
            % len(stores))
    for name, m in stores:
        if not (name == ACCESSOR_FILE and span is not None
                and span[0] < m.start() < span[1]):
            failures.append(
                'OSWrite* store outside the guarded accessor %s(): %s'
                % (ACCESSOR, where(name, m.start())))
        src = code[name]
        close = _balanced(src, m.end() - 1, '(', ')')
        target = MEMBER_HEAD_RE.sub(
            '', first_argument(src[m.end():close] if close > 0 else ''))
        if target not in bars:
            failures.append(
                'OSWrite* store target %r is not a derived BAR pointer: %s'
                % (target, where(name, m.start())))

    # ---- property 5: no IOKit device store primitive at all ----------------
    for name in names:
        for m in DEVICE_STORE_RE.finditer(code[name]):
            failures.append(
                'IOKit device store primitive reaches the card without a BAR '
                'pointer or %s(): %s' % (ACCESSOR_SHORT,
                                         where(name, m.start())))

    # ---- property 6: every write callback routes through the accessor ------
    routed = set()
    for name in names:
        src = code[name]
        for m in INSTALL_RE.finditer(src):
            member, rhs = m.group(1), m.group(2).strip()
            if NULL_RE.match(rhs):
                continue
            census['installs'] += 1
            target = re.fullmatch(r'&?\s*([A-Za-z_][\w:]*)', rhs)
            if target is None:
                failures.append(
                    'write callback %s installed from %r, which is not a '
                    'function name the gate can follow: %s'
                    % (member, rhs, where(name, m.start())))
                continue
            function = target.group(1)
            body = None
            for other in names:
                found = find_definition(code[other], function)
                if found is not None:
                    body = code[other][found[0]:found[1] + 1]
                    break
            if body is None:
                failures.append(
                    'write callback %s targets %s, which has no definition '
                    'in the NV sources: %s'
                    % (member, function, where(name, m.start())))
            elif not re.search(r'\b%s\s*\(' % re.escape(ACCESSOR_SHORT),
                               body):
                failures.append(
                    'write callback %s targets %s, which does not route '
                    'through %s(): %s' % (member, function, ACCESSOR_SHORT,
                                          where(name, m.start())))
            else:
                routed.add(member)
    if census['installs'] == 0:
        failures.append(
            'no write callback installation found -- the callback half of '
            'this gate would pass over nothing')
    for name in names:
        for m in CALLBACK_CALL_RE.finditer(code[name]):
            member = m.group(1)
            if member == ACCESSOR_SHORT or NOT_A_CALLBACK_RE.match(member):
                continue
            if member not in routed:
                failures.append(
                    'write callback %s is called but never installed by a '
                    'routed field assignment: %s'
                    % (member, where(name, m.start())))

    return failures, census


def audit(kext_dir):
    """Failure reasons for the NV sources in kext_dir; empty means clean."""
    return survey(kext_dir)[0]


BOOT0_ANCHOR = 'UInt32 boot0 = regRead32(NV_PMC_BOOT_0);'


def copy_sources(dest_root):
    """Copy every NV* source into dest_root/phase0-kext; return that dir."""
    kext = os.path.join(dest_root, 'phase0-kext')
    os.makedirs(kext)
    for name in source_files(KEXT):
        shutil.copyfile(os.path.join(KEXT, name), os.path.join(kext, name))
    return kext


def _read_exact(path):
    with open(path, 'r', encoding='utf-8', errors='surrogateescape',
              newline='') as handle:
        return handle.read()


def plant(kext_dir, name, old, new, occurrences=1):
    """Rewrite one file of the temp copy.

    The anchor must occur exactly as often as stated. A stale anchor fails
    the test loudly instead of planting nothing and letting a vacuous mutant
    pass.
    """
    path = os.path.join(kext_dir, name)
    text = _read_exact(path)
    if text.count(old) != occurrences:
        raise AssertionError(
            'plant anchor %r occurs %d times in %s, expected %d'
            % (old, text.count(old), name, occurrences))
    with open(path, 'w', encoding='utf-8', errors='surrogateescape',
              newline='') as handle:
        handle.write(text.replace(old, new))


def plant_after_boot0(kext_dir, line):
    plant(kext_dir, ACCESSOR_FILE, BOOT0_ANCHOR,
          BOOT0_ANCHOR + '\n    ' + line)


class NoUnguardedWritesTests(unittest.TestCase):

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='nv_unguarded_')
        self.kext = copy_sources(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def assertReddens(self, needles, reasons):
        """At least one reason names EVERY needle, so a control asserts which
        property broke and where, not merely that something did."""
        hits = [r for r in reasons if all(n in r for n in needles)]
        self.assertTrue(
            hits, 'expected a reason containing all of %r, got %r'
            % (needles, reasons))

    def test_live_tree_passes(self):
        """Positive control on the real tree, with a census: a pass over an
        empty subject set must not look like a pass."""
        failures, census = survey(KEXT)
        self.assertEqual(failures, [])
        self.assertTrue(census['bar_pointers'], census)
        self.assertEqual(census['write'], 1, census)
        self.assertGreater(census['read'], 0, census)
        self.assertGreater(census['decl'], 0, census)
        self.assertGreater(census['seat'], 0, census)
        self.assertGreater(census['installs'], 0, census)

    def test_untouched_copy_passes(self):
        """The baseline every control below is planted into. If the copy
        reddened by itself, the controls would measure the copy, not the
        plant."""
        self.assertEqual(audit(self.kext), [])

    def test_store_in_comment_or_string_is_not_a_store(self):
        """Prose and log text are blanked before scanning. This is what lets
        the red controls below be read as reddening on CODE."""
        plant_after_boot0(
            self.kext,
            '// *(volatile UInt32 *)(fBar0Base + 4) = 0;\n'
            '    NVLOG("*(fBar0Base + 4) = 0");')
        self.assertEqual(audit(self.kext), [])

    def test_bare_store_through_bar_pointer_is_caught(self):
        """The separating input this gate exists for: a store that never goes
        near regWrite32(), planted in the shell itself."""
        plant_after_boot0(
            self.kext,
            '*(volatile UInt32 *)(fBar0Base + NV_PMC_BOOT_0) = boot0;')
        self.assertReddens(('matches no audited shape', ACCESSOR_FILE),
                           audit(self.kext))

    def test_subscript_store_in_an_observer_is_caught(self):
        """Planted outside NV3050Probe.cpp. The subject set is derived by
        glob, and this is what proves it reaches past the shell."""
        anchor = ('first.commandHead='
                  'OSReadLittleInt32(fBar0Base,regs.commandHead);')
        plant(self.kext, 'NVGspQueueObserver.cpp', anchor,
              anchor + '\n    ((volatile UInt32 *)fBar0Base)[0]='
              'first.commandHead;')
        self.assertReddens(('matches no audited shape',
                            'NVGspQueueObserver.cpp'), audit(self.kext))

    def test_second_store_primitive_outside_the_accessor_is_caught(self):
        """An OSWrite* is an audited USE shape, so property 2 alone passes
        it. Only the count and the location check can see a second one."""
        anchor = ('const UInt32 boot0='
                  'OSReadLittleInt32(fBar0Base,NV_PMC_BOOT_0);')
        plant(self.kext, 'NVGspWprFactsObserver.cpp', anchor,
              anchor + '\n        OSWriteLittleInt32(fBar0Base,NV_PMC_BOOT_0,'
              'boot0);')
        reasons = audit(self.kext)
        self.assertReddens(('occurs 2 times',), reasons)
        self.assertReddens(('outside the guarded accessor',
                            'NVGspWprFactsObserver.cpp'), reasons)

    def test_bulk_copy_into_bar_is_caught(self):
        """(void *) on a BAR pointer is audited only as a log argument. The
        same cast handed to bcopy is a store."""
        plant_after_boot0(
            self.kext, 'bcopy(&boot0, (void *)fBar0Base, sizeof(boot0));')
        self.assertReddens(('matches no audited shape', 'bcopy'),
                           audit(self.kext))

    def test_unnamed_virtual_address_store_is_caught(self):
        """No named pointer, so property 2 never sees it. Property 1 must."""
        plant_after_boot0(
            self.kext,
            '*(volatile UInt32 *)(fBar0Map->getVirtualAddress() + '
            'NV_PMC_BOOT_0) = boot0;')
        self.assertReddens(('does not seat a named BAR pointer',),
                           audit(self.kext))

    def test_device_store_primitive_is_caught(self):
        plant_after_boot0(
            self.kext, 'fPCIDevice->memoryWrite32(NV_PMC_BOOT_0, boot0);')
        self.assertReddens(('device store primitive', 'memoryWrite32'),
                           audit(self.kext))

    def test_callback_trampoline_bypassing_the_accessor_is_caught(self):
        """NVGspBoot.hpp writes only through io.write32. A trampoline behind
        it that stores directly takes every Phase 4 write past the build
        gate."""
        plant(self.kext, ACCESSOR_FILE,
              'self->regWrite32((UInt32)bar0Offset, (UInt32)value);',
              'OSWriteLittleInt32(self->fBar0Base, bar0Offset, value);')
        self.assertReddens(('does not route through',), audit(self.kext))

    def test_stray_volatile_pointer_is_caught(self):
        """The PANIC_TEST null is exempt only inside its #ifdef arm. The same
        declaration anywhere else is an MMIO pointer of unknown origin."""
        plant_after_boot0(self.kext, 'volatile UInt32 *doorbell = nullptr;')
        self.assertReddens(('volatile pointer doorbell',), audit(self.kext))

    def test_audit_refuses_when_the_accessor_is_absent(self):
        """A gate that cannot find its subject must REFUSE, never pass."""
        plant(self.kext, ACCESSOR_FILE, ACCESSOR + '(',
              ACCESSOR + 'Renamed(')
        self.assertReddens(('guarded accessor', 'body not found'),
                           audit(self.kext))

    def test_audit_refuses_when_no_bar_pointer_is_derived(self):
        """Renaming the map call in every file empties the derived set. A
        gate that then found zero uses to audit would pass; this one must
        refuse."""
        planted = 0
        for name in source_files(self.kext):
            n = _read_exact(os.path.join(self.kext, name)).count(
                'getVirtualAddress()')
            if n:
                plant(self.kext, name, 'getVirtualAddress()',
                      'getVirtualAddressRenamed()', occurrences=n)
                planted += n
        self.assertGreater(planted, 0)
        self.assertReddens(('no BAR pointer derived',), audit(self.kext))


if __name__ == '__main__':
    unittest.main()

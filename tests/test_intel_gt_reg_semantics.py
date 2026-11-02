#!/usr/bin/env python3
"""INTEL OPEN 3 gate: IntelGtRegSemantics.hpp agrees with the vendored i915.

phase0-kext/IntelGtRegSemantics.hpp classifies every MMIO location the opt-in
GT bring-up writes by what a store means (plain, masked, write-only,
doorbell) and the ONE verification that is legitimate for it, and cites the
vendored reference/i915 lines that establish each row. This gate re-reads the
cited lines and refuses any row they do not support. Every expected value
comes from the cited vendored text, never from a list kept in this file.

Each property is its own failure label, so a negative control can assert
WHICH one broke:

  table     the table parses, holds IG_GTRS_ROW_COUNT rows with no duplicate,
            every base/kind/verify is a defined IG_GTRS_* value, and the
            accessor returns nullptr past the end.
  cite      every citation is file:line or file:first-last inside a file that
            exists under reference/i915. A missing file fails; it never skips.
  define    defineCite is the #define of i915Name (gen8_set_pte() for the
            GGTT row), the offset is the defined one, and the width is 32
            (64 for a PTE).
  queue     submit-queue rows sit on dwords of the defined queue; even dwords
            cite the lower_32_bits store and odd ones the upper, and the
            dwords form whole ports 0..n-1, each written once.
  kind      kindCite holds a store of the right shape: a masked idiom for
            MASKED, none for PLAIN, the submit_reg store for WRITE_ONLY, the
            ctrl_reg store for DOORBELL. An ENGINE_WRITE must name the
            register. SELF_CLEARING has no rows.
  pair      the verification is one the kind permits.
  verify    verifyCite says what the verification relies on.
  ack       ACK_REGISTER rows cite the matching FORCEWAKE_ACK_* define at
            ackOffset; every other row has ackOffset 0.
  hwowned   hwOwnedMask and hwOwnedCite come together, the cited define
            evaluates to the mask, and a READBACK_EQUAL row owns no bits.
  unread    no vendored source reads a WRITE_ONLY or DOORBELL location back.
  consts    *_I915_<value> names match their values, and the masked-field
            shift and the BCS0 base match their vendored definitions.
  isolated  the header has no MMIO spelling, includes only IntelGtCmd.hpp,
            and no other phase0-kext source includes it or uses its names.

Design rule, carried from the sibling gates: the live tree and every mutant
go through ONE audit() function. Mutants are planted in a TEMP COPY (the
header plus reference/i915), so audit() reads real files in both cases.
"""

import collections
import os
import re
import shutil
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

KEXT_DIR = 'phase0-kext'
I915_DIR = os.path.join('reference', 'i915')
HEADER = 'IntelGtRegSemantics.hpp'
ALLOWED_INCLUDES = ('IntelGtCmd.hpp',)
SOURCE_EXTS = ('.c', '.cpp', '.h', '.hpp', '.m', '.mm')

PLAIN = 'IG_GTRS_KIND_PLAIN'
MASKED = 'IG_GTRS_KIND_MASKED'
WRITE_ONLY = 'IG_GTRS_KIND_WRITE_ONLY'
DOORBELL = 'IG_GTRS_KIND_DOORBELL'
SELF_CLEARING = 'IG_GTRS_KIND_SELF_CLEARING'
WRITE_1_TO_CLEAR = 'IG_GTRS_KIND_WRITE_1_TO_CLEAR'
READBACK_EQUAL = 'IG_GTRS_VERIFY_READBACK_EQUAL'
MASKED_FIELD = 'IG_GTRS_VERIFY_MASKED_FIELD'
ACK_REGISTER = 'IG_GTRS_VERIFY_ACK_REGISTER'
INDIRECT_ONLY = 'IG_GTRS_VERIFY_INDIRECT_ONLY'
BASE_ABSOLUTE = 'IG_GTRS_BASE_ABSOLUTE'
BASE_ENGINE = 'IG_GTRS_BASE_ENGINE'
BASE_GGTT_PTE = 'IG_GTRS_BASE_GGTT_PTE'

# The verifications each kind permits. SELF_CLEARING has none on purpose.
# WRITE_1_TO_CLEAR: a stored 1 clears the bit and the hardware may set it
# again at any time (i915_irq.c:548-550), so neither "reads back as written"
# nor "reads back as 0" is evidence.
PAIRS = {
    PLAIN: (READBACK_EQUAL,),
    MASKED: (MASKED_FIELD, ACK_REGISTER),
    WRITE_ONLY: (INDIRECT_ONLY,),
    DOORBELL: (INDIRECT_ONLY,),
    WRITE_1_TO_CLEAR: (INDIRECT_ONLY,),
}

Row = collections.namedtuple(
    'Row', 'name base offset width kind verify hw ack dcite kcite vcite hcite')

_N = r'(?:0x[0-9A-Fa-f]+|[0-9]+)'

# Comments go; strings are kept or blanked. Char literals are matched so a
# quote inside one cannot open a fake string.
_TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"'
                    r"|'(?:\\.|[^'\\\n])*'", re.S)

HDR_DEFINE_RE = re.compile(
    r'^[ \t]*#define[ \t]+(IG_GTRS_\w+)[ \t]+(' + _N + r')u?\b', re.M)
TABLE_RE = re.compile(
    r'static\s+const\s+IGGtRegSemantics\s+table\s*\[\s*IG_GTRS_ROW_COUNT\s*\]'
    r'\s*=\s*\{(?P<body>.*?)\n\s*\}\s*;', re.S)
GUARD_RE = re.compile(
    r'if\s*\(\s*index\s*>=\s*IG_GTRS_ROW_COUNT\s*\)\s*return\s+nullptr\s*;')
ROW_RE = re.compile(
    r'\{\s*"(?P<name>\w+)"\s*,\s*(?P<base>\w+)\s*,'
    r'\s*(?P<offset>' + _N + r')u?\s*,\s*(?P<width>[0-9]+)u?\s*,'
    r'\s*(?P<kind>\w+)\s*,\s*(?P<verify>\w+)\s*,'
    r'\s*(?P<hw>' + _N + r')u?\s*,\s*(?P<ack>' + _N + r')u?\s*,'
    r'\s*"(?P<dcite>[^"]*)"\s*,\s*"(?P<kcite>[^"]*)"\s*,'
    r'\s*"(?P<vcite>[^"]*)"\s*,\s*"(?P<hcite>[^"]*)"\s*\}')
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*["<]([^">]+)[">]', re.M)
CITE_RE = re.compile(r'^([A-Za-z0-9_.]+\.[ch]):([0-9]+)(?:-([0-9]+))?$')

ACK_DEFINE_RE = re.compile(
    r'#define\s+(FORCEWAKE_ACK_\w+)\s+_MMIO\(\s*(0x[0-9A-Fa-f]+)\s*\)')
MASKED_FIELD_DEFINE_RE = re.compile(
    r'#define\s+__MASKED_FIELD\s*\(\s*mask\s*,\s*value\s*\)'
    r'\s*\(\s*\(mask\)\s*<<\s*([0-9]+)\s*\|\s*\(value\)\s*\)')
BLT_BASE_RE = re.compile(
    r'^[ \t]*#define[ \t]+BLT_RING_BASE[ \t]+(0x[0-9A-Fa-f]+)\b', re.M)
DEFINE_LINE_RE = re.compile(r'#define\s+(\w+)\s+(.+?)\s*$')
STORE_RE = re.compile(
    r'\b(?:writel|writeq|ENGINE_WRITE_FW|ENGINE_WRITE|intel_uncore_write_fw'
    r'|intel_uncore_write|raw_reg_write)\s*\(')
# Stores whose register argument is a symbol: the cited line must name the
# row's register.
NAMED_STORE_RE = re.compile(r'\b(ENGINE_WRITE(?:_FW)?|raw_reg_write)\s*\(')
# A read and the register symbol it names (second argument).
READ_RE = re.compile(
    r'\b(?:raw_reg_read|intel_uncore_read(?:_fw)?|ENGINE_READ(?:_FW)?)'
    r'\s*\(\s*[^,()]+,\s*(\w+)')
MASKED_IDIOM_RE = re.compile(
    r'\b_MASKED_BIT_ENABLE\s*\(|\b_MASKED_BIT_DISABLE\s*\(|\b_?_MASKED_FIELD\s*\('
    r'|0xffff\s*<<\s*16')
QUEUE_LOW_RE = re.compile(
    r'writel\(\s*lower_32_bits\(desc\)\s*,'
    r'\s*execlists->submit_reg\s*\+\s*port\s*\*\s*2\s*\)')
QUEUE_HIGH_RE = re.compile(
    r'writel\(\s*upper_32_bits\(desc\)\s*,'
    r'\s*execlists->submit_reg\s*\+\s*port\s*\*\s*2\s*\+\s*1\s*\)')
MMIO_SPELLING_RE = re.compile(
    r'\b(?:volatile|OSWrite\w*|OSRead\w*|ioWrite\w*|ioRead\w*|writel|writeq'
    r'|readl|readq|write32|read32|write64|read64|getVirtualAddress'
    r'|getBytesNoCopy|IOMemoryMap)\b')
USE_RE = re.compile(
    r'#\s*include\s*["<][^">]*IntelGtRegSemantics\.hpp[">]'
    r'|\bigGtRegSemanticsRow\b|\bIGGtRegSemantics\b|\bIG_GTRS_\w+')


def strip_comments(src, keep_strings=True):
    def sub(m):
        t = m.group(0)
        if t.startswith('//'):
            return ''
        if t.startswith('/*'):
            return '\n' * t.count('\n')
        return t if keep_strings else '""'
    return _TOKEN.sub(sub, src)


def _read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


class Tree:
    """reference/i915 under one root, read once per audit."""

    def __init__(self, root):
        self.i915 = os.path.join(root, I915_DIR)
        self._lines = {}

    def vendored(self, fname):
        """Lines of reference/i915/<fname>, or None when it is absent."""
        if fname not in self._lines:
            path = os.path.join(self.i915, fname)
            self._lines[fname] = (_read(path).splitlines()
                                  if os.path.isfile(path) else None)
        return self._lines[fname]

    def cite(self, cite):
        """(text, None) for a resolvable citation, else (None, reason)."""
        m = CITE_RE.match(cite)
        if not m:
            return None, '%r is not file:line or file:first-last' % cite
        fname, first = m.group(1), int(m.group(2))
        last = int(m.group(3)) if m.group(3) else first
        lines = self.vendored(fname)
        if lines is None:
            return None, '%s: reference/i915/%s not found' % (cite, fname)
        if not 1 <= first <= last <= len(lines):
            return None, '%s: outside reference/i915/%s (%d lines)' % (
                cite, fname, len(lines))
        return '\n'.join(lines[first - 1:last]), None


def eval_define(tree, fname, text):
    """Value of the one #define on a cited line, or None if not evaluable."""
    m = DEFINE_LINE_RE.search(text)
    if not m:
        return None
    expr = strip_comments(m.group(2)).strip()
    bit = re.fullmatch(r'REG_BIT\(\s*([0-9]+)\s*\)', expr)
    if bit:
        return 1 << int(bit.group(1))
    lit = re.fullmatch(r'\(?\s*(' + _N + r')u?\s*\)?', expr)
    if lit:
        return int(lit.group(1), 0)
    shift = re.fullmatch(r'\(\s*(\w+)\s*<<\s*([0-9]+)\s*\)', expr)
    if not shift:
        return None
    sym = shift.group(1)
    if re.fullmatch(_N, sym):
        value = int(sym, 0)
    else:
        found = re.search(r'^[ \t]*#define[ \t]+' + re.escape(sym)
                          + r'[ \t]+(' + _N + r')u?\b',
                          '\n'.join(tree.vendored(fname) or ()), re.M)
        if not found:
            return None
        value = int(found.group(1), 0)
    return value << int(shift.group(2))


def parse_rows(code, defines, fail):
    m = TABLE_RE.search(code)
    if not m:
        fail('table', 'table',
             'no static const IGGtRegSemantics table[IG_GTRS_ROW_COUNT]')
        return []
    body = m.group('body')
    rows = [Row(r.group('name'), r.group('base'), int(r.group('offset'), 0),
                int(r.group('width')), r.group('kind'), r.group('verify'),
                int(r.group('hw'), 0), int(r.group('ack'), 0),
                r.group('dcite'), r.group('kcite'), r.group('vcite'),
                r.group('hcite'))
            for r in ROW_RE.finditer(body)]
    entries = body.count('{')
    if entries != len(rows):
        fail('table', 'table', '%d brace entries but %d parse as rows'
             % (entries, len(rows)))
    count = defines.get('IG_GTRS_ROW_COUNT')
    if count != len(rows):
        fail('table', 'table', 'IG_GTRS_ROW_COUNT is %s but the table holds '
             '%d rows' % (count, len(rows)))
    if not GUARD_RE.search(code[m.end():]):
        fail('table', 'table', 'no index >= IG_GTRS_ROW_COUNT -> nullptr '
             'guard after the table')
    seen = set()
    for row in rows:
        if (row.name, row.offset) in seen:
            fail('table', row.name, 'duplicate row at 0x%X' % row.offset)
        seen.add((row.name, row.offset))
        for field, prefix in (('base', 'IG_GTRS_BASE_'),
                              ('kind', 'IG_GTRS_KIND_'),
                              ('verify', 'IG_GTRS_VERIFY_')):
            sym = getattr(row, field)
            if (not sym.startswith(prefix) or sym not in defines
                    or sym.endswith(('_INVALID', '_LIMIT'))):
                fail('table', row.name, '%s %s is not a defined %s* value'
                     % (field, sym, prefix))
    return rows


def check_row(tree, row, fail):
    """Every per-row property. Returns what the queue check needs."""
    facts = {'half': None, 'define_offset': None}
    texts = {}
    for field in ('dcite', 'kcite', 'vcite', 'hcite'):
        cite = getattr(row, field)
        if not cite:
            if field != 'hcite':
                fail('cite', row.name, '%s is empty' % field)
            continue
        text, why = tree.cite(cite)
        if why:
            fail('cite', row.name, why)
        else:
            texts[field] = text

    k = texts.get('kcite')
    if k is not None:
        if QUEUE_LOW_RE.search(k):
            facts['half'] = 'low'
        elif QUEUE_HIGH_RE.search(k):
            facts['half'] = 'high'

    d = texts.get('dcite')
    if d is not None:
        if row.base == BASE_GGTT_PTE:
            if not re.search(r'\bstatic\s+void\s+' + re.escape(row.name)
                             + r'\s*\(', d):
                fail('define', row.name, '%s does not define %s()'
                     % (row.dcite, row.name))
            if row.offset != 0:
                fail('define', row.name, 'a PTE row has offset 0, not 0x%X'
                     % row.offset)
        elif row.base in (BASE_ABSOLUTE, BASE_ENGINE):
            name = re.escape(row.name)
            if row.base == BASE_ABSOLUTE:
                # Plain NAME _MMIO(0x...), or indexed NAME(x) _MMIO(0x... +
                # ((x) * stride)), where the row is instance base + n * stride.
                pat = (r'#define\s+' + name
                       + r'(?:\s+_MMIO\(\s*(?P<off>0x[0-9A-Fa-f]+)\s*\)'
                       r'|\(\s*(?P<arg>\w+)\s*\)\s+_MMIO\(\s*'
                       r'(?P<ioff>0x[0-9A-Fa-f]+)\s*\+\s*\(\s*\(\s*(?P=arg)\s*\)'
                       r'\s*\*\s*(?P<stride>[0-9]+)\s*\)\s*\))')
            else:
                pat = (r'#define\s+' + name
                       + r'\(base\)\s+_MMIO\(\s*\(base\)\s*\+\s*'
                       r'(?P<off>0x[0-9A-Fa-f]+)\s*\)')
            m = re.search(pat, d)
            if not m:
                fail('define', row.name, '%s does not define %s as %s'
                     % (row.dcite, row.name, row.base))
            else:
                stride = m.groupdict().get('stride')
                facts['define_offset'] = int(m.group('ioff' if stride else 'off'), 0)
                delta = row.offset - facts['define_offset']
                if stride:
                    if not int(stride) or delta < 0 or delta % int(stride):
                        fail('define', row.name, '%s defines 0x%X + n * %s, row '
                             'says 0x%X' % (row.dcite, facts['define_offset'],
                                            stride, row.offset))
                elif facts['half'] is None and delta:
                    fail('define', row.name, '%s defines 0x%X, row says 0x%X'
                         % (row.dcite, facts['define_offset'], row.offset))
        want = 64 if row.base == BASE_GGTT_PTE else 32
        if row.width != want:
            fail('define', row.name, 'width %d, expected %d'
                 % (row.width, want))

    if k is not None:
        masked = MASKED_IDIOM_RE.search(k)
        named = re.search(STORE_RE.pattern + r'[^;]*\b' + re.escape(row.name)
                          + r'\b', k)
        if not STORE_RE.search(k):
            fail('kind', row.name, '%s holds no store' % row.kcite)
        elif row.kind == MASKED and not masked:
            fail('kind', row.name, '%s stores no masked value' % row.kcite)
        elif row.kind == PLAIN and masked:
            fail('kind', row.name, '%s stores a masked value; a PLAIN '
                 'readback would refuse it' % row.kcite)
        elif row.kind == WRITE_ONLY and 'submit_reg' not in k and not named:
            fail('kind', row.name, '%s is neither the submit_reg store nor a '
                 'store naming %s' % (row.kcite, row.name))
        elif row.kind == DOORBELL and 'ctrl_reg' not in k and not named:
            fail('kind', row.name, '%s is neither the ctrl_reg store nor a '
                 'store naming %s' % (row.kcite, row.name))
        elif row.kind == WRITE_1_TO_CLEAR and (masked or not named):
            fail('kind', row.name, '%s is not an unmasked store naming %s'
                 % (row.kcite, row.name))
        named_store = NAMED_STORE_RE.search(k)
        if named_store and not re.search(
                r'\b' + re.escape(row.name) + r'\b', k):
            fail('kind', row.name, 'the %s at %s does not name %s'
                 % (named_store.group(1), row.kcite, row.name))
    if row.kind == SELF_CLEARING:
        fail('kind', row.name, 'SELF_CLEARING: no vendored line shows any '
             'register in this set clears itself; the header has no such rows')

    allowed = PAIRS.get(row.kind)
    if allowed is not None and row.verify not in allowed:
        fail('pair', row.name, '%s permits only %s, not %s'
             % (row.kind, ' or '.join(allowed), row.verify))

    v = texts.get('vcite')
    if v is not None:
        ok = True
        if row.verify == READBACK_EQUAL:
            if row.base == BASE_GGTT_PTE:
                ok = bool(re.search(r'\breadback\b', v)
                          and re.search(r'\bPTE\b', v))
            else:
                ok = bool(re.search(r'POSTING_READ\s*\([^)]*\b'
                                    + re.escape(row.name) + r'\b', v))
        elif row.verify == MASKED_FIELD:
            ok = bool(MASKED_FIELD_DEFINE_RE.search(v))
        elif row.verify == INDIRECT_ONLY:
            read = [r.group(1) for r in READ_RE.finditer(v)]
            if row.kind == WRITE_1_TO_CLEAR:
                # The sample whose set bits the store writes back.
                ok = row.name in read
            elif row.kind == WRITE_ONLY and 'submit_reg' in (k or ''):
                ok = bool(re.search(r'not\s+cleared', v))
            elif row.kind == DOORBELL and 'ctrl_reg' in (k or ''):
                ok = bool(re.search(r'\bCSB\b', v))
            elif row.kind in (WRITE_ONLY, DOORBELL):
                # Outside the execlist the effect shows in another register.
                ok = any(r != row.name for r in read)
        elif row.verify == ACK_REGISTER:
            m = ACK_DEFINE_RE.search(v)
            expected = row.name.replace('FORCEWAKE_', 'FORCEWAKE_ACK_', 1)
            if not m or m.group(1) != expected:
                fail('ack', row.name, '%s does not define %s'
                     % (row.vcite, expected))
            elif int(m.group(2), 0) != row.ack:
                fail('ack', row.name, '%s is 0x%X, ackOffset is 0x%X'
                     % (expected, int(m.group(2), 0), row.ack))
        if not ok:
            fail('verify', row.name, '%s does not support %s'
                 % (row.vcite, row.verify))
    if row.verify == ACK_REGISTER and row.ack == 0:
        fail('ack', row.name, 'ACK_REGISTER with ackOffset 0')
    if row.verify != ACK_REGISTER and row.ack != 0:
        fail('ack', row.name, 'ackOffset 0x%X on a %s row'
             % (row.ack, row.verify))

    if row.hw and not row.hcite:
        fail('hwowned', row.name, 'hwOwnedMask 0x%X has no hwOwnedCite'
             % row.hw)
    if row.hcite and not row.hw:
        fail('hwowned', row.name, 'hwOwnedCite %s but hwOwnedMask is 0'
             % row.hcite)
    if row.hw and row.verify == READBACK_EQUAL:
        fail('hwowned', row.name, 'READBACK_EQUAL compares every bit, so no '
             'bit can be hardware-owned')
    if row.hw > 0xFFFF and row.kind == MASKED:
        fail('hwowned', row.name, 'a masked register keeps state only in '
             'the low 16 bits')
    h = texts.get('hcite')
    if h is not None and row.hw:
        value = eval_define(tree, CITE_RE.match(row.hcite).group(1), h)
        if value is None:
            fail('hwowned', row.name, '%s is not a define this gate can '
                 'evaluate' % row.hcite)
        elif value != row.hw:
            fail('hwowned', row.name, '%s evaluates to 0x%X, row says 0x%X'
                 % (row.hcite, value, row.hw))
    return facts


def check_queue(rows, facts, fail):
    dwords = collections.defaultdict(list)
    for row, fact in zip(rows, facts):
        if fact['half'] is None:
            continue
        if row.base != BASE_ENGINE:
            fail('queue', row.name, 'a submit-queue dword is engine-relative')
            continue
        base = fact['define_offset']
        if base is None:
            continue  # the define failure is already reported
        delta = row.offset - base
        if delta < 0 or delta % 4:
            fail('queue', row.name, '0x%X is not a dword of the queue at 0x%X'
                 % (row.offset, base))
            continue
        index = delta // 4
        want = 'low' if index % 2 == 0 else 'high'
        if fact['half'] != want:
            fail('queue', row.name, 'dword %d (0x%X) is the %s half of port '
                 '%d but %s is the %s-half store'
                 % (index, row.offset, want, index // 2, row.kcite,
                    fact['half']))
        dwords[row.name].append(index)
    for name, idx in sorted(dwords.items()):
        if sorted(idx) != list(range(len(idx))) or len(idx) % 2:
            fail('queue', name, 'dwords %s are not whole ports 0..n-1, each '
                 'written once' % sorted(idx))


def check_unread(tree, rows, fail):
    sources = []
    if os.path.isdir(tree.i915):
        for fname in sorted(os.listdir(tree.i915)):
            if fname.endswith(('.c', '.h')):
                lines = tree.vendored(fname)
                if lines:
                    sources.append((fname, lines))
    done = set()
    for row in rows:
        if row.kind not in (WRITE_ONLY, DOORBELL) or row.name in done:
            continue
        done.add(row.name)
        # Only the execlist rows are stored through a cached accessor
        # (el->submit_reg, el->ctrl_reg), so a read of that accessor reads
        # them back. Every other row is stored by its own name (the GT-2
        # interrupt rows, intel_gt_irq.c), so only a read of that name
        # counts against it; keying the accessor on kind flagged them all.
        accessor = {'RING_EXECLIST_SQ_CONTENTS': 'submit_reg',
                    'RING_EXECLIST_CONTROL': 'ctrl_reg'}.get(row.name)
        names = [re.escape(row.name)] + ([accessor] if accessor else [])
        pat = re.compile(r'(?:readl|READ|_read)[^;]*\b(?:%s)\b'
                         % '|'.join(names))
        for fname, lines in sources:
            for n, line in enumerate(lines, 1):
                if pat.search(line):
                    fail('unread', row.name, '%s:%d reads it back: %s'
                         % (fname, n, line.strip()))


def check_consts(tree, defines, fail):
    for name, value in sorted(defines.items()):
        m = re.search(r'_I915_(' + _N + r')$', name)
        if m and int(m.group(1), 0) != value:
            fail('consts', name, 'name says %s but the value is 0x%X'
                 % (m.group(1), value))
    for name, fname, pattern in (
            ('IG_GTRS_MASK_SHIFT_I915_16', 'i915_reg_defs.h',
             MASKED_FIELD_DEFINE_RE),
            ('IG_GTRS_BCS0_BASE_I915_0x22000', 'i915_reg.h', BLT_BASE_RE)):
        ours = defines.get(name)
        lines = tree.vendored(fname)
        if ours is None:
            fail('consts', name, 'not defined')
        elif lines is None:
            fail('consts', name, 'reference/i915/%s not found' % fname)
        else:
            m = pattern.search('\n'.join(lines))
            if not m:
                fail('consts', name, 'no definition found in %s' % fname)
            elif int(m.group(1), 0) != ours:
                fail('consts', name, '%s says 0x%X, header says 0x%X'
                     % (fname, int(m.group(1), 0), ours))


def check_isolated(root, raw, fail):
    for m in MMIO_SPELLING_RE.finditer(strip_comments(raw, keep_strings=False)):
        fail('isolated', HEADER, 'MMIO spelling %r outside a comment'
             % m.group(0))
    for inc in INCLUDE_RE.findall(strip_comments(raw)):
        if inc not in ALLOWED_INCLUDES:
            fail('isolated', HEADER, 'includes %s; the table stays '
                 'dependency-free' % inc)
    kext = os.path.join(root, KEXT_DIR)
    for fname in sorted(os.listdir(kext)):
        path = os.path.join(kext, fname)
        if (fname == HEADER or not fname.endswith(SOURCE_EXTS)
                or not os.path.isfile(path)):
            continue
        m = USE_RE.search(strip_comments(_read(path)))
        if m:
            fail('isolated', fname, 'reaches the semantics table (%s); it '
                 'has no call sites yet' % m.group(0).strip())


def audit(root):
    """(failures, rows audited). Each failure is (property, subject, detail)."""
    failures = []

    def fail(prop, subject, detail):
        failures.append((prop, subject, detail))

    path = os.path.join(root, KEXT_DIR, HEADER)
    if not os.path.isfile(path):
        fail('table', HEADER, 'header not found')
        return failures, 0
    raw = _read(path)
    code = strip_comments(raw)
    defines = {n: int(v, 0) for n, v in HDR_DEFINE_RE.findall(code)}
    tree = Tree(root)
    rows = parse_rows(code, defines, fail)
    facts = [check_row(tree, row, fail) for row in rows]
    check_queue(rows, facts, fail)
    check_unread(tree, rows, fail)
    check_consts(tree, defines, fail)
    check_isolated(root, raw, fail)
    return failures, len(rows)


class IntelGtRegSemanticsGate(unittest.TestCase):

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gtrs_')
        self.addCleanup(self.temp.cleanup)
        self.root = self.temp.name
        os.makedirs(os.path.join(self.root, KEXT_DIR))
        shutil.copytree(os.path.join(ROOT, I915_DIR),
                        os.path.join(self.root, I915_DIR))
        self.hdr = os.path.join(self.root, KEXT_DIR, HEADER)
        self.original = _read(os.path.join(ROOT, KEXT_DIR, HEADER))
        self.write(self.hdr, self.original)

    @staticmethod
    def write(path, text):
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)

    def row(self, name, offset):
        for m in ROW_RE.finditer(self.original):
            if m.group('name') == name and int(m.group('offset'), 0) == offset:
                return m
        self.fail('no row %s at 0x%X in the live header' % (name, offset))

    def plant_row(self, row_name, row_offset, /, **fields):
        # Positional-only, so a planted 'name' or 'offset' group goes to fields.
        m = self.row(row_name, row_offset)
        src = self.original
        for group in sorted(fields, key=m.start, reverse=True):
            src = src[:m.start(group)] + fields[group] + src[m.end(group):]
        self.write(self.hdr, src)

    def plant_header(self, pattern, repl):
        src, n = re.subn(pattern, repl, self.original)
        self.assertEqual(n, 1, pattern)
        self.write(self.hdr, src)

    def plant_vendored(self, fname, pattern, repl):
        path = os.path.join(self.root, I915_DIR, fname)
        src, n = re.subn(pattern, repl, _read(path), flags=re.M)
        self.assertEqual(n, 1, pattern)
        self.write(path, src)

    def assertBroke(self, subject, *props):
        failures, _ = audit(self.root)
        self.assertTrue(failures, 'mutant passed')
        self.assertEqual({s for _, s, _ in failures}, {subject}, failures)
        got = {p for p, _, _ in failures}
        for prop in props:
            self.assertIn(prop, got, failures)
        return failures

    def test_live_tree_passes(self):
        """The real header against the real vendored tree."""
        failures, rows = audit(ROOT)
        self.assertEqual(failures, [])
        self.assertEqual(rows, 19)

    def test_untouched_copy_passes(self):
        """The baseline every control below is planted into."""
        failures, rows = audit(self.root)
        self.assertEqual(failures, [])
        self.assertEqual(rows, 19)

    def test_wrong_offset_fails_define(self):
        self.plant_row('RING_HWS_PGA', 0x80, offset='0x84')
        self.assertBroke('RING_HWS_PGA', 'define')

    def test_shifted_define_citation_fails_define(self):
        self.plant_row('RING_HWS_PGA', 0x80, dcite='intel_engine_regs.h:67')
        self.assertBroke('RING_HWS_PGA', 'define')

    def test_vendored_offset_drift_fails_define(self):
        """The header is untouched; the vendored #define moves."""
        self.plant_vendored('intel_engine_regs.h',
                            r'(#define\s+RING_HWS_PGA\(base\)\s+_MMIO\(\(base\) \+ )0x80\)',
                            r'\g<1>0x84)')
        self.assertBroke('RING_HWS_PGA', 'define')

    def test_masked_register_called_plain_fails_kind(self):
        """A plain compare would refuse a correct masked write."""
        self.plant_row('RING_MODE_GEN7', 0x29C, kind=PLAIN,
                       verify=READBACK_EQUAL)
        self.assertBroke('RING_MODE_GEN7', 'kind', 'verify')

    def test_doorbell_readback_fails_pair(self):
        """The hazard the table exists for: proving a doorbell by readback."""
        self.plant_row('RING_EXECLIST_CONTROL', 0x550, verify=READBACK_EQUAL)
        self.assertBroke('RING_EXECLIST_CONTROL', 'pair', 'verify')

    def test_self_clearing_row_fails_kind(self):
        self.plant_row('RING_EXECLIST_CONTROL', 0x550, kind=SELF_CLEARING)
        self.assertBroke('RING_EXECLIST_CONTROL', 'kind')

    def test_queue_dword_citing_wrong_half_fails_queue(self):
        self.plant_row('RING_EXECLIST_SQ_CONTENTS', 0x514,
                       kcite='intel_execlists_submission.c:729')
        self.assertBroke('RING_EXECLIST_SQ_CONTENTS', 'queue')

    def test_dropped_queue_dword_fails_table_and_queue(self):
        m = self.row('RING_EXECLIST_SQ_CONTENTS', 0x51C)
        tail = re.match(r'\s*,', self.original[m.end():])
        self.write(self.hdr, self.original[:m.start()]
                   + self.original[m.end() + tail.end():])
        failures, rows = audit(self.root)
        self.assertEqual(rows, 18)
        self.assertEqual({s for _, s, _ in failures},
                         {'table', 'RING_EXECLIST_SQ_CONTENTS'}, failures)
        self.assertEqual({p for p, _, _ in failures}, {'table', 'queue'})

    def test_wrong_hw_owned_mask_fails_hwowned(self):
        self.plant_row('RING_MI_MODE', 0x9C, hw='0x100')
        self.assertBroke('RING_MI_MODE', 'hwowned')

    def test_uncited_hw_owned_mask_fails_hwowned(self):
        self.plant_row('RING_CONTEXT_STATUS_PTR', 0x3A0, hcite='')
        self.assertBroke('RING_CONTEXT_STATUS_PTR', 'hwowned')

    def test_wrong_ack_offset_fails_ack(self):
        self.plant_row('FORCEWAKE_GT_GEN9', 0xA188, ack='0xD84')
        self.assertBroke('FORCEWAKE_GT_GEN9', 'ack')

    def test_missing_vendored_file_fails_not_skips(self):
        os.remove(os.path.join(self.root, I915_DIR, 'intel_lrc_reg.h'))
        failures = self.assertBroke('RING_CONTEXT_STATUS_PTR', 'cite')
        self.assertIn('not found', failures[0][2])

    def test_planted_upstream_readback_fails_unread(self):
        """Appended, so every cited line number stays put."""
        path = os.path.join(self.root, I915_DIR, 'intel_execlists_submission.c')
        with open(path, 'a', encoding='utf-8') as f:
            f.write('\nstatic u32 planted(struct intel_engine_execlists *el)\n'
                    '{\n\treturn readl(el->ctrl_reg);\n}\n')
        self.assertBroke('RING_EXECLIST_CONTROL', 'unread')

    def test_vendored_mask_shift_drift_fails_consts(self):
        self.plant_vendored('i915_reg_defs.h',
                            r'(#define\s+__MASKED_FIELD\(mask, value\) \(\(mask\) << )16',
                            r'\g<1>15')
        self.assertBroke('IG_GTRS_MASK_SHIFT_I915_16', 'consts')

    def test_constant_disagreeing_with_its_name_fails_consts(self):
        self.plant_header(r'(#define\s+IG_GTRS_BCS0_BASE_I915_0x22000\s+)0x22000u',
                          r'\g<1>0x20000u')
        self.assertBroke('IG_GTRS_BCS0_BASE_I915_0x22000', 'consts')

    def test_unparseable_row_fails_table(self):
        """A row the parser cannot read is refused, not silently dropped."""
        self.plant_row('RING_HWS_PGA', 0x80, kind=PLAIN + ' + 0')
        self.assertBroke('table', 'table')

    def test_mmio_in_header_fails_isolated(self):
        self.plant_header(r'(#include "IntelGtCmd\.hpp"\n)',
                          r'\g<1>static inline void igGtrsPoke(volatile IGU32 *r)'
                          r' { *r = 0u; }\n')
        self.assertBroke(HEADER, 'isolated')

    def test_call_site_fails_isolated(self):
        self.write(os.path.join(self.root, KEXT_DIR, 'IntelGtBringup.cpp'),
                   '#include "IntelGtRegSemantics.hpp"\n')
        self.assertBroke('IntelGtBringup.cpp', 'isolated')

    def test_comment_mention_is_not_a_call_site(self):
        """Prose about the table is documentation, not wiring."""
        self.write(os.path.join(self.root, KEXT_DIR, 'IntelGtNote.cpp'),
                   '// IntelGtRegSemantics.hpp: see igGtRegSemanticsRow().\n'
                   '/* IG_GTRS_KIND_DOORBELL */\n')
        failures, _ = audit(self.root)
        self.assertEqual(failures, [])


if __name__ == '__main__':
    unittest.main()

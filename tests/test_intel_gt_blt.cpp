//
//  test_intel_gt_blt.cpp -- drives phase0-kext/IntelGtBlt.hpp, the Gen12 2D
//  BLITTER packet composer.
//
//  WHAT THIS SUITE CAN AND CANNOT ESTABLISH
//  ----------------------------------------
//  Its subject is the first header in this tree that describes GPU WORK rather
//  than the plumbing that would deliver it. Unlike test_intel_gt_context.cpp
//  and test_intel_gt_image.cpp, this subject DOES have a cited half:
//  reference/i915/intel_gpu_commands.h carries XY_COLOR_BLT_CMD (0x50),
//  XY_SRC_COPY_BLT_CMD (0x53), COLOR_BLT_CMD (0x40) and INSTR_BC_CLIENT (2).
//  Those are pinned here against the vendored file's own values.
//
//  What it CANNOT establish: that any packet composed here would do anything.
//  No dword written by this header has ever been submitted to an engine. A
//  packet that is byte-perfect against upstream's macros and semantically wrong
//  about Gen12 blitter behaviour passes every check below. The suite pins
//  COMPOSITION, not execution.
//
//  WHY THIS FILE WAS REWRITTEN RATHER THAN PATCHED (2026-08-28)
//  ------------------------------------------------------------
//  The previous revision was authored against an EARLIER SKETCH of its own
//  subject and stopped compiling. It called igGtbWriteXyColorBlt /
//  igGtbWriteXySrcCopyBlt (the header spells them igGtbWriteColorBlit /
//  igGtbWriteCopyBlit), expected an EIGHT-dword copy packet (the header writes
//  ten), and omitted the source origin and the depth flag from the copy call
//  entirely.
//
//  The header is the authority, and not merely because it is the shipping side.
//  The seven-dword fill already carries a SIXTY-FOUR-BIT destination address --
//  low half in dword 4, high half in dword 5 -- so an eight-dword copy cannot
//  also carry a second 64-bit address, a source pitch and a source origin. It
//  does not fit. Seven and ten are the Gen8+ 64-bit-address forms, and the old
//  eight was the pre-Gen8 32-bit shape carried over by hand.
//
//  A second reason points the same way and is worth stating because it is the
//  weaker of the two: fixing the test leaves phase0-kext/ byte-identical, so
//  build-payload/kext-src.tar needs no rebuild and the Python gate stays quiet.
//  That is a convenience, not an argument, and if the header had been the wrong
//  side it would not have saved it.
//
//  Two properties get disproportionate attention, and both are refusal-ORDERING
//  rather than value pins:
//
//    1. The span check runs BEFORE the surface and rect checks. A caller
//       handing a three-dword span and an inverted rectangle must be told the
//       span is too short, because writing the packet is what would scribble --
//       the rectangle is merely wrong. A subject that reordered these would
//       return RECT_INVERTED and then, on a fixed rectangle, store past the end
//       of the caller's array.
//
//    2. outDwordsWritten is written on EVERY path, including the refusals that
//       return before any argument is inspected. A caller that advances a ring
//       tail by a stale count on a refused compose walks the tail into dwords
//       the engine will execute as commands.
//

#include <cstdio>
#include <cstring>

#include "IntelGtBlt.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %s: got 0x%08X want 0x%08X\n",
                    label, (unsigned)got, (unsigned)want);
    }
}

static void checkBool(const char *label, bool got, bool want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %s: got %s want %s\n",
                    label, got ? "true" : "false", want ? "true" : "false");
    }
}

static void checkStr(const char *label, const char *got, const char *want)
{
    gChecks++;
    if (got == 0 || want == 0 || std::strcmp(got, want) != 0) {
        gFailures++;
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n",
                    label, got ? got : "(null)", want ? want : "(null)");
    }
}

//
// 1. The cited constants, against reference/i915/intel_gpu_commands.h.
//
// These are the only values in the header with an upstream line to point at,
// and they are the ones a hand-transcription error would silently corrupt. The
// register gate does not parse this header at all, so nothing before this suite
// checked what they EQUAL.
//
static void testCitedConstants()
{
    checkU32("client BC", IG_GTB_CLIENT_I915_2, 2u);

    checkU32("opcode XY_COLOR_BLT",    IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50, 0x50u);
    checkU32("opcode XY_SRC_COPY_BLT", IG_GTB_OPCODE_XY_SRC_COPY_BLT_I915_0x53, 0x53u);
    checkU32("opcode COLOR_BLT",       IG_GTB_OPCODE_COLOR_BLT_I915_0x40, 0x40u);

    // The three opcodes must be DISTINCT. A copy-paste arm giving the fill
    // command the copy command's opcode satisfies every "is 0x50 present"
    // check and composes a packet the engine reads as the wrong command.
    checkBool("opcodes distinct: color vs srccopy",
              IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50 !=
              IG_GTB_OPCODE_XY_SRC_COPY_BLT_I915_0x53, true);
    checkBool("opcodes distinct: color vs plain",
              IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50 !=
              IG_GTB_OPCODE_COLOR_BLT_I915_0x40, true);
    checkBool("opcodes distinct: srccopy vs plain",
              IG_GTB_OPCODE_XY_SRC_COPY_BLT_I915_0x53 !=
              IG_GTB_OPCODE_COLOR_BLT_I915_0x40, true);

    // Every opcode must survive the 7-bit field it is shifted into. An opcode
    // above 0x7F truncates silently and lands on a DIFFERENT valid command.
    checkU32("opcode fits mask: XY_COLOR_BLT",
             IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50 & ~IG_GTB_OPCODE_MASK_POLICY_0x7F, 0u);
    checkU32("opcode fits mask: XY_SRC_COPY_BLT",
             IG_GTB_OPCODE_XY_SRC_COPY_BLT_I915_0x53 & ~IG_GTB_OPCODE_MASK_POLICY_0x7F, 0u);
    checkU32("opcode fits mask: COLOR_BLT",
             IG_GTB_OPCODE_COLOR_BLT_I915_0x40 & ~IG_GTB_OPCODE_MASK_POLICY_0x7F, 0u);

    checkU32("client shift", IG_GTB_CLIENT_SHIFT_POLICY_29, 29u);
    checkU32("opcode shift", IG_GTB_OPCODE_SHIFT_POLICY_22, 22u);
    checkU32("opcode mask",  IG_GTB_OPCODE_MASK_POLICY_0x7F, 0x7Fu);

    // The two shifted fields must not overlap. Client at 29 over a 3-bit space
    // and opcode at 22 over 7 bits exactly abut at bit 29; one shift wrong by
    // a single position ORs the two together.
    checkBool("client and opcode fields abut without overlap",
              IG_GTB_OPCODE_SHIFT_POLICY_22 + 7u == IG_GTB_CLIENT_SHIFT_POLICY_29,
              true);
}

//
// 2. igGtbHeader() -- the composed command word.
//
// The BLT client/opcode encoding is NOT the MI encoding, which is why the
// header composes its own word rather than reusing igGtMiHeader(): client 2
// sits at bit 29 and the opcode is SEVEN bits at bit 22, where MI puts a
// six-bit opcode at 23. A header composed with MI's geometry is a valid
// instruction of some other kind, so these pins are against the exact word.
//
static void testHeaderComposition()
{
    // XY_COLOR_BLT, length field 5 (a 7-dword packet), no flags.
    // (2 << 29) | (0x50 << 22) | 5 == 0x40000000 | 0x14000000 | 5
    checkU32("header XY_COLOR_BLT len5",
             igGtbHeader(IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50, 5u, 0u),
             0x54000005u);

    // XY_SRC_COPY_BLT, length field 8 (a 10-dword packet).
    // (2 << 29) | (0x53 << 22) | 8 == 0x40000000 | 0x14C00000 | 8
    checkU32("header XY_SRC_COPY_BLT len8",
             igGtbHeader(IG_GTB_OPCODE_XY_SRC_COPY_BLT_I915_0x53, 8u, 0u),
             0x54C00008u);

    checkU32("header COLOR_BLT len3",
             igGtbHeader(IG_GTB_OPCODE_COLOR_BLT_I915_0x40, 3u, 0u),
             0x50000003u);

    // Flags OR in without disturbing either field.
    checkU32("header carries flags",
             igGtbHeader(IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50, 5u, 0x00200000u),
             0x54200005u);

    // The length field is masked to 8 bits. A caller passing a packet length
    // that does not fit must not have its overflow bits land in the opcode
    // field, which is what an unmasked OR would do -- 0x100 sets bit 8, and
    // bit 8 is nowhere near the opcode, but 0x40000000 would reach bit 30 and
    // corrupt the client. Pin the mask directly.
    checkU32("header masks length to 8 bits",
             igGtbHeader(IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50, 0x105u, 0u),
             0x54000005u);

    // The opcode is masked to 7 bits for the same reason in the other
    // direction: 0xD0 is 0x50 with bit 7 set, and bit 7 shifted to 22 lands on
    // bit 29 -- the client field.
    checkU32("header masks opcode to 7 bits",
             igGtbHeader(0xD0u, 5u, 0u),
             0x54000005u);

    // The client is ALWAYS 2. There is no arm that composes a header with any
    // other client, and a subject that let the opcode reach the client field
    // would produce a word whose top three bits are not 010.
    checkU32("client field is 2 for every opcode",
             (igGtbHeader(IG_GTB_OPCODE_XY_SRC_COPY_BLT_I915_0x53, 8u, 0u) >> 29) & 0x7u,
             2u);
}

//
// 3. igGtbLengthField() -- packet length minus the bias.
//
// The interesting arm is the SHORT one. The bias is 2, so a packet of 0 or 1
// dwords would underflow an unsigned subtraction to 0xFFFFFFFE / 0xFFFFFFFF,
// and the header masks the low 8 bits -- so the composed length field would be
// 0xFE or 0xFF. That is not a refusal, it is a packet claiming to be 256 dwords
// long, and the engine would execute 250-odd dwords of whatever followed it in
// the ring.
//
static void testLengthField()
{
    checkU32("length bias is 2", IG_GTB_LENGTH_BIAS_POLICY_2, 2u);
    checkU32("length mask is 0xFF", IG_GTB_LENGTH_MASK_POLICY_0xFF, 0xFFu);

    checkU32("length field of 7-dword fill",  igGtbLengthField(7u), 5u);
    checkU32("length field of 10-dword copy", igGtbLengthField(10u), 8u);
    checkU32("length field at the bias",      igGtbLengthField(2u), 0u);

    // Below the bias: 0 rather than an underflow. Both cases, because a guard
    // written as == 0 catches only one of them.
    checkU32("length field of 1 dword does not underflow", igGtbLengthField(1u), 0u);
    checkU32("length field of 0 dwords does not underflow", igGtbLengthField(0u), 0u);

    // The separating assertion: an underflowed value would be 0xFFFFFFFE, and
    // 0xFFFFFFFE & 0xFF is 0xFE. Pin what the HEADER would carry, not just what
    // the helper returns, because the mask is where the damage would surface.
    checkU32("short packet composes length 0 in the header",
             igGtbHeader(IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50,
                         igGtbLengthField(1u), 0u)
                 & IG_GTB_LENGTH_MASK_POLICY_0xFF,
             0u);
}

//
// 4. The verdict family.
//
// Same three rules every verdict family in this tree obeys, and each one exists
// because breaking it produced a real defect somewhere in this project:
//
//   - the codes are CONTIGUOUS from 0 to the family's own _LIMIT, so a family
//     that grows reddens on the commit that grows it rather than silently
//     acquiring an unnamed member;
//   - zero is NOT_ATTEMPTED -- the caller's initialiser -- and is never
//     returned by the subject, so an unwritten out-param cannot read as a
//     verdict somebody measured;
//   - every code below the limit has a NAME arm. igModeVerdictName() had arms
//     for 0..5 while its limit was 6, and the missing arm published the unknown
//     string into the IORegistry with all four gates green.
//
// The name loop bounds on IG_GTB_BLIT_LIMIT rather than on a count spelled
// here, because a count spelled in the test is a second copy of the fact and
// the two agree only until one moves.
//
static void testVerdictFamily()
{
    checkU32("NOT_ATTEMPTED is zero", IG_GTB_BLIT_NOT_ATTEMPTED, 0u);

    // Contiguity. Every value from 0 up to the limit must have a name, and the
    // value AT the limit must not -- that pairing is what forces whoever adds
    // an eleventh verdict to come back and extend the switch.
    for (IGU32 v = 0u; v < IG_GTB_BLIT_LIMIT; v++) {
        const char *name = igGtbBlitVerdictName(v);
        gChecks++;
        if (name == 0 || name[0] == '\0') {
            gFailures++;
            std::printf("FAIL verdict %u has no name\n", (unsigned)v);
        }
    }

    // Pairwise distinct names. A copy-paste arm returning a sibling's string
    // satisfies every "has a name" check above and reports the wrong refusal to
    // the one person who could act on it.
    for (IGU32 a = 0u; a < IG_GTB_BLIT_LIMIT; a++) {
        for (IGU32 b = a + 1u; b < IG_GTB_BLIT_LIMIT; b++) {
            gChecks++;
            if (std::strcmp(igGtbBlitVerdictName(a), igGtbBlitVerdictName(b)) == 0) {
                gFailures++;
                std::printf("FAIL verdicts %u and %u share a name: \"%s\"\n",
                            (unsigned)a, (unsigned)b, igGtbBlitVerdictName(a));
            }
        }
    }

    // The tripwire: one past the family answers the sentinel, and the sentinel
    // is distinct from every real name. A sentinel that happened to equal a
    // real arm's string would make the coverage loop above vacuous.
    const char *sentinel = igGtbBlitVerdictName(IG_GTB_BLIT_LIMIT);
    gChecks++;
    if (sentinel == 0 || sentinel[0] == '\0') {
        gFailures++;
        std::printf("FAIL one-past-limit has no sentinel name\n");
    }
    for (IGU32 v = 0u; v < IG_GTB_BLIT_LIMIT; v++) {
        gChecks++;
        if (std::strcmp(igGtbBlitVerdictName(v), sentinel) == 0) {
            gFailures++;
            std::printf("FAIL verdict %u collides with the sentinel\n", (unsigned)v);
        }
    }

    // Admissibility is an EQUALITY against OK, never a negation of one error
    // arm. "!= RECT_INVERTED" is true for nine other refusals and would report
    // a packet nobody could compose as admissible.
    checkBool("OK is admissible", igGtbBlitSucceeded(IG_GTB_BLIT_OK), true);
    for (IGU32 v = 0u; v < IG_GTB_BLIT_LIMIT; v++) {
        if (v == IG_GTB_BLIT_OK) { continue; }
        gChecks++;
        if (igGtbBlitSucceeded(v)) {
            gFailures++;
            std::printf("FAIL verdict %u reports admissible\n", (unsigned)v);
        }
    }
    checkBool("NOT_ATTEMPTED is not admissible",
              igGtbBlitSucceeded(IG_GTB_BLIT_NOT_ATTEMPTED), false);
    checkBool("one past the limit is not admissible",
              igGtbBlitSucceeded(IG_GTB_BLIT_LIMIT), false);
}

//
// 5. igGtbRectVerdict() -- and the order its arms run in.
//
// INVERTED is tested BEFORE EMPTY, and that order is load-bearing rather than
// stylistic. An inverted rectangle (right < left) is a bug in the caller's
// arithmetic; an empty one (right == left) is a legitimate no-op somebody asked
// for. If EMPTY ran first it would still be reachable, so the ordering claim
// needs an input that is bad in BOTH ways at once -- and there isn't one, since
// right < left and right == left are disjoint. The separating input is instead
// a rectangle inverted on ONE axis and empty on the OTHER: an EMPTY-first
// subject reports EMPTY and hides the arithmetic bug.
//
static void testRectVerdict()
{
    checkU32("ordinary rect is OK",
             igGtbRectVerdict(0u, 0u, 64u, 64u), IG_GTB_BLIT_OK);
    checkU32("offset rect is OK",
             igGtbRectVerdict(16u, 32u, 80u, 96u), IG_GTB_BLIT_OK);
    checkU32("one-pixel rect is OK",
             igGtbRectVerdict(10u, 10u, 11u, 11u), IG_GTB_BLIT_OK);

    checkU32("empty on x refuses",
             igGtbRectVerdict(10u, 0u, 10u, 64u), IG_GTB_BLIT_EMPTY_RECT);
    checkU32("empty on y refuses",
             igGtbRectVerdict(0u, 10u, 64u, 10u), IG_GTB_BLIT_EMPTY_RECT);

    checkU32("inverted on x refuses",
             igGtbRectVerdict(64u, 0u, 10u, 64u), IG_GTB_BLIT_RECT_INVERTED);
    checkU32("inverted on y refuses",
             igGtbRectVerdict(0u, 64u, 64u, 10u), IG_GTB_BLIT_RECT_INVERTED);

    // THE ORDER PIN: inverted on x, empty on y. INVERTED must win.
    checkU32("inverted beats empty",
             igGtbRectVerdict(64u, 10u, 10u, 10u), IG_GTB_BLIT_RECT_INVERTED);

    // Coordinates live in 16-bit halves of BR22/BR23. A coordinate above 65535
    // truncates into the neighbouring field, so the packet stays well formed
    // and blits somewhere else entirely.
    checkU32("right at the coord ceiling is OK",
             igGtbRectVerdict(0u, 0u, 65535u, 64u), IG_GTB_BLIT_OK);
    checkU32("right past the coord ceiling refuses",
             igGtbRectVerdict(0u, 0u, 65536u, 64u), IG_GTB_BLIT_COORD_TOO_LARGE);
    checkU32("bottom past the coord ceiling refuses",
             igGtbRectVerdict(0u, 0u, 64u, 65536u), IG_GTB_BLIT_COORD_TOO_LARGE);
    checkU32("left past the coord ceiling refuses",
             igGtbRectVerdict(65536u, 0u, 65600u, 64u), IG_GTB_BLIT_COORD_TOO_LARGE);
    checkU32("top past the coord ceiling refuses",
             igGtbRectVerdict(0u, 65536u, 64u, 65600u), IG_GTB_BLIT_COORD_TOO_LARGE);

    // THE OTHER ORDER PIN, and it runs the opposite way from the one intuition
    // suggests. Inversion is tested BEFORE the coordinate ceiling, so a
    // rectangle that is both inverted and out of range reports INVERTED. That
    // is the right call: right < left is a bug in the caller's arithmetic and
    // is true regardless of how the coordinates would encode, while the ceiling
    // is a limit of the 16-bit field somebody hits with an honest rectangle.
    //
    // Pinning this matters because the two orders are indistinguishable on
    // every input that is bad in only one way -- which is every other check in
    // this section.
    checkU32("inverted beats coord range",
             igGtbRectVerdict(65536u, 0u, 10u, 64u), IG_GTB_BLIT_RECT_INVERTED);

    // And the empty arm sits BETWEEN them: empty and out of range at once
    // reports EMPTY, because the ceiling test never runs on a rectangle that
    // describes no pixels at all.
    checkU32("empty beats coord range",
             igGtbRectVerdict(70000u, 0u, 70000u, 64u), IG_GTB_BLIT_EMPTY_RECT);

    checkU32("coord ceiling is 65535", IG_GTB_MAX_COORD_POLICY_65535, 65535u);
}

//
// 6. igGtbPitchVerdict() and igGtbSurfaceVerdict().
//
// The pitch ceiling is 32767 rather than 65535 because the field is SIGNED
// sixteen bits -- a pitch of 0x8000 encodes as negative and the engine strides
// backwards through memory. That is the failure mode this ceiling exists for,
// and it is why the ceiling is not the obvious 16-bit maximum.
//
// The surface check runs PITCH before ALIGNMENT, same reasoning as everywhere
// else in this tree: a pitch that cannot be encoded is a bug in this driver,
// while a misaligned address is a fact about the allocation somebody handed in.
// Report the one somebody can go and fix.
//
static void testPitchAndSurface()
{
    checkU32("ordinary pitch is OK", igGtbPitchVerdict(4096u), IG_GTB_BLIT_OK);
    checkU32("pitch at the ceiling is OK", igGtbPitchVerdict(32767u), IG_GTB_BLIT_OK);

    checkU32("zero pitch refuses", igGtbPitchVerdict(0u), IG_GTB_BLIT_PITCH_ZERO);
    checkU32("pitch past the signed ceiling refuses",
             igGtbPitchVerdict(32768u), IG_GTB_BLIT_PITCH_TOO_LARGE);
    checkU32("pitch at 0xFFFF refuses",
             igGtbPitchVerdict(0xFFFFu), IG_GTB_BLIT_PITCH_TOO_LARGE);

    checkU32("aligned surface is OK",
             igGtbSurfaceVerdict(0x00100000ull, 4096u), IG_GTB_BLIT_OK);

    checkU32("misaligned surface refuses",
             igGtbSurfaceVerdict(0x00100001ull, 4096u),
             IG_GTB_BLIT_ADDR_UNALIGNED);

    checkU32("surface carries the pitch refusal",
             igGtbSurfaceVerdict(0x00100000ull, 0u), IG_GTB_BLIT_PITCH_ZERO);

    // THE ORDER PIN: bad pitch AND misaligned address at once. Pitch wins.
    checkU32("pitch beats alignment",
             igGtbSurfaceVerdict(0x00100001ull, 0u), IG_GTB_BLIT_PITCH_ZERO);
    checkU32("pitch range beats alignment",
             igGtbSurfaceVerdict(0x00100001ull, 32768u), IG_GTB_BLIT_PITCH_TOO_LARGE);
}

//
// 7. igGtbWriteColorBlit() -- the seven-dword solid fill.
//
// Every dword is pinned by VALUE rather than by "is non-zero", because the
// failure this layer can produce is a well-formed packet that fills the wrong
// place. A packet with the right header and a transposed BR22 is structurally
// perfect and paints a rectangle nobody asked for.
//
// The Y-major packing of BR22/BR23 gets its own assertion for that reason: Y in
// the high half, X in the low half is what the XY_ in the opcode name means,
// and reversing it composes a valid packet that fills a transposed rectangle.
//
static void testWriteColorBlit()
{
    IGU32 buf[16];
    IGU32 written = 0xDEADBEEFu;
    IGU32 v;

    std::memset(buf, 0xCC, sizeof(buf));
    written = 0xDEADBEEFu;
    v = igGtbWriteColorBlit(buf, 16u,
                            0x0000000100200000ull, 4096u,
                            16u, 32u, 80u, 96u,
                            0xFF00FF00u, 0u,
                            &written);

    checkU32("fill verdict is OK", v, IG_GTB_BLIT_OK);
    checkU32("fill writes 7 dwords", written, 7u);

    // The writer ORs the RGBA write-mask flags into the HEADER word, not into
    // BR13. Pinning the header against igGtbHeader(..., 0u) would pass on a
    // writer that dropped the mask entirely -- and a packet with no write mask
    // enables no colour channels, so it executes cleanly and paints nothing.
    checkU32("fill dword0 header",
             buf[0],
             igGtbHeader(IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50,
                         igGtbLengthField(7u),
                         IG_GTB_WRITE_RGBA_POLICY_COMPOSED));

    // The composed mask is both siblings and nothing else. Spelling it as a
    // third literal is what the header refuses to do, so the test must not
    // either -- a corrected bit in one half has to move this check too.
    checkU32("RGBA mask is RGB | ALPHA",
             IG_GTB_WRITE_RGBA_POLICY_COMPOSED,
             IG_GTB_WRITE_RGB_I915_0x00100000 | IG_GTB_WRITE_ALPHA_I915_0x00200000);

    // A fill rasters COLOR_COPY, never SRC_COPY: a fill has no source, and
    // SRC_COPY here would raster against a surface the packet never names.
    checkU32("fill dword1 carries ROP COLOR_COPY",
             buf[1] & 0x00FF0000u, IG_GTB_ROP_COLOR_COPY_I915_0x00F00000);
    checkU32("fill dword1 carries the pitch", buf[1] & 0xFFFFu, 4096u);
    checkU32("fill dword2 is (top << 16) | left", buf[2], (32u << 16) | 16u);
    checkU32("fill dword3 is (bottom << 16) | right", buf[3], (96u << 16) | 80u);
    checkU32("fill dword4 is the address low half", buf[4], 0x00200000u);
    checkU32("fill dword5 is the address high half", buf[5], 0x00000001u);
    checkU32("fill dword6 is the colour", buf[6], 0xFF00FF00u);

    // The writer must not touch the dword past its own packet. A writer that
    // stored one too many would corrupt whatever the caller placed after it in
    // the ring, and "written" would still read 7.
    checkU32("fill leaves dword7 untouched", buf[7], 0xCCCCCCCCu);

    // Y-major packing, stated as its own claim rather than inferred from the
    // dword pins above: the HIGH half of BR22 is the Y coordinate.
    checkU32("BR22 high half is top", buf[2] >> 16, 32u);
    checkU32("BR22 low half is left", buf[2] & 0xFFFFu, 16u);
    checkU32("BR23 high half is bottom", buf[3] >> 16, 96u);
    checkU32("BR23 low half is right", buf[3] & 0xFFFFu, 80u);

    // The depth flag reaches dword1 alongside the pitch, and must not disturb
    // the pitch it shares the word with.
    std::memset(buf, 0xCC, sizeof(buf));
    written = 0xDEADBEEFu;
    v = igGtbWriteColorBlit(buf, 16u,
                            0x00100000ull, 4096u,
                            0u, 0u, 64u, 64u,
                            0x000000FFu, IG_GTB_DEPTH_32_I915_0x03000000,
                            &written);
    checkU32("fill with depth flag is OK", v, IG_GTB_BLIT_OK);
    checkU32("depth flag reaches dword1",
             buf[1] & IG_GTB_DEPTH_32_I915_0x03000000,
             IG_GTB_DEPTH_32_I915_0x03000000);
    checkU32("depth flag does not disturb the pitch", buf[1] & 0xFFFFu, 4096u);
}

//
// 8. igGtbWriteColorBlit() refusals.
//
// The span check runs BEFORE the argument checks, and that is the ordering this
// section exists to pin. A caller handing a three-dword span AND an inverted
// rectangle must be told the span is short: writing is what would scribble past
// the end of their array, while the rectangle is merely wrong. A subject that
// checked the rectangle first would return RECT_INVERTED, and the caller who
// then fixed the rectangle would get a store past the end of the buffer.
//
// outDwordsWritten is zeroed on EVERY refusal, including NULL_SPAN, which is
// the arm that returns before inspecting a single other argument. A caller that
// advanced a ring tail by a stale count on a refused compose would walk the
// tail into dwords the engine executes as commands.
//
static void testWriteColorBlitRefusals()
{
    IGU32 buf[16];
    IGU32 written;
    IGU32 v;

    written = 0xDEADBEEFu;
    v = igGtbWriteColorBlit(0, 16u, 0x00100000ull, 4096u,
                            0u, 0u, 64u, 64u, 0u, 0u, &written);
    checkU32("null span refuses", v, IG_GTB_BLIT_NULL_SPAN);
    checkU32("null span zeroes the count", written, 0u);

    written = 0xDEADBEEFu;
    v = igGtbWriteColorBlit(buf, 6u, 0x00100000ull, 4096u,
                            0u, 0u, 64u, 64u, 0u, 0u, &written);
    checkU32("span one short refuses", v, IG_GTB_BLIT_SPAN_TOO_SHORT);
    checkU32("short span zeroes the count", written, 0u);

    // Exactly seven is enough. The boundary is pinned on both sides, because a
    // `<=` written where `<` belongs refuses a span that fits exactly.
    written = 0u;
    v = igGtbWriteColorBlit(buf, 7u, 0x00100000ull, 4096u,
                            0u, 0u, 64u, 64u, 0u, 0u, &written);
    checkU32("span of exactly 7 is accepted", v, IG_GTB_BLIT_OK);
    checkU32("exact span writes 7", written, 7u);

    written = 0xDEADBEEFu;
    v = igGtbWriteColorBlit(buf, 16u, 0x00100000ull, 0u,
                            0u, 0u, 64u, 64u, 0u, 0u, &written);
    checkU32("zero pitch refuses", v, IG_GTB_BLIT_PITCH_ZERO);
    checkU32("pitch refusal zeroes the count", written, 0u);

    written = 0xDEADBEEFu;
    v = igGtbWriteColorBlit(buf, 16u, 0x00100001ull, 4096u,
                            0u, 0u, 64u, 64u, 0u, 0u, &written);
    checkU32("misaligned destination refuses", v, IG_GTB_BLIT_ADDR_UNALIGNED);
    checkU32("surface refusal zeroes the count", written, 0u);

    written = 0xDEADBEEFu;
    v = igGtbWriteColorBlit(buf, 16u, 0x00100000ull, 4096u,
                            64u, 0u, 16u, 64u, 0u, 0u, &written);
    checkU32("inverted rect refuses", v, IG_GTB_BLIT_RECT_INVERTED);
    checkU32("rect refusal zeroes the count", written, 0u);

    // THE ORDER PIN: a short span AND an inverted rectangle at once.
    written = 0xDEADBEEFu;
    v = igGtbWriteColorBlit(buf, 3u, 0x00100000ull, 4096u,
                            64u, 0u, 16u, 64u, 0u, 0u, &written);
    checkU32("span beats rect", v, IG_GTB_BLIT_SPAN_TOO_SHORT);

    // A refused compose must not have written anything at all. Checking the
    // count is not enough -- a subject that stored the header and then refused
    // would report 0 and still have scribbled.
    std::memset(buf, 0xCC, sizeof(buf));
    written = 0xDEADBEEFu;
    (void)igGtbWriteColorBlit(buf, 16u, 0x00100000ull, 4096u,
                              64u, 0u, 16u, 64u, 0u, 0u, &written);
    checkU32("refusal writes no dword0", buf[0], 0xCCCCCCCCu);
    checkU32("refusal writes no dword1", buf[1], 0xCCCCCCCCu);

    // A null out-param must not fault. The verdict is the return value, so a
    // caller with no use for the count is entitled to pass nothing.
    v = igGtbWriteColorBlit(buf, 16u, 0x00100000ull, 4096u,
                            0u, 0u, 64u, 64u, 0u, 0u, 0);
    checkU32("null out-param still returns a verdict", v, IG_GTB_BLIT_OK);
}

//
// 9. igGtbWriteCopyBlit() -- the ten-dword rectangle copy.
//
// TEN dwords, not eight. The previous revision of this file expected eight, and
// eight is the pre-Gen8 shape: it cannot hold two 64-bit addresses, two
// pitches, a destination rectangle and a source origin. Counting the fields is
// the whole argument -- header, dst pitch word, BR22, BR23, dst low, dst high,
// source origin, src pitch word, src low, src high is ten, and dropping any of
// them means the engine reads the next field from the wrong slot.
//
// The source ORIGIN is the field the old revision omitted entirely, and its
// absence is not a cosmetic gap: a copy with no source origin reads from
// wherever the previous packet left the field, which is a plausible address
// inside a real surface.
//
static void testWriteCopyBlit()
{
    IGU32 buf[20];
    IGU32 written = 0xDEADBEEFu;
    IGU32 v;

    std::memset(buf, 0xCC, sizeof(buf));
    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 20u,
                           0x0000000100200000ull, 4096u,
                           0x0000000200300000ull, 2048u,
                           16u, 32u, 80u, 96u,
                           8u, 24u,
                           0u,
                           &written);

    checkU32("copy verdict is OK", v, IG_GTB_BLIT_OK);
    checkU32("copy writes 10 dwords", written, 10u);

    // Same write-mask reasoning as the fill: the RGBA flags are ORed into the
    // HEADER word, so pinning against a bare 0u flags argument would pass on a
    // writer that dropped them and composed a copy enabling no channels.
    checkU32("copy dword0 header",
             buf[0],
             igGtbHeader(IG_GTB_OPCODE_XY_SRC_COPY_BLT_I915_0x53,
                         igGtbLengthField(10u),
                         IG_GTB_WRITE_RGBA_POLICY_COMPOSED));

    // A copy rasters SRC_COPY where the fill rasters COLOR_COPY, and the two
    // ROPs must not be interchanged: COLOR_COPY on a copy packet ignores the
    // source surface the other four dwords went to the trouble of naming, and
    // the result is a solid fill in the shape of the requested copy.
    checkU32("copy dword1 carries ROP SRC_COPY",
             buf[1] & 0x00FF0000u, IG_GTB_ROP_SRC_COPY_I915_0x00CC0000);
    checkU32("the two ROPs are distinct",
             IG_GTB_ROP_SRC_COPY_I915_0x00CC0000 ==
             IG_GTB_ROP_COLOR_COPY_I915_0x00F00000 ? 1u : 0u, 0u);
    checkU32("copy dword1 carries the destination pitch", buf[1] & 0xFFFFu, 4096u);
    checkU32("copy dword2 is (top << 16) | left", buf[2], (32u << 16) | 16u);
    checkU32("copy dword3 is (bottom << 16) | right", buf[3], (96u << 16) | 80u);
    checkU32("copy dword4 is the destination low half", buf[4], 0x00200000u);
    checkU32("copy dword5 is the destination high half", buf[5], 0x00000001u);
    checkU32("copy dword6 is (srcTop << 16) | srcLeft", buf[6], (24u << 16) | 8u);
    checkU32("copy dword7 carries the source pitch", buf[7] & 0xFFFFu, 2048u);
    checkU32("copy dword8 is the source low half", buf[8], 0x00300000u);
    checkU32("copy dword9 is the source high half", buf[9], 0x00000002u);

    checkU32("copy leaves dword10 untouched", buf[10], 0xCCCCCCCCu);

    // The two pitches must land in DIFFERENT words. A writer that stored the
    // source pitch over the destination pitch composes a packet that strides
    // both surfaces at the source rate -- valid, and wrong in a way that looks
    // like a tiling bug on screen.
    checkU32("destination and source pitches are distinct words",
             (buf[1] & 0xFFFFu) == (buf[7] & 0xFFFFu) ? 1u : 0u, 0u);

    // Source origin is Y-major too, same as the destination rectangle.
    checkU32("source origin high half is srcTop", buf[6] >> 16, 24u);
    checkU32("source origin low half is srcLeft", buf[6] & 0xFFFFu, 8u);

    // The two addresses must not be swapped. Pinning them by value above
    // already catches it, but the high halves differ by design here (1 vs 2)
    // precisely so a swap cannot pass by both halves happening to agree.
    checkU32("destination high differs from source high",
             buf[5] == buf[9] ? 1u : 0u, 0u);

    // Depth flag reaches the destination pitch word, same as the fill.
    std::memset(buf, 0xCC, sizeof(buf));
    written = 0u;
    v = igGtbWriteCopyBlit(buf, 20u,
                           0x00200000ull, 4096u,
                           0x00300000ull, 4096u,
                           0u, 0u, 64u, 64u,
                           0u, 0u,
                           IG_GTB_DEPTH_32_I915_0x03000000,
                           &written);
    checkU32("copy with depth flag is OK", v, IG_GTB_BLIT_OK);
    checkU32("copy depth flag reaches dword1",
             buf[1] & IG_GTB_DEPTH_32_I915_0x03000000,
             IG_GTB_DEPTH_32_I915_0x03000000);
    checkU32("copy depth flag does not disturb the destination pitch",
             buf[1] & 0xFFFFu, 4096u);
}

//
// 10. igGtbWriteCopyBlit() refusals -- and the SOURCE surface arm.
//
// A bad source with a good destination must refuse. This is the arm a
// destination-only subject leaves open: every check in section 8 passes on a
// writer that never looks at the source surface at all, because the destination
// is well formed in all of them. The separating input is a healthy destination
// beside a source with a zero pitch or a misaligned address.
//
static void testWriteCopyBlitRefusals()
{
    IGU32 buf[20];
    IGU32 written;
    IGU32 v;

    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(0, 20u, 0x00200000ull, 4096u, 0x00300000ull, 4096u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy null span refuses", v, IG_GTB_BLIT_NULL_SPAN);
    checkU32("copy null span zeroes the count", written, 0u);

    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 9u, 0x00200000ull, 4096u, 0x00300000ull, 4096u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy span one short refuses", v, IG_GTB_BLIT_SPAN_TOO_SHORT);
    checkU32("copy short span zeroes the count", written, 0u);

    written = 0u;
    v = igGtbWriteCopyBlit(buf, 10u, 0x00200000ull, 4096u, 0x00300000ull, 4096u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy span of exactly 10 is accepted", v, IG_GTB_BLIT_OK);
    checkU32("copy exact span writes 10", written, 10u);

    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 20u, 0x00200000ull, 0u, 0x00300000ull, 4096u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy zero destination pitch refuses", v, IG_GTB_BLIT_PITCH_ZERO);
    checkU32("copy destination pitch refusal zeroes the count", written, 0u);

    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 20u, 0x00200001ull, 4096u, 0x00300000ull, 4096u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy misaligned destination refuses", v, IG_GTB_BLIT_ADDR_UNALIGNED);

    // THE SOURCE ARM. Destination is healthy in both of these.
    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 20u, 0x00200000ull, 4096u, 0x00300000ull, 0u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy zero SOURCE pitch refuses", v, IG_GTB_BLIT_PITCH_ZERO);
    checkU32("copy source refusal zeroes the count", written, 0u);

    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 20u, 0x00200000ull, 4096u, 0x00300001ull, 4096u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy misaligned SOURCE refuses", v, IG_GTB_BLIT_ADDR_UNALIGNED);
    checkU32("copy source alignment refusal zeroes the count", written, 0u);

    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 20u, 0x00200000ull, 4096u, 0x00300000ull, 32768u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy SOURCE pitch past the signed ceiling refuses",
             v, IG_GTB_BLIT_PITCH_TOO_LARGE);

    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 20u, 0x00200000ull, 4096u, 0x00300000ull, 4096u,
                           64u, 0u, 16u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy inverted rect refuses", v, IG_GTB_BLIT_RECT_INVERTED);
    checkU32("copy rect refusal zeroes the count", written, 0u);

    // Source origin is a coordinate too, and lives in the same 16-bit halves.
    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 20u, 0x00200000ull, 4096u, 0x00300000ull, 4096u,
                           0u, 0u, 64u, 64u, 65536u, 0u, 0u, &written);
    checkU32("copy source origin past the coord ceiling refuses",
             v, IG_GTB_BLIT_COORD_TOO_LARGE);

    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 20u, 0x00200000ull, 4096u, 0x00300000ull, 4096u,
                           0u, 0u, 64u, 64u, 0u, 65536u, 0u, &written);
    checkU32("copy source origin top past the coord ceiling refuses",
             v, IG_GTB_BLIT_COORD_TOO_LARGE);

    // THE ORDER PIN: short span AND a bad source at once. Span wins, for the
    // same reason as the fill -- writing is what scribbles.
    written = 0xDEADBEEFu;
    v = igGtbWriteCopyBlit(buf, 3u, 0x00200000ull, 4096u, 0x00300000ull, 0u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy span beats source", v, IG_GTB_BLIT_SPAN_TOO_SHORT);

    // A refused copy must not have written anything.
    std::memset(buf, 0xCC, sizeof(buf));
    written = 0xDEADBEEFu;
    (void)igGtbWriteCopyBlit(buf, 20u, 0x00200000ull, 4096u, 0x00300000ull, 0u,
                             0u, 0u, 64u, 64u, 0u, 0u, 0u, &written);
    checkU32("copy refusal writes no dword0", buf[0], 0xCCCCCCCCu);
    checkU32("copy refusal writes no dword6", buf[6], 0xCCCCCCCCu);

    v = igGtbWriteCopyBlit(buf, 20u, 0x00200000ull, 4096u, 0x00300000ull, 4096u,
                           0u, 0u, 64u, 64u, 0u, 0u, 0u, 0);
    checkU32("copy null out-param still returns a verdict", v, IG_GTB_BLIT_OK);
}

int main(void)
{
    std::printf("IntelGtBlt.hpp -- Gen12 2D blitter packet composition\n");

    testCitedConstants();
    testHeaderComposition();
    testLengthField();
    testVerdictFamily();
    testRectVerdict();
    testPitchAndSurface();
    testWriteColorBlit();
    testWriteColorBlitRefusals();
    testWriteCopyBlit();
    testWriteCopyBlitRefusals();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

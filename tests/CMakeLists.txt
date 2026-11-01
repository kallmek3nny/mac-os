#
# Host-side tests for the kext's pure decode logic.
#
# This builds with MSVC on Windows, GCC/Clang on Linux, or Apple clang on
# macOS. It deliberately does NOT try to build the kext -- that needs Apple's
# clang with -mkernel -fapple-kext and the Kernel.framework headers, neither of
# which exists off macOS.
#
# What it does test is NVDecode.hpp: every register constant and every
# bit-field decode. That is the part of this project that has actually been
# wrong, twice, and it is testable anywhere.
#
# Usage from the project root:
#   cmake -S tests -B tests/build -G Ninja
#   cmake --build tests/build
#   ./tests/build/test_decode
#

cmake_minimum_required(VERSION 3.20)
project(nv3050_decode_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The headers live with the kext, not with the tests -- both compile the same
# files, which is the entire point.
#
# The absolute path to the vendored GA107 firmware, injected as a macro.
#
# test_riscv_ucode.cpp opens those files when they are present and SKIPS with a
# banner when they are not. A path relative to the binary's cwd would depend on
# where ctest was invoked from, and a wrong path SKIPS rather than fails -- the
# vacuity direction, so it has to come from the source tree at configure time.
#
set(NV_FIRMWARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../reference/ga107-firmware")

function(add_kext_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../phase0-kext)
    target_compile_definitions(${name} PRIVATE NV_FIRMWARE_DIR="${NV_FIRMWARE_DIR}")

    if(MSVC)
        # /W4 rather than /Wall: /Wall enables per-object padding warnings from
        # the Windows SDK headers themselves and buries real findings.
        target_compile_options(${name} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME ${name} COMMAND ${name})
endfunction()

enable_testing()

# Phase 3 / 3.5 -- identity and firmware-health decoding.
add_kext_test(test_decode test_decode.cpp)

# Phase 4 -- Falcon / RISC-V / GSP register layer and address translation.
add_kext_test(test_falcon test_falcon.cpp)

# Phase 4 -- RM_RISCV_UCODE_DESC parsing for the GA107 GSP firmware blobs.
add_kext_test(test_riscv_ucode test_riscv_ucode.cpp)

# Phase 4 -- the GSP bringup state machine itself, driven against a simulated
# BAR0. This is the only test that exercises a WRITE sequence, so it is also
# the only place the write ORDER (MOD_SEL last, BCR_CTRL after it) is pinned.
add_kext_test(test_gsp_boot test_gsp_boot.cpp)

# Phase 5 -- the staging arithmetic behind loadGspFirmware(): chunk padding,
# the 41-bit address window, and the blob-relative to file-relative offset
# translation. Before this suite NVGspStage.hpp was covered only by the clang
# front-end parse, which proves it compiles and nothing about the numbers.
add_kext_test(test_gsp_stage test_gsp_stage.cpp)

# Intel Gen12 (Tiger Lake) display engine -- transcoder/pipe offset TABLES, the
# value-1 timing convention, and TRANSCONF's pipe-block-vs-transcoder-index
# hazard. Constants from reference/i915/ at Linux v6.12, not from any dump.
add_kext_test(test_intel_display test_intel_display.cpp)

# Intel Gen12 device identification -- the PCI vendor/device match predicate and
# the eleven Tiger Lake IDs transcribed from reference/i915/i915_pciids.h at
# Linux v6.12. Every ID is pinned to a BARE LITERAL rather than to the #define
# that produced it, because an accessor checked against its own #define passes
# under any value; that exact defect has already cost this project fifteen
# undetectable offsets. The one observed value (0x9A68, from the 2026-08-16
# capture) is pinned separately and then asserted to AGREE with the vendored
# table, so two matching errors cannot cancel.
add_kext_test(test_intel_gpu_id test_intel_gpu_id.cpp)

# Intel Gen12 plane register DECODE -- framebuffer inheritance. Distinct from
# test_intel_display, which covers ADDRESSING (where is register X); this covers
# the 32-bit VALUES those registers hold. Every mask, shift and format code is
# pinned to a BARE LITERAL for the same reason as the suite above.
#
# Two arms here cannot be driven from the 2026-08-16 capture at all, and the
# suite says so in its own header comment: the observed CTL 0x82009000 has bit
# 15 SET, so igFbInheritable() early-returns COMPRESSED and never reaches
# format, stride or geometry; and its bit 23 is CLEAR, so the 5-bit icl+ format
# mask and the 4-bit pre-icl mask both decode it to a supported 4-byte format
# and agree. Synthetic SEPARATING inputs -- bit 15 cleared, bit 23 set -- are
# what make those arms non-vacuous.
add_kext_test(test_intel_framebuffer test_intel_framebuffer.cpp)

# Intel Gen12 verdict NAMES and IORegistry property-key composition. These lived
# as file statics inside IntelDisplayProbe.cpp until 2026-08-17, i.e. inside the
# one Intel-track translation unit that includes <IOKit/IOService.h> and so cannot
# be compiled here at all -- which meant nothing ever EXECUTED them. The parse
# gate proved they compile; the register gate checks constants; no binary called a
# single one. A case arm returning a neighbouring family member's literal was
# invisible to all four gates.
#
# That gap held a real defect: igModeVerdictName() had arms for verdicts 0 through
# 5 while IG_MODE_VERDICT_LIMIT is 6, so IG_MODE_SRC_MISMATCH -- PIPESRC
# disagreeing with the active region, the byte-swapped PLANE_SIZE/PIPESRC shape
# handoff15 measured on silicon -- published the unknown string. This suite walks
# each family to its own _VERDICT_LIMIT and refuses that string inside the range,
# so the next verdict added without an arm reddens here rather than on a laptop.
add_kext_test(test_intel_display_names test_intel_display_names.cpp)

# Intel Gen12 SCANOUT descriptor -- the last purely-arithmetic step before an
# IOFramebuffer subclass. Distinct from test_intel_framebuffer, which decodes
# what a plane register MEANS; this covers whether the plane it describes can be
# handed to IOFramebuffer at all: base, stride, byte EXTENT, and whether that
# extent fits the memory this driver actually mapped.
#
# Two properties here cannot be established by inspection and are the reason the
# suite exists. First, the two multiplications on this path (bytesPerRow *
# height, width * bytesPerPixel) WRAP rather than saturate in 32 bits, and the
# wrap direction is the dangerous one -- a truncated extent is SMALLER, and a
# small extent passes every containment test there is. Operands whose exact
# product exceeds 0xFFFFFFFF are driven so a header that widened after the
# multiply instead of before it reddens. Second, igScanDescribe()'s refusal
# ORDER is load-bearing at every step: the MASKED sentinel 0xFFFFF000 IS 4 KiB
# aligned, a zero width makes the minimum row zero so every stride satisfies
# >= 0, and a wrapped extent fits any aperture. Each ordering claim is pinned by
# an input that is bad in TWO ways at once, because an input bad in only one way
# cannot distinguish the order from any other.
add_kext_test(test_intel_scanout test_intel_scanout.cpp)

# Intel Gen12 driver-owned scanout SINK -- the surface the driver allocates in
# the GGTT for a plane flip to target, so the 3050 side has a DMA destination
# that is not the GOP framebuffer (intel-handoff40 section 7 (a)). Same refusal
# discipline as the scanout extent: each arm reached by a singly-bad input, each
# numeric test pinned at its last accepting value, order pinned by doubly-bad
# inputs.
add_kext_test(test_intel_sink_surface test_intel_sink_surface.cpp)

# Intel sink PUBLISH -- a sound sink surface plus the caller's claim of one
# contiguous page-aligned bus range turned into the descriptor the 3050 side
# reads, or a refusal. Same discipline as the sink surface: each arm reached by
# a singly-bad input, order pinned by doubly-bad inputs, and a range ending on
# the last byte of the address space accepted rather than called a wrap.
add_kext_test(test_intel_sink_publish test_intel_sink_publish.cpp)

# Intel plane PROGRAM -- the register set that points a primary plane at a
# driver-owned linear surface (the sink above), so a flip can retarget scanout
# away from the GOP framebuffer. Inverse of test_intel_framebuffer's decode:
# every encoded field must decode back through IntelFramebuffer.hpp. Each
# refusal arm reached by a singly-bad input, order pinned by doubly-bad inputs,
# and `out` proven untouched on every refusal.
add_kext_test(test_intel_plane_program test_intel_plane_program.cpp)

# Intel sink ALLOCATION -- where in the GGTT the driver-owned surface the 3050
# renders into goes: base rounded up past the reserved (GOP/stolen) range to
# 256 KiB, stride padded to 64 bytes, size = stride * height. Each refusal arm
# reached by a singly-bad input, order pinned by doubly-bad inputs, `out`
# proven untouched on every refusal, and a surface ending exactly at the GGTT
# end accepted rather than called a wrap.
add_kext_test(test_intel_sink_alloc test_intel_sink_alloc.cpp)

# Intel sink REHEARSAL -- step (b) composed over a firmware plane read: the
# reserved end (main surface, plus the AUX surface when the plane is
# compressed), the alloc plan past it, and the plane words a flip would write.
# Pure; IntelDisplayProbe publishes it and writes nothing. Sentinels on every
# refusal, each arm reached by a singly-bad input, and an agreement sweep that
# pins PLANE_REFUSED as unreachable from a sound plan.
add_kext_test(test_intel_sink_rehearse test_intel_sink_rehearse.cpp)

# Intel Gen12 PIXEL description -- the format code a plane carries turned into the
# fields IOPixelInformation holds, or a refusal. It sits between
# test_intel_framebuffer (what does PLANE_CTL MEAN) and an eventual IOFramebuffer
# subclass (what do we TELL IOKit).
#
# The reason it is its own suite: igFbBytesPerPixel() returns 4 for BOTH
# IG_FB_FORMAT_XRGB_8888 and IG_FB_FORMAT_XRGB_2101010, and those two are not
# interchangeable to IOKit -- 8 bits per component with masks
# 0x00FF0000/0x0000FF00/0x000000FF versus 10 bits with 0x3FF00000/0x000FFC00/
# 0x000003FF. A component width DERIVED from bytes-per-pixel is therefore right
# for one format and wrong for the other, and the wrong direction is the quiet
# one: describing a 2101010 surface as 8-8-8 programs cleanly and yields a
# picture whose COLOURS are wrong rather than one that is obviously broken --
# handoff15's byte-swapped PLANE_SIZE hazard in a second place. Section 2 pins
# the collision and pins that the component widths do not collide, so an edit
# deriving one from the other reddens here.
#
# Two further properties cannot be established by inspection. Describability is a
# WHITELIST BY NAME rather than a range, and the codes probed (3 and 5) are the
# ones that sit BETWEEN the two describable codes, so a range test admits them
# and reddens; a range would otherwise sweep every future Gen12 format in as
# describable and hand IOKit a component layout nobody chose. And every refusal
# path writes IG_PIXEL_INVALID32 into its out-params rather than leaving them
# untouched -- section 6b pre-loads them with 0x11111111 and with zero, because
# an untouched out-param carries the caller's initialiser and a zero mask is
# indistinguishable from a channel that is genuinely absent.
add_kext_test(test_intel_pixel test_intel_pixel.cpp)

# Intel Gen12 framebuffer PRESENTATION join -- the four plane words plus an
# aperture size turned into the numbers an IOFramebuffer subclass hands IOKit, or
# a refusal. Everything it computes is owned by a sibling header
# (IntelFramebuffer.hpp decodes, IntelScanout.hpp measures the extent,
# IntelPixel.hpp chooses the component layout); what it adds is the JOIN and the
# ORDER, and both are invisible to the three suites above.
#
# The reason it is its own suite rather than lines in one of theirs: the property
# it exists to hold is that NO accessor can answer for a group the verdict
# refused. Every occurrence of this tree's fabricated-publication class has been a
# caller that had a refusal available and did not consult it -- five times, in two
# tracks, in code whose own comments explained why it mattered. Section 7 walks
# all eight accessors against all eight refusing input classes, so an edit that
# simplifies the re-derivation out of any one accessor reddens rather than
# quietly restoring the class.
#
# Two ordering claims cannot be established by inspection and are each pinned by
# an input bad in TWO ways at once. A refused read is 0xFFFFFFFF, which has
# PLANE_CTL's enable bit SET and masks to a 4 KiB-aligned surface address -- so a
# layout-first or alignment-first order reports a plane nobody read as an enabled,
# well-aligned one. And SCANOUT is tested before PIXEL because a scanout refusal
# is fixable (a decode bug, or a mapping somebody can make bigger) while a pixel
# refusal is a permanent fact about IOKit's vocabulary; the same RGB_565 plane is
# driven with a zero aperture and with a good one, so the check proves an order
# rather than an unreachable arm.
#
# This header is NOT one of the two Intel headers tools/check-registers.py parses,
# so IG_PRESENT_ is the one Intel verdict family with no set-check from the
# register gate at all. Section 2's contiguity, distinctness, limit-equality and
# member-count loops are the only thing holding that relation.
add_kext_test(test_intel_fb_present test_intel_fb_present.cpp)

# Intel Gen12 render-engine COMMAND SUBMISSION -- the first suite in this tree
# that is not about display at all. Engine MMIO bases, the ring register block,
# ring geometry verdicts, head/tail wrap arithmetic, MI command word composition
# and the refusals that stand in front of a submission.
#
# Its evidence story is deliberately split and the header says so in its own
# comment: the four engine bases and the +0x30 ring block ARE citable to
# reference/i915/i915_reg.h (RENDER_RING_BASE at :289, and PRB0_BASE spelled
# (0x2030 - 0x30), which is what fixes the ring block above the base rather than
# at it), while the MI opcode family is POLICY, because i915 keeps it in gt/
# headers that were never vendored here. A green run therefore proves the
# composition is self-consistent and proves NOTHING about whether the hardware
# wants these words.
#
# No *_OBSERVED_* constant may enter that header: igpu-results/ holds display
# registers captured from a machine whose render engine was idle, so there is no
# MMIO capture of a running render engine in this tree and inventing one would
# file a guess beside values witnessed on silicon.
#
# Three ordering claims cannot be established by inspection and are each pinned
# by an input bad in TWO ways at once. Ring geometry is judged before head/tail,
# because a head is only out of the ring relative to a size that was never
# legal. Tail RANGE is judged before tail ALIGNMENT, since alignment of a value
# outside the buffer is not a meaningful complaint. And length == 0 is tested
# before the command-word multiple, because zero is trivially a multiple and a
# multiple-first order makes IG_GT_SUBMIT_EMPTY unreachable -- the dead-arm shape
# 3050-handoff32.md section 3 exists to remove, reintroduced one subsystem over.
#
# Like IntelFbPresent.hpp this header is NOT one of the two Intel headers
# tools/check-registers.py parses, so IG_GT_SUBMIT_ has no set-check from the
# register gate. The verdict-name loop bounds on IG_GT_SUBMIT_LIMIT rather than a
# typed count, so a family that grows reddens on the commit that grows it.
add_kext_test(test_intel_gt_cmd test_intel_gt_cmd.cpp)

# Intel Gen12 EXECLIST submission and GTT address composition -- the layer
# directly above IntelGtCmd.hpp. That header describes a ring; this one covers
# how a ring is handed to the hardware scheduler (the execlist port registers,
# the 64-bit context descriptor and its two written halves) and how the pages a
# ring points at are made visible to the engine (page-table entries and GTT
# entry offsets).
#
# ITS EVIDENCE STORY IS WEAKER THAN EVERY OTHER SUITE IN THIS FILE, AND THE
# SUITE SAYS SO IN ITS OWN BANNER: every constant in IntelGtExec.hpp is POLICY.
# reference/i915/ holds seven files and all seven are display -- greps for
# EXECLIST, ELSP, LRC, CTX_DESC, GEN8_PDE, GEN8_PTE, PPAT and GGTT across that
# whole directory return ZERO matches, measured 2026-08-26. i915 keeps the
# execlist port, the context descriptor layout and the page-table entry format
# in gt/ headers that were never vendored here. So unlike its sibling, which
# has a cited half (the engine bases and the ring block ARE in i915_reg.h),
# this header has no cited half at all and a green run establishes that the
# composition is self-consistent and NOTHING about whether silicon agrees.
#
# No *_OBSERVED_* constant may enter that header for the same reason as its
# sibling: igpu-results/ captured a machine whose render engine was idle, so
# there is no MMIO evidence of a running execlist port in this tree.
#
# Two sentinel choices here are the INVERSE of the rest of the project and both
# are load-bearing. A refused context descriptor and an absent page-table entry
# each answer ZERO rather than all-ones, because bit 0 of both words is the
# valid/present bit -- so zero is invalid by the hardware's own definition
# while all-ones is a VALID entry pointing at the top of the address space.
# Section 6 drives an all-ones descriptor through both halves to pin that the
# refusal is keyed on that bit rather than on a hand-rolled sentinel compare.
#
# Three ordering claims cannot be established by inspection and are each pinned
# by an input bad in TWO ways at once. LRCA range is judged before LRCA
# alignment, since the alignment of an address the field cannot hold is not a
# meaningful complaint. The context is judged whole before the ring it would
# run from. And RING GEOMETRY IS JUDGED BEFORE THE PORT: a ring defect is a bug
# in this driver, while a busy port is a fact about the machine one millisecond
# from now -- report the one somebody can go and fix, which is handoff22
# section 1b's ordering rule applied to a transient rather than to a mapping.
#
# The port-busy polarity is the most dangerous single bit in the header, so it
# is pinned on both sides plus the refused-read case: a refused MMIO read is
# all-ones, which has the busy bit SET, so a refusal reports BUSY -- the safe
# direction, because busy means do not write the queue.
#
# Like its sibling this header is NOT one of the two Intel headers
# tools/check-registers.py parses, so IG_GTX_READY_ has no set-check from the
# register gate. The verdict-name loop bounds on IG_GTX_READY_LIMIT rather than
# a typed count, so a family that grows reddens on the commit that grows it.
add_kext_test(test_intel_gt_exec test_intel_gt_exec.cpp)

# test_intel_gt_context drives the LOGICAL RING CONTEXT image layer directly
# above IntelGtExec.hpp, and it carries the same weak evidence story as that
# suite -- weaker, in fact, because its subject has NO cited half at all.
# reference/i915/ holds seven files and all seven are display; greps for LRC,
# CTX_DESC, RING_BUFFER_START, RING_BUFFER_CTL, BB_PER_CTX and CTX_TIMESTAMP
# across that whole directory return ZERO matches (measured 2026-08-26). So a
# green run here establishes that the header's arithmetic is self-consistent and
# that its refusals fire in the documented order. It establishes nothing about
# Gen12 silicon, and the suite prints that caveat above its own count so a
# reader of the output cannot miss it.
add_kext_test(test_intel_gt_context test_intel_gt_context.cpp)

# test_intel_gt_image drives the LOGICAL RING CONTEXT image WRITER directly above
# IntelGtContext.hpp, and it carries the same weak evidence story as that suite
# and as test_intel_gt_exec: its subject has NO cited half at all.
# reference/i915/ holds seven files and all seven are display; greps for LRC,
# CTX_DESC, RING_BUFFER_START, RING_BUFFER_CTL, BB_PER_CTX and CTX_TIMESTAMP
# across that whole directory return ZERO matches (measured 2026-08-26).
#
# The reason it is its own suite rather than lines in test_intel_gt_context: it
# is the first thing in the GT stack that MUTATES CALLER MEMORY, and that admits
# a failure mode pure arithmetic does not have -- a refusal that has already
# written. A half-composed page is not a broken page, it is a well-formed image
# describing a ring nobody configured, which is the quiet direction because it
# programs cleanly. So the span is POISONED with a value the writer cannot
# produce at any slot, and after every one of the nine refusal classes the WHOLE
# span is asserted still poisoned. A suite asserting only the return code cannot
# distinguish those two outcomes at all.
#
# Two ordering claims cannot be established by inspection and are each pinned by
# an input bad in TWO ways at once. NULL is judged before LENGTH, because a count
# is a promise about memory that does not exist and a length-first order would
# have to accept a legal count and fall through to dereference null. LENGTH is
# judged before the IMAGE verdict, since an image that could not have been
# written into this span in any case is a true complaint about the wrong thing.
#
# Section 2 asserts slots 2 and 3 are STILL POISONED on the success path. That is
# what separates a writer placing its pairs at the documented slots from one
# packing them contiguously from the first pair slot -- both fill ten dwords, and
# only the poison check tells them apart.
#
# Like the three headers below it this one is NOT parsed by tools/check-registers.py,
# so IG_GTI_WRITE_ has no set-check from the register gate. The verdict-name loop
# bounds on IG_GTI_WRITE_LIMIT rather than a typed count, so a family that grows
# reddens on the commit that grows it.
add_kext_test(test_intel_gt_image test_intel_gt_image.cpp)

# test_intel_gt_blt drives the Gen12 2D BLITTER packet composer, and it is the
# first suite in the GT stack whose subject has a CITED half again. Unlike
# IntelGtExec.hpp, IntelGtContext.hpp and IntelGtImage.hpp -- whose constants are
# all policy, because reference/i915/ holds seven files and all seven are display
# -- IntelGtBlt.hpp cites reference/i915/intel_gpu_commands.h for the three
# opcodes (XY_COLOR_BLT 0x50, XY_SRC_COPY_BLT 0x53, COLOR_BLT 0x40), the blitter
# client (INSTR_BC_CLIENT 2) and the client/opcode shift policy. Section 1 pins
# those values, which is the half a hand-transcription error would corrupt
# silently: the register gate checks that the tokens EXIST, never what they equal.
#
# What a green run does NOT establish: that any packet composed here would do
# anything. No dword written by this header has ever been submitted to an engine,
# there is no ring, no context and no GGTT binding behind it, and a packet that is
# byte-perfect against upstream's macros while being semantically wrong about
# Gen12 blitter behaviour passes every check in the file. The suite pins
# COMPOSITION, not execution, and its own banner says so.
#
# Two properties get disproportionate attention and both are ordering rather than
# value pins. THE SPAN CHECK RUNS BEFORE THE SURFACE AND RECT CHECKS: a caller
# handing a three-dword span and an inverted rectangle must be told the span is
# too short, because the span is what would scribble while the rectangle is merely
# wrong -- a subject checking the rectangle first answers RECT_INVERTED, and the
# caller who then fixes the rectangle gets a write past the end of a three-dword
# array. tests/test_check_count_floor.py records that exact class being found in
# test_intel_gt_image by a stack-corruption crash rather than by an assertion;
# this suite pins the order instead of waiting for the crash. And
# outDwordsWritten is zeroed on EVERY refusal path, including the ones refusing
# before any argument is inspected, because a caller advancing a ring tail by a
# stale count on a refused compose walks the tail into dwords the engine will
# execute as commands.
#
# The copy writer validates BOTH surfaces. A destination-only subject composes a
# packet that reads from an unaligned source, and that packet is otherwise well
# formed -- invisible to every check except the one driving a bad source past a
# good destination.
#
# Like the three headers above it this one is NOT parsed by
# tools/check-registers.py, and the reason is sharper here than for its siblings:
# a bare IG_GTB_ prefix sweep would drag in the client, opcode, shift, mask,
# coordinate-ceiling and dword-count constants alongside the verdicts and compute
# a nonsense family maximum -- handoff16 section 1's IG_PIPE_TRANSCONF trap
# exactly. Any arm added there must select IG_GTB_BLIT_ explicitly. The
# verdict-name loop bounds on IG_GTB_BLIT_LIMIT rather than a typed count, so a
# family that grows reddens on the commit that grows it.
add_kext_test(test_intel_gt_blt test_intel_gt_blt.cpp)

# test_intel_gt_batch drives IntelGtBatch.hpp, the JOIN between the packet layer
# and the delivery layer -- the first subject in this tree that is about neither
# one. IntelGtBlt.hpp composes what the engine would DO; IntelGtCmd.hpp composes
# the ring geometry and the MI commands that DELIVER it. Nothing joined them, so
# a caller holding a composed blit had no admissible way to ask where it goes,
# what terminates it, and what the ring must hold for the engine to find it.
#
# Every constant in the subject is POLICY and the header says so: reference/i915/
# carries no batch-assembly file in this tree, and igpu-results/ captured a
# machine whose render engine was idle, so there is nothing about batch assembly
# to observe. No constant here is _OBSERVED_ and none is invented to look cited.
#
# The verdict prefix is IG_GTQ_ rather than IG_GTB_ ON PURPOSE. intel_verdict_
# set_checks() in tools/check-registers.py groups families BY PREFIX, so a
# family sharing the blitter's prefix would sweep the blitter's constants into
# this one's contiguity check -- handoff16 section 1's IG_PIPE_TRANSCONF trap,
# which printed a failure correct about its own arithmetic and wrong about the
# world. Neither header is parsed by the register gate, so IG_GTQ_ has no
# set-check there either; the verdict-name loop in section 3 bounds on
# IG_GTQ_BATCH_LIMIT rather than a typed count, so a family that grows reddens
# on the commit that grows it.
#
# The load-bearing assertions are the OUT-PARAM ones rather than the return
# values. A ring tail advanced by a count left over from a refused compose walks
# the tail into dwords the engine executes as commands, so every refusal arm
# asserts a written count of zero and an untouched span -- and section 8's
# no-room-to-terminate case is the sharpest of them, because the BLIT succeeded
# there and reporting its count is exactly what would dispatch a batch with no
# MI_BATCH_BUFFER_END in it.
add_kext_test(test_intel_gt_batch test_intel_gt_batch.cpp)

# test_intel_gt_ring drives IntelGtRing.hpp, the WRAPPING STORE -- the one layer
# beneath the batch join that actually puts dwords into a circular span. Until
# this target existed the header was 413 lines that no suite drove, no CMake
# target compiled, and no shell called, which by intel-handoff26 section 1's own
# measurement is a header nothing has ever run.
#
# What belongs here rather than in test_intel_gt_cmd is everything about a STORE.
# IntelGtCmd.hpp owns the ring arithmetic and its suite covers it; the arithmetic
# cannot express the claim that the second half of the payload lands at index
# zero, and that is the property this subject exists for.
#
# Three assertions carry the weight, and in each case the VERDICT is identical
# under the bug and under the fix:
#   1. Every refusal arm runs against a canary-filled ring and asserts the WHOLE
#      span is pristine. A function that stores and then refuses has already
#      left an executable opcode in a ring the caller believes untouched --
#      handoff18 section 1's readCount == 0 reasoning, one subsystem over.
#   2. The wrap is pinned by the CONTENT of the low slots. A store that clamps
#      instead of wrapping returns the same verdict and the same new tail while
#      leaving the tail of the payload unwritten.
#   3. The MI_NOOP pad is inside the wrap. A pad written with a non-wrapping
#      cursor stores past the end of the caller's span, and the verdict is OK
#      either way.
#
# The batch-start group pins PROPAGATION rather than a value: the batch layer's
# refusal comes back through an out-param while the ring verdict says only what
# the ring did. Collapsing a misaligned batch address into the ring's retryable
# NOT_ADMISSIBLE sends the caller to the wrong layer and invites an infinite
# retry, which is why that group asserts the ring verdict is NOT that value.
#
# The pad group asserts a PROPERTY (the padded end is eight-byte aligned) plus
# minimality, rather than a table of expected pad counts -- a table can be
# written to match whatever the function currently returns, while the alignment
# rule is what the tail register actually requires one layer down.
add_kext_test(test_intel_gt_ring test_intel_gt_ring.cpp)

# test_intel_gt_fence drives IntelGtFence.hpp, the COMPLETION layer -- the one
# that answers how this driver would ever learn that submitted work finished.
# Every layer beneath it composes work or delivers it; none of them reports.
# Until this target existed the header had no suite, no CMake entry, no syntax
# line and no caller, which is intel-handoff26 section 1's measurement exactly:
# a pure header nothing runs.
#
# The properties here are the ones that can be wrong while composing a
# WELL-FORMED packet, which intel-handoff35 section 2 names as the dangerous
# direction -- a malformed page is caught by the next thing that reads it and a
# well-formed wrong one is not:
#   1. igGtfFenceSucceeded() is an EQUALITY, so exactly ONE verdict satisfies it.
#      A bool is satisfied by every refusal at once (handoff34 section 1).
#   2. The refusal ORDER, arm by arm. Zero trivially satisfies qword alignment,
#      so an alignment-first order makes ADDR_ZERO unreachable -- handoff32
#      section 3's dead-arm shape.
#   3. Every refusal writes NOTHING, asserted against a NON-zero poison fill. A
#      zero-filled buffer cannot tell a refusal that wrote nothing from one that
#      one of those is a refusal that scribbled.
#   4. The flush precedes the store BY INDEX. Both orderings compose the same
#      SET of dwords, the same count, the same byte length, and both parse as
#      well-formed MI packets. Index is the only thing that separates them, and
#      a flush after the store retires nothing: the CPU observes the seqno while
#      the render caches still hold unwritten pixels.
#
# The separating mutation for property 4 is a header SWAP against a composed
# span -- same dwords, same length, still well-formed, and the completion now
# retires nothing. That is what igGtfCompletionIsOrdered() exists to make
# assertable by a caller rather than only by a test.
add_kext_test(test_intel_gt_fence test_intel_gt_fence.cpp)

# test_intel_gt_reset drives IntelGtReset.hpp, the RECOVERY layer -- the one
# that answers what this driver does when the fence never fires. IntelGtFence
# above it says how completion would be OBSERVED; nothing beneath it decides
# what to do when that observation never arrives, and polling forever is not a
# decision at all -- it is a hang wearing the costume of one.
#
# The properties here are the ones that can be wrong while every arm still
# returns a plausible verdict:
#   1. The wait judgement's ORDER. A budget test placed before the seqno test
#      reports TIMED_OUT for work that in fact completed on the final poll, and
#      the two call for opposite next actions -- reset the engine, or proceed.
#      That is handoff32 section 3's dead-arm shape aimed at the arm that
#      matters most.
#   2. Success is an EQUALITY against one verdict, never a negation of an error
#      arm. A bool is satisfied by every refusal at once (handoff34 section 1),
#      and here the refusals call for different recoveries.
#   3. The reset verdict is judged over the register WORD, so the same judgement
#      holds for every engine base. An engine-specific arm would make the
#      blitter's recovery a separate untested path from the render engine's.
#   4. No MMIO store exists on this path, gated as an ABSENCE rather than
#      claimed in prose -- handoff37 section 3's construction. This layer
#      DECIDES a reset is warranted; it does not perform one.
add_kext_test(test_intel_gt_reset test_intel_gt_reset.cpp)

# test_intel_gt_forcewake drives IntelGtForcewake.hpp, the first GT layer whose
# output is meant to be WRITTEN to the hardware: which power well an offset
# needs, the masked request words, and a step-wise acquire sequence fed ack
# readings by the caller. The ordering it pins is all-ones BEFORE the kernel
# bit, because 0xFFFFFFFF has FORCEWAKE_KERNEL set and would otherwise report
# a dead BAR as an awake well.
add_kext_test(test_intel_gt_forcewake test_intel_gt_forcewake.cpp)

# Registered 2026-11-01. These 35 suites were committed under tests/ with no
# registration here, so ctest never built or ran them. Each was first compiled
# standalone with g++ 16.1.0 -std=c++17 -Wall -Wextra -Wpedantic (zero
# warnings) and run once: every one exited 0, and every one ends main() with
# the exact count printf that tests/test_check_count_last_line.py requires.
# Their first readings are recorded in MEASURED in
# tests/test_check_count_floor.py.
#
# 34 other tests/*.cpp suites are still unregistered on purpose. They compile
# and pass under g++, but their final line is not one of the two count formats,
# so listing them here -- and in the count gate, which must match this list --
# would redden that gate. test_intel_gt_fill also needs
# phase0-kext/IntelGtBlitter.cpp linked in, which a two-argument add_kext_test
# cannot express.

# Intel GT engine and sink layers.
add_kext_test(test_intel_gt_copy_plan test_intel_gt_copy_plan.cpp)
add_kext_test(test_intel_gt_csb test_intel_gt_csb.cpp)
add_kext_test(test_intel_gt_dispatch test_intel_gt_dispatch.cpp)
add_kext_test(test_intel_gt_lrc test_intel_gt_lrc.cpp)
add_kext_test(test_intel_gt_ppgtt test_intel_gt_ppgtt.cpp)
add_kext_test(test_intel_sink_candidate test_intel_sink_candidate.cpp)
add_kext_test(test_intel_sink_ggtt test_intel_sink_ggtt.cpp)
add_kext_test(test_intel_sink_peer test_intel_sink_peer.cpp)

# NVIDIA GA107 channel, copy, fence and submit rehearsals.
add_kext_test(test_nv_ampere_channel test_nv_ampere_channel.cpp)
add_kext_test(test_nv_ampere_copy test_nv_ampere_copy.cpp)
add_kext_test(test_nv_ampere_fence test_nv_ampere_fence.cpp)
add_kext_test(test_nv_ampere_rm_plan test_nv_ampere_rm_plan.cpp)
add_kext_test(test_nv_ampere_submit test_nv_ampere_submit.cpp)

# NVIDIA GA107 GSP boot, queue and RPC layers.
add_kext_test(test_nv_gsp_bootargs_rehearsal test_nv_gsp_bootargs_rehearsal.cpp)
add_kext_test(test_nv_gsp_booter_fuse test_nv_gsp_booter_fuse.cpp)
add_kext_test(test_nv_gsp_cached_args test_nv_gsp_cached_args.cpp)
add_kext_test(test_nv_gsp_fw_container test_nv_gsp_fw_container.cpp)
add_kext_test(test_nv_gsp_libos_init test_nv_gsp_libos_init.cpp)
add_kext_test(test_nv_gsp_queue_dma test_nv_gsp_queue_dma.cpp)
add_kext_test(test_nv_gsp_queue_init test_nv_gsp_queue_init.cpp)
add_kext_test(test_nv_gsp_queue_memory test_nv_gsp_queue_memory.cpp)
add_kext_test(test_nv_gsp_queue_observe test_nv_gsp_queue_observe.cpp)
add_kext_test(test_nv_gsp_queue_plan test_nv_gsp_queue_plan.cpp)
add_kext_test(test_nv_gsp_queue_regs test_nv_gsp_queue_regs.cpp)
add_kext_test(test_nv_gsp_queue_rx test_nv_gsp_queue_rx.cpp)
add_kext_test(test_nv_gsp_queue_tx test_nv_gsp_queue_tx.cpp)
add_kext_test(test_nv_gsp_radix3 test_nv_gsp_radix3.cpp)
add_kext_test(test_nv_gsp_rm_image test_nv_gsp_rm_image.cpp)
add_kext_test(test_nv_gsp_rpc_frame test_nv_gsp_rpc_frame.cpp)
add_kext_test(test_nv_gsp_rpc_response test_nv_gsp_rpc_response.cpp)
add_kext_test(test_nv_gsp_rpc_transaction test_nv_gsp_rpc_transaction.cpp)
add_kext_test(test_nv_gsp_wpr_boot_ucode test_nv_gsp_wpr_boot_ucode.cpp)
add_kext_test(test_nv_gsp_wpr_ga107 test_nv_gsp_wpr_ga107.cpp)
add_kext_test(test_nv_gsp_wpr_image test_nv_gsp_wpr_image.cpp)
add_kext_test(test_nv_gsp_wpr_meta test_nv_gsp_wpr_meta.cpp)

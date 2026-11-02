// SPDX-License-Identifier: MIT
//
//  IntelTiming.hpp -- Tiger Lake pipe A mode timing and pixel clock, decoded
//  from words a caller has ALREADY read. DRIVER DISP-1, crew card t-916874.
//
//  Dependency-free in the CLAUDE.md sense: no #include, no IOKit, no kernel
//  types, no allocation, no sleeping, and no register access of any kind.
//  Every function takes 32-bit words and returns numbers, so a host suite
//  compiles the identical code the kext would run.
//
//  Provenance rules for this file:
//    - register offsets and field layouts come ONLY from the vendored Linux
//      headers under reference/i915/, cited file:line beside each one;
//    - the value-minus-one storage convention comes from the vendored
//      reference/i915/intel_display.c read-back, cited file:line;
//    - clock arithmetic the vendored tree does not carry (refclk readout,
//      combo-PLL frequency, link symbol size) is cited against upstream
//      Linux v6.12 by file:line, in the pixel-clock section;
//    - every worked number is a decode of a word present in
//      igpu-results/tgl-capture-2026-08-16-022556.txt and/or
//      igpu-results/tgl-final-capture-2026-08-18-201412.txt, cited by line.
//
//  Nothing here reads or writes a register. The caller owns every MMIO
//  access, and IntelFbPresentProbe (eager-ibis-51, FB-1/FB-2) owns wiring.
//

#ifndef IntelTiming_hpp
#define IntelTiming_hpp

//
//  ---- Offsets: transcoder A timing block -----------------------------------
//
//  reference/i915/i915_reg.h. The TRANS_*() and PIPESRC() macros index these
//  by transcoder through _MMIO_TRANS2 (i915_reg.h:1139-1147). Only transcoder
//  A is named, because it is the only one either capture found programmed:
//  tgl-capture-2026-08-16-022556.txt:22-30 reads B, C, D, DSI0 and DSI1 as
//  all-zero, which CLAUDE.md says to read as UNPROGRAMMED, not absent.
//
#define IG_TIMING_TRANS_A_HTOTAL    0x60000u  // i915_reg.h:1073 _TRANS_HTOTAL_A
#define IG_TIMING_TRANS_A_HBLANK    0x60004u  // i915_reg.h:1078 _TRANS_HBLANK_A
#define IG_TIMING_TRANS_A_HSYNC     0x60008u  // i915_reg.h:1083 _TRANS_HSYNC_A
#define IG_TIMING_TRANS_A_VTOTAL    0x6000Cu  // i915_reg.h:1088 _TRANS_VTOTAL_A
#define IG_TIMING_TRANS_A_VBLANK    0x60010u  // i915_reg.h:1093 _TRANS_VBLANK_A
#define IG_TIMING_TRANS_A_VSYNC     0x60014u  // i915_reg.h:1098 _TRANS_VSYNC_A
#define IG_TIMING_PIPE_A_SRC        0x6001Cu  // i915_reg.h:1104 _PIPEASRC

//
//  ---- Field layout: the same two halves in all seven words ------------------
//
//    word      [31:16]                         [15:0]
//    HTOTAL    htotal       :1074 HTOTAL_MASK  hactive      :1076 HACTIVE_MASK
//    HBLANK    hblank end   :1079              hblank start :1081
//    HSYNC     hsync end    :1084              hsync start  :1086
//    VTOTAL    vtotal       :1089              vactive      :1091
//    VBLANK    vblank end   :1094              vblank start :1096
//    VSYNC     vsync end    :1099              vsync start  :1101
//    PIPESRC   width        :1105              height       :1107
//
//  All i915_reg.h; every [31:16] mask is REG_GENMASK(31, 16) and every [15:0]
//  mask is REG_GENMASK(15, 0). Every half stores value - 1: the read-back in
//  reference/i915/intel_display.c:2845-2872 (timings) and :2914-2915
//  (PIPESRC) follows every REG_FIELD_GET() with + 1.
//
//  Named IG_TIMING_WORD_* and igTimingWord*(), not IG_TIMING_HI_* and
//  igTimingHi()/igTimingLo(): IntelDisplay.hpp:184-185 already defines
//  IG_TIMING_HI_SHIFT and IG_TIMING_HALF_MASK, and IntelDisplay.hpp:599-607
//  already defines igTimingHi() and igTimingLo(). A translation unit that
//  includes both headers must not see one inline function defined twice.
//
#define IG_TIMING_WORD_HI_MASK   0xFFFF0000u  // REG_GENMASK(31, 16), i915_reg.h:1074
#define IG_TIMING_WORD_HI_SHIFT  16u
#define IG_TIMING_WORD_LO_MASK   0x0000FFFFu  // REG_GENMASK(15, 0), i915_reg.h:1076

//
//  ---- The captured mode -----------------------------------------------------
//
//  Both captures hold the same seven words, on two independent boots:
//
//    word      value        2026-08-16 capture   2026-08-18 final capture
//    HTOTAL    0x081F077F   :15                  :1006
//    HBLANK    0x081F077F   :16                  :1006
//    HSYNC     0x081B07EB   :17                  :1006
//    VTOTAL    0x049B0437   :18                  :1006
//    VBLANK    0x049B0437   :19                  :1007
//    VSYNC     0x043F043A   :20                  :1007
//    PIPESRC   0x077F0437   :21                  :1007
//
//  igTimingDecode() turns them into
//
//    hdisplay 1920   hblank 1920..2080   hsync 2028..2076   htotal 2080
//    vdisplay 1080   vblank 1080..1180   vsync 1083..1088   vtotal 1180
//    PIPESRC  1920 x 1080
//
//  which agrees with the one mode the 2026-08-16 capture lists for the
//  connected panel, card1-eDP-1 modes=[1920x1080] (:9), and with the raw
//  halves IntelDisplay.hpp:1332-1337 records as IG_OBSERVED_TRANS_A_*.
//
//  Not handled: interlace. This header takes no TRANSCONF word, so it
//  describes a progressive mode only. The vendored read-back past
//  intel_display.c:2872 was not quoted for this card; read it before using
//  this decode for an interlaced pipe.
//

struct IgTimingWords {
    unsigned int htotal;    // TRANS_HTOTAL  @ IG_TIMING_TRANS_A_HTOTAL
    unsigned int hblank;    // TRANS_HBLANK  @ IG_TIMING_TRANS_A_HBLANK
    unsigned int hsync;     // TRANS_HSYNC   @ IG_TIMING_TRANS_A_HSYNC
    unsigned int vtotal;    // TRANS_VTOTAL  @ IG_TIMING_TRANS_A_VTOTAL
    unsigned int vblank;    // TRANS_VBLANK  @ IG_TIMING_TRANS_A_VBLANK
    unsigned int vsync;     // TRANS_VSYNC   @ IG_TIMING_TRANS_A_VSYNC
    unsigned int pipesrc;   // PIPESRC       @ IG_TIMING_PIPE_A_SRC
};

struct IgModeTiming {
    unsigned int hdisplay;
    unsigned int hblankStart;
    unsigned int hblankEnd;
    unsigned int hsyncStart;
    unsigned int hsyncEnd;
    unsigned int htotal;
    unsigned int vdisplay;
    unsigned int vblankStart;
    unsigned int vblankEnd;
    unsigned int vsyncStart;
    unsigned int vsyncEnd;
    unsigned int vtotal;
    unsigned int srcWidth;
    unsigned int srcHeight;
};

//
//  An enum, not #defines: these are this driver's own bookkeeping, not
//  hardware values, so they have no business beside the register constants.
//
enum IgTimingVerdict {
    IG_TIMING_DECODED       = 0,
    IG_TIMING_NULL_ARGUMENT = 1,  // words or out was nullptr
    IG_TIMING_UNPROGRAMMED  = 2,  // one of the seven words read 0x00000000
    IG_TIMING_VERDICT_LIMIT = 3
};

static inline unsigned int igTimingWordHi(unsigned int word)
{
    return ((word & IG_TIMING_WORD_HI_MASK) >> IG_TIMING_WORD_HI_SHIFT) + 1u;
}

static inline unsigned int igTimingWordLo(unsigned int word)
{
    return (word & IG_TIMING_WORD_LO_MASK) + 1u;
}

//
//  A word of 0x00000000 decodes to 1 on both halves -- a 1x1 mode that would
//  look like a measurement. The captures read every unprogrammed transcoder
//  as exactly that word, so zero in any of the seven is refused instead of
//  decoded. *out is zeroed on every refusal, so a caller that ignores the
//  verdict publishes zeros rather than a stale mode.
//
static inline IgTimingVerdict igTimingDecode(const IgTimingWords *words,
                                             IgModeTiming *out)
{
    if (out == nullptr)
        return IG_TIMING_NULL_ARGUMENT;

    *out = IgModeTiming();

    if (words == nullptr)
        return IG_TIMING_NULL_ARGUMENT;

    if (words->htotal == 0u || words->hblank == 0u || words->hsync == 0u ||
        words->vtotal == 0u || words->vblank == 0u || words->vsync == 0u ||
        words->pipesrc == 0u)
        return IG_TIMING_UNPROGRAMMED;

    out->hdisplay    = igTimingWordLo(words->htotal);
    out->htotal      = igTimingWordHi(words->htotal);
    out->hblankStart = igTimingWordLo(words->hblank);
    out->hblankEnd   = igTimingWordHi(words->hblank);
    out->hsyncStart  = igTimingWordLo(words->hsync);
    out->hsyncEnd    = igTimingWordHi(words->hsync);
    out->vdisplay    = igTimingWordLo(words->vtotal);
    out->vtotal      = igTimingWordHi(words->vtotal);
    out->vblankStart = igTimingWordLo(words->vblank);
    out->vblankEnd   = igTimingWordHi(words->vblank);
    out->vsyncStart  = igTimingWordLo(words->vsync);
    out->vsyncEnd    = igTimingWordHi(words->vsync);
    out->srcWidth    = igTimingWordHi(words->pipesrc);
    out->srcHeight   = igTimingWordLo(words->pipesrc);

    return IG_TIMING_DECODED;
}

//
//  ==== Pixel clock ===========================================================
//
//  The seven words above count the pixels in a frame. They say nothing about
//  how fast those pixels go, so on their own they cannot give a refresh rate.
//  On this machine the pipe A pixel clock takes four steps to reach, each one
//  from words in igpu-results/tgl-final-capture-2026-08-18-201412.txt:
//
//    1. which port and mode transcoder A drives  TRANS_DDI_FUNC_CTL A  :1011
//    2. which PLL clocks that port, and is it    ICL_DPCLKA_CFGCR0     :1190
//       enabled and locked                       _DPLL0_ENABLE         :997
//    3. that PLL's output, the DP link rate      SKL_DSSM              :1002
//                                                _TGL_DPLL0_CFGCR0/1   :1190
//    4. pixel clock = link M/N x link rate       _PIPEA_LINK_M1/N1     :1008
//
//  Single-boot evidence. A grep of igpu-results/tgl-*.txt for 0x51004, DSSM,
//  refclk and CDCLK matched only the final capture, so the reference clock
//  in step 3 has no second boot behind it, unlike the timing words.
//
//  Offsets and field layouts are still taken ONLY from the vendored
//  reference/i915/i915_reg.h and cited by line. The dot-clock formula in step
//  4 is vendored too (reference/i915/intel_display.c:3939-3959). The
//  reference-clock readout and the combo-PLL frequency in step 3, and the link
//  symbol size in step 4, live in files reference/i915/ does not carry, so
//  they are cited against upstream Linux v6.12 by file:line instead:
//
//    intel_cdclk.c:1585            icl_readout_refclk(): DSSM field -> ref kHz
//                                  (cases :1594, :1597, :1600), called for
//                                  display version 11+ at :1614
//    intel_dpll_mgr.c:4074-4077    icl_update_dpll_ref_clks(): nssc = cdclk ref,
//                                  which tgl_pll_mgr uses (:4209, :4215)
//    intel_dpll_mgr.c:2764-2772    icl_wrpll_ref_clock(): use 38400 as 19200,
//                                  "because the DPLL automatically divides
//                                  that by 2"
//    intel_dpll_mgr.c:2595-2604    Display WA #22010492432: Tiger Lake with a
//                                  38400 nssc programs HALF the DCO fraction
//    intel_dpll_mgr.c:2822-2881    icl_ddi_combo_pll_get_freq(): doubles the
//                                  fraction back (:2874), then returns
//                                  dco / (p0 * p1 * p2 * 5) (:2881)
//    intel_dp.c:152-155            intel_dp_link_symbol_size(): 32 if UHBR,
//                                  otherwise 10
//    drm_dp_helper.h:302-305       drm_dp_is_uhbr_rate(): rate >= 1000000
//
//  Two enum values come from headers reference/i915/ does not carry either:
//  PORT_A is the first enum port value (0), and DPLL0 is PLL id 0. The
//  captured TRANS_CLK_SEL A word, 0x10000000 at :997, uses the same
//  TGL_TRANS_CLK_SEL_PORT(x) = (x + 1) << 28 encoding (i915_reg.h:4015) and
//  gives the same port, port field 1.
//
//  Here is what each step makes of the captured words:
//
//    TRANS_DDI_FUNC_CTL A  0x8A000006  enable set; TGL port field 1, which is
//                                      PORT_A + 1 (i915_reg.h:3757); mode 2,
//                                      DP SST (i915_reg.h:3761)
//    ICL_DPCLKA_CFGCR0     0x01E17800  phy A select 0, which is DPLL0; the
//                                      phy A clock-off bit (bit 10) is clear
//    _DPLL0_ENABLE         0xCC000000  PLL_ENABLE and PLL_LOCK both set
//    SKL_DSSM              0x40000031  refclk field 2, 38.4 MHz
//    _TGL_DPLL0_CFGCR0     0x00E001A5  DCO integer 0x1A5, fraction 0x3800
//    _TGL_DPLL0_CFGCR1     0x00000048  PDIV_3, KDIV_1, QDIV mode off
//    _PIPEA_LINK_M1 / N1   0x00053DD1 / 0x00080000  m 343505, n 524288
//
//  38.4 MHz is used as 19.2 MHz, and the workaround doubles the fraction back
//  to 0x7000:
//
//    DCO     = 421 x 19200 + (0x7000 x 19200) / 0x8000  = 8100000 kHz
//    link    = 8100000 / (3 x 1 x 1 x 5)                =  540000 (5.4 Gb/s)
//    pixel   = ceil(343505 x 540000 x 10 / (524288 x 10)) = 353800 kHz
//    refresh = 353800000 / (2080 x 1180)                ~= 144.149 Hz
//            = 0x00902637 in 16.16
//
//  The arithmetic cross-checks against the firmware's own table: integer
//  0x1A5, fraction 0x7000, pdiv 3, kdiv 1 is EXACTLY the 540000 entry of
//  icl_dp_combo_pll_19_2MHz_values[] (upstream intel_dpll_mgr.c:2645, "Also
//  used for 38.4 MHz values"), with the fraction halved as the workaround
//  programs it. Without the doubling, the DCO would be 8091600 kHz and the
//  link rate 539440, which matches no entry in that table.
//
//  There is no measured refresh rate to compare the result against. The
//  2026-08-16 capture lists the panel as modes=[1920x1080] (:9) and nothing
//  more.
//
//  Not handled, and refused rather than guessed: HDMI, DVI, DP MST and
//  128b/132b; any DDI other than A; any PLL other than DPLL0 (DPLL1's words
//  at 0x16428C/0x164290 were captured, but phy A does not select DPLL1); and a DSSM
//  refclk field outside the three listed. For that last case upstream logs
//  MISSING_CASE and falls through to 24 MHz; this header refuses instead.
//  _PIPEA_DATA_M1/N1 (0x7E3EE5D4 / 0x00800000 at :1007-1008) were captured
//  but are not read here, because intel_dotclock_calculate() uses only the
//  link M/N.
//

#define IG_TIMING_TRANS_A_LINK_M1       0x60040u   // i915_reg.h:2162 _PIPEA_LINK_M1
#define IG_TIMING_TRANS_A_LINK_N1       0x60044u   // i915_reg.h:2163 _PIPEA_LINK_N1
#define IG_TIMING_TRANS_A_DDI_FUNC_CTL  0x60400u   // i915_reg.h:3741 _TRANS_DDI_FUNC_CTL_A
#define IG_TIMING_DSSM                  0x51004u   // i915_reg.h:2880 SKL_DSSM
#define IG_TIMING_DPLL0_ENABLE          0x46010u   // i915_reg.h:4213 _DPLL0_ENABLE
#define IG_TIMING_DPCLKA_CFGCR0         0x164280u  // i915_reg.h:4158 ICL_DPCLKA_CFGCR0
#define IG_TIMING_TGL_DPLL0_CFGCR0      0x164284u  // i915_reg.h:4301 _TGL_DPLL0_CFGCR0
#define IG_TIMING_TGL_DPLL0_CFGCR1      0x164288u  // i915_reg.h:4316 _TGL_DPLL0_CFGCR1

// REG_GENMASK(23, 0), i915_reg.h:1557 DATA_LINK_M_N_MASK; applied to link M
// and link N by intel_get_m_n(), reference/i915/intel_display.c:3349-3350.
#define IG_TIMING_CLK_M_N_MASK          0x00FFFFFFu

#define IG_TIMING_CLK_DDI_FUNC_ENABLE   0x80000000u  // i915_reg.h:3750 (1 << 31)
#define IG_TIMING_CLK_DDI_PORT_SHIFT    27u          // i915_reg.h:3753 TGL_TRANS_DDI_PORT_SHIFT
#define IG_TIMING_CLK_DDI_PORT_MASK     0x78000000u  // i915_reg.h:3755 (0xf << 27)
#define IG_TIMING_CLK_DDI_PORT_A        1u           // i915_reg.h:3757, PORT_A (0) + 1
#define IG_TIMING_CLK_DDI_MODE_MASK     0x07000000u  // i915_reg.h:3758 (7 << 24)
#define IG_TIMING_CLK_DDI_MODE_DP_SST   0x02000000u  // i915_reg.h:3761 (2 << 24)

//  For phy A, the ICL and RKL forms of these two macros give the same bits,
//  so it does not matter which one a Tiger Lake caller uses.
//  Select: ICL_DPCLKA_CFGCR0_DDI_CLK_SEL_MASK(phy) is 3 << (phy * 2)
//  (i915_reg.h:4164-4165), and RKL_..._SEL_SHIFT(phy) picks 0 for phy A
//  (i915_reg.h:4167). Clock off: ICL_..._DDI_CLK_OFF(phy) picks bit 10 for
//  phy A (i915_reg.h:4159), and RKL_..._DDI_CLK_OFF(phy) is REG_BIT(phy + 10)
//  (i915_reg.h:4160).
#define IG_TIMING_CLK_DPCLKA_PHY_A_SEL_MASK   0x00000003u
#define IG_TIMING_CLK_DPCLKA_PHY_A_SEL_DPLL0  0x00000000u  // DDI_CLK_SEL(DPLL0, phy A), :4166
#define IG_TIMING_CLK_DPCLKA_PHY_A_CLK_OFF    0x00000400u

#define IG_TIMING_CLK_PLL_ENABLE        0x80000000u  // i915_reg.h:4217 REG_BIT(31)
#define IG_TIMING_CLK_PLL_LOCK          0x40000000u  // i915_reg.h:4218 REG_BIT(30)

#define IG_TIMING_CLK_DSSM_REFCLK_MASK     0xE0000000u  // i915_reg.h:2881 (7 << 29)
#define IG_TIMING_CLK_DSSM_REFCLK_24MHZ    0x00000000u  // i915_reg.h:2882 (0 << 29)
#define IG_TIMING_CLK_DSSM_REFCLK_19_2MHZ  0x20000000u  // i915_reg.h:2883 (1 << 29)
#define IG_TIMING_CLK_DSSM_REFCLK_38_4MHZ  0x40000000u  // i915_reg.h:2884 (2 << 29)

#define IG_TIMING_CLK_CFGCR0_DCO_FRACTION_MASK   0x01FFFC00u  // i915_reg.h:4270 (0x7fff << 10)
#define IG_TIMING_CLK_CFGCR0_DCO_FRACTION_SHIFT  10u          // i915_reg.h:4271
#define IG_TIMING_CLK_CFGCR0_DCO_INTEGER_MASK    0x000003FFu  // i915_reg.h:4273 (0x3ff)

#define IG_TIMING_CLK_CFGCR1_QDIV_RATIO_MASK     0x0003FC00u  // i915_reg.h:4279 (0xff << 10)
#define IG_TIMING_CLK_CFGCR1_QDIV_RATIO_SHIFT    10u          // i915_reg.h:4280
#define IG_TIMING_CLK_CFGCR1_QDIV_MODE           0x00000200u  // i915_reg.h:4283 QDIV_MODE(1)
#define IG_TIMING_CLK_CFGCR1_KDIV_MASK           0x000001C0u  // i915_reg.h:4284 (7 << 6)
#define IG_TIMING_CLK_CFGCR1_KDIV_1              0x00000040u  // i915_reg.h:4287 (1 << 6)
#define IG_TIMING_CLK_CFGCR1_KDIV_2              0x00000080u  // i915_reg.h:4288 (2 << 6)
#define IG_TIMING_CLK_CFGCR1_KDIV_3              0x00000100u  // i915_reg.h:4289 (4 << 6)
#define IG_TIMING_CLK_CFGCR1_PDIV_MASK           0x0000003Cu  // i915_reg.h:4290 (0xf << 2)
#define IG_TIMING_CLK_CFGCR1_PDIV_2              0x00000004u  // i915_reg.h:4293 (1 << 2)
#define IG_TIMING_CLK_CFGCR1_PDIV_3              0x00000008u  // i915_reg.h:4294 (2 << 2)
#define IG_TIMING_CLK_CFGCR1_PDIV_5              0x00000010u  // i915_reg.h:4295 (4 << 2)
#define IG_TIMING_CLK_CFGCR1_PDIV_7              0x00000020u  // i915_reg.h:4296 (8 << 2)

struct IgTimingClockWords {
    unsigned int ddiFuncCtl;    // TRANS_DDI_FUNC_CTL A @ IG_TIMING_TRANS_A_DDI_FUNC_CTL
    unsigned int dpclkaCfgcr0;  // ICL_DPCLKA_CFGCR0    @ IG_TIMING_DPCLKA_CFGCR0
    unsigned int dpll0Enable;   // _DPLL0_ENABLE        @ IG_TIMING_DPLL0_ENABLE
    unsigned int dssm;          // SKL_DSSM             @ IG_TIMING_DSSM
    unsigned int dpll0Cfgcr0;   // _TGL_DPLL0_CFGCR0    @ IG_TIMING_TGL_DPLL0_CFGCR0
    unsigned int dpll0Cfgcr1;   // _TGL_DPLL0_CFGCR1    @ IG_TIMING_TGL_DPLL0_CFGCR1
    unsigned int linkM1;        // _PIPEA_LINK_M1       @ IG_TIMING_TRANS_A_LINK_M1
    unsigned int linkN1;        // _PIPEA_LINK_N1       @ IG_TIMING_TRANS_A_LINK_N1
};

struct IgTimingClock {
    unsigned int refKHz;         // cdclk reference: 24000, 19200 or 38400
    unsigned int dpllRefKHz;     // what the DPLL multiplies: 38400 is used as 19200
    unsigned int dcoKHz;
    unsigned int portClock;      // i915 port_clock: DP link rate, 10 kbit/s units
    unsigned int linkM;
    unsigned int linkN;
    unsigned int pixelClockKHz;  // intel_dotclock_calculate()
};

enum IgTimingClockVerdict {
    IG_TIMING_CLOCK_DECODED         = 0,
    IG_TIMING_CLOCK_NULL_ARGUMENT   = 1,   // words or out was nullptr
    IG_TIMING_CLOCK_TRANSCODER_OFF  = 2,   // TRANS_DDI_FUNC_ENABLE clear
    IG_TIMING_CLOCK_NOT_PORT_A      = 3,   // transcoder A drives another DDI
    IG_TIMING_CLOCK_NOT_DP_SST      = 4,   // HDMI, DVI, MST or 128b/132b
    IG_TIMING_CLOCK_NOT_DPLL0       = 5,   // phy A clocked off, or by another PLL
    IG_TIMING_CLOCK_PLL_UNLOCKED    = 6,   // DPLL0 not enabled, or not locked
    IG_TIMING_CLOCK_REFCLK_UNKNOWN  = 7,   // DSSM refclk field not one of three
    IG_TIMING_CLOCK_DIVIDER_INVALID = 8,   // PDIV/KDIV unlisted, or QDIV ratio 0
    IG_TIMING_CLOCK_LINK_N_ZERO     = 9,   // intel_dotclock_calculate() gives 0
    IG_TIMING_CLOCK_OUT_OF_RANGE    = 10,  // a zero DCO, link rate or pixel
                                           // clock, or one past 32 bits
    IG_TIMING_CLOCK_VERDICT_LIMIT   = 11
};

// intel_dp.c:152-155 and drm_dp_helper.h:302-305 (upstream v6.12).
static inline unsigned int igTimingClockLinkSymbolSize(unsigned int portClock)
{
    return portClock >= 1000000u ? 32u : 10u;
}

//
//  Steps 1-4 above, in order, refusing at the first word that does not
//  describe a DP SST link on DDI A clocked by a locked DPLL0. *out is zeroed
//  on every refusal, the same contract as igTimingDecode(). The arithmetic is
//  64-bit; upstream's 32-bit DCO math cannot overflow for any CFGCR0 value
//  (at most 1023 x 24000 + 65534 x 24000 / 0x8000), so the two agree on
//  every word.
//
static inline IgTimingClockVerdict igTimingClockDecode(
    const IgTimingClockWords *words, IgTimingClock *out)
{
    if (out == nullptr)
        return IG_TIMING_CLOCK_NULL_ARGUMENT;

    *out = IgTimingClock();

    if (words == nullptr)
        return IG_TIMING_CLOCK_NULL_ARGUMENT;

    // Step 1: transcoder A is on, drives DDI A, and is in DP SST mode.
    const unsigned int ddi = words->ddiFuncCtl;
    if ((ddi & IG_TIMING_CLK_DDI_FUNC_ENABLE) == 0u)
        return IG_TIMING_CLOCK_TRANSCODER_OFF;
    if (((ddi & IG_TIMING_CLK_DDI_PORT_MASK) >> IG_TIMING_CLK_DDI_PORT_SHIFT) !=
        IG_TIMING_CLK_DDI_PORT_A)
        return IG_TIMING_CLOCK_NOT_PORT_A;
    if ((ddi & IG_TIMING_CLK_DDI_MODE_MASK) != IG_TIMING_CLK_DDI_MODE_DP_SST)
        return IG_TIMING_CLOCK_NOT_DP_SST;

    // Step 2: phy A's clock is on and comes from DPLL0, which is locked.
    const unsigned int dpclka = words->dpclkaCfgcr0;
    if ((dpclka & IG_TIMING_CLK_DPCLKA_PHY_A_CLK_OFF) != 0u ||
        (dpclka & IG_TIMING_CLK_DPCLKA_PHY_A_SEL_MASK) !=
            IG_TIMING_CLK_DPCLKA_PHY_A_SEL_DPLL0)
        return IG_TIMING_CLOCK_NOT_DPLL0;
    if ((words->dpll0Enable & IG_TIMING_CLK_PLL_ENABLE) == 0u ||
        (words->dpll0Enable & IG_TIMING_CLK_PLL_LOCK) == 0u)
        return IG_TIMING_CLOCK_PLL_UNLOCKED;

    IgTimingClock c = IgTimingClock();

    // Step 3a: reference clock (intel_cdclk.c:1585, :1594-1600).
    switch (words->dssm & IG_TIMING_CLK_DSSM_REFCLK_MASK) {
    case IG_TIMING_CLK_DSSM_REFCLK_24MHZ:   c.refKHz = 24000u; break;
    case IG_TIMING_CLK_DSSM_REFCLK_19_2MHZ: c.refKHz = 19200u; break;
    case IG_TIMING_CLK_DSSM_REFCLK_38_4MHZ: c.refKHz = 38400u; break;
    default:
        return IG_TIMING_CLOCK_REFCLK_UNKNOWN;
    }
    c.dpllRefKHz = (c.refKHz == 38400u) ? 19200u : c.refKHz;  // :2772

    // Step 3b: dividers (intel_dpll_mgr.c:2822-2881).
    const unsigned int cfgcr1 = words->dpll0Cfgcr1;
    unsigned int p0;
    unsigned int p1;
    unsigned int p2;

    switch (cfgcr1 & IG_TIMING_CLK_CFGCR1_PDIV_MASK) {
    case IG_TIMING_CLK_CFGCR1_PDIV_2: p0 = 2u; break;
    case IG_TIMING_CLK_CFGCR1_PDIV_3: p0 = 3u; break;
    case IG_TIMING_CLK_CFGCR1_PDIV_5: p0 = 5u; break;
    case IG_TIMING_CLK_CFGCR1_PDIV_7: p0 = 7u; break;
    default:
        return IG_TIMING_CLOCK_DIVIDER_INVALID;
    }

    switch (cfgcr1 & IG_TIMING_CLK_CFGCR1_KDIV_MASK) {
    case IG_TIMING_CLK_CFGCR1_KDIV_1: p2 = 1u; break;
    case IG_TIMING_CLK_CFGCR1_KDIV_2: p2 = 2u; break;
    case IG_TIMING_CLK_CFGCR1_KDIV_3: p2 = 3u; break;
    default:
        return IG_TIMING_CLOCK_DIVIDER_INVALID;
    }

    if ((cfgcr1 & IG_TIMING_CLK_CFGCR1_QDIV_MODE) != 0u)
        p1 = (cfgcr1 & IG_TIMING_CLK_CFGCR1_QDIV_RATIO_MASK) >>
             IG_TIMING_CLK_CFGCR1_QDIV_RATIO_SHIFT;
    else
        p1 = 1u;

    if (p1 == 0u)
        return IG_TIMING_CLOCK_DIVIDER_INVALID;

    // Step 3c: DCO and link rate. The fraction is doubled back on a 38.4 MHz
    // reference (Display WA #22010492432, intel_dpll_mgr.c:2595-2604, :2874).
    const unsigned int cfgcr0 = words->dpll0Cfgcr0;
    unsigned long long fraction =
        (cfgcr0 & IG_TIMING_CLK_CFGCR0_DCO_FRACTION_MASK) >>
        IG_TIMING_CLK_CFGCR0_DCO_FRACTION_SHIFT;
    if (c.refKHz == 38400u)
        fraction *= 2u;

    const unsigned long long dco =
        (unsigned long long) (cfgcr0 & IG_TIMING_CLK_CFGCR0_DCO_INTEGER_MASK) *
            c.dpllRefKHz +
        (fraction * c.dpllRefKHz) / 0x8000u;
    const unsigned long long port = dco / (p0 * p1 * p2 * 5u);

    if (dco == 0u || port == 0u)
        return IG_TIMING_CLOCK_OUT_OF_RANGE;

    c.dcoKHz    = (unsigned int) dco;
    c.portClock = (unsigned int) port;

    // Step 4: pixel clock (reference/i915/intel_display.c:3349-3350, :3955-3959).
    c.linkM = words->linkM1 & IG_TIMING_CLK_M_N_MASK;
    c.linkN = words->linkN1 & IG_TIMING_CLK_M_N_MASK;

    if (c.linkN == 0u)
        return IG_TIMING_CLOCK_LINK_N_ZERO;

    const unsigned long long num = (unsigned long long) c.linkM * (port * 10u);
    const unsigned long long den = (unsigned long long) c.linkN *
                                   igTimingClockLinkSymbolSize(c.portClock);
    const unsigned long long pixel = (num + den - 1u) / den;  // DIV_ROUND_UP_ULL

    if (pixel == 0u || pixel > 0xFFFFFFFFull)
        return IG_TIMING_CLOCK_OUT_OF_RANGE;

    c.pixelClockKHz = (unsigned int) pixel;

    *out = c;
    return IG_TIMING_CLOCK_DECODED;
}

//
//  Refresh rate in 16.16 fixed point, the unit IntelFbPresentProbe reports to
//  IOKit (IntelFbPresentProbe.hpp:121-131, where IG_PRESENT_REFRESH_UNMEASURED
//  is 60 << 16). Rounded to the nearest 1/65536 Hz. Returns 0, which is never
//  a plausible rate, when the mode has no totals or the result is past 32 bits.
//
static inline unsigned int igTimingRefresh16_16(const IgModeTiming *mode,
                                                unsigned int pixelClockKHz)
{
    if (mode == nullptr || mode->htotal == 0u || mode->vtotal == 0u)
        return 0u;

    const unsigned long long frame =
        (unsigned long long) mode->htotal * mode->vtotal;
    const unsigned long long num =
        ((unsigned long long) pixelClockKHz * 1000ull) << 16;
    const unsigned long long rate = (num + frame / 2u) / frame;

    return rate > 0xFFFFFFFFull ? 0u : (unsigned int) rate;
}

#endif // IntelTiming_hpp

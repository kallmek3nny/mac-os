// SPDX-License-Identifier: MIT
//
//  IntelPanel.hpp
//  Gen12 (Tiger Lake) eDP panel power sequencing and backlight PWM decode,
//  read-only. DRIVER DISP-2 (t-57f258).
//
//  Like IntelDisplay.hpp, this file is DELIBERATELY DEPENDENCY-FREE: no
//  includes, no IOKit, no kernel types, no allocation, no sleep. Every
//  function is a total function of its arguments. Nothing here touches MMIO:
//  the caller reads the words and this header only decodes them. There is no
//  write helper, and none of the write-only keys or enables below is used.
//
//  WHY THIS FILE EXISTS
//
//  FB-2 reports the built-in connection. Until now it could only assert that
//  the eDP panel is powered and lit. The words that answer that question were
//  captured once, on 2026-08-18, but the register definitions they decode
//  against were not vendored, so nothing could read them. This header turns
//  those words into three answers: panel powered, backlight enabled and duty
//  percent.
//
//  PROVENANCE OF EVERY NUMBER BELOW
//
//  Offsets, masks and encodings come only from two vendored Linux v6.12
//  files, cited by line at each definition:
//
//    reference/i915/display/intel_pps_regs.h        (PPS, cited as pps:N)
//    reference/i915/display/intel_backlight_regs.h  (PWM, cited as blc:N)
//
//  Which PPS base and which backlight register set i915 uses on this PCH come
//  from these, cited where used:
//
//    reference/i915/display/intel_pps.c
//    reference/i915/display/intel_backlight.c
//    reference/i915/soc/intel_pch.h
//    reference/i915/soc/intel_pch.c
//
//  See reference/i915/PROVENANCE.md for the md5 of each file. Nothing below is
//  derived from the capture. The capture is what these decoders are CHECKED
//  AGAINST, and its words are kept as IG_PANEL_OBSERVED_*_2026_08_18 at the
//  bottom.
//
//  THE PCH TYPE IS INFERRED, NOT MEASURED
//
//  The choice of backlight registers depends on the PCH type, and this
//  laptop's PCH device ID was never captured. The chain below is i915's
//  mapping for Tiger Lake, not a measurement of this machine:
//
//    reference/i915/soc/intel_pch.c:120-128  TGP, TGP2 and MCC IDs -> PCH_TGP
//    reference/i915/soc/intel_pch.h:24-26    enum order PCH_CNP < PCH_ICP < PCH_TGP
//    reference/i915/display/intel_backlight.c:1802-1803
//                                  INTEL_PCH_TYPE >= PCH_CNP -> cnp_pwm_funcs
//    reference/i915/display/intel_backlight.c:411-419, 714-741
//                                  the cnp_* paths use BXT_BLC_PWM_*(controller)
//    reference/i915/soc/intel_pch.h:85       HAS_PCH_SPLIT is pch_type != PCH_NONE
//    reference/i915/display/intel_pps.c:1714-1715
//                                  HAS_PCH_SPLIT -> pps.mmio_base = PCH_PPS_BASE
//
//  If a measured PCH ID ever lands, it is checked against that chain. It does
//  not replace it.
//
//  THE HAZARD THIS FILE IS SHAPED AROUND: ONE OFFSET, TWO LAYOUTS
//
//  0xC8250 has two names in the same vendored header. BLC_PWM_PCH_CTL1 (blc:81)
//  is the older PCH layout, and its CTL2 at 0xC8254 (blc:85) packs frequency
//  and duty into one word, "like the normal CTL from gen4" (blc:79-80).
//  _BXT_BLC_PWM_CTL1 (blc:88) is the layout the cnp_* paths use, where
//  0xC8254 is the frequency and 0xC8258 is the duty, each a whole word
//  (blc:91-92). Decoding the captured 0x00017700 at 0xC8254 with the old
//  layout produces a frequency and a duty that look plausible and are both
//  wrong. So this header names and decodes only the BXT layout, for the reason
//  given in the chain above.
//
//  WHAT IS DELIBERATELY ABSENT
//
//  Delay units. PP_ON_DELAYS and PP_OFF_DELAYS are decoded to RAW field values
//  only. Neither vendored header gives a time unit, so no millisecond figure
//  appears here.
//
//  PP_READY (pps:30). The capture reads it clear on a lit panel, so no verdict
//  here depends on it.
//
//  PP_DIVISOR (pps:73). The capture page has only four live words, at
//  0xC7200..0xC720C, so 0xC7210 read trivial and nothing is decoded from it.
//
//  A PPS instance index. _MMIO_PPS (pps:17-18) adds pps_idx * 0x100, but the
//  instance count comes from intel_num_pps
//  (reference/i915/display/intel_pps.c:353), which is not transcribed here.
//  Only instance 0 is live in the capture, so only instance 0 is addressable.
//

#ifndef INTEL_PANEL_HPP
#define INTEL_PANEL_HPP

//
// Plain builtins, for the reason given at IntelDisplay.hpp's IGU32: <cstdint>
// is not available to a -mkernel -fapple-kext translation unit. The names are
// this header's own so it can be included next to any sibling header.
//
typedef unsigned int       IGPN32;
typedef unsigned long long IGPN64;

//
// Returned by the address helpers for an input they cannot translate. 0 would
// be a valid MMIO offset, so the sentinel is a value no display offset can
// take. It has the same value as IntelDisplay.hpp's IG_MMIO_INVALID.
//
#define IG_PANEL_REG_INVALID            0xFFFFFFFFu

//
// What a read of a block that is not decoding returns on this silicon. On the
// same boot, an unprogrammed page that IS decoding read 0, not all-ones
// (IntelDisplay.hpp:1432-1439). This matters here because PP_ON is bit 31 and
// BXT_BLC_PWM_ENABLE is bit 31: an all-ones refusal would otherwise decode as
// a powered panel with its backlight enabled. Every verdict below refuses it.
//
#define IG_PANEL_WORD_REFUSED           0xFFFFFFFFu

//
// Returned by igBacklightDutyPercent() when the words do not give a ratio.
//
#define IG_PANEL_DUTY_INVALID           0xFFFFFFFFu

static inline bool igPanelWordRefused(IGPN32 word)
{
    return word == IG_PANEL_WORD_REFUSED;
}

//
// --- panel power sequencing: reference/i915/display/intel_pps_regs.h -------
//
// _MMIO_PPS(dev_priv, pps_idx, reg) (pps:17-18) is
//   pps.mmio_base - PPS_BASE + reg + pps_idx * 0x100
// with mmio_base = PCH_PPS_BASE on this platform (chain above). The _PP_*
// offsets are written relative to the legacy PPS_BASE, so the live address is
// the offset moved onto PCH_PPS_BASE: _PP_STATUS 0x61200 -> 0xC7200.
//
#define IG_PANEL_PPS_BASE               0x61200u   // PPS_BASE, pps:13
#define IG_PANEL_PCH_PPS_BASE           0xC7200u   // PCH_PPS_BASE, pps:15

#define IG_PANEL_PP_STATUS              0x61200u   // _PP_STATUS, pps:20
#define IG_PANEL_PP_CONTROL             0x61204u   // _PP_CONTROL, pps:47
#define IG_PANEL_PP_ON_DELAYS           0x61208u   // _PP_ON_DELAYS, pps:57
#define IG_PANEL_PP_OFF_DELAYS          0x6120Cu   // _PP_OFF_DELAYS, pps:68

// PP_STATUS fields
#define IG_PANEL_PP_ON                  0x80000000u  // PP_ON, REG_BIT(31), pps:22
#define IG_PANEL_PP_READY               0x40000000u  // PP_READY, REG_BIT(30), pps:30
#define IG_PANEL_PP_SEQUENCE_MASK       0x30000000u  // REG_GENMASK(29, 28), pps:31
#define IG_PANEL_PP_SEQUENCE_SHIFT      28u
#define IG_PANEL_PP_SEQUENCE_NONE       0u           // pps:32
#define IG_PANEL_PP_SEQUENCE_POWER_UP   1u           // pps:33
#define IG_PANEL_PP_SEQUENCE_POWER_DOWN 2u           // pps:34
#define IG_PANEL_PP_CYCLE_DELAY_ACTIVE  0x08000000u  // REG_BIT(27), pps:35
#define IG_PANEL_PP_STATE_MASK          0x0000000Fu  // REG_GENMASK(3, 0), pps:36
#define IG_PANEL_PP_STATE_OFF_IDLE      0x0u         // pps:37
#define IG_PANEL_PP_STATE_OFF_S0_1      0x1u         // pps:38
#define IG_PANEL_PP_STATE_OFF_S0_2      0x2u         // pps:39
#define IG_PANEL_PP_STATE_OFF_S0_3      0x3u         // pps:40
#define IG_PANEL_PP_STATE_ON_IDLE       0x8u         // pps:41
#define IG_PANEL_PP_STATE_ON_S1_1       0x9u         // pps:42
#define IG_PANEL_PP_STATE_ON_S1_2       0xAu         // pps:43
#define IG_PANEL_PP_STATE_ON_S1_3       0xBu         // pps:44
#define IG_PANEL_PP_STATE_RESET         0xFu         // pps:45

// PP_CONTROL fields. PANEL_UNLOCK_REGS (pps:50) is a write key and is not
// used; the unlock field is exposed raw only.
#define IG_PANEL_UNLOCK_MASK            0xFFFF0000u  // REG_GENMASK(31, 16), pps:49
#define IG_PANEL_UNLOCK_SHIFT           16u
#define IG_PANEL_POWER_CYCLE_DELAY_MASK 0x000001F0u  // BXT_POWER_CYCLE_DELAY_MASK, REG_GENMASK(8, 4), pps:51
#define IG_PANEL_POWER_CYCLE_DELAY_SHIFT 4u
#define IG_PANEL_EDP_FORCE_VDD          0x00000008u  // REG_BIT(3), pps:52
#define IG_PANEL_EDP_BLC_ENABLE         0x00000004u  // REG_BIT(2), pps:53
#define IG_PANEL_POWER_RESET            0x00000002u  // REG_BIT(1), pps:54
#define IG_PANEL_POWER_ON               0x00000001u  // REG_BIT(0), pps:55

// PP_ON_DELAYS and PP_OFF_DELAYS fields, raw (no unit is cited, see above)
#define IG_PANEL_PORT_SELECT_MASK       0xC0000000u  // REG_GENMASK(31, 30), pps:59
#define IG_PANEL_PORT_SELECT_SHIFT      30u
#define IG_PANEL_POWER_UP_DELAY_MASK    0x1FFF0000u  // REG_GENMASK(28, 16), pps:65
#define IG_PANEL_LIGHT_ON_DELAY_MASK    0x00001FFFu  // REG_GENMASK(12, 0), pps:66
#define IG_PANEL_POWER_DOWN_DELAY_MASK  0x1FFF0000u  // REG_GENMASK(28, 16), pps:70
#define IG_PANEL_LIGHT_OFF_DELAY_MASK   0x00001FFFu  // REG_GENMASK(12, 0), pps:71
#define IG_PANEL_DELAY_HI_SHIFT         16u

//
// The live address of a PPS register on instance 0, or IG_PANEL_REG_INVALID
// for anything that is not one of the four _PP_* offsets above.
//
static inline IGPN32 igPanelPpsReg(IGPN32 ppReg)
{
    switch (ppReg) {
    case IG_PANEL_PP_STATUS:
    case IG_PANEL_PP_CONTROL:
    case IG_PANEL_PP_ON_DELAYS:
    case IG_PANEL_PP_OFF_DELAYS:
        return IG_PANEL_PCH_PPS_BASE - IG_PANEL_PPS_BASE + ppReg;
    default:
        return IG_PANEL_REG_INVALID;
    }
}

static inline IGPN32 igPanelSequence(IGPN32 ppStatus)
{
    return (ppStatus & IG_PANEL_PP_SEQUENCE_MASK) >> IG_PANEL_PP_SEQUENCE_SHIFT;
}

static inline IGPN32 igPanelSequenceState(IGPN32 ppStatus)
{
    return ppStatus & IG_PANEL_PP_STATE_MASK;
}

static inline bool igPanelCycleDelayActive(IGPN32 ppStatus)
{
    return (ppStatus & IG_PANEL_PP_CYCLE_DELAY_ACTIVE) != 0u;
}

//
// API: panel powered. Strict on purpose. PP_ON alone is one bit, so this also
// requires the sequencer to report no sequence in progress and the ON_IDLE
// state. A word caught mid-sequence, or an all-ones refusal, is not powered.
//
static inline bool igPanelPowered(IGPN32 ppStatus)
{
    if (igPanelWordRefused(ppStatus))
        return false;
    return (ppStatus & IG_PANEL_PP_ON) != 0u
        && igPanelSequence(ppStatus) == IG_PANEL_PP_SEQUENCE_NONE
        && igPanelSequenceState(ppStatus) == IG_PANEL_PP_STATE_ON_IDLE;
}

static inline bool igPanelPowerOnRequested(IGPN32 ppControl)
{
    return !igPanelWordRefused(ppControl)
        && (ppControl & IG_PANEL_POWER_ON) != 0u;
}

static inline bool igPanelForceVdd(IGPN32 ppControl)
{
    return !igPanelWordRefused(ppControl)
        && (ppControl & IG_PANEL_EDP_FORCE_VDD) != 0u;
}

static inline bool igPanelEdpBlcEnabled(IGPN32 ppControl)
{
    return !igPanelWordRefused(ppControl)
        && (ppControl & IG_PANEL_EDP_BLC_ENABLE) != 0u;
}

static inline IGPN32 igPanelUnlockRaw(IGPN32 ppControl)
{
    return (ppControl & IG_PANEL_UNLOCK_MASK) >> IG_PANEL_UNLOCK_SHIFT;
}

static inline IGPN32 igPanelPowerCycleDelayRaw(IGPN32 ppControl)
{
    return (ppControl & IG_PANEL_POWER_CYCLE_DELAY_MASK)
        >> IG_PANEL_POWER_CYCLE_DELAY_SHIFT;
}

// The raw port-select field. No port name is attached to its value here.
static inline IGPN32 igPanelPortSelectRaw(IGPN32 ppOnDelays)
{
    return (ppOnDelays & IG_PANEL_PORT_SELECT_MASK) >> IG_PANEL_PORT_SELECT_SHIFT;
}

static inline IGPN32 igPanelPowerUpDelayRaw(IGPN32 ppOnDelays)
{
    return (ppOnDelays & IG_PANEL_POWER_UP_DELAY_MASK) >> IG_PANEL_DELAY_HI_SHIFT;
}

static inline IGPN32 igPanelLightOnDelayRaw(IGPN32 ppOnDelays)
{
    return ppOnDelays & IG_PANEL_LIGHT_ON_DELAY_MASK;
}

static inline IGPN32 igPanelPowerDownDelayRaw(IGPN32 ppOffDelays)
{
    return (ppOffDelays & IG_PANEL_POWER_DOWN_DELAY_MASK) >> IG_PANEL_DELAY_HI_SHIFT;
}

static inline IGPN32 igPanelLightOffDelayRaw(IGPN32 ppOffDelays)
{
    return ppOffDelays & IG_PANEL_LIGHT_OFF_DELAY_MASK;
}

//
// --- backlight PWM: reference/i915/display/intel_backlight_regs.h ----------
//
// BXT_BLC_PWM_*(controller) is _MMIO_PIPE over the two literal register sets
// at blc:88-96, so the helper below is a table of those six literals rather
// than a stride. Which controller drives this panel is not in either vendored
// header. The capture's 0xC8000 page has three live words, all on controller
// 0, and controller 1's 0xC8350..0xC8358 read trivial.
//
#define IG_BLC_PWM_CTL                  0u
#define IG_BLC_PWM_FREQ                 1u
#define IG_BLC_PWM_DUTY                 2u

#define IG_BLC_PWM_CTL1                 0xC8250u     // _BXT_BLC_PWM_CTL1, blc:88
#define IG_BLC_PWM_FREQ1                0xC8254u     // _BXT_BLC_PWM_FREQ1, blc:91
#define IG_BLC_PWM_DUTY1                0xC8258u     // _BXT_BLC_PWM_DUTY1, blc:92
#define IG_BLC_PWM_CTL2                 0xC8350u     // _BXT_BLC_PWM_CTL2, blc:94
#define IG_BLC_PWM_FREQ2                0xC8354u     // _BXT_BLC_PWM_FREQ2, blc:95
#define IG_BLC_PWM_DUTY2                0xC8358u     // _BXT_BLC_PWM_DUTY2, blc:96

#define IG_BLC_PWM_ENABLE               0x80000000u  // BXT_BLC_PWM_ENABLE (1 << 31), blc:89
#define IG_BLC_PWM_POLARITY             0x20000000u  // BXT_BLC_PWM_POLARITY (1 << 29), blc:90

static inline IGPN32 igBlcPwmReg(IGPN32 controller, IGPN32 field)
{
    if (controller == 0u) {
        switch (field) {
        case IG_BLC_PWM_CTL:  return IG_BLC_PWM_CTL1;
        case IG_BLC_PWM_FREQ: return IG_BLC_PWM_FREQ1;
        case IG_BLC_PWM_DUTY: return IG_BLC_PWM_DUTY1;
        default:              return IG_PANEL_REG_INVALID;
        }
    }
    if (controller == 1u) {
        switch (field) {
        case IG_BLC_PWM_CTL:  return IG_BLC_PWM_CTL2;
        case IG_BLC_PWM_FREQ: return IG_BLC_PWM_FREQ2;
        case IG_BLC_PWM_DUTY: return IG_BLC_PWM_DUTY2;
        default:              return IG_PANEL_REG_INVALID;
        }
    }
    return IG_PANEL_REG_INVALID;
}

static inline bool igBlcPwmEnabled(IGPN32 pwmCtl)
{
    return !igPanelWordRefused(pwmCtl)
        && (pwmCtl & IG_BLC_PWM_ENABLE) != 0u;
}

// Reported, not interpreted: duty percent below is the raw register ratio
// whatever this bit says.
static inline bool igBlcPwmPolaritySet(IGPN32 pwmCtl)
{
    return !igPanelWordRefused(pwmCtl)
        && (pwmCtl & IG_BLC_PWM_POLARITY) != 0u;
}

//
// API: backlight enabled. The eDP backlight has two enables, EDP_BLC_ENABLE in
// PP_CONTROL (pps:53) and BXT_BLC_PWM_ENABLE in the PWM controller (blc:89).
// This reports enabled only when both read set. Each one is also exposed on
// its own above.
//
static inline bool igBacklightEnabled(IGPN32 ppControl, IGPN32 pwmCtl)
{
    return igPanelEdpBlcEnabled(ppControl) && igBlcPwmEnabled(pwmCtl);
}

//
// API: duty percent, truncated, 0..100. FREQ and DUTY are whole words in the
// BXT layout (blc:91-92). Returns IG_PANEL_DUTY_INVALID for a refused word, a
// zero frequency or a duty above the frequency, rather than clamping. The
// product is formed in 64 bits, so a duty near 2^32 cannot wrap.
//
static inline IGPN32 igBacklightDutyPercent(IGPN32 pwmFreq, IGPN32 pwmDuty)
{
    if (igPanelWordRefused(pwmFreq) || igPanelWordRefused(pwmDuty))
        return IG_PANEL_DUTY_INVALID;
    if (pwmFreq == 0u || pwmDuty > pwmFreq)
        return IG_PANEL_DUTY_INVALID;
    return (IGPN32)(((IGPN64)pwmDuty * 100u) / (IGPN64)pwmFreq);
}

//
// Powered, both backlight enables set and a nonzero duty that forms a valid
// ratio. The test is duty != 0 rather than percent != 0, so a dim panel whose
// percent truncates to 0 still counts as lit.
//
static inline bool igPanelLit(IGPN32 ppStatus, IGPN32 ppControl, IGPN32 pwmCtl,
                              IGPN32 pwmFreq, IGPN32 pwmDuty)
{
    if (!igPanelPowered(ppStatus) || !igBacklightEnabled(ppControl, pwmCtl))
        return false;
    if (igBacklightDutyPercent(pwmFreq, pwmDuty) == IG_PANEL_DUTY_INVALID)
        return false;
    return pwmDuty != 0u;
}

//
//  ============================================================================
//  VALUES WITNESSED ON SILICON
//  ============================================================================
//
//  igpu-results/tgl-final-capture-2026-08-18-201412.txt, 2026-08-18 20:14:12
//  UTC, lines 1126 (0x0C7200..0x0C720C) and 1129 (0x0C8250..0x0C8258). Each
//  word was read once. Lines 1509-1510 of that file list both ranges as
//  configuration that is safe to harvest. The 2026-08-16 capture,
//  igpu-results/tgl-capture-2026-08-16-022556.txt, holds none of these
//  registers. IntelDisplay.hpp:1498-1517 keeps the same seven words named by
//  offset; the names here come from the two vendored headers.
//
//  What they decode to with the helpers above:
//    PP_STATUS     PP_ON set, PP_READY clear, sequence NONE, state ON_IDLE
//                  -> igPanelPowered() true
//    PP_CONTROL    PANEL_POWER_ON, PANEL_POWER_RESET and EDP_BLC_ENABLE set,
//                  EDP_FORCE_VDD clear, power-cycle delay field 6, unlock 0
//    PP_ON_DELAYS  port select 0, power-up 0x7D0, light-on 0x1 (raw)
//    PP_OFF_DELAYS power-down 0x1F4, light-off 0x1 (raw)
//    PWM CTL1      enable set, polarity clear
//    PWM FREQ1 and DUTY1 are both 0x17700 (96000) -> igBacklightDutyPercent()
//                  100, igBacklightEnabled() true, igPanelLit() true
//
//  FREQ1 and DUTY1 are equal in the capture and stay two constants. They are
//  two registers that held the same value on this panel; one constant would
//  assert that they must.
//
#define IG_PANEL_OBSERVED_PP_STATUS_2026_08_18       0x80000008u  // 0x0C7200
#define IG_PANEL_OBSERVED_PP_CONTROL_2026_08_18      0x00000067u  // 0x0C7204
#define IG_PANEL_OBSERVED_PP_ON_DELAYS_2026_08_18    0x07D00001u  // 0x0C7208
#define IG_PANEL_OBSERVED_PP_OFF_DELAYS_2026_08_18   0x01F40001u  // 0x0C720C
#define IG_PANEL_OBSERVED_BLC_PWM_CTL1_2026_08_18    0x80000000u  // 0x0C8250
#define IG_PANEL_OBSERVED_BLC_PWM_FREQ1_2026_08_18   0x00017700u  // 0x0C8254
#define IG_PANEL_OBSERVED_BLC_PWM_DUTY1_2026_08_18   0x00017700u  // 0x0C8258

#endif // INTEL_PANEL_HPP

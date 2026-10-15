// SPDX-License-Identifier: MIT
// Host-only regression tests for the GA107 SEC2 Booter fuse observer planner.
// No IOKit, PCI access, BAR mapping, MMIO writes, or GPU is required.

#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVGspBooterFuse.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void checkBool(const char *label, bool actual, bool expected)
{
    ++gChecks;
    if (actual == expected) {
        std::printf("ok   %-56s %s\n", label, actual ? "true" : "false");
        return;
    }
    ++gFailures;
    std::printf("FAIL %-56s got %s expected %s\n", label,
                actual ? "true" : "false", expected ? "true" : "false");
}

static void checkU32(const char *label, NVU32 actual, NVU32 expected)
{
    ++gChecks;
    if (actual == expected) {
        std::printf("ok   %-56s 0x%08x\n", label, (unsigned)actual);
        return;
    }
    ++gFailures;
    std::printf("FAIL %-56s got 0x%08x expected 0x%08x\n", label,
                (unsigned)actual, (unsigned)expected);
}

static void checkStr(const char *label, const char *actual, const char *expected)
{
    ++gChecks;
    if (std::strcmp(actual, expected) == 0) {
        std::printf("ok   %-56s %s\n", label, actual);
        return;
    }
    ++gFailures;
    std::printf("FAIL %-56s got %s expected %s\n", label, actual, expected);
}

static const NVU32 kBoot0Ga107 = 0x177000a1u;
static const NVU32 kBoot42Ga107 = 0x177010a1u;
static const NVU32 kBoot0Ga106 = 0x176000a1u;
static const NVU32 kBoot42Ga106 = 0x176010a1u;

static void testConstantsAndTargetAdmission(void)
{
    std::puts("\nconstants + exact-target admission");

    checkU32("PCI vendor", NV_GSP_GA107_PCI_VENDOR_ID, 0x10deu);
    checkU32("PCI device", NV_GSP_GA107_PCI_DEVICE_ID, 0x25a2u);
    checkU32("SEC2 ucode1 fuse register",
             NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG, 0x00824140u);
    checkU32("SEC2 fuse register stride",
             NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE, 4u);
    checkU32("Booter HS ucode id", NV_GSP_GA107_BOOTER_HS_UCODE_ID, 3u);

    checkBool("exact 10de:25a2 + GA107 identity accepted",
              nvGspIsExactGa107Target(0x10deu, 0x25a2u,
                                      kBoot0Ga107, kBoot42Ga107), true);
    checkBool("wrong PCI vendor refused",
              nvGspIsExactGa107Target(0x8086u, 0x25a2u,
                                      kBoot0Ga107, kBoot42Ga107), false);
    checkBool("wrong PCI device refused",
              nvGspIsExactGa107Target(0x10deu, 0x25a3u,
                                      kBoot0Ga107, kBoot42Ga107), false);
    checkBool("dead BOOT_0 refused",
              nvGspIsExactGa107Target(0x10deu, 0x25a2u,
                                      0xffffffffu, kBoot42Ga107), false);
    checkBool("zero BOOT_42 refused",
              nvGspIsExactGa107Target(0x10deu, 0x25a2u,
                                      kBoot0Ga107, 0u), false);
    checkBool("GA106 identity refused",
              nvGspIsExactGa107Target(0x10deu, 0x25a2u,
                                      kBoot0Ga106, kBoot42Ga106), false);
    checkBool("mismatched BOOT_42 silicon refused",
              nvGspIsExactGa107Target(0x10deu, 0x25a2u,
                                      kBoot0Ga107, kBoot42Ga106), false);
}

static void testReadPlanning(void)
{
    std::puts("\nread planning + atomic refusals");

    NVGspBooterFuseReadPlan p = {0xaaaaaaaau, 0xbbbbbbbbu, false};
    checkU32("ucode 1 plan succeeds",
             nvGspPlanGa107BooterFuseRead(0x10deu, 0x25a2u,
                                          kBoot0Ga107, kBoot42Ga107,
                                          1u, &p),
             NV_GSP_BOOTER_FUSE_OK);
    checkU32("ucode 1 register", p.registerOffset, 0x00824140u);
    checkBool("ucode 1 plan valid", p.valid, true);

    checkU32("Booter ucode 3 plan succeeds",
             nvGspPlanGa107BooterFuseRead(0x10deu, 0x25a2u,
                                          kBoot0Ga107, kBoot42Ga107,
                                          3u, &p),
             NV_GSP_BOOTER_FUSE_OK);
    checkU32("Booter ucode 3 register", p.registerOffset, 0x00824148u);
    checkU32("ucode 16 plan succeeds",
             nvGspPlanGa107BooterFuseRead(0x10deu, 0x25a2u,
                                          kBoot0Ga107, kBoot42Ga107,
                                          16u, &p),
             NV_GSP_BOOTER_FUSE_OK);
    checkU32("ucode 16 register", p.registerOffset, 0x0082417cu);

    NVGspBooterFuseReadPlan sentinel = {0x11223344u, 0x55667788u, true};
    checkU32("ucode 0 refused",
             nvGspPlanGa107BooterFuseRead(0x10deu, 0x25a2u,
                                          kBoot0Ga107, kBoot42Ga107,
                                          0u, &sentinel),
             NV_GSP_BOOTER_FUSE_BAD_UCODE_ID);
    checkU32("ucode 0 refusal leaves id untouched", sentinel.ucodeId, 0x11223344u);
    checkU32("ucode 0 refusal leaves offset untouched", sentinel.registerOffset, 0x55667788u);
    checkBool("ucode 0 refusal leaves valid untouched", sentinel.valid, true);

    checkU32("ucode 17 refused",
             nvGspPlanGa107BooterFuseRead(0x10deu, 0x25a2u,
                                          kBoot0Ga107, kBoot42Ga107,
                                          17u, &sentinel),
             NV_GSP_BOOTER_FUSE_BAD_UCODE_ID);
    checkU32("wrong PCI refused before planning",
             nvGspPlanGa107BooterFuseRead(0x10deu, 0x25a3u,
                                          kBoot0Ga107, kBoot42Ga107,
                                          3u, &sentinel),
             NV_GSP_BOOTER_FUSE_WRONG_PCI);
    checkU32("dead identity refused before planning",
             nvGspPlanGa107BooterFuseRead(0x10deu, 0x25a2u,
                                          0xffffffffu, kBoot42Ga107,
                                          3u, &sentinel),
             NV_GSP_BOOTER_FUSE_DEAD_IDENTITY);
    checkU32("wrong silicon refused before planning",
             nvGspPlanGa107BooterFuseRead(0x10deu, 0x25a2u,
                                          kBoot0Ga106, kBoot42Ga106,
                                          3u, &sentinel),
             NV_GSP_BOOTER_FUSE_WRONG_SILICON);
    checkU32("null plan output refused",
             nvGspPlanGa107BooterFuseRead(0x10deu, 0x25a2u,
                                          kBoot0Ga107, kBoot42Ga107,
                                          3u, nullptr),
             NV_GSP_BOOTER_FUSE_NULL_OUTPUT);
}

static void testCanonicalPlanValidation(void)
{
    std::puts("\ncanonical plan validation");

    NVGspBooterFuseReadPlan p = {3u, 0x00824148u, true};
    checkBool("canonical ucode 3 plan accepted",
              nvGspBooterFusePlanIsCanonical(&p), true);
    p.registerOffset += 4u;
    checkBool("tampered register offset refused",
              nvGspBooterFusePlanIsCanonical(&p), false);
    p = {0u, 0x00824140u, true};
    checkBool("ucode 0 canonical check refused",
              nvGspBooterFusePlanIsCanonical(&p), false);
    p = {17u, 0x00824180u, true};
    checkBool("ucode 17 canonical check refused",
              nvGspBooterFusePlanIsCanonical(&p), false);
    p = {3u, 0x00824148u, false};
    checkBool("invalid flag refused",
              nvGspBooterFusePlanIsCanonical(&p), false);
    checkBool("null plan refused",
              nvGspBooterFusePlanIsCanonical(nullptr), false);
}

static void testFuseVersionDecode(void)
{
    std::puts("\nfuse bitmask -> version");

    checkU32("zero mask is version 0", nvGspBooterFuseVersionFromMask(0u), 0u);
    checkU32("bit 0 is version 1", nvGspBooterFuseVersionFromMask(0x1u), 1u);
    checkU32("bit 1 is version 2", nvGspBooterFuseVersionFromMask(0x2u), 2u);
    checkU32("bits 0+1 still version 2", nvGspBooterFuseVersionFromMask(0x3u), 2u);
    checkU32("bit 15 is version 16", nvGspBooterFuseVersionFromMask(0x00008000u), 16u);
    checkU32("reserved/high bit 16 mirrors RM as version 17",
             nvGspBooterFuseVersionFromMask(0x00010000u), 17u);
    checkU32("top bit is version 32",
             nvGspBooterFuseVersionFromMask(0x80000000u), 32u);
}

static void testObservationDecode(void)
{
    std::puts("\nobservation decode + dead-read refusal");

    NVGspBooterFuseReadPlan p = {3u, 0x00824148u, true};
    NVGspBooterFuseObservation o = {0u, 0u, 0u, 0u, false};

    checkU32("zero fuse succeeds",
             nvGspDecodeGa107BooterFuse(&p, 0u, &o),
             NV_GSP_BOOTER_FUSE_OK);
    checkU32("zero fuse keeps raw value", o.rawFuse, 0u);
    checkU32("zero fuse publishes version 0", o.fuseVersion, 0u);
    checkU32("observation keeps ucode id", o.ucodeId, 3u);
    checkU32("observation keeps canonical register", o.registerOffset, 0x00824148u);
    checkBool("observation valid", o.valid, true);

    checkU32("0x4 fuse succeeds",
             nvGspDecodeGa107BooterFuse(&p, 0x4u, &o),
             NV_GSP_BOOTER_FUSE_OK);
    checkU32("0x4 fuse is version 3", o.fuseVersion, 3u);

    NVGspBooterFuseObservation sentinel = {
        0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, true
    };
    checkU32("all-ones BAR refusal is dead read",
             nvGspDecodeGa107BooterFuse(&p, 0xffffffffu, &sentinel),
             NV_GSP_BOOTER_FUSE_DEAD_READ);
    checkU32("dead-read refusal leaves ucode untouched", sentinel.ucodeId, 0x11111111u);
    checkU32("dead-read refusal leaves raw untouched", sentinel.rawFuse, 0x33333333u);
    checkU32("dead-read refusal leaves version untouched", sentinel.fuseVersion, 0x44444444u);
    checkBool("dead-read refusal leaves valid untouched", sentinel.valid, true);

    NVGspBooterFuseReadPlan bad = {3u, 0x0082414cu, true};
    checkU32("noncanonical plan refused",
             nvGspDecodeGa107BooterFuse(&bad, 1u, &sentinel),
             NV_GSP_BOOTER_FUSE_BAD_PLAN);
    checkU32("null observation output refused",
             nvGspDecodeGa107BooterFuse(&p, 1u, nullptr),
             NV_GSP_BOOTER_FUSE_NULL_OUTPUT);
}

static void testVerdictNames(void)
{
    std::puts("\nverdict names");
    checkStr("ok", nvGspBooterFuseVerdictName(NV_GSP_BOOTER_FUSE_OK), "ok");
    checkStr("wrong pci", nvGspBooterFuseVerdictName(NV_GSP_BOOTER_FUSE_WRONG_PCI), "wrong-pci");
    checkStr("dead read", nvGspBooterFuseVerdictName(NV_GSP_BOOTER_FUSE_DEAD_READ), "dead-read");
    checkStr("unknown", nvGspBooterFuseVerdictName(0xffffffffu), "unknown");
    checkBool("success predicate accepts only OK",
              nvGspBooterFuseSucceeded(NV_GSP_BOOTER_FUSE_OK), true);
    checkBool("success predicate refuses dead read",
              nvGspBooterFuseSucceeded(NV_GSP_BOOTER_FUSE_DEAD_READ), false);
}

int main(void)
{
    testConstantsAndTargetAdmission();
    testReadPlanning();
    testCanonicalPlanValidation();
    testFuseVersionDecode();
    testObservationDecode();
    testVerdictNames();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

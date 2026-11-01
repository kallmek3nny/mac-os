// SPDX-License-Identifier: MIT
#include <cstdio>
#include "NVGa107PciIdentity.hpp"

static unsigned checks=0, failures=0;
static void checkThat(const char *name,bool value) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n",name); }
}
int main() {
    checkThat("GA107M 3D class",nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x030200a1u));
    checkThat("GA107M display VGA class",nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x030000ffu));
    checkThat("other subclass allowed",nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x03ffffffu));
    checkThat("intel vendor",!nvGa107PciIdentityEligible(0x8086u,0x25a2u,0x030200a1u));
    checkThat("missing vendor",!nvGa107PciIdentityEligible(0u,0x25a2u,0x030200a1u));
    checkThat("wide vendor",!nvGa107PciIdentityEligible(0x110deu,0x25a2u,0x030200a1u));
    checkThat("neighbor device",!nvGa107PciIdentityEligible(0x10deu,0x25a3u,0x030200a1u));
    checkThat("different GA107",!nvGa107PciIdentityEligible(0x10deu,0x25a1u,0x030200a1u));
    checkThat("missing device",!nvGa107PciIdentityEligible(0x10deu,0u,0x030200a1u));
    checkThat("wide device",!nvGa107PciIdentityEligible(0x10deu,0x125a2u,0x030200a1u));
    checkThat("network class",!nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x020000a1u));
    checkThat("multimedia class",!nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x040200a1u));
    checkThat("subclass not base class",!nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x000300a1u));
    checkThat("prog-if not base class",!nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x000003a1u));
    checkThat("dead PCI read",!nvGa107PciIdentityEligible(0xffffffffu,0xffffffffu,0xffffffffu));
    checkThat("zero PCI read",!nvGa107PciIdentityEligible(0u,0u,0u));
    checkThat("same exact eligible identity preserved",
        nvGa107PciIdentityStableAcrossStage(
            0x10deu,0x25a2u,0x030200a1u,0x10deu,0x25a2u,0x030200a1u));
    checkThat("eligible differing revision refused",
        !nvGa107PciIdentityStableAcrossStage(
            0x10deu,0x25a2u,0x030200a1u,0x10deu,0x25a2u,0x030200a2u));
    checkThat("eligible differing subclass refused",
        !nvGa107PciIdentityStableAcrossStage(
            0x10deu,0x25a2u,0x030200a1u,0x10deu,0x25a2u,0x030000a1u));
    checkThat("removed vendor refused",
        !nvGa107PciIdentityStableAcrossStage(
            0x10deu,0x25a2u,0x030200a1u,0xffffu,0x25a2u,0x030200a1u));
    checkThat("substituted device refused",
        !nvGa107PciIdentityStableAcrossStage(
            0x10deu,0x25a2u,0x030200a1u,0x10deu,0x25a3u,0x030200a1u));
    checkThat("different class refused",
        !nvGa107PciIdentityStableAcrossStage(
            0x10deu,0x25a2u,0x030200a1u,0x10deu,0x25a2u,0x020000a1u));
    checkThat("bad original identity refused even if both identical",
        !nvGa107PciIdentityStableAcrossStage(
            0x8086u,0x25a2u,0x030200a1u,0x8086u,0x25a2u,0x030200a1u));
    checkThat("all-ones reads never establish stability",
        !nvGa107PciIdentityStableAcrossStage(
            0xffffffffu,0xffffffffu,0xffffffffu,
            0xffffffffu,0xffffffffu,0xffffffffu));
    std::printf("\n%d checks, %d failures\n", static_cast<int>(checks), static_cast<int>(failures));
    return failures?1:0;
}

// SPDX-License-Identifier: MIT
#include <cstdio>
#include "NVGa107PciIdentity.hpp"

static unsigned checks=0, failures=0;
static void check(const char *name,bool value) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n",name); }
}
int main() {
    check("GA107M 3D class",nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x030200a1u));
    check("GA107M display VGA class",nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x030000ffu));
    check("other subclass allowed",nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x03ffffffu));
    check("intel vendor",!nvGa107PciIdentityEligible(0x8086u,0x25a2u,0x030200a1u));
    check("missing vendor",!nvGa107PciIdentityEligible(0u,0x25a2u,0x030200a1u));
    check("wide vendor",!nvGa107PciIdentityEligible(0x110deu,0x25a2u,0x030200a1u));
    check("neighbor device",!nvGa107PciIdentityEligible(0x10deu,0x25a3u,0x030200a1u));
    check("different GA107",!nvGa107PciIdentityEligible(0x10deu,0x25a1u,0x030200a1u));
    check("missing device",!nvGa107PciIdentityEligible(0x10deu,0u,0x030200a1u));
    check("wide device",!nvGa107PciIdentityEligible(0x10deu,0x125a2u,0x030200a1u));
    check("network class",!nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x020000a1u));
    check("multimedia class",!nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x040200a1u));
    check("subclass not base class",!nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x000300a1u));
    check("prog-if not base class",!nvGa107PciIdentityEligible(0x10deu,0x25a2u,0x000003a1u));
    check("dead PCI read",!nvGa107PciIdentityEligible(0xffffffffu,0xffffffffu,0xffffffffu));
    check("zero PCI read",!nvGa107PciIdentityEligible(0u,0u,0u));
    std::printf("%u checks, %u failures\n",checks,failures);
    return failures?1:0;
}

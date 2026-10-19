// SPDX-License-Identifier: MIT
// Exact GA107M PCI eligibility, independent of IOKit or hardware I/O.
#ifndef NV_GA107_PCI_IDENTITY_HPP
#define NV_GA107_PCI_IDENTITY_HPP

using NVGa107PciU32 = unsigned int;

// The PCI class/revision dword has base class in bits 31:24.
static inline bool nvGa107PciIdentityEligible(NVGa107PciU32 vendor,
                                               NVGa107PciU32 device,
                                               NVGa107PciU32 classRevision)
{
    return vendor == 0x10deu && device == 0x25a2u &&
           ((classRevision >> 24u) & 0xffu) == 0x03u;
}

#endif // NV_GA107_PCI_IDENTITY_HPP

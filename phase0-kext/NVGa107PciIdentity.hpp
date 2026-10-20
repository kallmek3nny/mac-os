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

// Exact class/revision must remain pinned for an IOKit provider stage. This
 // comparison is read-only; it does not prove that DMA or GSP is operational.
static inline bool nvGa107PciIdentityStableAcrossStage(
    NVGa107PciU32 beforeVendor,
    NVGa107PciU32 beforeDevice,
    NVGa107PciU32 beforeClassRevision,
    NVGa107PciU32 afterVendor,
    NVGa107PciU32 afterDevice,
    NVGa107PciU32 afterClassRevision)
{
    return nvGa107PciIdentityEligible(beforeVendor,beforeDevice,
                                      beforeClassRevision) &&
           nvGa107PciIdentityEligible(afterVendor,afterDevice,
                                      afterClassRevision) &&
           beforeVendor == afterVendor &&
           beforeDevice == afterDevice &&
           beforeClassRevision == afterClassRevision;
}

#endif // NV_GA107_PCI_IDENTITY_HPP

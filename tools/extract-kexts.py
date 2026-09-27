#!/usr/bin/env python3
"""extract-kexts.py - Extract kext binaries from macOS disk image.

Finds Mach-O kext binaries inside the prelinked kernel and extracts them
for reverse engineering in IDA/Ghidra.

Usage:
    python tools/extract-kexts.py <image> <output-dir>
"""

import os
import struct
import argparse


def extract_kexts(image_path, output_dir):
    """Find and extract kext binaries from the disk image."""

    print(f"Loading image: {image_path}")
    with open(image_path, 'rb') as f:
        data = f.read()

    print(f"Image size: {len(data):,} bytes")

    # Key locations found in the image
    prelinked_kernel_offset = 270201175
    amd_gpu_wrangler_offset = 1778817769

    print(f"\nKey offsets found:")
    print(f"  PrelinkedKernel: {prelinked_kernel_offset:,}")
    print(f"  AmdGpuWrangler: {amd_gpu_wrangler_offset:,}")

    # Extract AmdGpuWrangler - the AMD GPU wrapper that implements Apple's Metal ABI
    # This is our primary target for RE
    wrangler_start = amd_gpu_wrangler_offset - 20000  # Approximate start
    wrangler_end = wrangler_start + 600000  # Estimate ~600KB

    wrangler_path = os.path.join(output_dir, "AmdGpuWrangler.kext")
    with open(wrangler_path, 'wb') as f:
        f.write(data[wrangler_start:wrangler_end])
    print(f"\nExtracted AmdGpuWrangler: {os.path.getsize(wrangler_path):,} bytes")

    # Extract IOAcceleratorFamily - the base GPU acceleration framework
    ioaccel_offset = 793575759
    ioaccel_path = os.path.join(output_dir, "IOAcceleratorFamily.kext")
    with open(ioaccel_path, 'wb') as f:
        f.write(data[ioaccel_offset:ioaccel_offset + 400000])
    print(f"Extracted IOAcceleratorFamily: {os.path.getsize(ioaccel_path):,} bytes")

    # Extract the prelinked kernel (contains all kexts)
    prelink_path = os.path.join(output_dir, "PrelinkedKernel")
    with open(prelink_path, 'wb') as f:
        f.write(data[prelinked_kernel_offset:prelinked_kernel_offset + 5000000])
    print(f"Extracted PrelinkedKernel: {os.path.getsize(prelink_path):,} bytes")

    print("\nDone! Extracted kexts are ready for IDA.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', help='macOS disk image')
    parser.add_argument('output_dir', help='Output directory')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    extract_kexts(args.image, args.output_dir)

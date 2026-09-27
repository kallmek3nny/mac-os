#!/usr/bin/env python3
"""extract-gpu-kexts.py - Extract GPU kext binaries from macOS disk image.

Extracts AMDRadeonX6000Framebuffer, AppleGPUWrangler, and IOAcceleratorFamily
for reverse engineering to discover Apple's Metal driver ABI.
"""

import os
import struct
import argparse


def extract_region(image_data, start, end, output_path):
    """Extract a region from the image and write to file."""
    # Extract and find the actual binary boundaries
    region = image_data[start:end]

    # Look for Mach-O header in this region
    macho_32 = b'\xfe\xed\xfa\xcf'
    macho_64 = b'\xfe\xed\xfa\xce'

    macho_pos = -1
    for magic in [macho_32, macho_64]:
        pos = region.find(magic)
        if pos >= 0:
            macho_pos = pos
            break

    if macho_pos >= 0:
        # Found Mach-O header, extract the binary
        # Read the header to find size
        header_start = macho_pos
        header_end = min(len(region), macho_pos + 28)

        if header_end - header_start == 28:
            header = region[header_start:header_end]
            ncmds = struct.unpack('<I', header[20:24])[0]
            sizesize = struct.unpack('<I', header[24:28])[0]

            # Mach-O binary continues until next section/boundary
            # For kexts, they're typically 200KB-1MB
            # Conservative: extract 500KB from Mach-O header
            binary_size = min(500 * 1024, len(region) - macho_pos)

            with open(output_path, 'wb') as f:
                f.write(region[header_start:header_start + binary_size])
            return os.path.getsize(output_path)

    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', help='macOS disk image')
    parser.add_argument('output_dir', help='Output directory')
    args = parser.parse_args()

    print(f"Loading image: {args.image}")
    with open(args.image, 'rb') as f:
        data = f.read()

    print(f"Image size: {len(data):,} bytes")
    os.makedirs(args.output_dir, exist_ok=True)

    # GPU kext regions identified from the scan
    gpu_kexts = [
        # AMD Radeon X6000 (AMD GPU Metal driver - best ABI reference)
        ("AMDRadeonX6000Framebuffer", 1778766295, 1779016295),
        # IOAcceleratorFamily (base GPU acceleration framework)
        ("IOAcceleratorFamily", 793525795, 793775795),
        # AppleGPUWrangler (Apple's GPU wrapper)
        ("AppleGPUWrangler", 1778817000, 1778917000),
    ]

    print("\nExtracting GPU kexts:")
    for name, start, end in gpu_kexts:
        output_path = os.path.join(args.output_dir, f"{name}.ext")
        size = extract_region(data, start, end, output_path)
        if size > 0:
            print(f"  {name}: {size:,} bytes")
        else:
            # Try fallback extraction
            fallback_path = os.path.join(args.output_dir, f"{name}.bin")
            with open(fallback_path, 'wb') as f:
                f.write(data[start:end])
            size = os.path.getsize(fallback_path)
            print(f"  {name}: {size:,} bytes (raw region)")

    print("\nDone! Extracted kexts are ready for IDA.")


if __name__ == "__main__":
    main()

# Metal GPU Driver Reverse Engineering Plan

## Goal
Discover Apple's Metal driver plugin ABI to enable GPU acceleration on unsupported hardware.

## Target Kexts
| Kext | Purpose | Size | Notes |
|------|---------|------|-------|
| AMDRadeonX6000Framebuffer.kext | AMD GPU Metal driver (best ABI reference) | ~2MB | Has IOGPUFamily hooks |
| IOGPUFamily.kext | Base GPU family (abstract GPU class) | ~100KB | Where plugins register |
| AppleGPUWrangler.kext | Apple's GPU wrapper | ~1MB | Part of AppleGraphicsControl |
| AppleIntelFramebufferController.kext | Intel driver (reference for Gen11/Gen12) | ~1MB | Most relevant for TGL |

## Phase 1: Acquisition

**Option A - Download macOS Recovery:**
```bash
# Download from Apple's CDN (use a script or manual)
# Search for "macOS Sequoia BaseSystem.img" (~1.5GB)
# Or "macOS Sonoma BaseSystem.img"

# Once downloaded:
hdiutil mount BaseSystem.dmg
cp /Volumes/Recovery/System/Library/Extensions/* ./reference/apple-kexts/
```

**Option B - Extract from existing VM:**
```bash
# If you have a macOS VM:
# ssh user@macos-vm "tar czf - /System/Library/Extensions/*.kext" > kexts.tar.gz
# or mount the VM disk image locally
```

## Phase 2: Analysis Setup

### Load into IDA
```bash
# AMDRadeonX6000Framebuffer.kext (Mach-O 64-bit x86_64)
ida64 AMDRadeonX6000Framebuffer.kext/Contents/MacOS/AMDRadeonX6000Framebuffer

# Key settings:
# - Processor: x86
# - Endian: Little
# - Bitness: 64-bit
```

### Initial Search Strategy
1. **Find plugin entry point:** Search for "IOGPU" or "register" in exports
2. **Find ABI methods:** Look for method names like "get*, "set*", "create*", "start*"
3. **Map IOGPUFamily hooks:** Find function pointers/tables in the kext
4. **Identify Metal IR:** Look for shader compilation code or SASS output

## Phase 3: Key Questions to Answer

### ABI Entry Points
- [ ] What function/entry does IOGPUFamily call to load a plugin?
- [ ] What class/struct must the plugin export?
- [ ] What version numbers/identifiers are checked?

### Method Interface
- [ ] What methods does IOGPUFamily call into the plugin?
- [ ] What are the method signatures (parameters/return types)?
- [ ] How does the plugin report errors/status?

### Metal IR
- [ ] What's the format of Metal IR we need to produce?
- [ ] Is it SASS, assembly, or intermediate representation?
- [ ] How is the compiled code loaded/executed on GPU?

### Driver Lifecycle
- [ ] How does the driver start/stop?
- [ ] How are device handles created/managed?
- [ ] What's the command submission path?

## Phase 4: Implementation Mapping

Once we understand the ABI, we'll need to:
1. Implement matching IOGPUFamily plugin interface
2. Map NVIDIA Metal IR -> GPU instruction stream
3. Wire up command submission and memory management

## Notes
- AMD drivers are the most complete reference (Apple has supported AMD for years)
- Intel drivers may be closer to our target (Gen11/Gen12, but still unsupported)
- AppleGPUWrangler is Apple's high-level GPU orchestration

#
# NV3050Probe -- Phase 0/2/3 kext
#
# Plain make rather than an Xcode project so the build is legible and you can
# see exactly which flags a kext needs. Xcode's kext template does the same
# thing behind a GUI.
#

KEXT_NAME    := NV3050Probe
BUNDLE_ID    := local.nv3050.$(KEXT_NAME)
KEXT_BUNDLE  := $(KEXT_NAME).kext
BUILD_DIR    := build
CONTENTS     := $(BUILD_DIR)/$(KEXT_BUNDLE)/Contents
MACOS_DIR    := $(CONTENTS)/MacOS
INSTALL_DIR  := /Library/Extensions

#
# Four shells, one bundle. NV3050Probe.cpp is the NVIDIA track's IOKit shell;
# the three Intel ones are the Gen12 display track's (IntelDisplayProbe.cpp),
# its inherited-framebuffer presenter (IntelFbPresentProbe.cpp), and the render
# engine consumer (IntelGtProbe.cpp). They share no code and match on different
# vendors, but they link into the same kext because a bundle is the unit macOS
# loads -- two bundles would mean two Info.plists, two signatures and two
# kextload steps for what is one project.
#
# Every Intel shell is READ-ONLY against the GPU by construction; see
# IntelDisplayProbe.hpp for why that asymmetry with the NVIDIA shell is
# deliberate rather than unfinished. IntelGtProbe.cpp carries the strongest form
# of it: it composes a context image and submits nothing, because this tree
# cannot cite the page layout it composes.
#
SOURCES      := NV3050Probe.cpp IntelDisplayProbe.cpp IntelFbPresentProbe.cpp \
	IntelGtProbe.cpp
OBJECTS      := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)

SDK          := $(shell xcrun --sdk macosx --show-sdk-path)
CXX          := $(shell xcrun --sdk macosx --find clang++)
ARCH         := -arch x86_64

#
# Kernel code has no libc, no C++ runtime, no exceptions, no RTTI, and no
# stack protector. -mkernel and -fapple-kext enforce that. Omitting any of
# these produces a kext that links but fails to load with undefined symbols.
#
KERNEL_FLAGS := \
	-mkernel \
	-fapple-kext \
	-fno-builtin \
	-fno-common \
	-fno-exceptions \
	-fno-rtti \
	-fno-stack-protector \
	-nostdinc \
	-nostdinc++ \
	-DKERNEL \
	-DKERNEL_PRIVATE \
	-DDRIVER_PRIVATE \
	-DAPPLE \
	-DNeXT

INCLUDES     := \
	-I$(SDK)/System/Library/Frameworks/Kernel.framework/Headers \
	-I$(SDK)/System/Library/Frameworks/Kernel.framework/PrivateHeaders

CXXFLAGS     := $(ARCH) $(KERNEL_FLAGS) $(INCLUDES) -isysroot $(SDK) \
	-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -MMD -MP

#
# -Xlinker passes exactly ONE token to ld. That matters here, because ld64
# splits its flags into two classes and they look identical on this line:
#
#   -kext              takes NO argument
#   -object_path_lto   takes a PATH, read from ld's next argv entry
#
# `-Xlinker -object_path_lto` used to sit between `-kext` and `-lkmod`. ld saw
# `-object_path_lto -lkmod` and took `-lkmod` as the LTO object path, so
# libkmod.a was never linked -- and libkmod.a is what supplies the kmod_info
# start/stop glue that makes a bundle loadable at all. Nothing on the host can
# see that: it is a linker-argument bug, and this project has never run a
# linker. The flag was also dead weight, since no -flto appears in CXXFLAGS.
#
# Passing it properly would need TWO -Xlinker tokens:
#   -Xlinker -object_path_lto -Xlinker /path/to/lto.o
# `tests/test_kext_makefile.py` asserts that rule for every argument-taking
# ld64 flag, so re-adding one bare cannot pass silently again.
#
LDFLAGS      := $(ARCH) -isysroot $(SDK) \
	-nostdlib \
	-Xlinker -kext \
	-lkmod \
	-lcc_kext

#
# NOT added blind: -lkmodc++. Xcode's kext product type links it for C++ kexts,
# and it is the usual supplier of the static-constructor glue that
# OSDefineMetaClassAndStructors relies on. It is left out here because adding a
# library that turns out not to exist in the guest's SDK fails the link with
# "library not found", which would burn the operator's one attempt on a fault
# this file introduced. If STEP 7 reports undefined C++ runtime symbols at link
# time, add -lkmodc++ above -lcc_kext and re-run; see
# OPERATOR-ACTION-step7-build.md.
#

# make PANIC_TEST=1 -- Phase 0 KDP milestone build. Faults on purpose.
ifdef PANIC_TEST
CXXFLAGS += -DPANIC_TEST
endif

#
# make PHASE4_ALLOW_WRITES=1 -- the ONLY way to build a kext that can write to
# the GPU. Without it NV3050Probe::regWrite32() refuses every store and the
# Phase 4 state machine stops at ERR_WRITES_DISABLED, reporting which write it
# would have issued.
#
# This is a separate flag rather than a runtime boot-arg on purpose: a runtime
# switch means a stock binary is one nvram command away from writing to the
# card, whereas this way the read-only property is a fact about the binary.
#
ifdef PHASE4_ALLOW_WRITES
CXXFLAGS += -DPHASE4_ALLOW_WRITES
endif

.PHONY: all clean install uninstall load unload reload log ioreg check

all: $(BUILD_DIR)/$(KEXT_BUNDLE)

$(BUILD_DIR)/$(KEXT_BUNDLE): $(MACOS_DIR)/$(KEXT_NAME) $(CONTENTS)/Info.plist
	@echo "built $@"
ifdef PANIC_TEST
	@echo "*** PANIC_TEST build -- this kext faults deliberately in start() ***"
endif
ifdef PHASE4_ALLOW_WRITES
	@echo "*** PHASE4_ALLOW_WRITES build -- this kext CAN WRITE to the GPU ***"
	@echo "*** Every other build is read-only by construction.            ***"
endif

$(MACOS_DIR)/$(KEXT_NAME): $(OBJECTS)
	@mkdir -p $(MACOS_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS)

$(CONTENTS)/Info.plist: Info.plist
	@mkdir -p $(CONTENTS)
	cp Info.plist $@
	@plutil -lint $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	rm -rf $(BUILD_DIR)

#
# Kexts in /Library/Extensions must be owned root:wheel with no group or
# other write bits, or kmutil refuses to load them regardless of SIP state.
#
install: all
	sudo rm -rf $(INSTALL_DIR)/$(KEXT_BUNDLE)
	sudo cp -R $(BUILD_DIR)/$(KEXT_BUNDLE) $(INSTALL_DIR)/
	sudo chown -R root:wheel $(INSTALL_DIR)/$(KEXT_BUNDLE)
	sudo chmod -R 755 $(INSTALL_DIR)/$(KEXT_BUNDLE)
	@echo "installed to $(INSTALL_DIR)/$(KEXT_BUNDLE)"

uninstall: unload
	sudo rm -rf $(INSTALL_DIR)/$(KEXT_BUNDLE)

load:
	sudo kmutil load -p $(INSTALL_DIR)/$(KEXT_BUNDLE)

unload:
	-sudo kmutil unload -b $(BUNDLE_ID)

reload: unload install load

log:
	log stream --predicate 'eventMessage CONTAINS "NV3050Probe"' --level debug

ioreg:
	ioreg -p IOService -w0 -l -c $(KEXT_NAME)

#
# Preflight. Everything here is a precondition that produces a confusing
# failure later if it is not true.
#
check:
	@echo "macOS build : $$(sw_vers -buildVersion)"
	@echo "SDK         : $(SDK)"
	@echo "clang++     : $(CXX)"
	@echo -n "SIP         : "; csrutil status 2>/dev/null || echo "csrutil unavailable"
	@echo -n "boot-args   : "; nvram boot-args 2>/dev/null || echo "(none set)"
	@echo -n "KDK         : "; ls -d /Library/Developer/KDKs/* 2>/dev/null || echo "none installed"
	@echo -n "NVIDIA PCI  : "; ioreg -p IOService -n pci10de,* -w0 2>/dev/null | head -1 || echo "none found"

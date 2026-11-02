//
//  OSBoolean.h -- name-only stand-in for libkern's <libkern/c++/OSBoolean.h>.
//
//  NOT APPLE'S HEADER. See ../../IOKit/IOTypes.h for the full caveat.
//
//  Added for t-fccff1. phase0-kext sources include this header by name, and
//  until it existed each of them stopped at a fatal file-not-found before
//  clang read any driver code.
//
//  The shim's OSBoolean class, kOSBooleanTrue and kOSBooleanFalse
//  already live in OSObject.h and stay there.
//  This file only makes the include resolve and declares nothing, so there is
//  still exactly one copy to keep in line with Apple's.
//
//  What it does NOT restore: Apple's include graph. Apple's OSObject.h does
//  not declare OSBoolean, so a source that uses it after including only
//  OSObject.h still parses here and fails under Apple clang. That gap is older
//  than this file, and this file does not widen it.
//

#ifndef NVSHIM_LIBKERN_OSBOOLEAN_H
#define NVSHIM_LIBKERN_OSBOOLEAN_H

#include <libkern/c++/OSObject.h>

#endif // NVSHIM_LIBKERN_OSBOOLEAN_H

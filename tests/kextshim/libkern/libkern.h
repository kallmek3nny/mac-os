//
//  libkern.h -- minimal stand-in for libkern/libkern.h.
//
//  NOT APPLE'S HEADER. See ../IOKit/IOTypes.h for the full caveat.
//
//  WHY THIS FILE EXISTS
//  --------------------
//  It was added on 2026-08-17 because IntelDisplayProbe.cpp composed IORegistry
//  property keys with snprintf -- "IGTranscoderA-Link" rather than
//  "IGTranscoder0-Link", because an index reads ambiguously between "the third
//  transcoder" and "transcoder C" depending on where the reader starts counting.
//
//  snprintf is a kernel export, but it is NOT declared by IOKit/IOLib.h. Apple
//  declares it in libkern/libkern.h, and a kext that calls it without including
//  that header is relying on nothing. Against the old shim the call was
//  `use of undeclared identifier 'snprintf'` -- which was the front end being
//  RIGHT: the shell was missing an include, and clang caught the omission this
//  shim exists to catch.
//
//  NO KEXT SOURCE CALLS snprintf ANY MORE (2026-08-17, later the same day)
//  ----------------------------------------------------------------------
//  The composition moved to igComposeKey() in phase0-kext/IntelDisplayNames.hpp,
//  and IntelDisplayProbe.hpp no longer includes this header. So nothing in
//  phase0-kext/ reaches this file, and clang never parses the declaration below
//  during any gate. Grep before assuming otherwise; the comment above is why the
//  file was written, not a description of a live call site.
//
//  The reason for the move is truncation, not availability. snprintf reports a
//  short buffer only in a return value nobody was reading, so a buffer one byte
//  tight publishes "IGTranscoderDSI0-Inher" -- a key that looks deliberate,
//  sorts next to the real one, and is wrong. igComposeKey() refuses and writes an
//  EMPTY string instead, so a caller that ignores the result publishes something
//  visibly broken rather than something plausible. It is also a pure function of
//  three const char*, which means tests/test_intel_display_names.cpp can drive it
//  on Windows -- and a kernel-side snprintf can never be driven anywhere but the
//  operator's laptop.
//
//  WHY THE FILE STAYS ANYWAY
//  -------------------------
//  Apple ships libkern/libkern.h. Deleting it here would make this shim FLATTER
//  than Apple's include graph again, which is the exact hazard handoff19 SS2 was
//  written about: a flat shim silently accepts a .cpp that OMITS an include Apple
//  requires, and the omission then surfaces on the operator's one build attempt.
//  The next kext file that wants snprintf must include this header to get it, and
//  that is only true while the header exists and declares nothing else.
//
//  The format attribute is the point of declaring it here rather than letting a
//  host <stdio.h> leak in. Any future key built with a %s must be paired with a
//  const char*, and a %u paired with one would be a kernel-side format-string bug
//  on a buffer the caller sized by hand.
//

#ifndef NVSHIM_LIBKERN_LIBKERN_H
#define NVSHIM_LIBKERN_LIBKERN_H

#include <IOKit/IOTypes.h>

//
// Apple's signature is `int snprintf(char *, size_t, const char *, ...)`.
// size_t is spelled unsigned long long here for the same reason IOLib.h spells
// memcpy's length that way: this shim has no <sys/types.h> and inventing one
// would widen its surface for no gain. sizeof() yields an unsigned integer that
// converts without a diagnostic either way, so the arity and the format
// checking -- the two things a parse can actually establish -- are preserved.
//
extern "C" int snprintf(char *buffer, unsigned long long size,
                        const char *format, ...)
    __attribute__((format(printf, 3, 4)));

#endif // NVSHIM_LIBKERN_LIBKERN_H

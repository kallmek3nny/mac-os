//
//  pexpert.h -- minimal stand-in for <pexpert/pexpert.h>.
//
//  NOT APPLE'S HEADER. See ../IOKit/IOTypes.h for the full caveat.
//
//  Added for t-fccff1. Twelve phase0-kext sources include this header by name
//  for PE_parse_boot_argn, the boot-arg opt-in each staged service checks
//  before it touches hardware. Until this file existed, each of them stopped
//  at a fatal file-not-found before clang read any driver code.
//
//  Transcribed from xnu pexpert/pexpert/pexpert.h (apple-oss-distributions,
//  main), where it sits inside __BEGIN_DECLS / __END_DECLS (C linkage under
//  C++):
//
//      extern boolean_t PE_parse_boot_argn(
//          const char      *arg_string,
//          void            *arg_ptr,
//          int                     max_arg);
//
//  Apple gets boolean_t from <mach/boolean.h>. There is no mach shim for that,
//  and IOTypes.h already declares boolean_t, so this file includes IOTypes.h.
//
//  NARROWER: the rest of pexpert (PE_parse_boot_arg_str, PE_get_default, the
//  platform-expert state) is not declared. PE_parse_boot_argn is the only PE_
//  name phase0-kext uses.
//

#ifndef NVSHIM_PEXPERT_PEXPERT_H
#define NVSHIM_PEXPERT_PEXPERT_H

#include <IOKit/IOTypes.h>

extern "C" boolean_t PE_parse_boot_argn(const char *arg_string,
                                        void *arg_ptr,
                                        int max_arg);

#endif // NVSHIM_PEXPERT_PEXPERT_H

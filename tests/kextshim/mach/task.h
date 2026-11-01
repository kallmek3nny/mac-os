//
//  task.h -- name-only stand-in for <mach/task.h>.
//
//  NOT APPLE'S HEADER. See ../IOKit/IOTypes.h for the full caveat.
//
//  Added for t-fccff1. Ten phase0-kext sources include this header by name:
//  IntelGtBringup, IntelGtBringupSurface, IntelGtSurfaceStageService,
//  IntelSinkCandidateService, NVGspBootArgsRehearsalService,
//  NVGspQueueBackingService, NVGspRmImageStageService,
//  NVGspWprBootUcodeStageService, NVGspWprFactsObserver and
//  NVGspWprMetaStageService. Until this file existed, each of them stopped at a
//  fatal file-not-found before clang read any driver code.
//
//  Those ten sources use four identifiers a mach header could supply:
//  kernel_task (11 uses), task_t, mach_vm_size_t and mach_vm_address_t. The
//  shim already declares all four in IOKit/IOTypes.h, and they stay there.
//  This file declares nothing, so each one still has exactly one shim copy.
//
//  A scratch probe with an empty mach/task.h moved all ten sources past the
//  include. The errors behind it name one more mach type, vm_offset_t. That
//  belongs in IOTypes.h next to vm_size_t, where the shim keeps its other mach
//  types, not here.
//
//  On macOS this header is MIG output generated from xnu's
//  osfmk/mach/task.defs, and it declares the task_* routines. It was not
//  fetched for this file. None of those routines are declared here, because
//  phase0-kext calls none of them.
//
//  What it does NOT restore: Apple's include graph, in either direction.
//    - Narrower: this file includes nothing, so a source that names task_t
//      after including only <mach/task.h> fails here. Whether that source
//      passes on macOS depends on what the MIG header includes, which was not
//      checked.
//    - Flatter, and older than this file: IOTypes.h declares kernel_task.
//      xnu declares it in osfmk/kern/task.h ("extern task_t kernel_task;",
//      inside __BEGIN_DECLS and guarded only by _KERN_TASK_H_), not in an
//      IOKit header.
//

#ifndef NVSHIM_MACH_TASK_H
#define NVSHIM_MACH_TASK_H

#endif // NVSHIM_MACH_TASK_H

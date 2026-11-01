//
//  IOLocks.h -- minimal stand-in for IOKit/IOLocks.h.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//
//  Added for t-fccff1. IntelGtBringup.hpp includes this header by name, so
//  every source that reaches it (IntelGtBringup, IntelGtBringupSurface,
//  IntelGtFill and IntelGtPersistentSurfaces among them) stopped at a fatal
//  file-not-found before clang read any driver code.
//
//  Transcribed from xnu iokit/IOKit/IOLocks.h (apple-oss-distributions, main),
//  in the branch without XNU_KERNEL_PRIVATE, which is the one a kext compiles:
//
//      typedef struct _IOLock  IOLock;
//      IOLock * IOLockAlloc( void );
//      void    IOLockFree( IOLock * lock);
//      void    IOLockLock( IOLock * lock);
//      void    IOLockUnlock( IOLock * lock);
//
//  Apple wraps these in extern "C". Inside xnu, IOLockLock and IOLockUnlock
//  are macros over lck_mtx_lock and lck_mtx_unlock. A kext never sees that
//  branch, so the shim declares them as functions, the way a kext's compiler
//  sees them.
//
//  IOLock stays an incomplete struct. Code that dereferences it or takes
//  sizeof(IOLock) fails here, as it does under Apple clang.
//
//  NARROWER: IOLockTryLock, IOLockSleep/IOLockWakeup, IORecursiveLock* and
//  IOSimpleLock* are not declared. The four functions above are the only
//  IOLock* names phase0-kext uses.
//

#ifndef NVSHIM_IOKIT_IOLOCKS_H
#define NVSHIM_IOKIT_IOLOCKS_H

#include <IOKit/IOTypes.h>

typedef struct _IOLock IOLock;

extern "C" IOLock *IOLockAlloc(void);
extern "C" void    IOLockFree(IOLock *lock);
extern "C" void    IOLockLock(IOLock *lock);
extern "C" void    IOLockUnlock(IOLock *lock);

#endif // NVSHIM_IOKIT_IOLOCKS_H

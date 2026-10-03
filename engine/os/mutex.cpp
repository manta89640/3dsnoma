// The SDK's mutexes (OSMutex.c), native. Taking a free mutex, or one the thread already holds,
// is a few loads and stores with interrupts off; waiting for one another thread holds still goes
// through the scheduler (the decompiled __OSPromoteThread and OSSleepThread).
#include "noma_decomp.h"
#include "runtime.h"

using guest::load32;
using guest::store32;

namespace g {

namespace {

// OSMutex: the threads waiting for it (a thread queue, first), its owner and how many times the
// owner took it, and its link in the owner's list of mutexes it holds
enum : u32 { MUTEX_THREAD = 0x08, MUTEX_COUNT = 0x0C, MUTEX_NEXT = 0x10, MUTEX_PREV = 0x14 };
// OSThread
enum : u32 {
    THREAD_PRIORITY = 0x2D0,     // effective priority (raised while it holds a mutex others want)
    THREAD_BASE = 0x2D4,         // its own priority
    THREAD_MUTEX = 0x2F0,        // the mutex it waits for
    THREAD_MUTEX_HEAD = 0x2F4,   // the mutexes it holds
    THREAD_MUTEX_TAIL = 0x2F8,
};
const u32 OS_CURRENT_THREAD = 0x800000E4;

// a free mutex goes to thread: at the end of the thread's list
void take_free(u32 mutex, u32 thread) {
    store32(mutex + MUTEX_THREAD, thread);
    store32(mutex + MUTEX_COUNT, load32(mutex + MUTEX_COUNT) + 1);
    u32 tail = load32(thread + THREAD_MUTEX_TAIL);
    if (tail)
        store32(tail + MUTEX_NEXT, mutex);
    else
        store32(thread + THREAD_MUTEX_HEAD, mutex);
    store32(mutex + MUTEX_PREV, tail);
    store32(mutex + MUTEX_NEXT, 0);
    store32(thread + THREAD_MUTEX_TAIL, mutex);
}

}  // namespace

// the return values are what the SDK's versions leave in r3/r4 (OSRestoreInterrupts')
u64 OSLockMutex(u32 mutex) {
    u32 enabled = NOMA_HI32(OSDisableInterrupts());
    u32 thread = load32(OS_CURRENT_THREAD);
    for (;;) {
        u32 owner = load32(mutex + MUTEX_THREAD);
        if (owner == 0) {
            take_free(mutex, thread);
            break;
        }
        if (owner == thread) {
            store32(mutex + MUTEX_COUNT, load32(mutex + MUTEX_COUNT) + 1);
            break;
        }
        // the owner runs at least at this thread's priority until it lets go
        store32(thread + THREAD_MUTEX, mutex);
        __OSPromoteThread(owner, load32(thread + THREAD_PRIORITY));
        OSSleepThread(mutex);
        store32(thread + THREAD_MUTEX, 0);
    }
    return OSRestoreInterrupts(enabled);
}

u64 OSUnlockMutex(u32 mutex) {
    u32 enabled = NOMA_HI32(OSDisableInterrupts());
    u32 thread = load32(OS_CURRENT_THREAD);
    if (load32(mutex + MUTEX_THREAD) == thread) {
        u32 count = load32(mutex + MUTEX_COUNT) - 1;
        store32(mutex + MUTEX_COUNT, count);
        if (count == 0) {
            u32 next = load32(mutex + MUTEX_NEXT), prev = load32(mutex + MUTEX_PREV);
            if (next)
                store32(next + MUTEX_PREV, prev);
            else
                store32(thread + THREAD_MUTEX_TAIL, prev);
            if (prev)
                store32(prev + MUTEX_NEXT, next);
            else
                store32(thread + THREAD_MUTEX_HEAD, next);
            store32(mutex + MUTEX_THREAD, 0);
            // back down from a priority it was lent for this mutex
            if ((s32)load32(thread + THREAD_PRIORITY) < (s32)load32(thread + THREAD_BASE))
                store32(thread + THREAD_PRIORITY, __OSGetEffectivePriority(thread));
            OSWakeupThread(mutex);
        }
    }
    return OSRestoreInterrupts(enabled);
}

u32 OSTryLockMutex(u32 mutex) {
    u32 enabled = NOMA_HI32(OSDisableInterrupts());
    u32 thread = load32(OS_CURRENT_THREAD);
    u32 owner = load32(mutex + MUTEX_THREAD);
    u32 locked = 1;
    if (owner == 0)
        take_free(mutex, thread);
    else if (owner == thread)
        store32(mutex + MUTEX_COUNT, load32(mutex + MUTEX_COUNT) + 1);
    else
        locked = 0;
    OSRestoreInterrupts(enabled);
    return locked;
}

}  // namespace g

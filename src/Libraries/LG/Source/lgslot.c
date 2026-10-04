// Thread slots: see lgslot.h.

#include "lgslot.h"

#if defined(VITA) || defined(LG_SLOT_PTHREADS)

#include <stdatomic.h>

#ifdef VITA
#include <psp2/kernel/threadmgr.h>
typedef SceUID slot_thread;
#define slot_self() sceKernelGetThreadId()
#define slot_same(a, b) ((a) == (b))
#else
#include <pthread.h>
typedef pthread_t slot_thread;
#define slot_self() pthread_self()
#define slot_same(a, b) pthread_equal(a, b)
#endif

static slot_thread threads[LG_MAX_SLOTS];
// Slots 1..registered-1 are taken. Workers register in slot order.
static atomic_int registered;

void lg_slot_register(int slot) {
    threads[slot] = slot_self();
    atomic_store(&registered, slot + 1);
}

int lg_slot(void) {
    int n = atomic_load_explicit(&registered, memory_order_relaxed);
    int i;

    if (n == 0)
        return 0;
    {
        slot_thread self = slot_self();
        for (i = 1; i < n; i++)
            if (slot_same(threads[i], self))
                return i;
    }
    return 0;
}

#endif

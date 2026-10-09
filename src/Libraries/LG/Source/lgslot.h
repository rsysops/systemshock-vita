#ifndef __LGSLOT_H
#define __LGSLOT_H

// Which rasterizer thread is running. Slot 0 is the main thread, and any
// thread that never registered; the others are the workers that fill pixels
// in parallel (see docs/PERFORMANCE-CPU.md, "Multicore rasterizer"). Per-thread
// state, such as temporary memory, is kept in one entry per slot.

#define LG_MAX_SLOTS 3

#if defined(__vita__) || defined(LG_SLOT_PTHREADS)

int lg_slot(void);
// A worker registers itself, once, before the first lookup that must find it.
void lg_slot_register(int slot);

#else

// No worker threads on this platform.
#define lg_slot() 0

#endif

#endif // __LGSLOT_H

#ifndef __RASTQTHR_H
#define __RASTQTHR_H

// The rasterizer queue's worker threads: runs one job on every thread slot
// at once. See docs/PERFORMANCE.md, "Threads".

#include "lgslot.h"

#define RASTQ_MAX_THREADS LG_MAX_SLOTS

typedef void (*rastq_job)(int slot);

// Makes sure `threads` threads are available, the caller included, creating
// workers as needed. Returns how many there are; 1 where there are no worker
// threads. Workers are never destroyed: without work they go to sleep.
int rastq_threads_start(int threads);

// Runs job(0) on the caller and job(slot) on each of the first threads - 1
// workers, and returns once all have finished.
void rastq_threads_run(rastq_job job, int threads);

// Keeps the calling (main) thread on its own core while it shares work with
// the workers, or lets it roam again.
void rastq_threads_pin_main(int pin);

long long rastq_clock_us(void);
// The core the calling thread is running on, for the profiler
int rastq_thread_cpu(void);

#endif // __RASTQTHR_H

#ifndef __RASTQTHR_H
#define __RASTQTHR_H

// The rasterizer queue's worker threads: runs one job on every thread slot
// at once. See docs/PERFORMANCE-CPU.md, "Threads".

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

// Starts job(slot) on each of the first threads - 1 workers and returns at
// once, without running job(0): the caller does its share itself and decides
// when the work is done. A worker held up elsewhere comes to the job late,
// perhaps after the caller has moved on, so the job must find nothing left
// to do then and touch nothing. Returns 0, having started nothing, where
// there are no workers.
//
// rastq_threads_settle waits until every worker is through with such a job:
// call it before changing anything the job looks at. Starting the next job,
// of either kind, does it too.
int rastq_threads_kick(rastq_job job, int threads);
void rastq_threads_settle(void);
// For a caller waiting on its workers in a loop
void rastq_threads_relax(void);

// Keeps the calling (main) thread on its own core while it shares work with
// the workers, or lets it roam again.
void rastq_threads_pin_main(int pin);

long long rastq_clock_us(void);
// The core the calling thread is running on, for the profiler
int rastq_thread_cpu(void);

#endif // __RASTQTHR_H

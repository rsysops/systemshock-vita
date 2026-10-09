// The rasterizer queue's worker threads: see rastqthr.h.

#include "rastqthr.h"

#if defined(__vita__) || defined(LG_SLOT_PTHREADS)

#include <stdatomic.h>
#include <stddef.h>

#include "lg.h"
#include "memall.h"
#include "tmpalloc.h"

#ifdef __vita__

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

typedef SceUID os_sem;
#define os_sem_wait(s) sceKernelWaitSema(s, 1, NULL)
#define os_sem_signal(s) sceKernelSignalSema(s, 1)
#define os_pause() sceKernelDelayThread(100)
#define cpu_relax() __asm__ volatile("yield")

long long rastq_clock_us(void) { return sceKernelGetProcessTimeWide(); }
int rastq_thread_cpu(void) { return sceKernelGetCpuId(); }

#else

#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <time.h>

typedef sem_t os_sem;
#define os_sem_wait(s) sem_wait(&(s))
#define os_sem_signal(s) sem_post(&(s))
#define os_pause() sched_yield()
#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __builtin_ia32_pause()
#else
#define cpu_relax() ((void)0)
#endif

long long rastq_clock_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
int rastq_thread_cpu(void) { return -1; }

#endif

#define MAX_WORKERS (RASTQ_MAX_THREADS - 1)
// Each worker's temporary memory, as big as the main thread's default
#define WORKER_TEMP_BYTES 16384
#define WORKER_STACK_BYTES (128 * 1024)
// A worker without work keeps watching for some, and only sleeps after this
// long: waking a sleeping worker takes far longer than a job does.
#define IDLE_BEFORE_SLEEP_US 100000
// After a job the caller didn't wait for: a little over a frame, so that the
// workers stay awake only while such jobs keep coming.
#define IDLE_AFTER_KICK_US 25000
#define SPINS_PER_CLOCK_READ 2048

typedef struct {
    int slot;
    atomic_int ready;
    atomic_int asleep;
    os_sem wake;
    MemStack temp;
    uchar temp_mem[WORKER_TEMP_BYTES];
} worker_t;

static worker_t workers[MAX_WORKERS];
static int worker_count;

// The main thread publishes a job by bumping `generation`. Every worker
// answers by bumping `done`, after running the job if its slot takes part,
// and the main thread publishes nothing more until all have answered: so the
// job and the thread count never change under a worker.
static atomic_uint generation;
static atomic_uint done;
static rastq_job current_job;
static int current_threads;
static atomic_int kicked; // the job under way wasn't waited for
static int kick_pending;  // its workers haven't all answered yet

static void worker_loop(worker_t *w) {
    unsigned seen = atomic_load(&generation);
    unsigned spins = 0;
    long long idle_since = 0, idle_limit = IDLE_BEFORE_SLEEP_US;

    lg_slot_register(w->slot);
    w->temp.baseptr = w->temp_mem;
    w->temp.sz = WORKER_TEMP_BYTES;
    MemStackInit(&w->temp);
    temp_mem_init(&w->temp);
    atomic_store(&w->ready, 1);

    for (;;) {
        unsigned g = atomic_load(&generation);

        if (g != seen) {
            seen = g;
            if (w->slot < current_threads)
                current_job(w->slot);
            idle_limit = atomic_load(&kicked) ? IDLE_AFTER_KICK_US : IDLE_BEFORE_SLEEP_US;
            atomic_fetch_add(&done, 1);
            spins = 0;
            idle_since = 0;
            continue;
        }
        cpu_relax();
        if (++spins < SPINS_PER_CLOCK_READ)
            continue;
        spins = 0;
        {
            long long now = rastq_clock_us();
            if (idle_since == 0) {
                idle_since = now;
            } else if (now - idle_since > idle_limit) {
                // Say so, then look once more: the main thread bumps the
                // generation first and only then looks for sleepers, so one
                // of the two always notices the other.
                atomic_store(&w->asleep, 1);
                if (atomic_load(&generation) == seen)
                    os_sem_wait(w->wake);
                atomic_store(&w->asleep, 0);
                idle_since = 0;
            }
        }
    }
}

#ifdef __vita__

static int main_priority(void) {
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
        return 160;
    return info.currentPriority;
}

static int worker_entry(SceSize args, void *argp) {
    (void)args;
    worker_loop(*(worker_t **)argp);
    return 0;
}

static int worker_create(worker_t *w) {
    // one core per worker, and a priority one step below the main thread's so
    // that the audio threads can take a core from a worker that only spins
    static const int masks[MAX_WORKERS] = {SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_2};
    SceUID id;

    w->wake = sceKernelCreateSema("rastq_wake", 0, 0, 1, NULL);
    if (w->wake < 0)
        return 0;
    id = sceKernelCreateThread("rastq_worker", worker_entry, main_priority() + 1, WORKER_STACK_BYTES, 0,
                               masks[w->slot - 1], NULL);
    if (id < 0)
        return 0;
    return sceKernelStartThread(id, sizeof(w), &w) >= 0;
}

void rastq_threads_pin_main(int pin) {
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
                                         pin ? SCE_KERNEL_CPU_MASK_USER_0 : SCE_KERNEL_CPU_MASK_USER_ALL);
}

#else

static void *worker_entry(void *arg) {
    worker_loop(arg);
    return NULL;
}

static int worker_create(worker_t *w) {
    pthread_t thread;
    pthread_attr_t attr;
    int ok;

    if (sem_init(&w->wake, 0, 0) != 0)
        return 0;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_STACK_BYTES);
    ok = pthread_create(&thread, &attr, worker_entry, w) == 0;
    pthread_attr_destroy(&attr);
    if (ok)
        pthread_detach(thread);
    return ok;
}

void rastq_threads_pin_main(int pin) { (void)pin; }

#endif

int rastq_threads_start(int threads) {
    if (threads > RASTQ_MAX_THREADS)
        threads = RASTQ_MAX_THREADS;
    while (worker_count < threads - 1) {
        worker_t *w = &workers[worker_count];

        w->slot = worker_count + 1;
        if (!worker_create(w))
            break;
        // Slots are handed out in order, so one worker at a time.
        while (!atomic_load(&w->ready))
            os_pause();
        worker_count++;
    }
    return worker_count + 1;
}

void rastq_threads_settle(void) {
    if (!kick_pending)
        return;
    while (atomic_load(&done) != (unsigned)worker_count)
        cpu_relax();
    kick_pending = 0;
}

// Hands the job to the workers: the generation goes up first and the
// sleepers are woken after, so one of the two always notices the other.
static void start(rastq_job job, int threads, int kick) {
    int i;

    rastq_threads_settle();
    current_job = job;
    current_threads = threads;
    atomic_store(&kicked, kick);
    atomic_store(&done, 0);
    atomic_fetch_add(&generation, 1);
    for (i = 0; i < worker_count; i++)
        if (atomic_load(&workers[i].asleep))
            os_sem_signal(workers[i].wake);
}

void rastq_threads_run(rastq_job job, int threads) {
    if (threads > worker_count + 1)
        threads = worker_count + 1;
    if (threads <= 1) {
        job(0);
        return;
    }
    start(job, threads, 0);
    job(0);
    while (atomic_load(&done) != (unsigned)worker_count)
        cpu_relax();
}

int rastq_threads_kick(rastq_job job, int threads) {
    if (threads > worker_count + 1)
        threads = worker_count + 1;
    if (threads <= 1)
        return 0;
    start(job, threads, 1);
    kick_pending = 1;
    return 1;
}

void rastq_threads_relax(void) { cpu_relax(); }

#else

// No worker threads on this platform.
int rastq_threads_start(int threads) {
    (void)threads;
    return 1;
}
void rastq_threads_run(rastq_job job, int threads) {
    (void)threads;
    job(0);
}
void rastq_threads_settle(void) {}
int rastq_threads_kick(rastq_job job, int threads) {
    (void)job;
    (void)threads;
    return 0;
}
void rastq_threads_relax(void) {}
void rastq_threads_pin_main(int pin) { (void)pin; }
long long rastq_clock_us(void) { return 0; }
int rastq_thread_cpu(void) { return -1; }

#endif

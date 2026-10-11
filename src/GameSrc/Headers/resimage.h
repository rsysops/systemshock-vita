#ifndef __RESIMAGE_H
#define __RESIMAGE_H

// The art files held whole in memory, so that art first needed in the
// middle of the game is copied from there and not read from the card, which
// takes 5 ms and more for each piece. See docs/PERFORMANCE-GPU.md, step S6.

// Starts reading the files, on a thread of its own. Until a file is read
// the resource system takes it from the card as before.
void resimage_start(void);

// Hands the files read since the last call to the resource system
// (ResUseImage). From the main thread, between two uses of that system.
void resimage_poll(void);

// For the profiler: files read of those wanted, their size, and how long
// the reading has taken so far.
void resimage_stats(int *ready, int *wanted, int *bytes, int *ms);

#endif // __RESIMAGE_H

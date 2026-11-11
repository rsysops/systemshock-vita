#ifndef __FILECOPY_H
#define __FILECOPY_H

// Copies the file `from` to `to`, replacing what `to` held: read whole into
// memory, then written in one go. (The copy this replaces went a byte at a
// time through the C library's small buffers: a read and a write on the
// memory card for every kilobyte. See docs/PERFORMANCE.md, step S8.)
//
// Returns 0 if `to` now holds `from`'s bytes, and their count in *size if
// size isn't NULL. Otherwise -1; `to` is left as it was unless the writing
// itself failed.
int file_copy(const char *from, const char *to, long *size);

// The same copy for a save's slot, where `to` must hold either its old bytes
// or all of the new ones, whenever the game is stopped. Two names beside it:
//   `to`.tmp  being written, so maybe not whole: never used, only removed
//   `to`.new  whole and on the card: always good to put in `to`'s place
// `.tmp` is copied, synced, renamed `.new`, and `.new` renamed `to`. (On the
// Vita, rename removes the destination before renaming, so there is a moment
// with no `to`: `.new` covers it.)
//
// Returns 0 if `to` now holds `from`'s bytes, and their count in *size if
// size isn't NULL. Otherwise -1, and `to` holds its old bytes, unless only
// the last rename failed: then `.new` has the new ones, for
// file_replace_finish to put in place.
int file_replace(const char *from, const char *to, long *size);

// Settles what a stopped file_replace left beside `to`: `.tmp` is removed,
// and `.new`, if there, takes `to`'s place. file_replace starts with it.
void file_replace_finish(const char *to);

#endif // __FILECOPY_H

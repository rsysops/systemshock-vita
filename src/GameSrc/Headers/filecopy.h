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

#endif // __FILECOPY_H

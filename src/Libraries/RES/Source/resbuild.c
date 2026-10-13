/*

Copyright (C) 2015-2018 Night Dive Studios, LLC.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
//    RESBUILD.C    Resource-file building routines
//    Rex E. Bradford (REX)
/*
 * $Header: n:/project/lib/src/res/rcs/resbuild.c 1.10 1994/06/16 11:06:30 rex
 * Exp $
 * $Log: resbuild.c $
 * Revision 1.10  1994/06/16  11:06:30  rex
 * Got rid of RDF_NODROP flag
 *
 * Revision 1.9  1994/02/17  11:25:32  rex
 * Moved some stuff out to resmake.c and resfile.c
 *
 */

#include <assert.h>
#include <string.h>
#include <unistd.h>   // ftruncate

#include "lg.h"
#include "lzw.h"
#include "res.h"
#include "res_.h"
#include "vprof.h"

#include <stdlib.h>

// make sure comment ends with one, so can type a file
#define CTRL_Z 26

bool ResEraseIfInFile(Id id);

//  Internal prototypes
static void ResCopyBytes(FILE *fd, int32_t writePos, int32_t readPos, int32_t size);

//  -------------------------------------------------------
//
//  ResSetComment() sets comment in res header.
//  -------------------------------------------------------
//  For Mac version:  Does nothing.  May go back later and add comment via
// the
// desktop database, maybe.

void ResSetComment(int32_t filenum, char *comment) {

    ResFileHeader *phead;
    if (resFile[filenum].pedit == NULL) {
        WARN("%s: file %d not open for writing", __FUNCTION__, filenum);
        return;
    }
    TRACE("%s: setting comment for filenum %d to: %s", __FUNCTION__, filenum, comment);


    phead = &resFile[filenum].pedit->hdr;
    memset(phead->comment, 0, sizeof(phead->comment));
    strncpy(phead->comment, comment, sizeof(phead->comment) - 2);
    phead->comment[strlen(phead->comment)] = CTRL_Z;
}

//  -------------------------------------------------------
//
//  ResWrite() writes a resource to an open resource file.
//  This routine assumes that the file position is already set to
//  the current data position.
//      Returns the total number of bytes written out, or -1 if there
//      was a writing error.
//
//    id = id to write
//  -------------------------------------------------------
#define EXTRA 250

// A file being written to stands, between two resources, where the next
// one goes. Seeking there all the same makes the C library hand what it
// holds to the disk: one write on the card for each resource of a level
// saved. So the seek is left out when the last thing done to the file was
// the write before, and made after anything else (ResTailLost).
static bool resAtTail[MAX_RESFILENUM + 1];

void ResTailLost(int32_t filenum) { resAtTail[filenum] = false; }

static int32_t ResWriteNow(Id id);

int32_t ResWrite(Id id) {
    int32_t written;

    // (the profiler's sum of the frame's writes)
    VPROF_MARK_BEGIN(VPROF_RESWRITE);
    written = ResWriteNow(id);
    VPROF_MARK_END(VPROF_RESWRITE);
    return written;
}

static int32_t ResWriteNow(Id id) {
    static uint8_t pad[] = {0, 0, 0, 0, 0, 0, 0, 0};
    ResDesc *prd;
    ResDesc2 *prd2;
    ResFile *prf;
    ResDirEntry *pDirEntry;
    uint8_t *p;
    size_t size, sizeTable;
    void *pcompbuff;
    int32_t compsize, padBytes;

    TRACE("%s: writing", __FUNCTION__);
    if (!ResCheckId(id))
        return -1;

    prd = RESDESC(id);
    prf = &resFile[prd->filenum];

    if (prf->pedit == NULL) {
        ERROR("%s: file %i not open for writing!", __FUNCTION__, prd->filenum);
        return -1;
    }
    //});

    // Check if item already in directory, if so erase it
    ResEraseIfInFile(id);

    // If directory full, grow it
    if (prf->pedit->pdir->numEntries == prf->pedit->numAllocDir) {
        TRACE("%s: growing directory of filenum %d", __FUNCTION__, prd->filenum);

        prf->pedit->numAllocDir += DEFAULT_RES_GROWDIRENTRIES;
        prf->pedit->pdir =
            realloc(prf->pedit->pdir, sizeof(ResDirHeader) + (sizeof(ResDirEntry) * prf->pedit->numAllocDir));
    }

    // Set resource's file offset
    prd->offset = RES_OFFSET_REAL2DESC(prf->pedit->currDataOffset);

    // See if it needs encoding.
    assert(prd->format != NULL);
    size = prd->msize;
    if (prd->format->encoder != NULL) {
	p = prd->format->encoder(prd->ptr, &size, prd->format->data);
    } else {
	p = prd->ptr;
    }
    prd->fsize = size;

    // Fill in directory entry
    pDirEntry = ((ResDirEntry *)(prf->pedit->pdir + 1)) + prf->pedit->pdir->numEntries;

    pDirEntry->id = id;
    prd2 = RESDESC2(id);
    pDirEntry->flags = prd2->flags;
    pDirEntry->type = prd2->type;
    pDirEntry->size = size;

    TRACE("%s: writing $%x\n", __FUNCTION__, id);

    // If compound, write out reftable without compression
    // (the profiler's sum of the frame's seeks and writes is resio)
    if (!resAtTail[prd->filenum])
        VPROF_RUN_AS(VPROF_RESIO, NULL, 0, 0, fseek(prf->fd, prf->pedit->currDataOffset, SEEK_SET));
    sizeTable = 0;

    // Body (post compound res header) if compound, else main pointer.
    uint8_t *body = p;
    // FIXME will need rework for compound refs if we reinstate, e.g. if we want
    // to integrate a resource/level editor.
    if (prd2->flags & RDF_COMPOUND) {
        sizeTable = REFTABLESIZE(((RefTable *)p)->numRefs);
        VPROF_RUN_AS(VPROF_RESIO, NULL, 0, 0, fwrite(p, sizeTable, 1, prf->fd));
        body = p + sizeTable;
        size -= sizeTable;
    }

    // If compression, try it (may not work out)
    if (pDirEntry->flags & RDF_LZW) {
        pcompbuff = malloc(size + EXTRA);
        // (the profiler's sum of the frame's compressing)
        VPROF_RUN_AS(VPROF_RESLZW, NULL, 0, 0, compsize = LzwCompressBuff2Buff(p, size, pcompbuff, size));
        if (compsize < 0) {
            pDirEntry->flags &= ~RDF_LZW;
        } else {
            pDirEntry->csize = sizeTable + compsize;
            VPROF_RUN_AS(VPROF_RESIO, NULL, 0, 0, fwrite(pcompbuff, compsize, 1, prf->fd));
        }
        free(pcompbuff);
    }

    // If no compress (or failed to compress well), just write out
    if (!(pDirEntry->flags & RDF_LZW)) {
        pDirEntry->csize = size;
        VPROF_RUN_AS(VPROF_RESIO, NULL, 0, 0, fwrite(body, size, 1, prf->fd));
    }

    // Pad to align on data boundary
    padBytes = RES_OFFSET_PADBYTES(pDirEntry->csize);
    if (padBytes)
        VPROF_RUN_AS(VPROF_RESIO, NULL, 0, 0, fwrite(pad, padBytes, 1, prf->fd));

    // FIXME Error handling
    //  if (ftell(prf->fd) & 3)
    //    Warning(("ResWrite: misaligned writing!\n"));

    // If we encoded it, free the encode buffer.
    if (prd->format->encoder) {
	free(p);
    }
    // Advance dir num entries, current data offset
    prf->pedit->pdir->numEntries++;
    prf->pedit->currDataOffset = RES_OFFSET_ALIGN(prf->pedit->currDataOffset + pDirEntry->csize);
    // (asked of the file, not taken for granted: no seek is spared unless
    // the file is where the next resource goes)
    resAtTail[prd->filenum] = ftell(prf->fd) == (long)prf->pedit->currDataOffset;

    return 0;
}

//  -------------------------------------------------------------
//
//  ResPack() removes holes from a resource file.
//
//    filenum = resource filenum (must already be open for
//                create/edit)
//
//  Returns: # bytes reclaimed

static int32_t ResPackNow(int32_t filenum);

int32_t ResPack(int32_t filenum) {
    int32_t reclaimed;

    VPROF_MARK_BEGIN(VPROF_RESPACK);
    reclaimed = ResPackNow(filenum);
    VPROF_MARK_END(VPROF_RESPACK);
    return reclaimed;
}

static int32_t ResPackNow(int32_t filenum) {
    ResFile *prf;
    ResDirEntry *pDirEntry;
    int32_t numReclaimed, sizeReclaimed;
    int32_t dataRead, dataWrite;
    int32_t i;
    ResDirEntry *peWrite;
    int32_t firstHole = -1, dataEnd;
    uint8_t *image = NULL; // the file from the first empty entry on, where there is one

    // Check for errors
    prf = &resFile[filenum];
    if (prf->pedit == NULL) {
        ERROR("%s: filenum %d not open for editing", __FUNCTION__, filenum);
        return (0);
    }

    ResTailLost(filenum);

    // Everything after the first empty entry moves down. Moved in the file
    // itself, a resource at a time, that is a seek, a read, a seek and a
    // write for each: thousands, for a saved game. So that part of the
    // file is read whole, closed up in memory, and written back whole. The
    // bytes it ends up with are those the moving in the file gives.
    dataEnd = prf->pedit->pdir->dataOffset;
    pDirEntry = (ResDirEntry *)(prf->pedit->pdir + 1);
    for (i = 0; i < prf->pedit->pdir->numEntries; i++) {
        if (pDirEntry->id == 0 && firstHole < 0)
            firstHole = dataEnd;
        dataEnd = RES_OFFSET_ALIGN(dataEnd + pDirEntry->csize);
        pDirEntry++;
    }
    if (firstHole >= 0 && dataEnd > firstHole) {
        image = malloc(dataEnd - firstHole);
        if (image != NULL && (fseek(prf->fd, firstHole, SEEK_SET) != 0 ||
                              fread(image, 1, dataEnd - firstHole, prf->fd) != (size_t)(dataEnd - firstHole))) {
            // (then in the file, as before)
            free(image);
            image = NULL;
        }
    }

    // Set up
    sizeReclaimed = numReclaimed = 0;
    dataRead = dataWrite = prf->pedit->pdir->dataOffset;

    // Scan thru directory, copying over all empty entries
    pDirEntry = (ResDirEntry *)(prf->pedit->pdir + 1);
    for (i = 0; i < prf->pedit->pdir->numEntries; i++) {
        if (pDirEntry->id == 0) {
            numReclaimed++;
            sizeReclaimed += pDirEntry->csize;
        } else {
            if (gResDesc[pDirEntry->id].offset > RES_OFFSET_PENDING)
                gResDesc[pDirEntry->id].offset = RES_OFFSET_REAL2DESC(dataWrite);
            if (dataRead != dataWrite) {
                if (image != NULL)
                    memmove(image + (dataWrite - firstHole), image + (dataRead - firstHole), pDirEntry->csize);
                else
                    ResCopyBytes(prf->fd, dataWrite, dataRead, pDirEntry->csize);
            }
            dataWrite = RES_OFFSET_ALIGN(dataWrite + pDirEntry->csize);
        }
        dataRead = RES_OFFSET_ALIGN(dataRead + pDirEntry->csize);
        pDirEntry++;
    }
    if (image != NULL) {
        // what is past the new end is cut off below
        if (dataWrite > firstHole) {
            fseek(prf->fd, firstHole, SEEK_SET);
            fwrite(image, 1, dataWrite - firstHole, prf->fd);
        }
        free(image);
    }

    // Now pack directory itself
    pDirEntry = (ResDirEntry *)(prf->pedit->pdir + 1);
    peWrite = pDirEntry;
    for (i = 0; i < prf->pedit->pdir->numEntries; i++) {
        if (pDirEntry->id) {
            if (pDirEntry != peWrite)
                *peWrite = *pDirEntry;
            peWrite++;
        }
        pDirEntry++;
    }
    prf->pedit->pdir->numEntries -= numReclaimed;

    // Set new current data offset
    prf->pedit->currDataOffset = dataWrite;
    fseek(prf->fd, dataWrite, SEEK_SET);
    prf->pedit->flags &= ~RFF_NEEDSPACK;

    // Truncate file to just header & data (will be extended later when
    // write directory on closing)

    // FIXME Non-portable
    ftruncate(fileno(prf->fd), dataWrite);

    // Return # bytes reclaimed
    TRACE("%s: reclaimed %d bytes", __FUNCTION__, sizeReclaimed);

    return (sizeReclaimed);
}

#define SIZE_RESCOPY 32768

static void ResCopyBytes(FILE *fd, int32_t writePos, int32_t readPos, int32_t size) {
    int32_t sizeCopy;
    uint8_t *buff;

    buff = malloc(SIZE_RESCOPY);

    while (size > 0) {
        sizeCopy = lg_min(SIZE_RESCOPY, size);
        fseek(fd, readPos, SEEK_SET);
        fread(buff, sizeCopy, 1, fd);
        fseek(fd, writePos, SEEK_SET);
        fwrite(buff, sizeCopy, 1, fd);
        readPos += sizeCopy;
        writePos += sizeCopy;
        size -= sizeCopy;
    }

    free(buff);
}

//  --------------------------------------------------------
//    INTERNAL ROUTINES
//  --------------------------------------------------------
//
//  ResEraseIfInFile() erases a resource if it's in a file's directory.
//
//    id = id of item
//
//  Returns: TRUE if found & erased, FALSE otherwise

bool ResEraseIfInFile(Id id) {
    ResDesc *prd;
    ResFile *prf;
    ResDirEntry *pDirEntry;
    int32_t i;

    prd = RESDESC(id);
    prf = &resFile[prd->filenum];
    pDirEntry = (ResDirEntry *)(prf->pedit->pdir + 1);

    for (i = 0; i < prf->pedit->pdir->numEntries; i++) {
        if (id == pDirEntry->id) {
            TRACE("%s: $%x being erased\n", __FUNCTION__, id);
            pDirEntry->id = 0;
            prf->pedit->flags |= RFF_NEEDSPACK;
            if (prf->pedit->flags & RFF_AUTOPACK)
                ResPack(prd->filenum);
            return true;
        }
        pDirEntry++;
    }

    return false;
}

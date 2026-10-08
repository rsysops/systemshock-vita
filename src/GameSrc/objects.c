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
/*
** $Header: r:/prj/cit/src/RCS/objects.c 1.13 1994/08/31 16:27:36 tjs Exp $
*
*/

////////////////////////////////////////////////////////////
//
// READ ME FIRST!
//
//////////////////////////////
//
// So then, let us determine some determinology for our terms, here
//
// There are really three places a thing can be:
//   WORLD: in the world and being used
//   FLOATING: existent but not connected to the real world in any way
//   UNUSED: not used at all, ready to be reclaimed
//
// Any function modifying the world in some way will probably contain
// one of the following words:
//
// Grab: get an unused thing                  UNUSED -> FLOATING
// Free: return a thing to the unused pile  FLOATING -> UNUSED
//
//  Add: add to reality                     FLOATING -> WORLD
//  Rem: remove from reality                   WORLD -> FLOATING
//
// Make: spring fully formed into the world   UNUSED -> WORLD
//  Del: terminate with extreme prejudice      WORLD -> UNUSED
//
// For things for which there is no real FLOATING state, Del and Make are used.
// Basically, whenever you have Rem'ed something, assume that it is
//   still floating and that you must do something more to it.
//
// Object system functions tend to have names beginning with "Obj."
// If one main argument is being passed in, and it would otherwise
//   be unclear what type of argument it is, that type is then put in the name.
// Then follow any additional nouns and verbs describing what the function does.
// All Objs, ObjRefs, ObjSpecs, etc. are referred to by their ID's.
// Thus, ObjRefLinkDel deletes a link from an ObjRef to an Obj and is passed an ObjRefID.
//
//////////////////////////////

#include <string.h>

#include "objects.h"
#include "map.h"

/*
#define DBG_Check(x) DBG(DSRC_OBJECTS_Check,x)
#define SpewCheck(x) Spew(DSRC_OBJECTS_Check,x)
#define DBG_Anal(x) DBG(DSRC_OBJECTS_Anal,x)
#define SpewAnal(x) Spew(DSRC_OBJECTS_Anal,x)
#define DBG_Report(x) DBG(DSRC_OBJECTS_Report,x)
#define SpewReport(x) Spew(DSRC_OBJECTS_Report,x)
#define DBG_Hash(x) DBG(DSRC_OBJECTS_Hash,x)
#define SpewHash(x) Spew(DSRC_OBJECTS_Hash,x)
*/

// See objects.h and objapp.h for a description of global variables

ObjLocState objLocStates[MAX_OBJS_CHANGING];
uchar numObjLocStates;

static void ObjRefRem(ObjRefID ref);
static uchar ObjLinkMake(ObjRefID ref, ObjID obj);
static ObjID ObjRefLinkDel(ObjRefID ref);
static uchar ObjRefAdd(ObjRefID ref, ObjRefState refstate);
static uchar ObjDelRefs(ObjID ID);
static ObjID ObjGrab(void);
static uchar ObjFree(ObjID ref);
static ObjRefID ObjRefGrab(void);
static ObjID ObjRefFree(ObjRefID ref, uchar cleanup);
static ObjSpecID ObjSpecGrab(ObjClass obclass);
static uchar ObjSpecFree(ObjClass obclass, ObjSpecID id);
static uchar ObjAndSpecFree(ObjID obj);

////////////////////////////////////////////////////////////
//
// PUBLIC FUNCTIONS
//
////////////////////////////////////////////////////////////

//////////////////////////////
//
// Initializes all object stuff
//
// This entire function should be made faster, as it is currently quite stupid.
//
void ObjsInit(void) {
    int i;
    short c;
    ObjSpecHeader *head;
    ObjSpec *os;

    //	SpewReport (("ObjsInit ()\n"));

    // clear out everything
    LG_memset((void *)objs, 0, sizeof(Obj) * NUM_OBJECTS);
    LG_memset((void *)objRefs, 0, sizeof(ObjRef) * NUM_REF_OBJECTS);
    for (c = CLASS_FIRST; c < NUM_CLASSES; c++) {
        head = &objSpecHeaders[c];
        LG_memset((void *)head->data, 0, head->size * head->struct_size);
    }

    // set up the free chains for objects
    for (i = 0; i < NUM_OBJECTS - 1; i++)
        objs[i].next = i + 1;
    objs[NUM_OBJECTS - 1].next = 0;
    objs[0].headused = 0;

    // set up the free chains for references
    for (i = 0; i < NUM_REF_OBJECTS - 1; i++)
        objRefs[i].next = i + 1;
    objRefs[NUM_REF_OBJECTS - 1].next = 0;

    // set up the free chains for class-specific info
    for (c = CLASS_FIRST; c < NUM_CLASSES; c++) {
        head = &objSpecHeaders[c];
        ((ObjSpec *)(head->data))->bits.id = 0; // really head of used chain
        for (i = 0; i < head->size - 1; i++) {
            os = (ObjSpec *)(head->data + i * head->struct_size);
            os->next = i + 1;
        }
        ((ObjSpec *)(head->data + (head->size - 1) * head->struct_size))->next = 0;
    }

    // temp
    //	if (!ObjSysOkay())
    //		DebugString("Obj Sys bad after ObjsInit");
}

//////////////////////////////
//
// Finds a free Obj and a free ObjSpec of the appropriate class, and
// links them together.  Fills up the given fields.  Returns whether it
// succeeded.
//
uchar ObjAndSpecGrab(ObjClass obclass, ObjID *id, ObjSpecID *specid) {
    ObjSpecHeader *head;

    //	SpewReport (("ObjAndSpecGrab (class %d)\n", obclass));

    if ((*id = ObjGrab()) == OBJ_NULL)
        return FALSE;
    if ((*specid = ObjSpecGrab(obclass)) == OBJ_SPEC_NULL) {
        ObjFree(*id);
        return FALSE;
    }
    objs[*id].obclass = obclass;
    objs[*id].specID = *specid;
    head = &objSpecHeaders[obclass];
    ((ObjSpec *)(head->data + *specid * head->struct_size))->bits.id = *id;

    // temp
    //	if (!ObjSysOkay())
    //		DebugString("Obj Sys bad after ObjAndSpecGrab");

    return TRUE;
}

//////////////////////////////
//
// Sets the given fields of an object appropriately, and makes it active.
// Currently always returns TRUE.
//
uchar ObjPlace(ObjID id, ObjLoc *loc) {
    // DBG_Report ({
    //	char str[80];
    //	ObjLocSprint (str, *loc);
    //	SpewReport (("ObjPlace (%s)\n", str));
    //})
    ObjLocCopy(*loc, objs[id].loc);

    // temp
    //	if (!ObjSysOkay())
    //		DebugString("Obj Sys bad after ObjPlace");

    return TRUE;
}

//////////////////////////////
//
// Removes the specified ObjRef from the map,
//   destroys its reference to its Obj,
//   and then destroys it.
//
// Quite a vicious little function really.
//
ObjID ObjRefDel(ObjRefID ref) {
    uchar tmp;

    //	SpewReport (("ObjRefDel (ref %d)\n", ref));

    ObjRefRem(ref);
    tmp = ObjRefFree(ref, TRUE);

    // temp
    //	if (!ObjSysOkay())
    //		DebugString("Obj Sys bad after ObjRefDel");

    return tmp;
}

//////////////////////////////
//
// Gets a free ObjRef, assigns it to the given Obj,
// and puts it in the world in the given place.
//
// Returns the ObjRefID used, or OBJ_REF_NULL if it
// couldn't do it for any reason.
//
ObjRefID ObjRefMake(ObjID obj, ObjRefState refstate) {
    ObjRefID ref;
    uchar ok;

    // DBG_Report ({
    //	char str[80];
    //	ObjRefStateSprint (str, refstate);
    //	SpewReport (("ObjRefMake (obj %d %s)\n", obj, str));
    //})
    if ((ref = ObjRefGrab()) == OBJ_REF_NULL)
        return OBJ_REF_NULL;

    ok = ObjLinkMake(ref, obj);

    // DBG_Check({
    //	if (!ok)
    //	{
    //		Warning (("Could not make link from ObjRef %d to Obj %d\n", ref, obj));
    //		ObjRefFree (ref, TRUE);
    //		return OBJ_REF_NULL;
    //	}
    //})

    ok = ObjRefAdd(ref, refstate);

    // DBG_Check({
    //	if (!ok)
    //	{
    //		char str[80];
    //		ObjRefStateSprint (str, refstate);
    //		Warning (("Could not add ObjRef %d to %s\n", ref, str));
    //		ObjRefFree (ref, TRUE);
    //		return OBJ_REF_NULL;
    //	}
    //})

    // temp
    //	if (!ObjSysOkay())
    //		DebugString("Obj Sys bad after ObjRefMake");

    return ref;
}

//////////////////////////////
//
// Deletes the given obj and all its references.
// Returns whether success was achieved in this quest.
//
uchar ObjDel(ObjID obj) {
    uchar ok;

    //	SpewReport (("ObjDel (obj %d)\n", obj));

    ok = ObjDelRefs(obj);

    // DBG_Check ({
    //	if (!ok)
    //	{
    //		Warning (("Could not delete refs to Obj %d\n", obj));
    //		return FALSE;
    //	}
    //})

    ok = ObjAndSpecFree(obj);

    // DBG_Check ({
    //	if (!ok)
    //	{
    //		Warning (("Could not free obj & spec for Obj %d\n", obj));
    //		return FALSE;
    //	}
    //})

    // temp
    //	if (!ObjSysOkay())
    //		DebugString("Obj Sys bad after ObjDel");

    return TRUE;
}

//////////////////////////////
//
// Updates the location of a moving object.
// Returns whether it was successful in doing so.
//
// The given ObjLocState contains information about the new location of the
// object.  We know the old location of the object by following ref pointers
// around.  This function updates all the ObjRefs's referring to that object
// correctly.
//
uchar ObjUpdateLocs(ObjLocState *olsp) {
    ObjID obj;                        // this object
    ObjRefID ref;                     // each reference of it
    ObjRefID firstref;                // first ref that refers to it
    ObjRefState *newrefs;             // squares the object will soon be in
    ObjRefState in[MAX_REFS_PER_OBJ]; // places moved into
    ObjRefID out[MAX_REFS_PER_OBJ];   // old references of the object
    int incount = 0;                  // number of places moved into
    int outcount = 0;                 // number of places moved out of
    int newcount;                     // # of new square being moved into
    ObjRefState *stCur;               // current place being checked
    int i;                            // loopy loopy

    //	SpewReport (("ObjUpdateLocs (obj %d)\n", olsp->obj));
    //   DBG_Report ({
    //		char str[80];
    //      newrefs = olsp->refs;
    //      while (!ObjRefStateBinCheckNull(newrefs->bin))
    //      {
    //			ObjRefStateSprint (str, *newrefs);
    //         SpewReport (("[%s] ", str));
    //         newrefs++;
    //      }
    //      SpewReport (("\n"));
    //   })

    // Get some data first
    obj = olsp->obj;
    newrefs = olsp->refs;
    ObjLocCopy(olsp->loc, objs[obj].loc);

    // Figure out where we are now
    outcount = 0;
    firstref = ref = objs[obj].ref;

    if (firstref != OBJ_REF_NULL) {
        // We use a do-while so that we don't fail the loop condition
        // at the very beginning of the loop.
        do {
            out[outcount++] = ref;
            ref = objRefs[ref].nextref;
        } while (ref != firstref);
    }

    // Now, for each square we will be in,
    //
    //   - if the bin is in out[], then we are staying in it;
    //     update its info field and then delete it from out[]
    //
    //   - if the bin is not in out[], then we are moving into it;
    //     add it to in[]
    //
    // At the end of the following loop, all the bins we are moving out of
    // will be in out[], and all the bins we are moving into will be in in[].

    incount = 0;
    for (newcount = 0; !ObjRefStateBinCheckNull(newrefs[newcount].bin); newcount++) {
        stCur = &newrefs[newcount];

        // Check all bins in out[] for a match
        for (i = 0; i < outcount; i++) {
            if (ObjRefStateBinEqual(objRefs[out[i]].state.bin, stCur->bin)) {
                out[i] = out[--outcount]; // delete this bin from out
                goto found_bin_in_out;    // go back to the outer loop
            }
        }

        // this bin was not in out[], so we add it to in[].
        in[incount++] = *stCur;

    found_bin_in_out:;
    }

    // Now we must delete the references in out[] and add the ones in in[].
    // Note that we might be able to speed up the following code if we had to
    // by, instead of freeing RefID's and then grabbing them again, reusing
    // them somehow.  It is unclear how often both out[] and in[] will have
    // stuff in them; if this case occurs often it would probably be worth
    // speeding up.

    for (i = 0; i < outcount; i++)
        ObjRefDel(out[i]);

    for (i = 0; i < incount; i++)
        if (!ObjRefMake(obj, in[i]))
            return FALSE;

    // temp
    //	if (!ObjSysOkay())
    //		DebugString("Obj Sys bad after ObjUpdateLocs");

    return TRUE;
}

    ////////////////////////////////////////////////////////////
    //
    // DEBUGGING FUNCTIONS
    //
    ////////////////////////////////////////////////////////////

#define OBJ_NO_STATE 0
#define OBJ_FREE 1
#define OBJ_USED 2
#define OBJ_IN_MAP 4

//////////////////////////////
//
// Check whether the object system is consistent.
//
uchar ObjSysOkay(void) {
    char usedObj[NUM_OBJECTS];
    char usedRef[NUM_REF_OBJECTS];
    ObjID cur;
    ObjSpecHeader *head;
    int i, j;
    ObjRefStateBin refbin;
    ObjRefID ref;

    // 1. Every Obj is in either the free chain or the used chain,
    // and does not appear twice in any chain.

    LG_memset(usedObj, 0, NUM_OBJECTS);

    //	SpewAnal (("Free Objs: "));
    //	DBG_Anal ({RangeInit ();})

    cur = objs[OBJ_NULL].next;
    while (cur) {
        //		DBG_Anal ({RangeAdd (cur);})
        if (cur < 0 || cur >= NUM_OBJECTS) {
            //			DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));
            DEBUG("%s: Invalid ID in free chain", __FUNCTION__); // cur
            return FALSE;
        }
        if (objs[cur].active) {
            //			DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));
            DEBUG("%s: Active Obj in free chain", __FUNCTION__); // objs[cur]
            return FALSE;
        }
        if (usedObj[cur] == OBJ_FREE) {
            //			DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));
            DEBUG("%s: Obj is free more than once", __FUNCTION__); // usedObj[cur]
            return FALSE;
        }
        usedObj[cur] = OBJ_FREE;
        cur = objs[cur].next;
    }
    //	DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));

    //	SpewAnal (("Used Objs: "));
    //	DBG_Anal ({RangeInit ();})

    cur = objs[OBJ_NULL].ref;
    while (cur) {
        //		DBG_Anal ({RangeAdd (cur);})
        if (cur < 0 || cur >= NUM_OBJECTS) {
            //			DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));
            DEBUG("%s: Invalid ID in Obj used chain", __FUNCTION__); // cur
            return FALSE;
        }
        if (usedObj[cur] == OBJ_FREE) {
            //			DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));
            DEBUG("%s: Obj is free and used chain", __FUNCTION__); // cur
            return FALSE;
        }
        if (usedObj[cur] == OBJ_USED) {
            //			DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));
            DEBUG("%s: Obj is used twice", __FUNCTION__); // cur
            return FALSE;
        }
        usedObj[cur] = OBJ_USED;
        cur = objs[cur].next;
    }
    //	DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));

    for (cur = 1; cur < NUM_OBJECTS; cur++) {
        if (usedObj[cur] == 0) {
            DEBUG("%s: Obj is neither free nor used", __FUNCTION__); // cur
            return FALSE;
        }
    }

    // 3. No ObjRef occurs twice in the free chain.

    LG_memset(usedRef, 0, NUM_REF_OBJECTS);

    cur = objRefs[OBJ_REF_NULL].next;

    //	SpewAnal (("Free ObjRefs: "));
    //	DBG_Anal ({RangeInit ();})

    while (cur) {
        //		DBG_Anal ({RangeAdd (cur);})
        if (cur < 0 || cur >= NUM_REF_OBJECTS) {
            //			DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));
            DEBUG("%s: Invalid ID in ObjRef free chain", __FUNCTION__); // cur
            return FALSE;
        }
        if (usedRef[cur] == OBJ_FREE) {
            //			DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));
            DEBUG("%s: ObjRef is free more than once", __FUNCTION__); // cur
            return FALSE;
        }
        usedRef[cur] = OBJ_FREE;
        cur = objRefs[cur].next;
    }
    //	DBG_Anal ({RangeFlush ();}) SpewAnal (("%s\n", range_str));

    // 2. Every ObjSpec is in either the free chain or the used chain, and does
    // not appear twice in any chain.

    for (i = CLASS_FIRST; i < NUM_CLASSES; i++) {
        LG_memset(usedObj, 0, NUM_OBJECTS);
        head = &objSpecHeaders[i];
        cur = ((ObjSpec *)head->data)->next;
        while (cur) {
            if (cur < 0 || cur >= head->size) {
                DEBUG("%s: Invalid ID (cur) in Class (i) free chain", __FUNCTION__);
                return FALSE;
            }
            if (usedObj[cur] == OBJ_FREE) {
                DEBUG("%s: Class (i) ObjSpec (cur) is free more than once", __FUNCTION__);
                return FALSE;
            }
            usedObj[cur] = OBJ_FREE;
            cur = ((ObjSpec *)(head->data + cur * head->struct_size))->next;
        }
        cur = ((ObjSpec *)head->data)->bits.id;
        while (cur) {
            if (cur < 0 || cur >= head->size) {
                DEBUG("%s: Invalid ID (cur) in Class (i) used chain", __FUNCTION__);
                return FALSE;
            }
            if (usedObj[cur] == OBJ_FREE) {
                DEBUG("%s: Class (i) ObjSpec (cur) is free and used", __FUNCTION__);
                return FALSE;
            }
            if (usedObj[cur] == OBJ_USED) {
                DEBUG("%s: Class (i) ObjSpec (cur) is used twice", __FUNCTION__);
                return FALSE;
            }
            usedObj[cur] = OBJ_USED;
            cur = ((ObjSpec *)(head->data + cur * head->struct_size))->next;
        }
        for (cur = 1; cur < head->size; cur++) {
            if (usedObj[cur] == 0) {
                DEBUG("%s: Class (i) ObjSpec (cur) is neither free nor used", __FUNCTION__);
                return FALSE;
            }
        }
    }

        // 4. All active ObjRefs occur exactly once in the map.
        // 5. All active ObjRefs point to the map element in which they occur.
        // 6. All active ObjRefs point to active Objs.

    ObjRefStateBinIteratorInit();

    while (ObjRefStateBinIterator(&refbin))
    {
        ref = ObjRefHead(refbin);

        //		DBG_Anal ({
        //			if (ref != OBJ_REF_NULL)
        //			{
        //				ObjRefStateBinSprint (str, refbin);
        //				SpewAnal (("Contents of [%s]: ", str));
        //			}
        //		})

        while (ref != OBJ_REF_NULL) {
//			DBG_Anal ({
//				SpewAnal (("  ObjRef %3d -> Obj %3d\n", ref, objRefs[ref].obj));
//			})

            if (usedRef[ref] & OBJ_FREE) {
                ObjRefStateBinSprint(str, refbin);
                DEBUG("%s: ObjRef (ref) is free and in map bin (str)", __FUNCTION__);
                return FALSE;
            }

            if (usedRef[ref] & OBJ_IN_MAP) {
                ObjRefStateBinSprint(str, refbin);
                DEBUG("%s: ObjRef (ref) exists in two bins (one is str)", __FUNCTION__);
                return FALSE;
            }

            if (!ObjRefStateBinEqual(objRefs[ref].state.bin, refbin)) {
                ObjRefStateBinSprint(str, refbin);
                ObjRefStateBinSprint(str2, objRefs[ref].state.bin);
                DEBUG("%s: ObjRef (ref) thinks it is in (str2) but is in (str)", __FUNCTION__);
                return FALSE;
            }

            if (objRefs[ref].obj == OBJ_NULL) {
                ObjRefStateBinSprint(str, refbin);
                DEBUG("%s: ObjRef (ref) in (str) points to null Obj", __FUNCTION__);
                return FALSE;
            }

            usedRef[ref] |= OBJ_IN_MAP;

            ref = objRefs[ref].next;
        }
    }

    // 7. All ObjRefs referring to a single Obj reside in distinct map elements.
    // 8. The links between an Obj and its ObjSpec are valid.
    // 9. The links between an Obj and its ObjRefs are valid.

    cur = objs[OBJ_NULL].headused;
    while (cur != OBJ_NULL) {
        ObjRefStateBin refbins[MAX_REFS_PER_OBJ];
        ObjRefID firstref;
        ObjRefID ref;
        int i = 0;

        if (!objs[cur].active)
            goto done_checking_obj;

        ref = firstref = objs[cur].ref;
        if (ref == OBJ_REF_NULL)
            goto done_checking_refs;

        do {
            if (i == MAX_REFS_PER_OBJ) {
                DEBUG("%s: Too many ObjRefs refer to Obj (cur)", __FUNCTION__);
                return FALSE;
            }

            if (objRefs[ref].obj != cur) {
                DEBUG("%s: Obj (cur) points to ObjRef (ref) but not vice versa", __FUNCTION__);
                return FALSE;
            }

            for (j = 0; j < i; j++)
                if (ObjRefStateBinEqual(refbins[j], objRefs[ref].state.bin))
                    return FALSE;

            refbins[i++] = objRefs[ref].state.bin;
            ref = objRefs[ref].nextref;
        } while (ref != firstref);

    done_checking_refs:

        if (objs[cur].specID != OBJ_SPEC_NULL) {
            char *data;
            ObjSpecHeader *head;

            if (objs[cur].obclass < 0 || objs[cur].obclass >= NUM_CLASSES) {
                DEBUG("%s: Obj (cur) has invalid obclass (objs[cur].obclass)", __FUNCTION__);
                return FALSE;
            }

            head = &objSpecHeaders[objs[cur].obclass];
            data = head->data;

            if (((ObjSpec *)(data + head->struct_size * objs[cur].specID))->bits.id != cur ||
                ((ObjSpec *)(data + head->struct_size * objs[cur].specID))->bits.tile == TRUE) {
                DEBUG("%s: Obj (cur) obclass-specific data does not point back to it", __FUNCTION__);
                return FALSE;
            }
        }

    done_checking_obj:

        cur = objs[cur].next;
    }

        return TRUE;
}

////////////////////////////////////////////////////////////
//
// STATIC FUNCTIONS
//
// Some of these may want to become public at some point.
// However, before making one of these functions public, first
// make sure that there isn't already a public interface that you
// can use instead.
//
////////////////////////////////////////////////////////////

//////////////////////////////
//
// Returns a ObjID that is not currently being used
// or OBJ_NULL If there are none
//
// The caller is responsible for setting the fieds of the Obj, including active.
// This function does clear the ref field to ensure we don't follow bad pointers around.
//
static ObjID ObjGrab(void) {
    ObjID obj; // the next free ObjID

    //	SpewReport (("ObjGrab ()\n"));

    if (objs[OBJ_NULL].next == OBJ_NULL) // all gone
    {
        //		Warning (("No room in ObjGrab\n"));
        return OBJ_NULL;
    }

    // remove the head of the free chain and return it
    obj = objs[OBJ_NULL].next;
    objs[OBJ_NULL].next = objs[obj].next;

    // and put obj at the head of the used chain
    objs[obj].next = objs[OBJ_NULL].headused;
    objs[objs[OBJ_NULL].headused].prev = obj;
    objs[obj].prev = OBJ_NULL;
    objs[OBJ_NULL].headused = obj;

    objs[obj].ref = OBJ_REF_NULL;

    ObjInfoInit(&objs[obj].info);

    return obj;
}

//////////////////////////////
//
// Frees up the given object
// Returns FALSE (and refuses to free the object)
//   if there are objRefs referring to the object
//
static uchar ObjFree(ObjID obj) {
    //	SpewReport (("ObjFree (obj %d)\n", obj));

    // DBG_Check ({
    //	// Check if something still depends on this to be valid
    //	if (objs[obj].active && objs[obj].ref != OBJ_REF_NULL)
    //	{
    //		Warning (("Tried to free obj %d which was depended on\n", obj));
    //		return FALSE;
    //	}
    //})

    objs[obj].active = FALSE;

    // take obj out of the used chain
    if (objs[obj].prev == OBJ_NULL)
        objs[OBJ_NULL].headused = objs[obj].next;
    else
        objs[objs[obj].prev].next = objs[obj].next;

    objs[objs[obj].next].prev = objs[obj].prev;
    // don't need to clear objs[obj].next since we are resetting it immediately

    // and put it back at the head of the free chain
    objs[obj].next = objs[OBJ_NULL].next;
    objs[OBJ_NULL].next = obj;

    return TRUE;
}

//////////////////////////////
//
//	Returns an ObjRefID that is not currently being used
// or OBJ_REF_NULL if there are none
//
static ObjRefID ObjRefGrab(void) {
    ObjRefID thisobj; // the next free ObjRefID

    //	SpewReport (("ObjRefGrab ()\n"));

    if (objRefs[OBJ_REF_NULL].next == OBJ_REF_NULL) {
        //		Warning (("No room in ObjRefGrab\n"));
        return OBJ_REF_NULL;
    }

    // remove the head of the free chain and return it
    thisobj = objRefs[OBJ_REF_NULL].next;
    objRefs[OBJ_REF_NULL].next = objRefs[thisobj].next;

    objRefs[thisobj].obj = OBJ_NULL;
    objRefs[thisobj].next = OBJ_REF_NULL;
    objRefs[thisobj].nextref = OBJ_REF_NULL;

    return thisobj;
}

//////////////////////////////
//
// Frees up the space used by the ObjRef referred to by ref.
//
// If cleanup is TRUE, also deletes the reference of the ObjRef to the Obj.
//   If this is the last reference to an Obj, returns that Obj's ID.
//
static ObjID ObjRefFree(ObjRefID ref, uchar cleanup) {
    ObjID obj; // the object ref refers to

    //	SpewReport (("ObjRefFree (ref %d cleanup %d)\n", ref, cleanup));

    if (cleanup)
        obj = ObjRefLinkDel(ref); // remove the link
    else
        obj = OBJ_REF_NULL;
    objRefs[ref].next = objRefs[OBJ_NULL].next;
    objRefs[OBJ_REF_NULL].next = ref;

    return obj;
}

//////////////////////////////
//
// Returns an ObjSpecID of the specified obclass that is not currently being
// used, or OBJ_SPEC_NULL if there are none available
//
static ObjSpecID ObjSpecGrab(ObjClass obclass) {
    char *data;
    ObjSpecHeader *head;
    ObjSpecID thisid;
    ObjSpec *spec0, *thisspec;

    //	SpewReport (("ObjSpecGrab (obclass %d)\n", obclass));

    // DBG_Check ({
    //	if (obclass >= NUM_CLASSES)
    //	{
    //		Warning (("Invalid obclass %d in ObjSpecGrab\n", obclass));
    //		return OBJ_SPEC_NULL;
    //	}
    //})

    head = &objSpecHeaders[obclass];
    data = head->data;
    spec0 = (ObjSpec *)data;

    if (spec0->next == OBJ_SPEC_NULL)
        return OBJ_SPEC_NULL;

    // remove the head of the free chain and return it
    thisid = spec0->next;
    thisspec = (ObjSpec *)(data + head->struct_size * thisid);
    spec0->next = thisspec->next;

    // and put this at the head of the used chain
    thisspec->next = spec0->headused;
    ((ObjSpec *)(data + head->struct_size * spec0->headused))->prev = thisid;
    thisspec->prev = OBJ_SPEC_NULL;
    spec0->headused = thisid;

    return thisid;
}

//////////////////////////////
//
// Frees up the space used by the ObjSpec in the specified class
// referred to by id.
//
static uchar ObjSpecFree(ObjClass obclass, ObjSpecID id) {
    char *data;
    ObjSpecHeader *head;
    ObjSpec *spec0, *thisspec;

    //	SpewReport (("ObjSpecFree (obclass %d, specid %d)\n", obclass, id));

    // DBG_Check ({
    //	if (obclass >= NUM_CLASSES)
    //	{
    //		Warning (("Invalid obclass %d in ObjSpecFree\n", obclass));
    //		return FALSE;
    //	}
    //})

    head = &objSpecHeaders[obclass];
    data = head->data;
    spec0 = (ObjSpec *)&data[0];
    thisspec = (ObjSpec *)(data + head->struct_size * id);

    // take this out of the used chain
    if (thisspec->prev == OBJ_SPEC_NULL)
        spec0->headused = thisspec->next;
    else
        ((ObjSpec *)(data + head->struct_size * thisspec->prev))->next = thisspec->next;

    ((ObjSpec *)(data + head->struct_size * thisspec->next))->prev = thisspec->prev;
    // don't need to clear thisspec->next since we are resetting it immediately

    // and put it back at the head of the free chain
    thisspec->next = spec0->headfree;
    spec0->headfree = id;

    return TRUE;
}

//////////////////////////////
//
// Removes the given ObjRef from the object list in a map bin.
//
static void ObjRefRem(ObjRefID ref) {
    ObjRefID *ptr; // what we must change to splice ref out

    //	SpewReport (("ObjRefRem (ref %d)\n", ref));

    ptr = &ObjRefHead(objRefs[ref].state.bin);

    while (*ptr != ref)
        ptr = &(objRefs[*ptr].next);

    // ptr is now the address of an ObjRefID equal to ref.
    // It could be the address of the obj field of a MapPoint
    //   or the address of the next field of an ObjRef
    // Got it?

    *ptr = objRefs[ref].next;                      // we have spliced ref out
    objRefs[ref].next = OBJ_REF_NULL;              // we are no longer in a chain
    ObjRefStateBinSetNull(objRefs[ref].state.bin); // we are no longer in the world
}

//////////////////////////////
//
// Makes ref be a reference to obj
// Returns whether we could do it
//
static uchar ObjLinkMake(ObjRefID ref, ObjID obj) {
    //	SpewReport (("ObjLinkMake (ref %d, obj %d)\n", ref, obj));

    // check that obj is a real object
    // DBG_Check ({
    //	if (objs[obj].active == FALSE)
    //	{
    //		Warning (("Obj %d is inactive in ObjLinkMake\n", obj));
    //		return FALSE;
    //	}
    //})

    if (objs[obj].ref == OBJ_REF_NULL) {
        // we are the first ObjRef to refer to obj
        objs[obj].ref = ref;
        objRefs[ref].nextref = ref;
    } else {
        // there are other references already
        objRefs[ref].nextref = objRefs[objs[obj].ref].nextref;
        objRefs[objs[obj].ref].nextref = ref;
    }

    // add the reference
    objRefs[ref].obj = obj;
    return TRUE;
}

//////////////////////////////
//
// Deletes the references of ref to its obj
// If this was the last reference to that obj,
//   returns its ID, otherwise returns OBJ_NULL
//
static ObjID ObjRefLinkDel(ObjRefID ref) {
    ObjID obj;
    ObjRefID curref;

    //	SpewReport (("ObjRefLinkDel (ref %d)\n", ref));

    obj = objRefs[ref].obj;

    // DBG_Check ({
    //	// make sure we're actually referring to something
    //	if (obj == OBJ_NULL)
    //	{
    //		Warning (("ref %d refers to nothing in ObjRefLinkDel\n", ref));
    //		return OBJ_NULL;
    //	}
    //})

    if (objRefs[ref].nextref == ref) // last one
    {
        objs[obj].ref = OBJ_REF_NULL;
        return obj;
    } else {
        // run around the circular list until we reach ourselves
        // and splice ourselves out
        curref = ref;
        while (objRefs[curref].nextref != ref)
            curref = objRefs[curref].nextref;
        objRefs[curref].nextref = objRefs[ref].nextref;
        objs[obj].ref = curref; // easier than checking objs[obj].ref
    }

    // delete the reference
    objRefs[ref].obj = OBJ_NULL;
    return OBJ_NULL;
}

//////////////////////////////
//
// Takes an ObjRef and adds it to the real world at the given place.
// This ObjRef must already refer to a valid Obj.
// Returns whether everything worked okay.
//
static uchar ObjRefAdd(ObjRefID ref, ObjRefState refstate) {
    ObjID obj;
    ObjRefID *refhead;

    //	DBG_Report ({
    //		char str[80];
    //		ObjRefStateSprint (str, refstate);
    //		SpewReport (("ObjRefAdd (ref %d, %s)\n", ref, str));
    //	})

    obj = objRefs[ref].obj;

    // DBG_Check ({
    //	// make sure we point to a valid object
    //	if (obj == OBJ_NULL || objs[obj].active == FALSE)
    //	{
    //		Warning (("Ref %d points to invalid Obj %d in ObjRefAdd\n", ref, obj));
    //		return FALSE;
    //	}
    //})

    refhead = &ObjRefHead(refstate.bin);

    objRefs[ref].next = *refhead;
    *refhead = ref;

    ObjRefStateBinCopy(refstate.bin, objRefs[ref].state.bin);

    return TRUE;
}

//////////////////////////////
//
// Deletes all references to a given object.
// Returns whether the object really exists or not.
//
// NOTE: This is currently much slower than it could be because
//   we delete references one at a time and clean up each time.
//
static uchar ObjDelRefs(ObjID obj) {
    ObjRefID ref;
    ObjRefID nextref = OBJ_REF_NULL;

    //	SpewReport (("ObjDelRefs (obj %d)\n", obj));

    // DBG_Check ({
    //	if (objs[obj].active == FALSE)
    //	{
    //		Warning (("Obj %d inactive in ObjDelRefs\n", obj));
    //		return FALSE;
    //	}
    //})

    ref = objs[obj].ref;
    if (ref == OBJ_REF_NULL)
        return TRUE;

    while (TRUE) {
        nextref = objRefs[ref].nextref;
        ObjRefRem(ref);
        //		objRefs[ref].obj = OBJ_NULL;
        ObjRefFree(ref, TRUE);
        if (ref == nextref)
            break;
        ref = nextref;
    }

    objs[obj].ref = OBJ_REF_NULL;
    return TRUE;
}

//////////////////////////////
//
// Frees	up the given object and its class-specific data.
// Returns FALSE (and refuses to free the object)
//   if there are ObjRefs referring to the object.
//
static uchar ObjAndSpecFree(ObjID obj) {
    ObjClass obclass;
    ObjSpecID specID;
    uchar ok;

    //	SpewReport (("ObjAndSpecFree (obj %d)\n", obj));

    // The only place that things should be able to go wrong is in ObjFree,
    // so we do that first

    obclass = objs[obj].obclass;
    specID = objs[obj].specID;

    ok = ObjFree(obj);

    // DBG_Check ({
    //	if (!ok)
    //	{
    //		Warning (("ObjFree (%d) failed in ObjAndSpecFree\n",obj));
    //		return FALSE;
    //	}
    //})

    ok = ObjSpecFree(obclass, specID);

    // DBG_Check ({
    //	if (!ok)
    //	{
    //		Warning (("ObjSpecFree (%d, %d) failed in ObjAndSpecFree\n", obclass, specID));
    //		return FALSE;
    //	}
    //})

    return TRUE;
}

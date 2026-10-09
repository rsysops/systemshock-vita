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
** $Header: n:/project/cit/src/RCS/objapp.c 1.17 1994/05/14 03:30:03 xemu Exp $
*
*/

#include "objects.h"
#include "objwpn.h"
#include "objwarez.h"
#include "objstuff.h"
#include "objgame.h"
#include "objcrit.h"
#include "objprop.h"
#include "map.h"

////////////////////////////// APPLICATION-SPECIFIC DATA
//
// Here we define the arrays for all the application-specific
// classes defined in objapp.h.  Follow this example, and there
// won't be any trouble.
//
// ## INSERT NEW CLASS HERE

/*const*/ ObjSpecHeader objSpecHeaders[NUM_CLASSES] = {
    {NUM_OBJECTS_GUN,        sizeof(ObjGun),        (char *)&objGuns},
    {NUM_OBJECTS_AMMO,       sizeof(ObjAmmo),       (char *)&objAmmos},
    {NUM_OBJECTS_PHYSICS,    sizeof(ObjPhysics),    (char *)&objPhysicss},
    {NUM_OBJECTS_GRENADE,    sizeof(ObjGrenade),    (char *)&objGrenades},
    {NUM_OBJECTS_DRUG,       sizeof(ObjDrug),       (char *)&objDrugs},
    {NUM_OBJECTS_HARDWARE,   sizeof(ObjHardware),   (char *)&objHardwares},
    {NUM_OBJECTS_SOFTWARE,   sizeof(ObjSoftware),   (char *)&objSoftwares},
    {NUM_OBJECTS_BIGSTUFF,   sizeof(ObjBigstuff),   (char *)&objBigstuffs},
    {NUM_OBJECTS_SMALLSTUFF, sizeof(ObjSmallstuff), (char *)&objSmallstuffs},
    {NUM_OBJECTS_FIXTURE,    sizeof(ObjFixture),    (char *)&objFixtures},
    {NUM_OBJECTS_DOOR,       sizeof(ObjDoor),       (char *)&objDoors},
    {NUM_OBJECTS_ANIMATING,  sizeof(ObjAnimating),  (char *)&objAnimatings},
    {NUM_OBJECTS_TRAP,       sizeof(ObjTrap),       (char *)&objTraps},
    {NUM_OBJECTS_CONTAINER,  sizeof(ObjContainer),  (char *)&objContainers},
    {NUM_OBJECTS_CRITTER,    sizeof(ObjCritter),    (char *)&objCritters},
};

// const ObjSpecHeader ObjPropHeader = {NUM_OBJECT, sizeof(ObjProp), &ObjProps}

////////////////////////////// APPLICATION-SPECIFIC FUNCTIONS

static int map_x, map_y;

void ObjInfoInit(ObjInfo *info) {
    info->type = 0;
    info->ph = -1;
}

void ObjRefStateBinIteratorInit(void) { map_x = map_y = 0; }

uchar ObjRefStateBinIterator(ObjRefStateBin *bin) {
    if (map_y == MAP_YSIZE)
        return FALSE;
    bin->sq.x = map_x;
    bin->sq.y = map_y;
    if (++map_x == MAP_XSIZE) {
        map_x = 0;
        map_y++;
    }
    return TRUE;
}

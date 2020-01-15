/*****************************************************************************
* sysres "System Restore" Partition backup and restore utility.
* Copyright © 2019-2020 Micro Focus or one of its affiliates.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with this program; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

// (C) Copyright 2013 Hewlett-Packard Development Company, L.P.

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include "drive.h"			// mapTypes
#include "image.h" 			// iset
#include "partition.h"
#include "partutil.h"		// readable
#include "window.h"			// mode, currentFocus
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level

extern long totalMem;
extern int mallocCount;

char *labelBuf = NULL;

void initSel(int winspec, ...);

char nextAvailable(unsigned char state, unsigned char type, char isMBR);

#define RIGHTOFFSET 1

int expandSelection(selection *sel, int count) {
	unsigned int limit = sel->size + ITEMBLOCK;
 	if (count && (count + sel->count - sel->size > ITEMBLOCK)) limit = count + sel->count; // use a larger limit for directory scans to avoid multiple realloc calls
	if (ITEMLIMIT && (limit > ITEMLIMIT)) return 1; // maximum limit reached
	item *ptr = (item *)realloc(sel->sources,limit*sizeof(item));
	if (ptr != NULL) {
		totalMem += (limit - sel->size)*sizeof(item); mallocCount++;
		sel->sources = ptr; sel->size = limit; return 0;
		}
	return 1; // unsuccessful
	}

int expandPartition(disk *d) {
	unsigned int limit = d->psize + PARTBLOCK;
	if (PARTLIMIT && (limit > PARTLIMIT)) return 1; // maximum limit reached
	partition *ptr  = (partition *)realloc(d->parts,limit*sizeof(partition));
	if (ptr != NULL) {
		totalMem += PARTBLOCK*sizeof(partition); mallocCount++;
		d->parts = ptr; d->psize = limit; return 0;
		}
	return 1;
	}

int expandAuxPartition(disk *d, int reqSize) {
	if (PARTLIMIT && (reqSize > PARTLIMIT)) return 1; // maximum limit reached
	partition *ptr = (partition *)realloc(d->auxParts,reqSize*sizeof(partition));
	if (ptr != NULL) {
		totalMem += (reqSize-d->auxsize)*sizeof(partition); mallocCount++;
		d->auxParts = ptr; d->auxsize = reqSize; return 0;
		}
	return 1;
	}

int expandDisk(diskset *ds) {
	int i;
	unsigned int limit = ds->size + DRIVEBLOCK;
	if (DRIVELIMIT && (limit > DRIVELIMIT)) return 1; // maximum limit reached
	disk *ptr = (disk *)realloc(ds->drive,limit*sizeof(disk));
	if (ptr != NULL) {
		for (i=ds->size;i<limit;i++) { ptr[i].psize = 0; ptr[i].parts = NULL; ptr[i].auxParts = NULL; ptr[i].map = NULL; ptr[i].auxsize = 0; } // create additional partition space (required); some code assumes some stuff
		totalMem += DRIVEBLOCK*sizeof(disk); mallocCount++;
		ds->drive = ptr; ds->size = limit;
		return 0;
		}
	return 1;
	}

void initAttached(selection *sel) {
	resetPool(&sel->pool,1024);
	sel->horiz = 0;
	sel->sources = NULL;
	sel->size = sel->currentPathStart = 0;
	sel->currentPath = NULL;
	}

void allocateStorage(void) {
        selection *sel;
        int i, ch;
	long size = 0;
	if ((i = getpagesize()) < 4096) i = 4096;
        labelBuf = malloc(i);
	totalMem += getpagesize(); mallocCount++;
        for (ch = 0; ch < SELMAX; ch++) {
                sel = &sels[ch];
                sel->horiz = sel->size = sel->currentPathStart = sel->position = sel->top = sel->count = 0;
		sel->sources = NULL;
		resetPool(&sel->pool,4096);
                if (ch == LIST_IMAGE || ch == LIST_SOURCE || ch == LIST_TARGET) { // 12KB
			size = sizeof(char)*MAX_PATH;
                        if ((sel->currentPath = malloc(size)) == NULL) { debug(EXIT, 1,"Memory allocation error, currentPath.\n"); }
			totalMem += size; mallocCount++;
                        *sel->currentPath = 0;
                } else sel->currentPath = NULL;
        }
	initAttached(&sels[LIST_AUX1]);
	initAttached(&sels[LIST_AUX2]);
	sels[LIST_IMAGE].attached = sels[LIST_ERASE].attached = sels[LIST_SOURCE].attached = &sels[LIST_AUX1];
	sels[LIST_DRIVE].attached = sels[LIST_TARGET].attached = &sels[LIST_AUX2];

	initSel(LIST_ERASE,"Quick", (char *)NULL);
	initSel(LIST_ERASE,"Zero All", (char *)NULL);
	initSel(LIST_ERASE,"US DOD Triple Pass", (char *)NULL);
	modeMap();

	/* disks will be allocated then they are needed */
	dset.size = iset.size = 0;
	dset.drive = iset.drive = NULL;
	resetPool(&dset.pool,1024);
	resetPool(&iset.pool,1024);
	// there is no xset pool; it uses pointers from dset/iset
        }

void initSel(int winspec, ...) { // pre-define the partition entries (note: we don't use zero-separated identifier anymore; idLength is deprecated)
        selection *sel = &sels[winspec];
        int n,idsize = 0;
        va_list argp;
        char *p;
	if (sel->count >= sel->size && expandSelection(sel,0)) return; // out of memory
	sel->sources[sel->count].location = "empty";
        va_start(argp,winspec);
        while ((p = va_arg(argp, char*)) != NULL) sel->sources[sel->count].identifier = p;
//		strcpy(&(sel->sources[sel->count].identifier[idsize]),p); idsize += strlen(p)+1; }

        // sel->sources[sel->count].idLength = idsize;
        va_end(argp);
        sel->count++;
        }

int appendAnnunciator(char *buf, char *loc, int size) { // basename of /dev/...
	char *lastPath;
	int len;
	if (loc == NULL) return size;
	// while((lastPath = strchr(loc,'/')) != NULL) { loc = lastPath+1; }
	if ((lastPath = strrchr(loc,'/')) != NULL) loc = lastPath+1;
	len = strlen(loc);
	if (len >= size) return size; // too big
	size -= len;
	// strcpy(&buf[size],loc);
	memcpy(&buf[size],loc,len);
	return size-1;
	}

// return 0 if it can't be considered [complete] (i.e., mounted dirs interferring with source image partitions); do not use on loop devices
// ONLY reports [restore] if the restore mode OWNS the mount (i.e., it mounted it)
// we CANNOT reload the partition table of an incomplete thing, but we can still install stuff
// we should only reload the partition table of a restore IF it needs to be reloaded
// move to RAM option if we want to turn [restore] into [complete]

char driveComplete(disk *d) {
	int i;
	bool hasRestore = false;
	bool hasDirect = false;
	disk *dmap = (disk *)d->map;
	if (dmap == NULL) return MAP_INCOMPLETE; // this is for checking mapped drives
	if ((dmap->state & ST_BLOCK) == ST_FULL) hasDirect = true;
	for (i=0;i<dmap->partCount;i++) {
		if (d->auxParts[i].map == NULL) { // is empty; let's see if the original was full
			if (d->auxParts[i].state == CLR_RB) {
				if (currentImage.mount != NULL && !strcmp(d->auxParts[i].deviceName,(currentImage.isDisk)?((disk *)currentImage.mount)->deviceName:((partition *)currentImage.mount)->deviceName)) hasRestore = true;
				else return MAP_INCOMPLETE;
				}
			else if ((d->auxParts[i].state & ST_BLOCK) != PART_RAW) hasDirect = false;
			}
		}
	return (hasRestore)?MAP_RESTORE:(hasDirect)?MAP_DIRECT:MAP_COMPLETE;
	}

int determineMapType(disk *d, char **map) {
	int i;
	if (map != NULL) *map = NULL;
	if (mode == RESTORE) {
		if ((d->state & ST_BLOCK) == SI_LOOP) {
			// *map = ((disk *)d->map)->deviceName; return MAP_LOOP;
			*map = ((disk *)d->map)->deviceName;
			return (((disk *)d->map)->state == ST_FULL)?MAP_DIRECT:MAP_LOOP;
			}
		else if ((d->state & ST_BLOCK) == SI_PART) {
			*map = ((partition *)d->map)->deviceName;
			return (((partition *)d->map)->state == ST_FULL)?MAP_DIRECT:MAP_LOOP;
			}
		else if ((d->state & ST_BLOCK) == SI_MBR) { *map = ((disk *)d->map)->deviceName; return MAP_PARTIAL; }
		else if ((d->state & ST_BLOCK) == SI_AUTO) {
			*map = ((disk *)d->map)->deviceName;
			return driveComplete(d);
			}
		else {
			for (i=0;i<d->partCount;i++) {
				if (d->parts[i].map != NULL) return MAP_CUSTOM;
				}
			}
		}
	else if (mode == BACKUP) {
		if (!(d->diskType & DISK_MASK)) { // loop device
			if (d->state & ST_SEL) return ((d->state & ST_BLOCK) == ST_FULL)?MAP_DIRECT:MAP_LOOP;
			}
		else if (d->state & ST_SEL) {
			if (d->diskType == DISK_RAW) return MAP_DIRECT;
			if ((d->state & ST_BLOCK) == ST_AUTO) {
				if ((d->state & CLR_MASK) == CLR_BUSY) { // check to see how busy
					for (i=0;i<d->partCount;i++) {
						if (d->parts[i].state == CLR_RB && (currentImage.mount == NULL || strcmp(d->parts[i].deviceName,(currentImage.isDisk)?((disk *)currentImage.mount)->deviceName:((partition *)currentImage.mount)->deviceName))) return MAP_INCOMPLETE;
						}
					return MAP_BACKUP;
					}
				return MAP_COMPLETE;
				}
			else if ((d->state & ST_BLOCK) == ST_MAX) return MAP_DIRECT;
			else return MAP_PARTIAL;
			}
		else {
			for(i=0;i<d->partCount;i++) {
				if (d->parts[i].state & ST_SEL) return MAP_PARTIAL;
				}
			}
		}
	return 0;
	}

// should be able to just scan the drive instead
unsigned char scanDrive(selection *sel, int index) {
	int i;
	char hasMount = 0;
	for (i=index+1; i < sel->count; i++) {
		if (sel->sources[i].state == ST_NOSEL) break;
		if (verifyItemMount(&sel->sources[i])) { // clear state and set color
			sel->sources[i].state = CLR_RB; hasMount = 1;
         //             ((partition *)sel->sources[i].location)->state = CLR_RB;
			}
		else { // clear previous color state
			sel->sources[i].state &= ST_BLOCK;
	//		((partition *)sel->sources[i].location)->state &= ST_BLOCK;
			}
		}
	sel->sources[index].state &= ST_BLOCK;
	// ((disk *)sel->sources[index].location)->state &= ST_BLOCK;
	if (hasMount) {
		if ((sel->sources[index].state & ST_BLOCK) == ST_MAX) {
			sel->sources[index].state = 0;
		((disk *)sel->sources[index].location)->state = 0; // must clear this for 'ST_MAX'
			}
		sel->sources[index].state |= CLR_BUSY;
	//	((disk *)sel->sources[index].location)->state |= CLR_WB;
		}
	i = sel->sources[index].state & ST_BLOCK;
	return ((i & ST_GROUPED) == ST_GROUPED)?i:0;
	// return ((sel->sources[index].state & ST_BLOCK) == ST_MAX)?1:0;
	}

// find first entry which is an MBR or loop device
// should be able to just use diskNum to get to the disk, but we need to update it in the UI so this is ok, too
unsigned char findTopMBR(selection *sel, int top) {
	int i;
	if (!sel->count) return 0;
	for(i = top; i >= 0; i--) {
		if (sel->sources[i].state == ST_NOSEL) return 0;
		if (sel->sources[i].diskNum == -1) { // we found the top
			if (i == top) return 0; // we're already selecting a drive
			i = sel->sources[i].state & ST_BLOCK;
			return ((i & ST_GROUPED) == ST_GROUPED)?i:0;
			// return scanDrive(sel,i);  // see if the top still has a group, or if it is canceled with active mounts
			// return ((sel->sources[i].state & ST_GROUPED) == ST_GROUPED);
			}
		}
	return 0;
	}

int verifyAllSelected(selection *sel) {
	int i;
	item *itm, *parent = NULL;
	int mapChanged = 0;
	for (i=0;i<sel->count;i++) {
		itm = &sel->sources[i];
		if (itm->state == ST_NOSEL) { parent = NULL; continue; }
		if (itm->diskNum == -1) { // remove colors
			if (!(((disk *)itm->location)->diskType & DISK_MASK) || !(((disk *)itm->location)->diskType & DISK_TABLE)) { // loop device; see if it's mounted -- if RAW or GPT, also see if there are deviceName mounts through /sys/block and /proc/mounts
				// if (verifyItemMount(itm)) {
				if (diskMounted(itm->location)) {
					if (itm->state != CLR_RB && !mapChanged) mapChanged = 1;
					itm->state = CLR_RB;
					((disk *)itm->location)->state = CLR_RB;
					if (((disk *)itm->location)->map != NULL) {
						((disk *)itm->location)->map = NULL;
						mapChanged = 2;
						}
					}
				else if (itm->state == CLR_RB || ((disk *)itm->location)->state == CLR_RB) { itm->state = 0; ((disk *)itm->location)->state = 0; if (!mapChanged) mapChanged = 1; }
				continue;
				}
			if (((disk *)itm->location)->map != NULL && (hasMountMismatch(NULL,itm->location) & 4)) { // use for feedback
				((disk *)itm->location)->map = NULL;
				((disk *)itm->location)->state = 0;
				return 3; // just reload and try again
				}
			// remove color info
			parent = itm; parent->state &= ST_BLOCK;
			((disk *)parent->location)->state &= ST_BLOCK;
			continue;
			}
		if (verifyItemMount(itm)) {
			if (parent != NULL) {
				if ((parent->state & ST_BLOCK) == ST_MAX) {
					((disk *)parent->location)->state = 0;
					parent->state = 0;
					}
				parent->state |= CLR_BUSY;
				((disk *)parent->location)->state |= CLR_BUSY;
				}
			if (itm->state != CLR_RB && !mapChanged) mapChanged = 1;
			itm->state = CLR_RB;
			((partition *)itm->location)->state = CLR_RB;
			if (((partition *)itm->location)->map != NULL) { // unmapped
				((partition *)itm->location)->map = NULL;
				// ((partition *)itm->location)->state = CLR_RB;
				mapChanged = 2;
				}
			}
		else if (itm->state == CLR_RB || ((partition *)itm->location)->state == CLR_RB) { itm->state = 0; ((partition *)itm->location)->state = 0; if (!mapChanged) mapChanged = 1; } // clear color
		}
	return mapChanged;
	}

char mapField(unsigned char state) {
	unsigned char field = ' ';
	switch(state) {
		case ST_IMG: field = '*'; break; // partimage; mbr+track1
		case ST_CLONE: field = '#'; break; // partclone
		case ST_FILE: field = '+'; break; // fsarchiver, mbr only
		case ST_FULL: field = 'x'; break; // dd, mbr + all free space
		case ST_SWAP: field = 's'; break;
		case ST_MAX: field = 'x'; break; // full drive dd; can't have any mounted partitions
		case ST_AUTO: field = '*'; break;
                }
	return field;
	}


// draws the selector
void displaySelector(selection *sel, char showSel) {
        int i, j, n, m, top = (showSel & 2)?sel->position:sel->top;
        char color;
        int lw = sel->width-sel->horiz;
        char selBuf[LINELIMIT];
        char *id;
        WINDOW *win = wins[location(sel)];
        selBuf[0] = ' ';
        if (lw >= LINELIMIT) lw=LINELIMIT-1;
        int boundary = (showSel & 2)?sel->position+1:(sel->top + sel->limit);
        bool fileList = (sel == &sels[LIST_IMAGE] || sel == &sels[LIST_SOURCE] || sel == &sels[LIST_TARGET]);
	bool driveList = (sel == &sels[LIST_DRIVE]);
        bool partTable = (sel == sels[LIST_DRIVE].attached);
	bool imageTable = (sel == sels[LIST_IMAGE].attached);
	bool sourceTables = partTable || imageTable;
	bool partList = ((!sel->currentPathStart) && ((sel == &sels[LIST_SOURCE]) || (sel == &sels[LIST_TARGET]) || (sel == &sels[LIST_IMAGE]))); // set if this is a partition list (top-level directory)
	bool partTarget = ((partTable) && (mode == RESTORE));
	bool lastDivide = false;
// secondary => determine if we're window1, or window2
	void *imageMap;
	unsigned char hasGroup = 0;
	item *itm;

	if (sourceTables) { // see if topmost list entry can have annunciator
		if (top && sel->sources[top-1].state != ST_NOSEL) {
			if ((sels[LIST_IMAGE].attached == sel) && (sel->sources[top].state != ST_NOSEL) &&
				(iset.drive[sel->sources[top].diskNum].diskType == PART_CONTAINER)) lastDivide = true; // image partition gets annunciator
			}
		else lastDivide = true;
		/* see if the current position needs to be moved. Conditions are:
			if (showSel &&



		*/
		// itm = &sel->sources[sel->position];
		// if (itm->diskNum == -1)
		}
        if (boundary > sel->count) boundary = sel->count;
	if (partTable) {
		n = verifyAllSelected(sel);
		if (n > 1) { // a mapped selection has become unmapped
			i = sel->position; j = sel->top;
			fillAttached(&sels[LIST_DRIVE]);
			if (n == 2) { sel->position = i; sel->top = j; }
			if (showSel & 2) { // update the map selector
				displaySelector(sel,0);
				if (n == 3) checkMapping(false,true);
				displaySelector(sel,showSel);
				}
			else displaySelector(sel,showSel);
			manageWindow(&sels[LIST_DRIVE],CLEAR);
			return;
			}
		hasGroup = findTopMBR(sel,top);
		}
	else hasGroup = 0;
	// hasGroup = (partTable)?findTopMBR(sel,top):0;
/*
if (showSel == 2) {  if (hasGroup) mvprintw(0,0,"Hello %i ",sel->top);
else mvprintw(0,0,         "Nogo  %i ",sel->top);
	} else mvprintw(0,0,"         ");
*/


        for (i=top;i<boundary;i++) {
		itm = &sel->sources[i];
		// this determines what we display on the selector; either the name, or the description
                if (itm->identifier == NULL || *itm->identifier == 0) id = itm->location; else id = itm->identifier;


		/* code for displaying selection annunicator to left of item */
		if (itm->state == ST_NOSEL) { selBuf[0] = ' '; hasGroup = 0; lastDivide = true; }
		else if (partTable && mode == BACKUP) { // only display this in the proper window
// obtain the selector state here
if (itm->diskNum == -1) itm->state |= ((disk *)itm->location)->state & ST_BLOCK;
else itm->state |= ((partition *)itm->location)->state & ST_BLOCK;
			if (hasGroup) {
				if (hasGroup == ST_MAX) selBuf[0] = 'x'; // previously uppercase
				else { // display the auto fields as they would be saved
					// if (verifyItemMount(itm)) selBuf[0] = ' '; // can't be loop since hasGroup is set
					if (itm->state == CLR_RB) selBuf[0] = ' ';
					else selBuf[0] = mapField(nextAvailable(0,((partition *)itm->location)->type,0));
/*
					else switch(nextAvailable(0,((partition *)itm->location)->type,0)) {
						case ST_IMG: selBuf[0] = '*'; break;
						case ST_FILE: selBuf[0] = '+'; break;
						case ST_CLONE: selBuf[0] = '#'; break;
						case ST_SWAP: selBuf[0] = 's'; break;
						default: selBuf[0] = 'x';
						}
*/
					}
				itm->state |= ST_GROUP;
				}
			else {
				if (itm->diskNum == -1) hasGroup = ((itm->state & ST_GROUPED) == ST_GROUPED)?(itm->state & ST_BLOCK):0; // hasGroup = scanDrive(sel,i);
				else {
					itm->state &= ~ST_GROUP;
					if (lastDivide) { // loop device
						// if (verifyItemMount(itm)) itm->state = CLR_RB;
						// else itm->state &= ST_BLOCK;
						}
					}
// SYSRES_DEBUG(0,2,"MAPFIELD: %i\n",itm->state & ST_BLOCK);
				selBuf[0] = mapField(itm->state & ST_BLOCK);
/*
				switch(itm->state & ST_BLOCK) {
					case ST_IMG: selBuf[0] = '*'; break; // partimage; mbr+track1
					case ST_CLONE: selBuf[0] = '#'; break; // partclone
					case ST_FILE: selBuf[0] = '+'; break; // fsarchiver, mbr only
					case ST_FULL: selBuf[0] = 'x'; break; // dd, mbr + all free space
					case ST_SWAP: selBuf[0] = 's'; break;
					case ST_MAX: selBuf[0] = 'x'; break; // full drive dd; can't have any mounted partitions
					case ST_AUTO: selBuf[0] = '*'; break;
					default: selBuf[0] = ' '; break;
					}
*/
				}
			}
		else if (!partTable) selBuf[0] = ((itm->state & ST_BLOCK) == ST_SEL)?'*':' '; // image selector, etc.
		else { // partTable, mode = ERASE or RESTORE

			if (hasGroup) { // determine partition mapping here (implies this item is a partition, not a disk, since a group mapping doesn't haven individually-assignable partition maps)
				if (((partition *)itm->location)->map != NULL) { // MUST be a part, since it's in a group
					selBuf[0] = mapField(((partition *)((partition *)itm->location)->map)->state & ST_BLOCK);


					/*
					switch(  ((partition *)((partition *)itm->location)->map)->state) {
						default: selBuf[0] = 'e';
						}
					*/
				} else selBuf[0] = ' ';
				itm->state |= ST_GROUP;
			}
			else { // individual entries
			if (itm->diskNum == -1) {
				hasGroup = ((itm->state & ST_GROUPED) == ST_GROUPED)?(itm->state & ST_BLOCK):0; // scanDrive(sel,i); // need this to set color on mounted partitions
				}
			else {
				itm->state &= ~ST_GROUP;
				if (lastDivide) { // loop device
                                                // if (verifyItemMount(itm)) itm->state = CLR_RB;
                                                // else itm->state &= ST_BLOCK;
                                                }
				}
			if (mode == RESTORE) {
				if (itm->state & ST_BLOCK) { // item selected
					if ((itm->state & ST_BLOCK) == SI_PART) {
						if (itm->diskNum == -1) selBuf[0] = mapField(((partition *)((disk *)itm->location)->map)->state & ST_BLOCK);
						else selBuf[0] = mapField(((partition *)((partition *)itm->location)->map)->state & ST_BLOCK);
						}
					else {
						if (itm->diskNum == -1) selBuf[0] = mapField(((disk *)((disk *)itm->location)->map)->state & ST_BLOCK);
						else selBuf[0] = mapField(((disk *)((partition *)itm->location)->map)->state & ST_BLOCK);
						}
					}
				else selBuf[0] = ' ';
/*
				switch(itm->state & ST_BLOCK) {
					case SI_MBR: selBuf[0] = 'm'; break;
					case SI_PART: selBuf[0] = 'p'; break;
					case SI_AUTO: selBuf[0] = 'O'; break;
					case SI_LOOP: selBuf[0] = 'L'; break;
					default: selBuf[0] = ' '; break;
					}
*/
				}
			else  selBuf[0] = (itm->state & ST_BLOCK)?'*':' ';
		}
			}


		/* end of selection annunciator code */

		color = itm->state & CLR_MASK; // extract color
		if (color == CLR_BUSY) color = 0;
if (partTable && !color && selBuf[0] == ' ' && mode == RESTORE) color = CLR_WDIM;
                n = strlen(id); // start 'n'
                if (n >= lw) n = lw-1; // compensate for the beginning ' '
                strncpy(&selBuf[1],id,n);
		if (hasGroup && (itm->diskNum != -1)) { // indent
			selBuf[1] = selBuf[0];
			selBuf[0] = ' ';
			}
                if (fileList && itm->identifier == NULL) { // check if directory
                        if (n == lw-1) n = lw-2; // compenstate for trailing '/'
                        selBuf[++n] = '/';
                        }
                memset(&selBuf[n+1],' ',lw-n); // end 'n'
		// display add'l info such as drive names, image maps, etc., to the right of an entry
		n = lw-RIGHTOFFSET; // if (m != lw), then highlight text between (m,n)
		if (sourceTables && (itm->state != ST_NOSEL)) { // show the entry here
			if (lastDivide) {
				if (itm->diskNum == -1) {
					n=appendAnnunciator(selBuf,((disk *)itm->location)->deviceName,n);
					lastDivide = false;
					}
				else {
					n=appendAnnunciator(selBuf,((partition *)itm->location)->deviceName,n); // loop device
					}
				}
			m = n;
			// if (partTable) sprintf(&selBuf[n-2],2,"%02X",itm->state);
			if (partTarget && (itm->state & ST_BLOCK) && (!(itm->state & ST_GROUP) || itm->diskNum == -1)) { // show image map
				imageMap = (itm->diskNum == -1)?((disk *)itm->location)->map:((partition *)itm->location)->map;
				if (imageMap != NULL) {
					// imageMap is either a disk, or a partition
					m=appendAnnunciator(selBuf,((itm->state & ST_BLOCK) == SI_PART)?((partition *)imageMap)->deviceName:((disk *)imageMap)->deviceName,n);
					}
				// if (itm->state & SI_MBR) m=appendAnnunciator(selBuf,((disk *)imageMap)->deviceName,n);
				// else m=appendAnnunciator(selBuf,((partition *)imageMap)->deviceName,n);
				}
		} else if (driveList) { // display information for the target device list
			// display drive size
			if (lw > 60) {
				readableSize( ((disk *)itm->location)->sectorSize * ((disk *)itm->location)->deviceLength);
				memcpy(&selBuf[40-strlen(readable)],readable,strlen(readable));
				// memcpy(&selBuf[40-strlen()->size)],((disk *)itm->location)->size,strlen(((disk *)itm->location)->size));
				}
			// if mode == restore, then show map w/details; if backup, then show backup delims
			if (i == sel->position || (itm->state & ST_SEL)) {

				char *map;
				char *flag = mapTypes[determineMapType(itm->location,&map)];
				if (map != NULL) { m = n = appendAnnunciator(selBuf,map,n); m = appendAnnunciator(selBuf,flag,n); }
				else m = n = appendAnnunciator(selBuf,flag,n);
				}
			if (diskMounted(itm->location)) color = CLR_YB; else color = 0; // TODO: if (i == sel->position || itm->state & ST_SEL), also check color state of attached selections
			// if (((disk *)itm->location)->state & CLR_MASK) color = CLR_RB; // disk color reflects any active mounts
			}

                selBuf[lw] = 0;
		if (partList) {
			color = 0; // clears colors, if any
			if (itm->diskNum == -2) {
				if (verifyQuickMount(itm->location)) color=(strcmp(itm->location,"/ram"))?CLR_YB:CLR_BLB;
				}
			else if (verifyItemMount(itm)) color = CLR_YB;
			}
		int highlight = ((showSel) && (i == sel->position))?A_REVERSE:0;  // A_BOLD for lighter text, A_BLINK for bright text

		/* compare location to disk selector; move disk selector accordingly */
		if (highlight && partTable) { // compare location to disk selection; move accordingly
			// int curDisk = sels[LIST_DRIVE].sources[sels[LIST_DRIVE].position].diskNum;
			int curDisk = sels[LIST_DRIVE].position;
			if (itm->diskNum == -1) {
				for (j = 0; j < dset.dcount; j++) {
					if (itm->location == &dset.drive[j]) break;
					}
				if (j == dset.dcount) j = curDisk;
				}
			else j = itm->diskNum;
			if (j != curDisk) { // select a different disk
				sels[LIST_DRIVE].sources[curDisk].state = ST_SEL;
				sels[LIST_DRIVE].position = j;
				manageWindow(&sels[LIST_DRIVE],CLEAR);
				}
// 			mvprintw(1,50,"disk %i\n",j);
			}

/* re-introduce this in a debug mode, perhaps

if (i == sel->position && partTable && mode == RESTORE) { // TEMPORARY
 	if (itm->diskNum == -1) {
		mvprintw(0,0,"Disk %i",i);
		mvprintw(0,8,"map [%p]",((disk *)itm->location)->map);
		mvprintw(0,20,"name [%s]",((disk *)itm->location)->deviceName);
	} else {
		mvprintw(0,0,"Part %i",i);
		mvprintw(0,8,"map [%p]",((partition *)itm->location)->map);
		mvprintw(0,20,"name [%s]",((partition *)itm->location)->deviceName);
		mvprintw(0,40,"label [%s]",((partition *)itm->location)->diskLabel);
		}
	}
else if (i== sel->position && partTable && mode == BACKUP) {
	mvprintw(0,0,"State: [%i]",itm->state);
	}
// END TEMPORARY
*/

		if ((showSel == 2) && (color != CLR_RB)) color = CLR_WDIM; // an unmapped item



                wattron(win,COLOR_PAIR(color) | highlight);
                mvwprintw(win,i-sel->top+sel->vert,sel->horiz-1,selBuf);
		if (hasGroup && selBuf[0] == ' ') { // left-side drill-down indicators
			if ((i == (sel->count-1)) || sel->sources[i+1].state == ST_NOSEL) mvwaddch(win,i-sel->top+sel->vert,sel->horiz-1,ACS_LLCORNER);
			else mvwaddch(win,i-sel->top+sel->vert,sel->horiz-1,ACS_LTEE); // ACS_LLCORNER);
			}
		if (partTarget && (n != lw-RIGHTOFFSET) && m != n) { // add an arrow
	//		mvwaddch(win,i-sel->top+sel->vert,sel->horiz-1+n,ACS_RARROW);
			mvwaddch(win,i-sel->top+sel->vert,sel->horiz-1+n,'@');
			}
                wattroff(win,COLOR_PAIR(color) | highlight);
/*
		if (partTarget && m != n) {
			selBuf[n] = 0;
			if (highlight) {
				if (color == CLR_BDIM) {
                                        wattron(win,COLOR_PAIR(CLR_DIMG));
                                        mvwprintw(win,i-sel->top+sel->vert,sel->horiz-1+m,&selBuf[m]);
                                        wattroff(win,COLOR_PAIR(CLR_DIMG));
					}
				else {
					wattron(win,COLOR_PAIR(CLR_HIGHG));
					mvwprintw(win,i-sel->top+sel->vert,sel->horiz-1+m,&selBuf[m]);
					wattroff(win,COLOR_PAIR(CLR_HIGHG));
					}
				}
			else {
				wattron(win,COLOR_PAIR(CLR_GB));
				mvwprintw(win,i-sel->top+sel->vert,sel->horiz-1+m,&selBuf[m]);
				wattroff(win,COLOR_PAIR(CLR_GB));
				}
			}
*/
                }
        if (sel->top) mvwaddch(win,sel->vert,sel->width,ACS_UARROW);
        if ((sel->top + sel->limit) < sel->count) mvwaddch(win,sel->vert+sel->limit-1,sel->width,ACS_DARROW);
        }

void updateSelectionStat(void) {
        int i;
        int state = 0;
        int count = 0;
        selection *sel = currentWindowSelection();
        WINDOW *win = wins[location(sel)];
        if (mode == BACKUP && (currentFocus == WIN_AUX1)) {
                for (i=0; i<sel->count;i++) {
                        char *selState = &(sel->sources[i].state);
                        if (*selState && *selState != ' ') {
                                count++;
                                if (*selState == '+') state = 1; // mixed
                                }
                        }
                if (count == sel->count) mvwprintw(win,1,1,"Complete");
                else if (count) mvwprintw(win,1,1," Partial");
                else mvwprintw(win,1,1,           "   Empty");
                if (state) wprintw(win," Mixed");
                else wprintw(win,      "      ");
                }
        }

char nextAvailable(unsigned char state, unsigned char type, char isMBR) {
	bool s1, s2, s3, s4;
	if (!type) s1 = s2 = s3 = s4 = isMBR;
	else {
		s1 = isSupported(type,ST_IMG);
		s2 = isSupported(type,ST_CLONE);
		s3 = isSupported(type,ST_FILE);
		s4 = isSupported(type,ST_SWAP);
		}
	state &= ST_BLOCK; // get rid of the colors
	if (!state || !(state & ST_SEL)) {  // ' '
		if (isMBR) return ST_AUTO;
		if (s1) return ST_IMG;
		state = ST_IMG;
		}
	if (state == ST_AUTO) return ST_IMG; // isMBR implied
	if (isMBR) { s4 = 0; } // MBR supports auto, * (mbr only), # (mbr + 32Kb), + (mbr + 32MB), and x (mbr + all free) right now.
	if (state == ST_IMG) {
		if (s2) return ST_CLONE;
		state = ST_CLONE;
		}
	if (state == ST_CLONE) {
		if (s3) return ST_FILE;
		state = ST_FILE;
		}
	if (state == ST_FILE) {
		if (s4) return ST_SWAP;
		state = ST_SWAP;
		}
	if (state == ST_SWAP) return ST_FULL;
	if (state == ST_FULL) return (isMBR)?ST_MAX:0; // <-- only applies to MBR selection
	return 0;
	}

char diskAvailable(char prev, disk *d) {
	if (d->diskType & DISK_MASK) return nextAvailable(prev,0,(d->diskType & DISK_TABLE));
	return nextAvailable(prev,d->diskType,0);
	}

char nextAvailableAux(char state) {
	state &= ST_BLOCK; // get rid of the colors
	if (state) return 0;
	else return ST_SEL;
	}

void toggleBackup(selection *sel, char multiple, char other) { //  '#' (dd), '*' (img), '+' (fs), ' ' (exclude)
// static c = 0;
	if (!sel->count) return;
	item *itm = &sel->sources[sel->position];
	bool nextState;
	unsigned char lastMBR = 0;
	if (sel->attached != NULL) nextState = (itm->state & (ST_SEL | ST_GROUP))?0:1;
	else { // partition selector
		if (itm->diskNum == -1) nextState = (itm->state & ST_SEL)?0:1;
		else nextState = (itm->state & (ST_SEL | ST_GROUP))?0:1;
// mvprintw(0,0,"[%i-%i-%02X-%i-%i]",nextState,c++,itm->state,ST_SEL,ST_GROUP);
		}
	char *stateStore;
	int i;
	bool unknownDisk;
	disk *d;
	// find the first non-mounted parition state for ctrl-shift condition
	if (!multiple && (itm->state & ST_GROUP) && (itm->diskNum != -1)) { // partition within a group selection
		if ((itm->state & CLR_MASK) == CLR_RB) return; // can't clear a mounted item
		d = &dset.drive[itm->diskNum];
		d->state = ST_IMG; // '*' is the only option for MBR right now
		for (i=sel->position;i>=0;i--) {
			if (sel->sources[i].diskNum != -1) continue;
			sel->sources[i].state = 0;
			break;
			}
		for (i=0;i<d->partCount;i++) {
			// if (&d->parts[i] == itm->location) d->parts[i].state = 0;
	 		if ((d->parts[i].state & CLR_MASK) != CLR_RB) d->parts[i].state = nextAvailable(0,d->parts[i].type,0);
			}
		manageWindow(NULL,INIT);
		return; // can't modify when in a group
		}
	if ((multiple && (sel->attached == NULL)) && verifyItemMount(itm)) { // find the first non-mounted state
		for (i=0;i<sel->count;i++) {
			itm = &(sel->sources[i]);
			if (itm->state == ST_NOSEL) continue;; // a separator
			if (verifyItemMount(itm)) continue;
			// nextState = (itm->state & (ST_SEL | ST_GROUP))?0:1;
                if (itm->diskNum == -1) nextState = (itm->state & ST_SEL)?0:1;
                else nextState = (itm->state & (ST_SEL | ST_GROUP))?0:1;

			break;
			}
		}
	// modify the state of one or all partitions
	for (i=((multiple)?0:sel->position);i<((multiple)?sel->count:(sel->position+1));i++) {
		itm = &(sel->sources[i]);
		if (itm->state == ST_NOSEL) { lastMBR = 0; continue; }
		if (itm->location != NULL && sel->attached == NULL) {  // party
			if (itm->diskNum == -1) stateStore = &((disk *)itm->location)->state;  // party
			else stateStore = &((partition *)itm->location)->state; // party
			}
		else stateStore = NULL;
		if (sel->attached != NULL) itm->state = (nextState)?ST_SEL:0; // drive list ONLY (not a partition list)
		// else if (verifyItemMount(itm)) { itm->state = 0; } // partition is mounted; can't be selected
		else if (itm->state == CLR_RB) { ; } // part is mounted; leave as-is (prev. itm->state = 0)
		else if (multiple) {
			if (lastMBR) { ; } // { mvprintw(1,0,"%i",c); } // don't toggle state; top-level MBR is a currently selected group
			else if (nextState) { // determine the next state if the original state is blank
				if (itm->state & ST_NEGATE) itm->state |= ST_SEL;
				// if (itm->state & ST_NEGATE) itm->state |= ST_SEL; // re-select it; previous wasn't blank
				else if (itm->diskNum == -1) {
					itm->state = (other)?nextAvailableAux(0):diskAvailable(0,itm->location);
					lastMBR = ((itm->state & ST_SEL) && (itm->state & ST_GROUP));
					}
				else itm->state = (other)?nextAvailableAux(0):nextAvailable(0,((partition *)itm->location)->type,0);  // party
			}
			else if (itm->diskNum == -1) {
				itm->state = (itm->state & ST_GRPNEG);
				lastMBR = ((itm->state & ST_SEL) && (itm->state & ST_GROUP));
				}
			else itm->state = (itm->state & ST_NEGATE); // clear, but keep a memory
			}
		else { // partition list; go through options
//			if (nextState && (itm->state & ST_BLOCK)) itm->state |= ST_SEL; // re-select previous value from blank
//			else {
				if (itm->diskNum == -1) { // it's the mbr; not a partition
					itm->state = (other)?nextAvailableAux(itm->state):diskAvailable(itm->state,itm->location);
					}
				else {
					itm->state = (other)?nextAvailableAux(itm->state):nextAvailable(itm->state & ST_BLOCK,((partition *)itm->location)->type,0);  // party
					}
//				}
			}
		if (stateStore != NULL) *stateStore = itm->state;
		}
	manageWindow(NULL,REFRESH);
        // updateSelectionStat();
	}

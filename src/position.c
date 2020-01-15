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

#include <stdio.h>
#include <string.h>

#include <ncurses.h>
#include "cli.h"
#include "partition.h"
#include "window.h"

bool busyMap = false;

// move through the drive list on the right pane (only use drives as presented by aux selector, not LIST_DRIVE selector)
void positionDrive(selection *sel, char direction) {  // 0 = up (left)
	selection *aux = (sel == &sels[LIST_AUX1])?&sels[LIST_AUX2]:&sels[LIST_AUX1];
// static int c=0;
// mvprintw(1,0,"%i [%i] [%i]",c++,aux->count,sel->sources[sel->position].state);
	if (!aux->count || (sel->sources[sel->position].state == CLR_WDIM && sel == &sels[LIST_AUX1]) || sel->sources[sel->position].state == CLR_RB) return; // can't do anything with mounted/inactive partitions
	manageWindow(aux,(direction)?DISKDOWN:DISKUP);
	wrefresh(wins[location(aux)]);
	}

void clearBusyOld(void) {
	int i, j;
	for (i=0;i<dset.dcount;i++) {
		dset.drive[i].map = NULL; dset.drive[i].state = 0;
		for(j=0;j<dset.drive[i].partCount;j++) { dset.drive[i].parts[j].map = NULL; dset.drive[i].parts[j].state = 0; }
		}
	}

void clearBusy(void) {
	disk *d;
	int i,j;
	for (i=0;i<dset.dcount;i++) {
		d = &dset.drive[i];
		d->map = NULL; d->state &= CLR_MASK;
		for(j=0;j<d->partCount;j++) { d->parts[j].map = NULL; d->parts[j].state = 0; }
		}
	busyMap = false;
	}

void clearBusy2(void) {
        selection *sel = &sels[LIST_DRIVE];
        disk *d;
        int i, j;
        for (i=0;i<sel->count;i++) {
                if (i != sel->position && !(sel->sources[i].state & ST_SEL)) continue;
		d = &dset.drive[sel->sources[i].diskNum];
		d->map = NULL; d->state &= CLR_MASK; //  0;
		for (j=0;j<d->partCount;j++) { d->parts[j].map = NULL; d->parts[j].state = 0; }
		}
	}

// check to see if any drives on display have an image map
bool checkBusyMap(void) {
	selection *sel = &sels[LIST_DRIVE];
	disk *d;
	int i, j;
	busyMap = true;
	for (i=0;i<sel->count;i++) {
		if (i != sel->position && !(sel->sources[i].state & ST_SEL)) continue;
		d = &dset.drive[sel->sources[i].diskNum];
		if (d->map != NULL) return true;
		for (j=0;j<d->partCount;j++) {
			if (d->parts[j].map != NULL) return true;
			}
		}
	busyMap = false;
	return false;
	}

/*
// check even the ones that aren't displayed
bool checkAnyBusyMap(void) {
	selection *sel = &sels[LIST_DRIVE];
	disk *d;
	int i, j;
	for (i=0; i<sel->count;i++) {
		d = &dset.drive[sel->sources[i].diskNum];
		if (d->map != NULL) return true;
		for (j=0;j<d->partCount;j++) {
			if (d->parts[j].map != NULL) return true;
			}
		}
	}
*/

// nearly identical to checkBusyMap()
bool checkBusyState(void) {
	selection *sel = &sels[LIST_DRIVE];
	disk *d;
	int i, j;
	for (i=0;i<sel->count;i++) {
		if (i != sel->position && !(sel->sources[i].state & ST_SEL)) continue;
		d = &dset.drive[sel->sources[i].diskNum];
		if (d->state & ST_SEL) return true;
		for (j=0;j<d->partCount;j++) { if (d->parts[j].state & ST_SEL) return true; }
		}
	return false;
	}

// check a target disk if it has any mounts (check any known /dev/sda sub-devices for GPT, etc.)
bool diskBusy(disk *d) {
	int i;
	if ((d->diskType & TYPE_MASK) == DISK_RAW) return false;
	else if (!(d->diskType & DISK_MASK)) {
		if (verifyQuickMount(d->deviceName)) return true;
		return false;
		}
	for (i=0;i<d->partCount;i++) {
		if (verifyQuickMount(d->parts[i].deviceName)) return true;
		}
	return false;
	}

void toggleMap(bool isValid, bool hasUI) {
	selection *selImage = &sels[LIST_AUX1];
	selection *selDrive = &sels[LIST_AUX2];
	if (selImage->count == 0 || selDrive->count == 0) return;
	item *selItem = &selImage->sources[selImage->position];
	item *selTarget = &selDrive->sources[selDrive->position];
	disk *d = NULL;
	int prevTop = selDrive->top;
	int prevPos = selDrive->position;
	int partCount;
	partition *parts;
	bool clearEntry = false; // only if we're allowing incompatible selectors to clear entries
	int i;

	int prevState = selTarget->state & ST_BLOCK;
	int color = 0; // save the state color for the disk
	if (!isValid) { // unselect something if it is set when a non-compatible selection is selected
		// return; // don't allow invalid entries to control the mapping
		if (!prevState) return;
		prevState = 0;
		clearEntry = true;
		}
	if (selItem->diskNum == -1) {
		selTarget->state = (((disk *)selItem->location)->partCount)?SI_AUTO:SI_LOOP;
		}
	else selTarget->state = SI_PART;
	// selTarget->state = (selItem->diskNum == -1)?SI_AUTO:SI_PART;
	if (selTarget->diskNum == -1) {
		d = selTarget->location;
		color = d->state & CLR_MASK;
		if (clearEntry || d->map == selItem->location) {
			selTarget->state = (prevState == SI_AUTO)?SI_MBR:0;
			// if (selTarget->state == SI_MBR && ((disk *)selItem->location)->state == ST_FULL) selTarget->state = 0;
			if (!selTarget->state) d->map = NULL;
			}
		else d->map = selItem->location;
		if (selItem->diskNum == -1 && selTarget->state == SI_AUTO) { // could be a loop device; only do a direct-select
			if (!(((disk *)selItem->location)->diskType & DISK_MASK)) selTarget->state = SI_LOOP;
			else if (((disk *)selItem->location)->diskType == DISK_RAW) selTarget->state = SI_LOOP;
			}
		d->state = selTarget->state | color;
		}
	else { // part
		if (selTarget->state  == SI_AUTO && selItem->diskNum == -1) selTarget->state = SI_LOOP;
		partition *p = selTarget->location;
		if (clearEntry || p->map == selItem->location) { selTarget->state = 0; p->map = NULL; }
		else p->map = selItem->location;


		d = &dset.drive[selTarget->diskNum]; // containing disk
		color = d->state & CLR_MASK;
/* THIS NO LONGER APPLIES SINCE THE TARGET WILL BE A PART NOW; NO LOOP 'PARTS' ANYMORE
		if (!selDrive->position || (selDrive->sources[selDrive->position-1].state == ST_NOSEL)) { // loop device (drive map will point to part or drive)
			d->map = p->map;
			d->state = selTarget->state;
			}
		else if (d->state & ST_GROUPED) {
*/
		if ((d->state & ST_GROUPED) == ST_GROUPED) { // ungroup it
			d->state = SI_MBR | color; // it's grouped; ungroup all and copy across other values
			for (i=0;i<((disk *)d->map)->partCount;i++) {
				if (p == &d->auxParts[i]) continue; // don't modify this one
				if (d->auxParts[i].map != NULL) d->auxParts[i].state = SI_PART; // enable it
				}
			d = NULL;
			}
		else d = NULL;
		p->state = selTarget->state;
		}
	if (d != NULL) {
		if (selTarget->state == SI_MBR || selTarget->state == SI_AUTO) { // figure out auxParts and copy onto 'd'
			partCount = ((disk *)selItem->location)->partCount;
			parts = ((disk *)selItem->location)->parts;
			if (((d->auxsize < partCount) && expandAuxPartition(d,partCount)) ||
				((selTarget->diskNum != -1) && diskBusy(d))) { // memory or target mapping issue; unselect this drive
				d->map = NULL;
				selTarget->state = 0;
				d->state = color;
				}
			else {
				if (selTarget->state == SI_MBR) { // clear all items when switching from 'O' to 'm'
					for(i=0;i<partCount;i++) { d->auxParts[i].map = NULL; d->auxParts[i].state = 0; }
					}
				else {
					memcpy(d->auxParts,parts,partCount*sizeof(partition)); // copy partition from image description (only need to do this for SI_MBR)
					for (i=0;i<partCount;i++) {
						d->auxParts[i].map = (parts[i].type)?&parts[i]:NULL;
						d->auxParts[i].state = SI_PART;
						}
					}
				// populate various things (if no source part, or target part is mounted, then don't map across, but set labels, etc. for feedback)
				if (selTarget->diskNum == -1 && (hasMountMismatch(NULL,d) & 4)) { // make sure restore/active partitions won't get destroyed
					d->map = NULL;
					selTarget->state = 0;
					d->state = color;
					}
				else {
					for (i=0;i<partCount;i++) {
						if (!d->auxParts[i].type) continue; // no partition within image
						}
					}
				}
			}
		else if (selTarget->state == SI_LOOP) { // populate loop device (no partition mapping needed)
/*
			if ((!d->auxsize  && expandAuxPartition(d,1)) || diskBusy(d)) {
				d->map = NULL;
				d->state = selTarget->state = 0;
				}
			else {
				memcpy(d->auxParts,selItem->location,sizeof(partition)); // single partition
				d->auxParts[0].state = SI_PART;
				d->auxParts[0].deviceName = d->deviceName;
				d->auxParts[0].map = selItem->location;
				}
*/
			}
		else if (!(d->diskType & DISK_TABLE)) { // clear drive state

//			d->state = 0;
//			d->map = NULL;
			}
	//	fillAttached(&sels[LIST_DRIVE]); selDrive->position = prevPos; selDrive->top = prevTop;
	//	manageWindow(sels[LIST_DRIVE].attached,CLEAR);
		}
	fillAttached(&sels[LIST_DRIVE]); selDrive->position = prevPos; selDrive->top = prevTop;
	if (hasUI) {
		manageWindow(sels[LIST_DRIVE].attached,CLEAR);
		manageWindow(&sels[LIST_DRIVE],CLEAR);
		busyMap = checkBusyMap();
		showFunctionMenu(0);
		displaySelector(sels[LIST_DRIVE].attached,(isValid)?3:2); // need this for keyboard selection feedback
		wrefresh(wins[WIN_AUX2]);
		}
	}
